// Runs with LD_PRELOAD=libmemloc.so (set by ctest) and is deliberately NOT
// linked against memloc_core: every allocation here, including GoogleTest's
// own, must reach MemLoc through symbol interposition. (D-18, D-20, D-21)
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <malloc.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string defining_object(const void* fn) {
  Dl_info info{};
  if (dladdr(fn, &info) == 0 || info.dli_fname == nullptr) return "?";
  return info.dli_fname;
}

bool aligned_to(const void* p, std::size_t a) {
  return reinterpret_cast<std::uintptr_t>(p) % a == 0;
}

}  // namespace

TEST(Preload, EveryFamilyMemberResolvesIntoMemloc) {
  const std::pair<const char*, const void*> fns[] = {
      {"malloc", reinterpret_cast<const void*>(&malloc)},
      {"free", reinterpret_cast<const void*>(&free)},
      {"calloc", reinterpret_cast<const void*>(&calloc)},
      {"realloc", reinterpret_cast<const void*>(&realloc)},
      {"reallocarray", reinterpret_cast<const void*>(&reallocarray)},
      {"posix_memalign", reinterpret_cast<const void*>(&posix_memalign)},
      {"aligned_alloc", reinterpret_cast<const void*>(&aligned_alloc)},
      {"memalign", reinterpret_cast<const void*>(&memalign)},
      {"valloc", reinterpret_cast<const void*>(&valloc)},
      {"pvalloc", reinterpret_cast<const void*>(&pvalloc)},
      {"malloc_usable_size", reinterpret_cast<const void*>(&malloc_usable_size)},
  };
  for (const auto& [name, fn] : fns) {
    EXPECT_NE(defining_object(fn).find("libmemloc.so"), std::string::npos)
        << name << " resolves to " << defining_object(fn) << " - is LD_PRELOAD set?";
  }
  EXPECT_NE(dlsym(RTLD_DEFAULT, "memloc_version"), nullptr);
}

TEST(Preload, GlibcOnlyEntryPointsFollowGlibcSemantics) {
  // memalign rounds a non-power-of-two alignment up (volatile: clang rejects
  // a constant one at compile time).
  volatile std::size_t odd_alignment = 48;
  void* p = memalign(odd_alignment, 100);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(aligned_to(p, 64));
  free(p);

  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  p = valloc(10);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(aligned_to(p, page));
  free(p);

  p = pvalloc(page + 1);  // rounded up to two pages
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(aligned_to(p, page));
  EXPECT_GE(malloc_usable_size(p), 2 * page);
  free(p);

  errno = 0;
  volatile std::size_t huge_count = SIZE_MAX / 2;  // hide the overflow from -Werror
  EXPECT_EQ(reallocarray(nullptr, huge_count, 3), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  p = reallocarray(nullptr, 10, 10);
  ASSERT_NE(p, nullptr);
  EXPECT_GE(malloc_usable_size(p), 100u);
  free(p);
}

TEST(Preload, CppNewAndDeleteReachMemloc) {
  // libstdc++'s operator new calls malloc. 100 bytes maps to MemLoc's 112-byte
  // class (glibc would report 104), so the usable size identifies the source.
  auto* bytes = new char[100];
  EXPECT_EQ(malloc_usable_size(bytes), 112u);
  delete[] bytes;

  struct alignas(256) Wide {
    char data[300];
  };
  auto wide = std::make_unique<Wide>();  // aligned new -> aligned_alloc
  EXPECT_TRUE(aligned_to(wide.get(), 256));

  std::vector<std::string> strings;
  for (int i = 0; i < 10000; ++i)
    strings.push_back(std::string(static_cast<std::size_t>(i % 300), 'x'));
  EXPECT_EQ(strings[299].size(), 299u);
}

TEST(Preload, BlocksAllocatedInsideGlibcAreFreedByMemloc) {
  char* dup = strdup("interposed");
  EXPECT_STREQ(dup, "interposed");
  free(dup);

  char* formatted = nullptr;
  ASSERT_GT(asprintf(&formatted, "%d-%s", 42, "memloc"), 0);
  EXPECT_STREQ(formatted, "42-memloc");
  free(formatted);

  char* path = realpath("/", nullptr);
  ASSERT_NE(path, nullptr);
  free(path);

  char* buf = nullptr;
  std::size_t len = 0;
  FILE* f = open_memstream(&buf, &len);
  ASSERT_NE(f, nullptr);
  for (int i = 0; i < 5000; ++i) std::fprintf(f, "line %d\n", i);
  std::fclose(f);
  EXPECT_GT(len, 40000u);
  free(buf);
}

TEST(Preload, ForkWhileOtherThreadsAllocate) {
  // Without the atfork handlers (D-20) a child forked while another thread
  // holds an allocator lock deadlocks on its first malloc; the child's alarm
  // turns such a hang into a failure instead of a stuck test.
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&stop, t] {
      // Bursts larger than a thread cache keep every central list and the
      // page heap busy, so forks regularly land while one of their locks is held.
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      std::vector<void*> burst(512);
      while (!stop.load(std::memory_order_relaxed)) {
        for (auto& p : burst) p = malloc(16 + rng() % (32 * 1024));
        void* medium = malloc(64 * 1024 + rng() % (512 * 1024));
        for (void* p : burst) free(p);
        free(medium);
      }
    });
  }
  int failures = 0;
  int forks = 0;
  for (; forks < 200; ++forks) {
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      alarm(5);
      // Touch every size class (each needs its central lock on a cache miss),
      // the page heap and the huge path.
      bool ok = true;
      for (std::size_t n = 16; n <= 3000000; n += n / 8 + 16) {
        auto* p = static_cast<char*>(malloc(n));
        ok = ok && p != nullptr;
        if (p != nullptr) std::memset(p, 1, n);
        free(p);
      }
      _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      ++failures;
      break;
    }
  }
  stop = true;
  for (auto& w : workers) w.join();
  EXPECT_EQ(failures, 0) << "child " << forks << " deadlocked or failed";
}
