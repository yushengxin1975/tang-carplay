// ce_main.h — header shim for ce_main.cpp
//
// In a CE build (_WIN32_WCE defined), this pulls in real Win32 + our
// protocol. In a PC build this header is empty + ce_main.cpp body is
// disabled (see #ifdef), so the TU compiles to nothing.

#ifdef _WIN32_WCE
#  include <windows.h>
#  include "protocol/connection.h"
#  include "protocol/usb_transport.h"
#  include <cstdio>
#  include <cstring>
#  include <memory>
#  include <vector>
#else
// PC build: include nothing. The CE-only code body in ce_main.cpp is
// disabled (see #ifdef _WIN32_WCE) so this TU is empty.
#endif