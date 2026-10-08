// ce_stubs.cpp — WinCE libc/libstdc++ missing symbols we don't use.
//
// CeGCC 9.3's libstdc++ declares atoll() in <cstdlib> header but never
// links a definition unless -D_GLIBCXX_USE_C99=1 is passed. Provide a
// trivial stub to keep the build happy. (We don't use atoll().)
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
extern "C" long long atoll(const char *nptr) {
    (void)nptr;
    return 0;
}
#endif