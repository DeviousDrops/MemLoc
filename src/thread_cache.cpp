#include "thread_cache.h"

#include <pthread.h>

#include <cerrno>

#include "fork.h"

namespace memloc {

constinit thread_local ThreadCache t_cache __attribute__((tls_model("initial-exec"))) = {};

namespace {

pthread_key_t g_exit_key;
pthread_once_t g_exit_key_once = PTHREAD_ONCE_INIT;

// Returns every cached object to the central lists; afterwards the thread
// never caches again (kDead).
void flush(ThreadCache& tc) {
  for (unsigned c = 0; c < kNumClasses; ++c) {
    FreeList& fl = tc.lists[c];
    if (fl.head != nullptr) g_central[c].release(fl.head);
    fl = FreeList{};
  }
  tc.state = TcState::kDead;
}

// pthread key destructor, run at thread exit. Any malloc/free issued by later
// TSD destructors bypasses the cache: there would be nobody left to flush it.
void flush_on_exit(void* arg) { flush(*static_cast<ThreadCache*>(arg)); }

void create_exit_key() { pthread_key_create(&g_exit_key, flush_on_exit); }

void activate(ThreadCache& tc) {
  // Set the state first: if pthread_setspecific ever needs memory (only after
  // 32 keys exist), the nested malloc finds an active cache instead of
  // recursing back here.
  tc.state = TcState::kActive;
  pthread_once(&g_exit_key_once, create_exit_key);
  pthread_setspecific(g_exit_key, &tc);
  install_fork_handlers();
}

}  // namespace

void* tc_refill(unsigned cls) {
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

// Detaches the first batch of the list and returns it to the central list.
void tc_release_batch(unsigned cls, FreeList& fl) {
  const unsigned n = batch_size(cls);
  void* first = fl.head;
  void* last = first;
  for (unsigned i = 1; i < n; ++i) last = next_of(last);
  fl.head = next_of(last);
  fl.count -= n;
  next_of(last) = nullptr;
  g_central[cls].release_batch(cls, first);
}

void tc_free_slow(unsigned cls, void* p) {
  ThreadCache& tc = t_cache;
  if (tc.state == TcState::kUninit) {
    activate(tc);
    tc_push(cls, p);
    return;
  }
  next_of(p) = nullptr;  // kDead: straight to the central list
  g_central[cls].release(p);
}

void tc_disable() { flush(t_cache); }

std::size_t tc_cached_count(unsigned cls) { return t_cache.lists[cls].count; }

}  // namespace memloc
