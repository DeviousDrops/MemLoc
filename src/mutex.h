// mutex.h - a pthread mutex with a constexpr constructor and no exceptions.
// (D-13, D-19)
//
// std::mutex::lock() reports failure by throwing std::system_error, which
// would make libmemloc.so depend on libstdc++'s exception runtime. The
// allocator can neither throw nor recover from a broken mutex, so this
// wrapper just calls pthread directly. It is BasicLockable, so
// std::scoped_lock works with it, and PTHREAD_MUTEX_INITIALIZER keeps it
// constinit-friendly: usable before any constructor has run.
#pragma once

#include <pthread.h>

namespace memloc {

class Mutex {
 public:
  constexpr Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void lock() { pthread_mutex_lock(&m_); }
  void unlock() { pthread_mutex_unlock(&m_); }

 private:
  pthread_mutex_t m_ = PTHREAD_MUTEX_INITIALIZER;
};

}  // namespace memloc
