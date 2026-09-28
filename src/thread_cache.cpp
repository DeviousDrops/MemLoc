#include "thread_cache.h"

#include <pthread.h>

#include <cerrno>
#include <cstdint>

#include "central.h"
#include "size_class.h"

namespace memloc {
namespace {

enum class TcState : std::uint8_t { kUninit, kActive, kDead };

struct FreeList {
  void* head;
  std::uint32_t count;
};

struct ThreadCache {
  FreeList lists[kNumClasses];
  TcState state;
};

// Zero-initialised, trivially destructible and in static TLS: usable from
// the very first malloc of a thread without running any code. D-14
constinit thread_local ThreadCache t_cache __attribute__((tls_model("initial-exec"))) = {};

pthread_key_t g_exit_key;
pthread_once_t g_exit_key_once = PTHREAD_ONCE_INIT;

// Returns every cached object to the central lists. Runs as the pthread key
// destructor when the thread exits.
void flush_on_exit(void* arg) {
  auto* tc = static_cast<ThreadCache*>(arg);
  for (unsigned c = 0; c < kNumClasses; ++c) {
    FreeList& fl = tc->lists[c];
    if (fl.head != nullptr) g_central[c].release(fl.head);
    fl = FreeList{};
  }
  // Any malloc/free issued by later TSD destructors bypasses the cache: there
  // would be nobody left to flush it.
  tc->state = TcState::kDead;
}

void create_exit_key() { pthread_key_create(&g_exit_key, flush_on_exit); }

void activate(ThreadCache& tc) {
  // Set the state first: if pthread_setspecific ever needs memory (only after
  // 32 keys exist), the nested malloc finds an active cache instead of
  // recursing back here.
  tc.state = TcState::kActive;
  pthread_once(&g_exit_key_once, create_exit_key);
  pthread_setspecific(g_exit_key, &tc);
}

__attribute__((noinline)) void* refill(unsigned cls) {
  ThreadCache& tc = t_cache;
  if (tc.state == TcState::kUninit) activate(tc);
  const unsigned want = tc.state == TcState::kActive ? batch_size(cls) : 1;
  void* head = nullptr;
  const unsigned got = g_central[cls].fetch(cls, want, &head);
  if (got == 0) {
    errno = ENOMEM;
    return nullptr;
  }
  // Keep the rest of the batch; hand out the first object.
  FreeList& fl = tc.lists[cls];
  fl.head = next_of(head);
  fl.count = got - 1;
  return head;
}

// Detaches the first n objects of the list and returns them to the central list.
__attribute__((noinline)) void release_batch(unsigned cls, FreeList& fl, unsigned n) {
  void* first = fl.head;
  void* last = first;
  for (unsigned i = 1; i < n; ++i) last = next_of(last);
  fl.head = next_of(last);
  fl.count -= n;
  next_of(last) = nullptr;
  g_central[cls].release_batch(cls, first);
}

__attribute__((noinline)) void free_slow(unsigned cls, void* p) {
  ThreadCache& tc = t_cache;
  if (tc.state == TcState::kUninit) {
    activate(tc);
    tc_free(cls, p);
    return;
  }
  next_of(p) = nullptr;  // kDead: straight to the central list
  g_central[cls].release(p);
}

}  // namespace

void* tc_alloc(unsigned cls) {
  FreeList& fl = t_cache.lists[cls];
  void* p = fl.head;
  if (p == nullptr) [[unlikely]] {
    return refill(cls);
  }
  fl.head = next_of(p);
  --fl.count;
  return p;
}

void tc_free(unsigned cls, void* p) {
  ThreadCache& tc = t_cache;
  if (tc.state != TcState::kActive) [[unlikely]] {
    free_slow(cls, p);
    return;
  }
  FreeList& fl = tc.lists[cls];
  next_of(p) = fl.head;
  fl.head = p;
  if (++fl.count > 2 * batch_size(cls)) [[unlikely]] {
    release_batch(cls, fl, batch_size(cls));
  }
}

std::size_t tc_cached_count(unsigned cls) { return t_cache.lists[cls].count; }

}  // namespace memloc
