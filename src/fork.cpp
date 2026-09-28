#include "fork.h"

#include <pthread.h>

#include "central.h"
#include "page_heap.h"

namespace memloc {
namespace {

pthread_once_t g_fork_once = PTHREAD_ONCE_INIT;

void before_fork() {
  for (auto& list : g_central) list.lock();
  g_page_heap.lock();
}

void after_fork() {
  g_page_heap.unlock();
  for (unsigned c = kNumClasses; c-- > 0;) g_central[c].unlock();
}

void register_handlers() { pthread_atfork(before_fork, after_fork, after_fork); }

}  // namespace

void install_fork_handlers() { pthread_once(&g_fork_once, register_handlers); }

}  // namespace memloc
