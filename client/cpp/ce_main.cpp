// ce_main.cpp — Carlinkit client for BYD 唐 2015 (Phase B.1 CE bring-up)
//
// 零写卡。需要 USBKIT.DLL 已经被部署（运行 usbtest v0.6 / 类似流程）。
// 启动后弹一个全屏窗口：4 行状态 + 9 行事件环，显示握手过程。
// 收到 PLUGGED / 设备数据时主动发送 heartbeat 等。
//
// 编译：CeGCC 9.3 (arm-mingw32ce-g++)  -std=c++17 -static-libstdc++
//
// CE-only: wrap whole TU in #ifdef (__MINGW32CE__) || defined(_WIN32_WCE) so PC builds see an empty TU.

#include "ce_main.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// ---------------- GUI (zero-persistence) ----------------
#ifdef (__MINGW32CE__) || defined(_WIN32_WCE)
static HWND g_hwnd = nullptr;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;
static char g_lines[4][96] = {{"CARLINKIT 0.1.0 (Phase B.1)"},
                              {"waiting USB init..."},
                              {""},
                              {"TAP to exit"}};
#define EVT_LINES 9
#define EVT_LEN   100
static char g_events[EVT_LINES][EVT_LEN];
static int g_evtHead = 0, g_evtN = 0;

static void SetLine(int i, const char *fmt, ...) {
    if (i < 0 || i > 3) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_lines[i], 95, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void PushEvent(const char *fmt, ...) {
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_events[g_evtHead], EVT_LEN - 1, fmt, ap);
    va_end(ap);
    g_evtHead = (g_evtHead + 1) % EVT_LINES;
    if (g_evtN < EVT_LINES) g_evtN++;
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

#define WC_CARLINKIT L"Carlinkit"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_done) PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0, 20, 0));
        FillRect(hdc, &rc, br); DeleteObject(br);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(150, 255, 200));
        char l4[4][96], eL[EVT_LINES][EVT_LEN];
        EnterCriticalSection(&g_cs);
        memcpy(l4, g_lines, sizeof(l4));
        memcpy(eL, g_events, sizeof(eL));
        int en = g_evtN, eh = g_evtHead;
        LeaveCriticalSection(&g_cs);
        WCHAR w[120];
        int y = 8;
        for (int i = 0; i < 4; ++i) {
            MultiByteToWideChar(CP_ACP, 0, l4[i], -1, w, 120);
            ExtTextOutW(hdc, 10, y, 0, nullptr, w, wcslen(w), nullptr);
            y += 28;
        }
        SetTextColor(hdc, RGB(200, 255, 200));
        for (int i = 0; i < EVT_LINES; ++i) {
            int idx = (en < EVT_LINES) ? i : ((eh + i) % EVT_LINES);
            MultiByteToWideChar(CP_ACP, 0, eL[idx], -1, w, 120);
            ExtTextOutW(hdc, 10, y, 0, nullptr, w, wcslen(w), nullptr);
            y += 22;
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static carlinkit::Connection *g_conn = nullptr;

// ---------------- heartbeat sender thread ----------------
static DWORD WINAPI HeartbeatThread(LPVOID) {
    while (InterlockedCompareExchange((LONG *)&g_running, 1, 1) == 1) {
        if (g_conn) {
            try { g_conn->send(carlinkit::Cmd::HEARTBEAT); } catch (...) {}
        }
        Sleep(1000);
    }
    return 0;
}

// ---------------- Connection main thread ----------------
static DWORD WINAPI ClientThread(LPVOID) {
    using namespace carlinkit;
    InboundCallbacks cb;
    cb.on_state = [](ConnState s) {
        PushEvent("STATE: %d", (int)s);
        if (s == ConnState::NO_DEVICE) SetLine(1, "WAIT: plug iPhone to box");
        else if (s == ConnState::ONLINE) SetLine(1, "iPHONE PLUGGED (box online)");
        else if (s == ConnState::ERROR) SetLine(1, "STATE=ERR (see log)");
    };
    cb.on_plugged = [](){ PushEvent("CMD_PLUGGED received"); SetLine(2, "iPhone -> Box -> BYD"); };
    cb.on_unplugged = [](){ PushEvent("CMD_UNPLUGGED received"); SetLine(2, "iPhone disconnected"); };
    cb.on_control  = [](Cmd, const uint8_t*, std::size_t n) { PushEvent("control %u bytes", (unsigned)n); };
    cb.on_video    = [](Cmd, const uint8_t*, std::size_t n) { PushEvent("video %u bytes", (unsigned)n); };
    cb.on_audio    = [](Cmd, const uint8_t*, std::size_t n) { PushEvent("audio %u bytes", (unsigned)n); };
    cb.on_any      = [](Cmd, const uint8_t*, std::size_t n) { if (n) PushEvent("msg type=%u len=%u", (unsigned)*(uint32_t*)nullptr /*avoid UB*/, (unsigned)n); };

    try {
        auto t = std::make_unique<UsbTransport_CE>();
        SetLine(0, "open CE USB transport...");
        t->open();
        SetLine(0, "OK  open CE USB transport");
        SetLine(1, "polling for box...");

        g_conn = new Connection(std::move(t), cb);
        g_conn->start();
        SetLine(0, "READY: waiting plug/cmd");

        // Background heartbeat + main thread blocks on window msgs
        HANDLE hbeat = CreateThread(nullptr, 256 * 1024, HeartbeatThread,
                                   nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        MSG m;
        while (GetMessage(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        InterlockedExchange((LONG *)&g_running, 0);
        WaitForSingleObject(hbeat, 2000);
        CloseHandle(hbeat);
        g_conn->stop();
        delete g_conn;
        g_conn = nullptr;
    } catch (const std::exception &e) {
        char buf[200];
        _snprintf(buf, 199, "ERR: %s", e.what());
        SetLine(0, buf);
        PushEvent("FATAL: %s", e.what());
    } catch (...) {
        PushEvent("FATAL: unknown");
    }
    g_done = TRUE;
    SetLine(0, "DONE - tap to exit");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_CARLINKIT;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    g_hwnd = CreateWindowW(WC_CARLINKIT, L"Carlinkit",
                           WS_VISIBLE | WS_CAPTION,
                           0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE ct = CreateThread(nullptr, 256 * 1024, ClientThread, nullptr,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    InterlockedExchange((LONG *)&g_running, 0);
    if (ct) { WaitForSingleObject(ct, 3000); CloseHandle(ct); }
    return 0;
}
