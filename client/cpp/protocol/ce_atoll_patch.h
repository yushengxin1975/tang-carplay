// ce_atoll_patch.h
//
// CeGCC 9.3's <cstdlib> uses `using ::atoll;` but never declares atoll
// (the symbol exists only in the libstdc++ runtime, not in the header).
// Inject this declaration *before* any STL header is parsed so the `using`
// in <cstdlib> resolves to our prototype. The actual definition is
// provided by ce_stubs.cpp so the link step succeeds.
//
// Used via: -include client/cpp/protocol/ce_atoll_patch.h
#pragma once
#ifdef __MINGW32CE__
#  ifndef _CARLINKIT_ATOLL_DECLARED
#    define _CARLINKIT_ATOLL_DECLARED
extern "C" long long atoll(const char *nptr);
#  endif
#endif