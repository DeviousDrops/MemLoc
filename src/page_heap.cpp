#include "page_heap.h"

#include <cstring>

#include "os.h"

namespace memloc {

alignas(std::atomic_ref<std::uint64_t>::required_alignment) constinit std::uint64_t
    g_chunk_registry[kRegistryWords] = {};
constinit PageHeap g_page_heap;

namespace {

void registry_add(const Chunk* c) {
  const std::uintptr_t idx = reinterpret_cast<std::uintptr_t>(c) >> kChunkShift;
  registry_word(idx >> 6).fetch_or(std::uint64_t{1} << (idx & 63), std::memory_order_relaxed);
}

constexpr std::uint64_t run_mask(unsigned first, unsigned units) {
  // units <= 63 because unit 0 is always the header, so the shift is defined.
  return ((std::uint64_t{1} << units) - 1) << first;
}

}  // namespace

Chunk* PageHeap::grow() {
  void* mem = os_map_aligned(kChunkSize, kChunkSize);
  if (mem == nullptr) return nullptr;
  // Fresh anonymous memory is already zero; only the non-zero fields need setting.
  auto* c = static_cast<Chunk*>(mem);
  c->magic = kChunkMagic;
  c->used = 1;  // unit 0 = this header
  registry_add(c);
  if (last_ != nullptr) {
    last_->next = c;
  } else {
    first_ = c;
  }
  last_ = c;
  chunk_count_.fetch_add(1, std::memory_order_relaxed);
  return c;
}

Span* PageHeap::alloc_span(unsigned units, SpanKind kind) {
  const std::scoped_lock lock(mu_);
  Chunk* chunk = nullptr;
  int first = -1;
  for (Chunk* c = first_; c != nullptr; c = c->next) {
    if (~c->used == 0) continue;
    first = find_free_run(~c->used, units);
    if (first >= 0) {
      chunk = c;
      break;
    }
  }
  if (chunk == nullptr) {
    chunk = grow();
    if (chunk == nullptr) return nullptr;
    first = find_free_run(~chunk->used, units);
  }

  const auto u = static_cast<unsigned>(first);
  chunk->used |= run_mask(u, units);
  std::memset(&chunk->owner[u], static_cast<int>(u), units);
  Span* s = &chunk->spans[u];
  *s = Span{};
  s->units = static_cast<std::uint8_t>(units);
  s->kind = kind;  // under mu_, so memloc_stats() can read it (D-28)
  return s;
}

void PageHeap::free_span(Span* s) {
  const std::scoped_lock lock(mu_);
  Chunk* c = chunk_of(s);
  const auto u = static_cast<unsigned>(s - c->spans);
  s->kind = SpanKind::kFree;
  c->used &= ~run_mask(u, s->units);
}

bool PageHeap::resize_span(Span* s, unsigned units) {
  const std::scoped_lock lock(mu_);
  Chunk* c = chunk_of(s);
  const auto u = static_cast<unsigned>(s - c->spans);
  if (units < s->units) {
    c->used &= ~run_mask(u + units, s->units - units);
  } else if (units > s->units) {
    if (u + units > kUnitsPerChunk) return false;
    const std::uint64_t extra = run_mask(u + s->units, units - s->units);
    if ((c->used & extra) != 0) return false;
    c->used |= extra;
    std::memset(&c->owner[u + s->units], static_cast<int>(u), units - s->units);
  }
  s->units = static_cast<std::uint8_t>(units);
  return true;
}

}  // namespace memloc
