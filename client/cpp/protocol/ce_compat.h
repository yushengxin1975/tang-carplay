#pragma once

// CeGCC 9.3's libstdc++ lacks:
//   - std::mutex (no gthreads in wince-thread-model runtime)
//   - atoll() in <cstdlib> (declared but never defined)
// Windows desktop is fine.

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
#  define CARLINKIT_PLATFORM_CE 1
#  include <windows.h>
#  include <string>
#else
#  define CARLINKIT_PLATFORM_CE 0
#  include <mutex>
#  ifdef _WIN32
     // Desktop MinGW/MSVC: declare Sleep without pulling in all of
     // <windows.h> (avoids min/max macro pollution). Same signature as
     // winbase.h, so it is a compatible redeclaration if windows.h was
     // already included by another header in the same TU.
     extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);
#  else
#    include <unistd.h>
     // Win32-style Sleep(ms) shim so sendInit's inter-message delays
     // compile on POSIX hosts (PC unit-test build on Linux CI).
     inline void Sleep(unsigned int ms) { usleep(ms * 1000U); }
#  endif
#endif

namespace carlinkit {

#if CARLINKIT_PLATFORM_CE
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