// ce_compat.h — WinCE / desktop compatibility shims.
//
// CeGCC 9.3's libstdc++ lacks:
//   - std::mutex (no gthreads in wince-thread-model runtime)
//   - atoll() in <cstdlib>
// Windows desktop is fine.
//
// All shims are no-ops or native-equivalent on desktop.
#pragma once

#ifdef _WIN32_WCE
#  include <windows.h>
#  include <string>
#  define CARLINKIT_NO_ATOLL 1
#else
#  include <mutex>
#  define CARLINKIT_NO_ATOLL 0
#endif

namespace carlinkit {

#ifdef _WIN32_WCE
class Mutex {
    CRITICAL_SECTION cs_;
public:
    Mutex()  { InitializeCriticalSection(&cs_); }
    ~Mutex() { DeleteCriticalSection(&cs_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock()   { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
};
class LockGuard {
    Mutex& m_;
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};
#else
using Mutex = std::mutex;
using LockGuard = std::lock_guard<std::mutex>;
#endif

} // namespace carlinkit