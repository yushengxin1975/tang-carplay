// ce_main.cpp — Carlinkit client for BYD 唐 2015 (Phase B.2 Bring-up)
//
// 零写卡。支持冷启动驱动自注入（Usb_Box / ceusbbox.dll）。
// 启动后弹一个全屏窗口（1024x600）：
//   - 顶部适配比亚迪原车状态栏（y >= 48 开始绘制，不被顶栏遮挡）
//   - 右上角 [ 退出 EXIT ] 实体大按钮，单次点击安全退出
//   - 独立音量触控调节 [ 音量 - ] [ 音量 + ] 与方向盘/旋钮实体按键映射
//   - 全屏深色质感座舱仪表盘：连接状态卡片 + 实时遥测卡片 + 诊断日志终端
//   - 会话拉起后全屏显示真实 Apple CarPlay 视频流（FFmpeg H.264 硬件/软件极速解码，RGB565 直推）
//   - 全原生 Unicode (WCHAR) 绘制，彻底消除中文乱码
//   - 屏幕触摸 -> Message::Touch 自动上报
//   - 音频通过 WavePlayer (waveOutOpen/Write) 直接流式输出到车机喇叭（带数字音量线性调谐）
//
// 编译：CeGCC 9.3 (arm-mingw32ce-g++) -std=c++17 -static-libstdc++

#include "ce_main.h"

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
#include "protocol/message.h"
#include "audio/wave_player.h"
#include "video/h264_decoder_ce.h"
#include "protocol_const.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <setjmp.h>

// ---------------- GUI & State (zero-persistence, Unicode native) ----------------
static HWND g_hwnd = nullptr;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;

static WCHAR g_lines[4][128] = {
    L"Antigravity CarPlay",
    L"[1/3] 正在检测 USB 盒子 (0x1314:0x1520)...",
    L"[2/3] 等待插入 iPhone (长插盒子即插即用)",
    L"全时供电轮询 · 仅需插拔手机 · 零写TF卡"
};

#define EVT_LINES 9
#define EVT_LEN   128
static WCHAR g_events[EVT_LINES][EVT_LEN];
static int g_evtHead = 0, g_evtN = 0;

static volatile DWORD g_video_frames = 0;
static volatile DWORD g_decoded_frames = 0;
static volatile DWORD g_audio_frames = 0;
static volatile DWORD g_touch_count = 0;
static volatile int   g_conn_state = 0; // 0=init, 1=ready, 2=online, 3=error
static volatile int   g_current_volume = 50;  // 0..150% (initial 50% to avoid pop)
static volatile LONG  g_write_failures = 0;   // consecutive transport write failures (self-heal trigger)

static carlinkit::WavePlayer g_ce_wave_player;
static carlinkit::Connection *g_conn = nullptr;
static carlinkit::H264Decoder_CE g_decoder;

// ---------------- SEH defense net (WinCE GCC: SJLJ-based SEH emulation) ----------------
// CeGCC has no MSVC __try/__except. A hardware access violation inside the
// FFmpeg ARM assembly (swscale/avcodec) would silently kill the whole process
// (画面瞬间退回原车界面). We install a WinCE unhandled-exception filter and
// arm it around every decode call; when it fires we longjmp back to the
// decode thread's safe point and drop just that frame — the process survives.
static void LogDebugW(const WCHAR *fmt, ...);   // forward decl (defined below)
static jmp_buf g_vid_seh_jmp;
static volatile LONG g_vid_seh_armed = 0;
static volatile DWORD g_vid_seh_tid = 0;
static volatile LONG g_seh_caught = 0;

// Minimal SEH types (CeGCC headers lack the full WinNT definitions).
// EXCEPTION_RECORD's first field is ExceptionCode; EXCEPTION_POINTERS is
// { ExceptionRecord*, ContextRecord* } — layout-stable on WinCE.
typedef struct SehExceptionRecord { DWORD ExceptionCode; } SehExceptionRecord;
typedef struct SehExceptionPointers { SehExceptionRecord *ExceptionRecord; void *ContextRecord; } SehExceptionPointers;
typedef LONG (WINAPI *SehTopLevelFilter)(SehExceptionPointers *);
typedef SehTopLevelFilter (WINAPI *PfnSetUnhandledExceptionFilter)(SehTopLevelFilter);

static LONG WINAPI VidSehFilter(SehExceptionPointers *ep) {
    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    // Only intercept when armed by the decode thread on the same thread —
    // a fault in any other thread must not longjmp to a foreign stack.
    if (g_vid_seh_armed && GetCurrentThreadId() == g_vid_seh_tid) {
        g_vid_seh_armed = 0;   // disarm first so a fault in the handler can't loop
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_seh_caught));
        LogDebugW(L"[VID] SEH: hardware exception 0x%08X — frame dropped, process survives", code);
        longjmp(g_vid_seh_jmp, 1);  // never returns
    }
    // Fault in a non-armed thread (e.g. USB stack during replug): we cannot
    // longjmp to a foreign stack, but logging here captures the crash code
    // in carplay.log before the process dies — essential diagnostics.
    LogDebugW(L"[SEH] FATAL: unhandled exception 0x%08X in thread %u (net not armed)",
              code, (unsigned)GetCurrentThreadId());
    return 0; // EXCEPTION_CONTINUE_SEARCH
}

static void InstallSehNet() {
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    if (!core) return;
    PfnSetUnhandledExceptionFilter pSet =
        reinterpret_cast<PfnSetUnhandledExceptionFilter>(
            GetProcAddressW(core, L"SetUnhandledExceptionFilter"));
    if (pSet) {
        pSet(VidSehFilter);
        LogDebugW(L"[SEH] Unhandled-exception defense net installed");
    } else {
        LogDebugW(L"[SEH] SetUnhandledExceptionFilter unavailable — net not installed");
    }
}

// Video frame rendering surface (1024x600 16bpp RGB565)
// Static 8-byte aligned BSS buffer — zero allocation failure risk, never NULL
static uint16_t g_carplay_pixels[1024 * 600] __attribute__((aligned(8)));
static volatile BOOL g_has_carplay_frame = FALSE;

struct CarplayBmi {
    BITMAPINFOHEADER bmiHeader;
    DWORD bmiColors[3];
};
static const CarplayBmi g_carplay_bmi = {
    { sizeof(BITMAPINFOHEADER), 1024, 600, 1, 16, BI_BITFIELDS, 1024 * 600 * 2, 0, 0, 0, 0 },
    { 0xF800, 0x07E0, 0x001F }
};

// Video packet queue for decoupled decoding thread
static CRITICAL_SECTION g_vid_cs;
static HANDLE g_hVidEvent = nullptr;
static std::vector<uint8_t> g_pending_video;
static volatile BOOL g_has_pending_video = FALSE;

// Persistent diagnostic file logging via WinCE native CreateFileW
static HANDLE g_hLogFile = INVALID_HANDLE_VALUE;

// UDP Live Log Streaming (broadcasts logs to port 9999 over vehicle router)
static SOCKET g_udpLogSock = INVALID_SOCKET;
static sockaddr_in g_udpBroadcastAddr{};

static void InitUdpLogging() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        g_udpLogSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_udpLogSock != INVALID_SOCKET) {
            BOOL opt = TRUE;
            setsockopt(g_udpLogSock, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt));
            memset(&g_udpBroadcastAddr, 0, sizeof(g_udpBroadcastAddr));
            g_udpBroadcastAddr.sin_family = AF_INET;
            g_udpBroadcastAddr.sin_port = htons(9999);
            g_udpBroadcastAddr.sin_addr.s_addr = 0xFFFFFFFF; // 255.255.255.255 (INADDR_BROADCAST)
        }
    }
}

static void InitLogging() {
    InitUdpLogging();
    WCHAR exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        WCHAR *pSlash = wcsrchr(exePath, L'\\');
        if (pSlash) {
            *(pSlash + 1) = L'\0';
            wcscat(exePath, L"carplay.log");
            g_hLogFile = CreateFileW(exePath, GENERIC_WRITE, FILE_SHARE_READ,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_hLogFile != INVALID_HANDLE_VALUE) {
                SetFilePointer(g_hLogFile, 0, nullptr, FILE_END);
                char hdr[128];
                _snprintf(hdr, sizeof(hdr), "\r\n=== CarPlay Startup (Tick=%u) ===\r\n", GetTickCount());
                DWORD written = 0;
                WriteFile(g_hLogFile, hdr, strlen(hdr), &written, nullptr);
                FlushFileBuffers(g_hLogFile);
                LogDebugW(L"[LOG] carplay.log at: %s", exePath);
            }
        }
    }
}

static void LogDebugW(const WCHAR *fmt, ...) {
    WCHAR wbuf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(wbuf, 511, fmt, ap);
    va_end(ap);
    wbuf[511] = L'\0';

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR tsbuf[600];
    _snwprintf(tsbuf, 599, L"[%02d:%02d:%02d.%03d] %s",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, wbuf);
    tsbuf[599] = L'\0';
    char abuf[600];
    int alen = WideCharToMultiByte(CP_UTF8, 0, tsbuf, -1, abuf, sizeof(abuf) - 3, nullptr, nullptr);

    if (g_hLogFile != INVALID_HANDLE_VALUE && alen > 0) {
        abuf[alen - 1] = '\r';
        abuf[alen] = '\n';
        DWORD written = 0;
        WriteFile(g_hLogFile, abuf, alen + 1, &written, nullptr);
        FlushFileBuffers(g_hLogFile);
    }

    if (g_udpLogSock != INVALID_SOCKET && alen > 0) {
        sendto(g_udpLogSock, abuf, alen + 1, 0,
               (const sockaddr *)&g_udpBroadcastAddr, sizeof(g_udpBroadcastAddr));
    }

    OutputDebugStringW(wbuf);
}

void SetLineW(int i, const WCHAR *fmt, ...) {
    if (i < 0 || i > 3) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(g_lines[i], 127, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void PushEventW(const WCHAR *fmt, ...) {
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(g_events[g_evtHead], EVT_LEN - 1, fmt, ap);
    va_end(ap);
    g_events[g_evtHead][EVT_LEN - 1] = L'\0';
    LogDebugW(L"[EVT] %s", g_events[g_evtHead]);
    g_evtHead = (g_evtHead + 1) % EVT_LINES;
    if (g_evtN < EVT_LINES) g_evtN++;
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AdjustVolume(int delta) {
    int v = g_current_volume + delta;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    g_current_volume = v;
    g_ce_wave_player.set_volume(v);
    // Sync the volume key to the box (CMD_CONTROL 300=down / 301=up) so the
    // iPhone-side volume follows the head-unit adjustment.
    if (g_conn && delta != 0) {
        try {
            g_conn->send(carlinkit::Message::Control(
                delta < 0 ? carlinkit::BTN_VOLUME_DOWN : carlinkit::BTN_VOLUME_UP));
        } catch (...) {}
    }
    PushEventW(L"音量调至 %d%%", v);
    LogDebugW(L"[VOL] Volume adjusted to %d%%", v);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------- WinCE Network & RNDIS Environment Probe ----------------
static bool StrContainsIgnoreCase(const WCHAR *haystack, const WCHAR *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = wcslen(needle);
    if (nlen == 0) return true;
    size_t hlen = wcslen(haystack);
    if (hlen < nlen) return false;
    for (size_t i = 0; i <= hlen - nlen; ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            WCHAR c1 = haystack[i + j];
            WCHAR c2 = needle[j];
            if (c1 >= L'A' && c1 <= L'Z') c1 += (L'a' - L'A');
            if (c2 >= L'A' && c2 <= L'Z') c2 += (L'a' - L'A');
            if (c1 != c2) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

#define MAX_ADAPTER_NAME_LEN_CE 256
#define MAX_ADAPTER_DESC_LEN_CE 128

typedef struct {
    char String[16];
} IP_ADDR_STR_CE;

typedef struct _IP_ADDR_NODE_CE {
    struct _IP_ADDR_NODE_CE *Next;
    IP_ADDR_STR_CE IpAddress;
    IP_ADDR_STR_CE IpMask;
    DWORD Context;
} IP_ADDR_NODE_CE;

typedef struct _IP_ADAPTER_INFO_CE {
    struct _IP_ADAPTER_INFO_CE *Next;
    DWORD ComboIndex;
    char AdapterName[MAX_ADAPTER_NAME_LEN_CE + 4];
    char Description[MAX_ADAPTER_DESC_LEN_CE + 4];
    UINT AddressLength;
    BYTE Address[8];
    DWORD Index;
    UINT Type;
    UINT DhcpEnabled;
    IP_ADDR_NODE_CE *CurrentIpAddress;
    IP_ADDR_NODE_CE IpAddressList;
    IP_ADDR_NODE_CE GatewayList;
    IP_ADDR_NODE_CE DhcpServer;
    BOOL HaveWins;
    IP_ADDR_NODE_CE PrimaryWinsServer;
    IP_ADDR_NODE_CE SecondaryWinsServer;
    long LeaseObtained;
    long LeaseExpires;
} IP_ADAPTER_INFO_CE;

typedef DWORD (WINAPI *PfnGetAdaptersInfo)(IP_ADAPTER_INFO_CE *pAdapterInfo, ULONG *pOutBufLen);

static void ProbeNetworkAndRndis() {
    LogDebugW(L"[RNDIS] ========================================================");
    LogDebugW(L"[RNDIS] Starting WinCE Network & RNDIS Driver Diagnostic Probe");
    LogDebugW(L"[RNDIS] ========================================================");

    struct FileProbe {
        const WCHAR *path;
        const WCHAR *desc;
        const WCHAR *category;
    };

    static const FileProbe kTargets[] = {
        // RNDIS Host & Device
        { L"\\Windows\\rndismp.dll", L"RNDIS Host Miniport (USB Tethering / PC-RNDIS Link)", L"RNDIS" },
        { L"\\Windows\\rndisfn.dll", L"RNDIS Function Driver (Device OTG Client)", L"RNDIS" },
        { L"\\Windows\\usbrndis.dll", L"USB RNDIS Class Driver", L"RNDIS" },
        { L"\\Windows\\usb8023.dll", L"USB 802.3 Adapter Driver", L"RNDIS" },

        // USB-to-Ethernet Adapter Drivers
        { L"\\Windows\\qf9700usb.dll", L"CoreChip QF9700/SR9700 USB Fast Ethernet Driver", L"USBNET" },
        { L"\\Windows\\QF9700USB.dll", L"CoreChip QF9700/SR9700 USB Fast Ethernet Driver", L"USBNET" },
        { L"\\Windows\\ax88772.dll", L"ASIX AX88772 USB 2.0 Fast Ethernet", L"USBNET" },
        { L"\\Windows\\asix.dll",    L"ASIX USB Ethernet Generic Driver", L"USBNET" },
        { L"\\Windows\\ax88178.dll", L"ASIX AX88178 USB Gigabit Ethernet", L"USBNET" },
        { L"\\Windows\\dm9621.dll",  L"Davicom DM9621 USB Fast Ethernet", L"USBNET" },
        { L"\\Windows\\dm9620.dll",  L"Davicom DM9620 USB Fast Ethernet", L"USBNET" },
        { L"\\Windows\\dm9000.dll",  L"Davicom DM9000 Ethernet Driver", L"USBNET" },
        { L"\\Windows\\rtl8150.dll", L"Realtek RTL8150 USB Ethernet", L"USBNET" },
        { L"\\Windows\\rtl8152.dll", L"Realtek RTL8152 USB Ethernet", L"USBNET" },
        { L"\\Windows\\rt8150.dll",  L"Realtek RT8150 Alternative Driver", L"USBNET" },
        { L"\\Windows\\pegasus.dll", L"Pegasus USB Ethernet Driver", L"USBNET" },
        { L"\\Windows\\usbnet.dll",  L"Generic USB Network Driver", L"USBNET" },

        // Core Network Stack & Protocol DLLs
        { L"\\Windows\\ndis.dll",     L"NDIS Core Subsystem", L"STACK" },
        { L"\\Windows\\tcpstk.dll",   L"TCP/IP Protocol Stack", L"STACK" },
        { L"\\Windows\\iphlpapi.dll", L"IP Helper API Library", L"STACK" },
        { L"\\Windows\\dhcp.dll",     L"DHCP Client Service", L"STACK" },
        { L"\\Windows\\afd.dll",      L"Ancillary Function Driver (Sockets)", L"STACK" },
        { L"\\Windows\\ws2.dll",      L"Winsock 2.0 Subsystem", L"STACK" },

        // Remote Debugging, Console & System Daemons
        { L"\\Windows\\cmd.exe",      L"WinCE Command Prompt Shell", L"DEBUG" },
        { L"\\Windows\\telnetd.dll",  L"Telnet Server Daemon", L"DEBUG" },
        { L"\\Windows\\ftpd.dll",     L"FTP Server Daemon", L"DEBUG" },
        { L"\\Windows\\services.exe", L"WinCE Services Manager", L"DEBUG" },
        { L"\\Windows\\conman.exe",   L"Visual Studio Device Connectivity Manager", L"DEBUG" },
    };

    BOOL has_rndismp = FALSE;
    BOOL has_rndisfn = FALSE;
    BOOL has_qf9700 = FALSE;
    BOOL has_asix = FALSE;
    BOOL has_davicom = FALSE;
    BOOL has_realtek = FALSE;
    BOOL has_tcpstk = FALSE;
    BOOL has_ndis = FALSE;
    BOOL has_cmd = FALSE;
    BOOL has_telnet = FALSE;

    // 1. File existence checks in \Windows\
    LogDebugW(L"[RNDIS] --- Checking Driver & Tool Files in \\Windows\\ ---");
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        DWORD attr = GetFileAttributesW(kTargets[i].path);
        BOOL exists = (attr != 0xFFFFFFFF);
        if (exists) {
            LogDebugW(L"[RNDIS] [FOUND] %s (%s)", kTargets[i].path, kTargets[i].desc);
            if (wcscmp(kTargets[i].path, L"\\Windows\\rndismp.dll") == 0) has_rndismp = TRUE;
            if (wcscmp(kTargets[i].path, L"\\Windows\\rndisfn.dll") == 0) has_rndisfn = TRUE;
            if (StrContainsIgnoreCase(kTargets[i].path, L"qf9700")) has_qf9700 = TRUE;
            if (StrContainsIgnoreCase(kTargets[i].path, L"asix") || StrContainsIgnoreCase(kTargets[i].path, L"ax88")) has_asix = TRUE;
            if (StrContainsIgnoreCase(kTargets[i].path, L"dm96")) has_davicom = TRUE;
            if (StrContainsIgnoreCase(kTargets[i].path, L"rtl81") || StrContainsIgnoreCase(kTargets[i].path, L"rt81")) has_realtek = TRUE;
            if (wcscmp(kTargets[i].path, L"\\Windows\\tcpstk.dll") == 0) has_tcpstk = TRUE;
            if (wcscmp(kTargets[i].path, L"\\Windows\\ndis.dll") == 0) has_ndis = TRUE;
            if (wcscmp(kTargets[i].path, L"\\Windows\\cmd.exe") == 0) has_cmd = TRUE;
            if (wcscmp(kTargets[i].path, L"\\Windows\\telnetd.dll") == 0) has_telnet = TRUE;
        } else {
            LogDebugW(L"[RNDIS] [absent] %s", kTargets[i].path);
        }
    }

    // 2. Registry Scan: HKLM\Comm
    LogDebugW(L"[RNDIS] --- Scanning Registry: HKLM\\Comm ---");
    HKEY hComm = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Comm", 0, 0, &hComm) == ERROR_SUCCESS) {
        DWORD idx = 0;
        WCHAR subName[256];
        DWORD nameLen = 256;
        while (RegEnumKeyExW(hComm, idx, subName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub = nullptr;
            WCHAR dispName[128] = {0};
            WCHAR group[64] = {0};
            WCHAR imgPath[128] = {0};
            DWORD cbDisp = sizeof(dispName);
            DWORD cbGroup = sizeof(group);
            DWORD cbImg = sizeof(imgPath);

            if (RegOpenKeyExW(hComm, subName, 0, 0, &hSub) == ERROR_SUCCESS) {
                RegQueryValueExW(hSub, L"DisplayName", nullptr, nullptr, (LPBYTE)dispName, &cbDisp);
                RegQueryValueExW(hSub, L"Group", nullptr, nullptr, (LPBYTE)group, &cbGroup);
                RegQueryValueExW(hSub, L"ImagePath", nullptr, nullptr, (LPBYTE)imgPath, &cbImg);
                RegCloseKey(hSub);
            }
            dispName[127] = L'\0';
            group[63] = L'\0';
            imgPath[127] = L'\0';

            LogDebugW(L"[RNDIS] Comm Adapter Entry: %s | DisplayName=\"%s\", Group=\"%s\", Image=\"%s\"",
                      subName, dispName, group, imgPath);

            if (StrContainsIgnoreCase(subName, L"RNDIS") || StrContainsIgnoreCase(imgPath, L"RNDIS")) {
                LogDebugW(L"[RNDIS] -> MATCHED RNDIS DRIVER IN COMM: %s", subName);
            }
            if (StrContainsIgnoreCase(subName, L"ASIX") || StrContainsIgnoreCase(imgPath, L"asix") || StrContainsIgnoreCase(subName, L"AX88")) {
                LogDebugW(L"[RNDIS] -> MATCHED ASIX DRIVER IN COMM: %s", subName);
            }
            if (StrContainsIgnoreCase(subName, L"DM96") || StrContainsIgnoreCase(imgPath, L"dm96")) {
                LogDebugW(L"[RNDIS] -> MATCHED DAVICOM DRIVER IN COMM: %s", subName);
            }

            idx++;
            nameLen = 256;
        }

        // Query TCP/IP Bindings
        HKEY hTcpLink = nullptr;
        if (RegOpenKeyExW(hComm, L"Tcpip\\Linkage", 0, 0, &hTcpLink) == ERROR_SUCCESS) {
            WCHAR bindBuf[512] = {0};
            DWORD cbBind = sizeof(bindBuf);
            if (RegQueryValueExW(hTcpLink, L"Bind", nullptr, nullptr, (LPBYTE)bindBuf, &cbBind) == ERROR_SUCCESS) {
                bindBuf[511] = L'\0';
                WCHAR *p = bindBuf;
                LogDebugW(L"[RNDIS] TCP/IP Active Bindings:");
                while (*p && (p - bindBuf < 510)) {
                    LogDebugW(L"[RNDIS]   -> Bound: %s", p);
                    p += wcslen(p) + 1;
                }
            }
            RegCloseKey(hTcpLink);
        }

        RegCloseKey(hComm);
    } else {
        LogDebugW(L"[RNDIS] WARN: Could not open HKLM\\Comm");
    }

    // 3. Registry Scan: HKLM\Drivers\USB\LoadClients
    LogDebugW(L"[RNDIS] --- Scanning Registry: HKLM\\Drivers\\USB\\LoadClients ---");
    static const WCHAR *kRndisClassKeys[] = {
        L"Drivers\\USB\\LoadClients\\Default\\Default\\224_1_3",
        L"Drivers\\USB\\LoadClients\\Default\\Default\\2_2_255",
        L"Drivers\\USB\\LoadClients\\Default\\Default\\2_2_1",
    };
    for (size_t k = 0; k < sizeof(kRndisClassKeys) / sizeof(kRndisClassKeys[0]); ++k) {
        HKEY hRndis = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRndisClassKeys[k], 0, 0, &hRndis) == ERROR_SUCCESS) {
            WCHAR dllVal[128] = {0};
            DWORD cbDll = sizeof(dllVal);
            RegQueryValueExW(hRndis, L"DLL", nullptr, nullptr, (LPBYTE)dllVal, &cbDll);
            dllVal[127] = L'\0';
            LogDebugW(L"[RNDIS] USB LoadClient [FOUND]: %s -> DLL=\"%s\"", kRndisClassKeys[k], dllVal);
            RegCloseKey(hRndis);
        } else {
            LogDebugW(L"[RNDIS] USB LoadClient [not set]: %s", kRndisClassKeys[k]);
        }
    }

    // 4. Registry Scan: HKLM\Drivers\Active
    LogDebugW(L"[RNDIS] --- Scanning Registry: HKLM\\Drivers\\Active ---");
    HKEY hActive = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &hActive) == ERROR_SUCCESS) {
        DWORD idx = 0;
        WCHAR subName[64];
        DWORD nameLen = 64;
        while (RegEnumKeyExW(hActive, idx, subName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub = nullptr;
            WCHAR drvName[64] = {0};
            WCHAR drvKey[256] = {0};
            DWORD cbName = sizeof(drvName);
            DWORD cbKey = sizeof(drvKey);

            if (RegOpenKeyExW(hActive, subName, 0, 0, &hSub) == ERROR_SUCCESS) {
                RegQueryValueExW(hSub, L"Name", nullptr, nullptr, (LPBYTE)drvName, &cbName);
                RegQueryValueExW(hSub, L"Key", nullptr, nullptr, (LPBYTE)drvKey, &cbKey);
                RegCloseKey(hSub);
            }
            drvName[63] = L'\0';
            drvKey[255] = L'\0';

            if (StrContainsIgnoreCase(drvName, L"NDS") || StrContainsIgnoreCase(drvName, L"ETH") ||
                StrContainsIgnoreCase(drvName, L"NET") || StrContainsIgnoreCase(drvKey, L"Comm") ||
                StrContainsIgnoreCase(drvKey, L"RNDIS") || StrContainsIgnoreCase(drvKey, L"asix") ||
                StrContainsIgnoreCase(drvKey, L"dm96")) {
                LogDebugW(L"[RNDIS] Active Network/Comm Driver [%s]: Name=\"%s\", Key=\"%s\"",
                          subName, drvName, drvKey);
            }

            idx++;
            nameLen = 64;
        }
        RegCloseKey(hActive);
    }

    // 5. Dynamic IP Helper Probe (iphlpapi.dll)
    LogDebugW(L"[RNDIS] --- Probing Active Adapters via iphlpapi.dll ---");
    HMODULE hIpHlp = LoadLibraryW(L"iphlpapi.dll");
    int liveAdapters = 0;
    if (hIpHlp) {
        PfnGetAdaptersInfo pGetAdaptersInfo =
            reinterpret_cast<PfnGetAdaptersInfo>(GetProcAddressW(hIpHlp, L"GetAdaptersInfo"));
        if (pGetAdaptersInfo) {
            BYTE ipBuf[4096];
            ULONG outLen = sizeof(ipBuf);
            DWORD dwRet = pGetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO_CE *>(ipBuf), &outLen);
            if (dwRet == 0) {
                IP_ADAPTER_INFO_CE *cur = reinterpret_cast<IP_ADAPTER_INFO_CE *>(ipBuf);
                while (cur) {
                    liveAdapters++;
                    LogDebugW(L"[RNDIS] Live Adapter #%d: [%S] Desc=\"%S\", IP=%S, Mask=%S, GW=%S, MAC=%02X:%02X:%02X:%02X:%02X:%02X, DHCP=%u",
                              liveAdapters,
                              cur->AdapterName,
                              cur->Description,
                              cur->IpAddressList.IpAddress.String,
                              cur->IpAddressList.IpMask.String,
                              cur->GatewayList.IpAddress.String,
                              cur->Address[0], cur->Address[1], cur->Address[2],
                              cur->Address[3], cur->Address[4], cur->Address[5],
                              cur->DhcpEnabled);
                    PushEventW(L"网卡#%d: %S IP:%S",
                               liveAdapters, cur->AdapterName, cur->IpAddressList.IpAddress.String);
                    cur = cur->Next;
                }
            } else {
                LogDebugW(L"[RNDIS] GetAdaptersInfo returned %u (no active network adapter or media disconnected)", dwRet);
            }
        } else {
            LogDebugW(L"[RNDIS] GetAdaptersInfo export missing in iphlpapi.dll");
        }
        FreeLibrary(hIpHlp);
    } else {
        LogDebugW(L"[RNDIS] iphlpapi.dll not available on system");
    }

    // 6. Summary Verdict & UI Output
    LogDebugW(L"[RNDIS] ==================== PROBE SUMMARY ====================");
    LogDebugW(L"[RNDIS]  - RNDIS Host miniport (rndismp.dll) : %s", has_rndismp ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - RNDIS OTG function  (rndisfn.dll) : %s", has_rndisfn ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - QF9700 USB-Ethernet(qf9700usb.dll): %s", has_qf9700 ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - ASIX USB-Ethernet   (asix/ax88772): %s", has_asix ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - Davicom USB-Ethernet(dm9621.dll)  : %s", has_davicom ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - Realtek USB-Ethernet(rtl8150.dll) : %s", has_realtek ? L"YES (SUPPORTED)" : L"NO");
    LogDebugW(L"[RNDIS]  - TCP/IP Stack        (tcpstk/ndis) : %s", (has_tcpstk && has_ndis) ? L"YES (READY)" : L"PARTIAL/NO");
    LogDebugW(L"[RNDIS]  - WinCE Shell Tools   (cmd/telnetd) : %s / %s",
              has_cmd ? L"CMD: YES" : L"CMD: NO",
              has_telnet ? L"Telnet: YES" : L"Telnet: NO");
    LogDebugW(L"[RNDIS]  - Live Network Adapters Count        : %d", liveAdapters);
    LogDebugW(L"[RNDIS] ========================================================");

    PushEventW(L"=== 网络/网卡驱动探针 ===");
    if (has_qf9700) {
        PushEventW(L"[OK] 发现 QF9700 驱动 (已挂载 SR9700 网卡)");
    }
    if (has_rndismp) {
        PushEventW(L"[OK] 发现 RNDIS 主机驱动 (rndismp.dll)");
    } else if (has_rndisfn) {
        PushEventW(L"[提示] 发现 RNDIS 从机驱动 (rndisfn.dll)");
    }

    if (has_asix) PushEventW(L"[OK] 发现 ASIX 驱动 (支持 AX88772 网卡)");
    if (has_davicom) PushEventW(L"[OK] 发现 Davicom 驱动 (支持 DM9621 网卡)");
    if (has_realtek) PushEventW(L"[OK] 发现 Realtek 驱动 (支持 RTL8150 网卡)");

    if (has_cmd || has_telnet) {
        PushEventW(L"[调试] 发现: %s%s", has_telnet ? L"Telnet服务 " : L"", has_cmd ? L"CMD控制台" : L"");
    }
    if (liveAdapters > 0) {
        PushEventW(L"[网络] 实时日志已在 UDP:9999 广播推流中");
    }
    PushEventW(L"探针完毕，详报已写入 carplay.log [RNDIS]");
}

#define WC_CARLINKIT L"Carlinkit"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        switch (wp) {
        case 0xAE: // VK_VOLUME_DOWN
        case VK_DOWN:
        case VK_LEFT:
        case VK_SUBTRACT:
        case 0xBD: // VK_OEM_MINUS
            AdjustVolume(-10);
            return 0;
        case 0xAF: // VK_VOLUME_UP
        case VK_UP:
        case VK_RIGHT:
        case VK_ADD:
        case 0xBB: // VK_OEM_PLUS
            AdjustVolume(+10);
            return 0;
        case 0xAD: // VK_VOLUME_MUTE
            AdjustVolume(g_current_volume > 0 ? -g_current_volume : 100);
            return 0;
        case VK_ESCAPE:
            PostQuitMessage(0);
            return 0;
        }
        break;
    }

    case 0x0319: /* WM_APPCOMMAND */ {
        int cmd = ((int)(short)HIWORD(lp)) & ~0xF000;
        if (cmd == 9 /* APPCOMMAND_VOLUME_DOWN */) {
            AdjustVolume(-10);
            return TRUE;
        } else if (cmd == 10 /* APPCOMMAND_VOLUME_UP */) {
            AdjustVolume(+10);
            return TRUE;
        } else if (cmd == 8 /* APPCOMMAND_VOLUME_MUTE */) {
            AdjustVolume(g_current_volume > 0 ? -g_current_volume : 100);
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int px = LOWORD(lp), py = HIWORD(lp);
        BOOL isCarplay = g_has_carplay_frame;

        // 0. Filter spurious (0, 0) noise
        if (px == 0 && py == 0) {
            LogDebugW(L"[TOUCH] Ignored spurious (0,0) tap");
            return 0;
        }

        // 1. Diagnostic Mode Button Interceptions
        if (!isCarplay) {
            // [ 退出 EXIT ] button (x: 870..1015, y: 40..100)
            if (px >= 870 && px <= 1015 && py >= 40 && py <= 100) {
                LogDebugW(L"[EXIT] Diagnostic screen EXIT button clicked");
                PostQuitMessage(0);
                return 0;
            }
            // Volume [ 音量 - ] (x: 580..720, y: 40..100)
            if (px >= 580 && px <= 720 && py >= 40 && py <= 100) {
                AdjustVolume(-10);
                return 0;
            }
            // Volume [ 音量 + ] (x: 750..880, y: 40..100)
            if (px >= 750 && px <= 880 && py >= 40 && py <= 100) {
                AdjustVolume(+10);
                return 0;
            }
        } else {
            // 2. CarPlay Overlay Button Interceptions (top floating bar y <= 46)
            if (py <= 46) {
                // Exit button (x: 865..1015)
                if (px >= 865 && px <= 1015) {
                    LogDebugW(L"[EXIT] CarPlay overlay EXIT button clicked");
                    PostQuitMessage(0);
                    return 0;
                }
                // Volume [-] (x: 600..710)
                if (px >= 600 && px <= 710) {
                    AdjustVolume(-10);
                    return 0;
                }
                // Volume [+] (x: 750..860)
                if (px >= 750 && px <= 860) {
                    AdjustVolume(+10);
                    return 0;
                }
                return 0;
            }
        }

        // 3. Top-left corner (10..60, 10..60) emergency triple-tap to return to car
        if (px >= 10 && px <= 60 && py >= 10 && py <= 60) {
            static DWORD last_exit_tap = 0;
            static int exit_tap_count = 0;
            DWORD now = GetTickCount();
            if (now - last_exit_tap < 600) {
                exit_tap_count++;
                if (exit_tap_count >= 3) {
                    LogDebugW(L"[EXIT] Emergency triple-tap triggered exit");
                    PostQuitMessage(0);
                    return 0;
                }
            } else {
                exit_tap_count = 1;
            }
            last_exit_tap = now;
        }

        // 4. Touch event translation to CarPlay (0..10000 normalized)
        uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
        uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
        if (nx > 10000) nx = 10000;
        if (ny > 10000) ny = 10000;

        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_touch_count));
        if (g_conn) {
            try { g_conn->send(carlinkit::Message::Touch(14, nx, ny)); } catch (...) {}
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (wp & MK_LBUTTON) {
            int px = LOWORD(lp), py = HIWORD(lp);
            if (!g_has_carplay_frame || py > 44) {
                uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
                uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
                if (nx > 10000) nx = 10000;
                if (ny > 10000) ny = 10000;

                if (g_conn) {
                    try { g_conn->send(carlinkit::Message::Touch(15, nx, ny)); } catch (...) {}
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int px = LOWORD(lp), py = HIWORD(lp);
        if (!g_has_carplay_frame || py > 44) {
            uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
            uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
            if (nx > 10000) nx = 10000;
            if (ny > 10000) ny = 10000;

            if (g_conn) {
                try { g_conn->send(carlinkit::Message::Touch(16, nx, ny)); } catch (...) {}
            }
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);

        // Flicker-free double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

        if (g_has_carplay_frame) {
            // ================= Mode A: Active Apple CarPlay Display =================
            // 1. Direct blast decoded RGB565 CarPlay video frame
            SetDIBitsToDevice(memDC, 0, 0, 1024, 600, 0, 0, 0, 600,
                              g_carplay_pixels,
                              reinterpret_cast<const BITMAPINFO*>(&g_carplay_bmi),
                              DIB_RGB_COLORS);

            // 2. Translucent floating top overlay bar for Exit and Volume
            // 2. Translucent floating top overlay bar for Exit and Volume
            RECT barRc = {610, 4, 1010, 40};
            HBRUSH barBr = CreateSolidBrush(RGB(18, 24, 32));
            FillRect(memDC, &barRc, barBr);
            DeleteObject(barBr);

            HPEN barPen = CreatePen(PS_SOLID, 1, RGB(65, 90, 120));
            HPEN oldP = static_cast<HPEN>(SelectObject(memDC, barPen));
            MoveToEx(memDC, barRc.left, barRc.top, nullptr);
            LineTo(memDC, barRc.right, barRc.top);
            LineTo(memDC, barRc.right, barRc.bottom);
            LineTo(memDC, barRc.left, barRc.bottom);
            LineTo(memDC, barRc.left, barRc.top);
            SelectObject(memDC, oldP);
            DeleteObject(barPen);

            SetBkMode(memDC, TRANSPARENT);

            // [ 音量 - ] button
            RECT vmRc = {618, 8, 698, 36};
            HBRUSH vmBr = CreateSolidBrush(RGB(32, 44, 60));
            FillRect(memDC, &vmRc, vmBr);
            DeleteObject(vmBr);
            SetTextColor(memDC, RGB(180, 220, 255));
            WCHAR vmTxt[] = L"音量 -";
            ExtTextOutW(memDC, 630, 12, 0, nullptr, vmTxt, wcslen(vmTxt), nullptr);

            // Current Volume
            WCHAR volStr[32];
            _snwprintf(volStr, 31, L"%d%%", g_current_volume);
            SetTextColor(memDC, RGB(255, 255, 255));
            ExtTextOutW(memDC, 715, 12, 0, nullptr, volStr, wcslen(volStr), nullptr);

            // [ 音量 + ] button
            RECT vpRc = {768, 8, 848, 36};
            HBRUSH vpBr = CreateSolidBrush(RGB(32, 44, 60));
            FillRect(memDC, &vpRc, vpBr);
            DeleteObject(vpBr);
            SetTextColor(memDC, RGB(180, 220, 255));
            WCHAR vpTxt[] = L"音量 +";
            ExtTextOutW(memDC, 780, 12, 0, nullptr, vpTxt, wcslen(vpTxt), nullptr);

            // [ 退出 EXIT ] button
            RECT exRc = {868, 8, 1000, 36};
            HBRUSH exBr = CreateSolidBrush(RGB(48, 62, 82));
            FillRect(memDC, &exRc, exBr);
            DeleteObject(exBr);

            SetTextColor(memDC, RGB(255, 220, 120));
            WCHAR exTxt[] = L"退出 EXIT";
            ExtTextOutW(memDC, 895, 12, 0, nullptr, exTxt, wcslen(exTxt), nullptr);

        } else {
            // ================= Mode B: Cockpit Diagnostic Dashboard =================
            // 1. Deep automotive cockpit background
            HBRUSH bgBr = CreateSolidBrush(RGB(15, 20, 26));
            FillRect(memDC, &rc, bgBr);
            DeleteObject(bgBr);

            // Copy strings under lock
            WCHAR l4[4][128], eL[EVT_LINES][EVT_LEN];
            EnterCriticalSection(&g_cs);
            memcpy(l4, g_lines, sizeof(l4));
            memcpy(eL, g_events, sizeof(eL));
            int en = g_evtN, eh = g_evtHead;
            int state = g_conn_state;
            DWORD vFrames = g_video_frames;
            DWORD dFrames = g_decoded_frames;
            DWORD aFrames = g_audio_frames;
            DWORD tCount = g_touch_count;
            int curVol = g_current_volume;
            LeaveCriticalSection(&g_cs);

            SetBkMode(memDC, TRANSPARENT);

            // 2. Header Area
            SetTextColor(memDC, RGB(255, 255, 255));
            WCHAR titleW[] = L"Antigravity CarPlay";
            ExtTextOutW(memDC, 28, 50, 0, nullptr, titleW, wcslen(titleW), nullptr);

            SetTextColor(memDC, RGB(130, 175, 205));
            WCHAR subW[] = L"比亚迪·唐 (2015旗舰版) · 独立流媒体客户端";
            ExtTextOutW(memDC, 28, 74, 0, nullptr, subW, wcslen(subW), nullptr);

            // Volume Controls [ 音量 - ] [ 100% ] [ 音量 + ] [ 退出 EXIT ]
            RECT vDownRc = {600, 50, 700, 92};
            HBRUSH vDownBr = CreateSolidBrush(RGB(32, 44, 60));
            FillRect(memDC, &vDownRc, vDownBr);
            DeleteObject(vDownBr);

            HPEN vDownPen = CreatePen(PS_SOLID, 1, RGB(70, 95, 125));
            HPEN oldP1 = static_cast<HPEN>(SelectObject(memDC, vDownPen));
            MoveToEx(memDC, vDownRc.left, vDownRc.top, nullptr);
            LineTo(memDC, vDownRc.right, vDownRc.top);
            LineTo(memDC, vDownRc.right, vDownRc.bottom);
            LineTo(memDC, vDownRc.left, vDownRc.bottom);
            LineTo(memDC, vDownRc.left, vDownRc.top);
            SelectObject(memDC, oldP1);
            DeleteObject(vDownPen);

            SetTextColor(memDC, RGB(180, 225, 255));
            WCHAR vDownTxt[] = L"音量 -";
            ExtTextOutW(memDC, 625, 60, 0, nullptr, vDownTxt, wcslen(vDownTxt), nullptr);

            WCHAR vCurTxt[32];
            _snwprintf(vCurTxt, 31, L"%d%%", curVol);
            SetTextColor(memDC, RGB(255, 255, 255));
            ExtTextOutW(memDC, 720, 60, 0, nullptr, vCurTxt, wcslen(vCurTxt), nullptr);

            RECT vUpRc = {765, 50, 865, 92};
            HBRUSH vUpBr = CreateSolidBrush(RGB(32, 44, 60));
            FillRect(memDC, &vUpRc, vUpBr);
            DeleteObject(vUpBr);

            HPEN vUpPen = CreatePen(PS_SOLID, 1, RGB(70, 95, 125));
            HPEN oldP2 = static_cast<HPEN>(SelectObject(memDC, vUpPen));
            MoveToEx(memDC, vUpRc.left, vUpRc.top, nullptr);
            LineTo(memDC, vUpRc.right, vUpRc.top);
            LineTo(memDC, vUpRc.right, vUpRc.bottom);
            LineTo(memDC, vUpRc.left, vUpRc.bottom);
            LineTo(memDC, vUpRc.left, vUpRc.top);
            SelectObject(memDC, oldP2);
            DeleteObject(vUpPen);

            SetTextColor(memDC, RGB(180, 225, 255));
            WCHAR vUpTxt[] = L"音量 +";
            ExtTextOutW(memDC, 790, 60, 0, nullptr, vUpTxt, wcslen(vUpTxt), nullptr);

            // Top-right EXIT Button: [ 退出 EXIT ]
            RECT btnRc = {880, 50, 1000, 92};
            HBRUSH btnBr = CreateSolidBrush(RGB(38, 50, 66));
            FillRect(memDC, &btnRc, btnBr);
            DeleteObject(btnBr);

            HPEN btnPen = CreatePen(PS_SOLID, 1, RGB(80, 110, 145));
            HPEN oldP3 = static_cast<HPEN>(SelectObject(memDC, btnPen));
            MoveToEx(memDC, btnRc.left, btnRc.top, nullptr);
            LineTo(memDC, btnRc.right, btnRc.top);
            LineTo(memDC, btnRc.right, btnRc.bottom);
            LineTo(memDC, btnRc.left, btnRc.bottom);
            LineTo(memDC, btnRc.left, btnRc.top);
            SelectObject(memDC, oldP3);
            DeleteObject(btnPen);

            SetTextColor(memDC, RGB(230, 245, 255));
            WCHAR exitTxt[] = L"退出 EXIT";
            ExtTextOutW(memDC, 905, 60, 0, nullptr, exitTxt, wcslen(exitTxt), nullptr);

            // 3. Card 1: Connection Status Card
            RECT c1Rc = {24, 106, 584, 290};
            HBRUSH c1Br = CreateSolidBrush(RGB(23, 30, 39));
            FillRect(memDC, &c1Rc, c1Br);
            DeleteObject(c1Br);

            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(45, 60, 78));
            HPEN oldP = static_cast<HPEN>(SelectObject(memDC, borderPen));
            MoveToEx(memDC, c1Rc.left, c1Rc.top, nullptr);
            LineTo(memDC, c1Rc.right, c1Rc.top);
            LineTo(memDC, c1Rc.right, c1Rc.bottom);
            LineTo(memDC, c1Rc.left, c1Rc.bottom);
            LineTo(memDC, c1Rc.left, c1Rc.top);

            const WCHAR *pillTxt = L"● 正在初始化驱动栈 (DRIVER INIT)";
            COLORREF pillColor = RGB(120, 180, 240);
            if (state == 1) {
                pillTxt = L"● 盒子就绪 · 等待插入 iPhone (READY)";
                pillColor = RGB(255, 200, 60);
            } else if (state == 2) {
                pillTxt = L"● CarPlay 会话已保持 (ONLINE)";
                pillColor = RGB(50, 225, 125);
            } else if (state == 3) {
                pillTxt = L"● 连接异常正在自愈 (RECONNECTING)";
                pillColor = RGB(255, 95, 85);
            }
            SetTextColor(memDC, pillColor);
            ExtTextOutW(memDC, 42, 118, 0, nullptr, pillTxt, wcslen(pillTxt), nullptr);

            SetTextColor(memDC, RGB(255, 255, 255));
            ExtTextOutW(memDC, 42, 150, 0, nullptr, l4[1], wcslen(l4[1]), nullptr);

            SetTextColor(memDC, RGB(160, 230, 200));
            ExtTextOutW(memDC, 42, 182, 0, nullptr, l4[2], wcslen(l4[2]), nullptr);

            SetTextColor(memDC, RGB(140, 200, 240));
            ExtTextOutW(memDC, 42, 214, 0, nullptr, l4[3], wcslen(l4[3]), nullptr);

            SetTextColor(memDC, RGB(110, 145, 175));
            WCHAR guideW[] = L"提示: 盒子长插在车机主板USB，上车插上手机即可秒连";
            ExtTextOutW(memDC, 42, 248, 0, nullptr, guideW, wcslen(guideW), nullptr);

            // 4. Card 2: Telemetry Card
            RECT c2Rc = {600, 106, 1000, 290};
            HBRUSH c2Br = CreateSolidBrush(RGB(23, 30, 39));
            FillRect(memDC, &c2Rc, c2Br);
            DeleteObject(c2Br);

            MoveToEx(memDC, c2Rc.left, c2Rc.top, nullptr);
            LineTo(memDC, c2Rc.right, c2Rc.top);
            LineTo(memDC, c2Rc.right, c2Rc.bottom);
            LineTo(memDC, c2Rc.left, c2Rc.bottom);
            LineTo(memDC, c2Rc.left, c2Rc.top);

            SetTextColor(memDC, RGB(100, 215, 255));
            WCHAR telemTitle[] = L"● 实时流媒体遥测 (TELEMETRY)";
            ExtTextOutW(memDC, 618, 118, 0, nullptr, telemTitle, wcslen(telemTitle), nullptr);

            WCHAR rowW[96];
            SetTextColor(memDC, RGB(225, 240, 255));
            _snwprintf(rowW, 95, L"视频流:   %u 帧 (已解码: %u 帧)", vFrames, dFrames);
            ExtTextOutW(memDC, 618, 152, 0, nullptr, rowW, wcslen(rowW), nullptr);

            _snwprintf(rowW, 95, L"音频流:   %u 帧 (当前音量: %d%%)", aFrames, curVol);
            ExtTextOutW(memDC, 618, 188, 0, nullptr, rowW, wcslen(rowW), nullptr);

            _snwprintf(rowW, 95, L"触控事件: %u 次 (双向触控已启用)", tCount);
            ExtTextOutW(memDC, 618, 224, 0, nullptr, rowW, wcslen(rowW), nullptr);

            SetTextColor(memDC, RGB(120, 190, 150));
            WCHAR volTip[] = L"音量调节: 点击上方按钮，或转动中控旋钮/方向盘按键";
            ExtTextOutW(memDC, 618, 256, 0, nullptr, volTip, wcslen(volTip), nullptr);

            // 5. Card 3: Diagnostic Terminal
            RECT c3Rc = {24, 302, 1000, 582};
            HBRUSH c3Br = CreateSolidBrush(RGB(10, 13, 17));
            FillRect(memDC, &c3Rc, c3Br);
            DeleteObject(c3Br);

            MoveToEx(memDC, c3Rc.left, c3Rc.top, nullptr);
            LineTo(memDC, c3Rc.right, c3Rc.top);
            LineTo(memDC, c3Rc.right, c3Rc.bottom);
            LineTo(memDC, c3Rc.left, c3Rc.bottom);
            LineTo(memDC, c3Rc.left, c3Rc.top);

            RECT tHeadRc = {24, 302, 1000, 328};
            HBRUSH tHeadBr = CreateSolidBrush(RGB(18, 24, 32));
            FillRect(memDC, &tHeadRc, tHeadBr);
            DeleteObject(tHeadBr);

            SetTextColor(memDC, RGB(130, 160, 190));
            WCHAR logTitle[] = L"诊断事件日志 (DIAGNOSTIC EVENT LOG) — 驱动加载与会话协议追踪";
            ExtTextOutW(memDC, 38, 308, 0, nullptr, logTitle, wcslen(logTitle), nullptr);

            int ly = 338;
            for (int i = 0; i < EVT_LINES; ++i) {
                int idx = (en < EVT_LINES) ? i : ((eh + i) % EVT_LINES);
                const WCHAR *str = eL[idx];
                if (wcsncmp(str, L"ERR", 3) == 0 || wcsstr(str, L"FAIL") != nullptr) {
                    SetTextColor(memDC, RGB(255, 100, 90));
                } else if (wcsncmp(str, L"OK", 2) == 0) {
                    SetTextColor(memDC, RGB(90, 240, 220));
                } else {
                    SetTextColor(memDC, RGB(120, 225, 140));
                }
                ExtTextOutW(memDC, 38, ly, 0, nullptr, str, wcslen(str), nullptr);
                ly += 24;
            }

            SelectObject(memDC, oldP);
            DeleteObject(borderPen);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(h, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------- Video Decoding Worker Thread ----------------
static DWORD WINAPI VideoDecodeThread(LPVOID) {
    std::vector<uint8_t> nal_buffer;
    LogDebugW(L"[VID] VideoDecodeThread started");
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        DWORD waitRes = WaitForSingleObject(g_hVidEvent, 50);
        if (waitRes == WAIT_OBJECT_0) {
            EnterCriticalSection(&g_vid_cs);
            if (g_has_pending_video) {
                nal_buffer.swap(g_pending_video);
                g_has_pending_video = FALSE;
            } else {
                nal_buffer.clear();
            }
            LeaveCriticalSection(&g_vid_cs);

            if (!nal_buffer.empty() && g_decoder.is_ready()) {
                int fw = 0, fh = 0;
                size_t actual_len = (nal_buffer.size() > 64) ? (nal_buffer.size() - 64) : nal_buffer.size();
                volatile bool ok = false;
                if (setjmp(g_vid_seh_jmp) == 0) {
                    // Arm the SEH net: any hardware access violation inside the
                    // FFmpeg ARM assembly lands in VidSehFilter and longjmps
                    // back here — only this frame is dropped, process survives.
                    g_vid_seh_tid = GetCurrentThreadId();
                    g_vid_seh_armed = 1;
                    try {
                        ok = g_decoder.decode_frame(nal_buffer.data(), actual_len,
                                                    reinterpret_cast<uint8_t*>(g_carplay_pixels),
                                                    1024 * 2, &fw, &fh, /*flip_vertical=*/true);
                    } catch (...) {
                        LogDebugW(L"[VID] EXCEPTION in decode_frame!");
                    }
                    g_vid_seh_armed = 0;
                } else {
                    // SEH filter caught a hardware exception — frame dropped silently
                    ok = false;
                }
                if (ok) {
                    if (!g_has_carplay_frame) {
                        LogDebugW(L"[VID] First CarPlay frame decoded successfully (%dx%d)!", fw, fh);
                        PushEventW(L"OK: 首帧画面已就绪 (%dx%d)！", fw, fh);
                    }
                    g_has_carplay_frame = TRUE;
                    InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_decoded_frames));
                    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
                } else {
                    // Decode failed — log throttled (first 3, then every 200th) so the
                    // carplay.log reveals WHY (SPS/PPS-only frame = legit no-picture;
                    // large frame failing = real decoder problem).
                    static volatile LONG g_decode_fails = 0;
                    LONG fails = InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_decode_fails));
                    if (fails <= 10 || (fails % 200) == 0) {
                        LogDebugW(L"[VID] decode_frame FAILED #%u: h264_len=%u (decoder_ready=%d)",
                                  (unsigned)fails, (unsigned)actual_len,
                                  g_decoder.is_ready() ? 1 : 0);
                    }
                }
            }
        }
    }
    LogDebugW(L"[VID] VideoDecodeThread stopped");
    return 0;
}

// ---------------- Heartbeat Thread ----------------
static DWORD WINAPI HeartbeatThread(LPVOID) {
    int tick = 0;
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        if (g_conn) {
            try {
                g_conn->send(carlinkit::Cmd::HEARTBEAT);
                // While waiting for iPhone (state 1), pulse CMD_9 phone discovery every 6s
                // to trigger OTG host enumeration for late/hot-plugged iPhones.
                if (g_conn_state == 1 && (++tick % 3 == 0)) {
                    g_conn->send(carlinkit::Message(carlinkit::Cmd::CMD_9, 1, false));
                }
            } catch (...) {}
        }
        Sleep(2000);
    }
    return 0;
}

// ---------------- Connection Main Worker Thread ----------------
static DWORD WINAPI ClientThread(LPVOID) {
    using namespace carlinkit;

    InboundCallbacks cb;
    cb.on_state = [](ConnState s) {
        if (s == ConnState::UNKNOWN) {
            PushEventW(L"STATE: %d (SYNC)", static_cast<int>(s));
        } else {
            PushEventW(L"STATE: %d", static_cast<int>(s));
        }
        if (s == ConnState::NO_DEVICE) {
            g_conn_state = 1;
            SetLineW(1, L"[就绪] 等待插入 iPhone (USB 盒子已就绪)");
            SetLineW(2, L"盒子常驻供电，插拔 iPhone 即可热拉起");
        } else if (s == ConnState::ONLINE) {
            g_conn_state = 2;
            SetLineW(1, L"[在线] iPhone 已连接！CarPlay 会话已建立");
            SetLineW(2, L"画面解码中，双向触控与车机喇叭已激活");
        } else if (s == ConnState::ERROR_STATE) {
            g_conn_state = 3;
            SetLineW(1, L"[错误] 连接状态异常，正在自动恢复...");
        }
    };

    cb.on_plugged = []() {
        static int s_plug_count = 0;
        ++s_plug_count;
        g_conn_state = 2;
        if (s_plug_count > 1) {
            PushEventW(L"OK: CMD_PLUGGED (re-plug #%d) -> 重发 sendInit", s_plug_count);
        } else {
            PushEventW(L"OK: CMD_PLUGGED 收到 -> 会话拉起");
        }
        SetLineW(1, L"[在线] iPhone 已连接！CarPlay 运行中");
        SetLineW(2, L"双向触控与音频流已激活");
        if (g_conn) {
            try { g_conn->send(carlinkit::Message::Control(12)); } catch (...) {}
        }
    };

    cb.on_unplugged = []() {
        g_conn_state = 1;
        g_has_carplay_frame = FALSE;
        PushEventW(L"CMD_UNPLUGGED 收到 -> iPhone 断开");
        SetLineW(1, L"[就绪] 等待插入 iPhone (无需拔插盒子)");
        SetLineW(2, L"下次插上 iPhone 立即自动重连");
        g_ce_wave_player.reset();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    };

    cb.on_any = [](Cmd c, const uint8_t *p, std::size_t n) {
        if (c == Cmd::VIDEO_DATA) {
            InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_video_frames));
            // Scan for Annex-B start code (00 00 00 01 or 00 00 01) in first 64 bytes
            size_t nal_offset = 20; // Default Carlinkit subheader is 20 bytes
            for (size_t i = 0; i + 4 <= n && i < 64; ++i) {
                if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) {
                    nal_offset = i;
                    break;
                }
                if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
                    nal_offset = i;
                    break;
                }
            }
            {
                static volatile LONG g_vid_logged = 0;
                LONG logged = InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_vid_logged));
                if (logged <= 5) {
                    unsigned b0 = (n > 0)?p[0]:0, b1 = (n > 1)?p[1]:0, b2 = (n > 2)?p[2]:0, b3 = (n > 3)?p[3]:0;
                    unsigned b4 = (n > 4)?p[4]:0, b5 = (n > 5)?p[5]:0, b6 = (n > 6)?p[6]:0, b7 = (n > 7)?p[7]:0;
                    const uint8_t *h = (n > nal_offset) ? (p + nal_offset) : nullptr;
                    size_t hlen = (n > nal_offset) ? (n - nal_offset) : 0;
                    unsigned h0 = (hlen > 0)?h[0]:0, h1 = (hlen > 1)?h[1]:0, h2 = (hlen > 2)?h[2]:0, h3 = (hlen > 3)?h[3]:0;
                    unsigned h4 = (hlen > 4)?h[4]:0, h5 = (hlen > 5)?h[5]:0;
                    LogDebugW(L"[VID] video payload #%u: len=%u nal_off=%u head8=%02X %02X %02X %02X %02X %02X %02X %02X nalu=%02X %02X %02X %02X %02X %02X",
                              (unsigned)logged, (unsigned)n, (unsigned)nal_offset,
                              b0, b1, b2, b3, b4, b5, b6, b7,
                              h0, h1, h2, h3, h4, h5);
                }
            }
            if (n > nal_offset) {
                EnterCriticalSection(&g_vid_cs);
                const size_t h264_sz = n - nal_offset;
                if (g_has_pending_video) {
                    size_t old_sz = g_pending_video.size() >= 64 ? g_pending_video.size() - 64 : g_pending_video.size();
                    g_pending_video.resize(old_sz + h264_sz + 64);
                    std::memcpy(g_pending_video.data() + old_sz, p + nal_offset, h264_sz);
                    std::memset(g_pending_video.data() + old_sz + h264_sz, 0, 64);
                } else {
                    g_pending_video.resize(h264_sz + 64);
                    std::memcpy(g_pending_video.data(), p + nal_offset, h264_sz);
                    std::memset(g_pending_video.data() + h264_sz, 0, 64);
                    g_has_pending_video = TRUE;
                }
                LeaveCriticalSection(&g_vid_cs);
                SetEvent(g_hVidEvent);
            }
            if ((g_video_frames % 60) == 1) {
                SetLineW(3, L"视频: %u 帧 (解: %u) | 音量: %d%% | 触控: %u",
                         g_video_frames, g_decoded_frames, g_current_volume, g_touch_count);
                LogDebugW(L"[STATS] Vid=%u (Dec=%u) Aud=%u Touch=%u Vol=%d",
                          g_video_frames, g_decoded_frames, g_audio_frames, g_touch_count, g_current_volume);
            }
        } else if (c == Cmd::AUDIO_DATA) {
            InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_audio_frames));
            if (n == 13) {
                if (p[12] == 0) {
                    g_ce_wave_player.reset();
                }
            } else if (n > 12) {
                uint32_t fmt = Message::read_u32_le(p);
                uint32_t rate = 44100;
                uint16_t ch = 2;
                switch (fmt) {
                case 1: rate = 16000; ch = 1; break;
                case 2: rate = 44100; ch = 2; break;
                case 3: rate = 8000;  ch = 1; break;
                case 4: rate = 48000; ch = 2; break;
                case 5: rate = 16000; ch = 1; break;
                case 6: rate = 24000; ch = 1; break;
                case 7: rate = 16000; ch = 2; break;
                default: rate = 44100; ch = 2; break;
                }
                g_ce_wave_player.open(rate, ch, 16);
                g_ce_wave_player.write(p + 12, n - 12);
            }
        } else {
            if (c == Cmd::STATE && n >= 4) {
                // Log the RAW state value — the dispatch layer maps unknown
                // values to ConnState::UNKNOWN, losing what the box sent.
                int32_t raw = static_cast<int32_t>(Message::read_u32_le(p));
                PushEventW(L"msg type=3 (STATE) len=%u raw=%d", (unsigned)n, raw);
            } else {
                PushEventW(L"msg type=%u len=%u", static_cast<unsigned>(c), static_cast<unsigned>(n));
            }
        }
    };

    // Resilient auto-reconnect loop
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        try {
            g_conn_state = 0;
            InterlockedExchange(&g_write_failures, 0);  // reset self-heal counter per session
            auto t = std::make_unique<UsbTransport_CE>();
            t->setErrorCallback([](int err) {
                if (err == -999) {
                    // Watchdog: USB endpoint stall / zero-read timeout
                    InterlockedExchange(&g_write_failures, 5);
                } else {
                    InterlockedIncrement(&g_write_failures);
                }
            });
            SetLineW(1, L"[1/3] 正在检测 USB 盒子 (0x1314:0x1520)...");

            g_conn = new Connection(std::move(t), cb);
            g_conn->start();

            SetLineW(1, L"[2/3] USB 盒子已认领！正在初始化协议...");
            PushEventW(L"发送 sendInit 配置脉冲 (mode=2 iPhone)...");
            g_conn->sendInit(false, 2);
            PushEventW(L"配置完成！全时监听 iPhone 中...");
            g_conn_state = 1;
            SetLineW(1, L"[2/3] 盒子就绪！请将 iPhone 插入盒子 USB");
            SetLineW(2, L"盒子常驻，插拔 iPhone 即可热拉起");

            HANDLE hbeat = CreateThread(nullptr, 256 * 1024, HeartbeatThread,
                                       nullptr, 0, nullptr);

            while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
                // Self-heal: after 5 consecutive transport write failures or watchdog trigger
                // the box link is stale — tear down and rebuild the whole connection automatically.
                if (InterlockedCompareExchange(&g_write_failures, 0, 0) >= 5) {
                    PushEventW(L"自愈: 检测到链路异常/假死，自动重建连接...");
                    SetLineW(1, L"[自愈] USB 链路异常，正在自动重连...");
                    break;
                }
                Sleep(200);
            }

            if (hbeat) {
                WaitForSingleObject(hbeat, 1500);
                CloseHandle(hbeat);
            }
            if (g_conn) {
                g_conn->stop();
                delete g_conn;
                g_conn = nullptr;
            }
        } catch (const std::exception &e) {
            g_conn_state = 3;
            PushEventW(L"ERR: %S", e.what());
            SetLineW(1, L"[等待] 驱动加载或未检测到盒子，重试中...");
            if (g_conn) {
                try { g_conn->stop(); } catch (...) {}
                delete g_conn;
                g_conn = nullptr;
            }
            Sleep(2000);
        } catch (...) {
            g_conn_state = 3;
            PushEventW(L"ERR: unknown exception");
            Sleep(2000);
        }
    }

    g_decoder.close();
    g_ce_wave_player.close();
    g_done = TRUE;
    SetLineW(1, L"已退出 — 点击屏幕返回车机");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    InitLogging();
    LogDebugW(L"[MAIN] Application starting");
    InstallSehNet();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_CARLINKIT;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1024;
    if (sh <= 0) sh = 600;
    LogDebugW(L"[MAIN] Screen resolution: %dx%d", sw, sh);

    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_vid_cs);
    g_hVidEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    g_hwnd = CreateWindowW(WC_CARLINKIT, L"Carlinkit",
                           WS_POPUP | WS_VISIBLE,
                           0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);

    // Initialize H.264 video decoder early before spawning worker threads
    LogDebugW(L"[MAIN] Initializing H.264 video decoder...");
    if (g_decoder.init()) {
        PushEventW(L"OK: H.264 解码器就绪 (FFmpeg 2.x)");
        LogDebugW(L"[MAIN] OK: H.264 decoder ready");
    } else {
        PushEventW(L"WARN: 解码器加载失败 (检查 avcodec DLL)");
        LogDebugW(L"[MAIN] WARN: H.264 decoder init failed");
    }

    ShowWindow(g_hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    // Network & RNDIS driver diagnostic probe
    ProbeNetworkAndRndis();

    // Dedicated video decode thread (flags = 0)
    HANDLE vdt = CreateThread(nullptr, 256 * 1024, VideoDecodeThread, nullptr, 0, nullptr);

    // Main protocol client thread (flags = 0)
    HANDLE ct = CreateThread(nullptr, 256 * 1024, ClientThread, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    InterlockedExchange(reinterpret_cast<volatile LONG *>(&g_running), 0);
    if (g_hVidEvent) SetEvent(g_hVidEvent);

    if (vdt) {
        WaitForSingleObject(vdt, 1500);
        CloseHandle(vdt);
    }
    if (ct) {
        WaitForSingleObject(ct, 2000);
        CloseHandle(ct);
    }

    if (g_hVidEvent) CloseHandle(g_hVidEvent);

    DeleteCriticalSection(&g_vid_cs);
    DeleteCriticalSection(&g_cs);

    LogDebugW(L"[MAIN] Application exited cleanly");
    if (g_hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hLogFile);
        g_hLogFile = INVALID_HANDLE_VALUE;
    }
    if (g_udpLogSock != INVALID_SOCKET) {
        closesocket(g_udpLogSock);
        g_udpLogSock = INVALID_SOCKET;
        WSACleanup();
    }
    return 0;
}

#endif // _WIN32_WCE / __MINGW32CE__
