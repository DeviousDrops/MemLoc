#include "central.h"

#include "page_heap.h"

namespace memloc {

constinit CentralList g_central[kNumClasses];

void CentralList::link(Span* s) {
  s->prev = nullptr;
  s->next = spans_;
  if (spans_ != nullptr) spans_->prev = s;
  spans_ = s;
  s->in_list = true;
}

void CentralList::unlink(Span* s) {
  if (s->prev != nullptr) {
    s->prev->next = s->next;
  } else {
    spans_ = s->next;
  }
  if (s->next != nullptr) s->next->prev = s->prev;
  s->prev = s->next = nullptr;
  s->in_list = false;
}

unsigned CentralList::fetch(unsigned cls, unsigned n, void** head) {
  const std::size_t size = class_size(cls);
  const std::scoped_lock lock(mu_);
  if (n == batch_size(cls) && batches_ != nullptr) {
    *head = batches_;
    batches_ = next_batch_of(batches_);
    --num_batches_;
    return n;
  }
  void* list = nullptr;
  unsigned got = 0;
  while (got < n) {
    Span* s = spans_;
    if (s == nullptr) {
      s = g_page_heap.alloc_span(span_units(cls), SpanKind::kSmall);
      if (s == nullptr) break;
      s->size_class = static_cast<std::uint8_t>(cls);
      s->carve = span_base(s);
      s->limit = s->carve + objects_per_span(cls) * size;
      link(s);
    }
    // Drain this span as far as the request allows.
    while (got < n) {
      void* obj = s->free_list;
      if (obj != nullptr) {
        s->free_list = next_of(obj);
      } else if (s->carve < s->limit) {
        obj = s->carve;
        s->carve += size;
      } else {
        break;
      }
      next_of(obj) = list;
      list = obj;
      ++s->live;
      ++got;
    }
    if (s->free_list == nullptr && s->carve == s->limit) unlink(s);  // exhausted
  }
  *head = list;
  return got;
}

void CentralList::release(void* head) {
  const std::scoped_lock lock(mu_);
  release_to_spans(head);
}

void CentralList::release_batch(unsigned cls, void* head) {
  const std::size_t max_batches = kTransferBytes / (batch_size(cls) * class_size(cls));
  const std::scoped_lock lock(mu_);
  if (num_batches_ < max_batches) {
    next_batch_of(head) = batches_;
    batches_ = head;
    ++num_batches_;
    return;
  }
  release_to_spans(head);
}

unsigned CentralList::cached_batches() {
  const std::scoped_lock lock(mu_);
  return num_batches_;
}

void CentralList::release_to_spans(void* head) {
  while (head != nullptr) {
    void* obj = head;
    head = next_of(obj);
    Span* s = span_of(obj);
    next_of(obj) = s->free_list;
    s->free_list = obj;
    --s->live;
    if (!s->in_list) link(s);
    const bool only_span = spans_ == s && s->next == nullptr;
    if (s->live == 0 && !only_span) {
      unlink(s);
      g_page_heap.free_span(s);
    }
  }
}

}  // namespace memloc
