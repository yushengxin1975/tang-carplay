// ce_main.cpp — Carlinkit client for BYD 唐 2015 (Phase B.1 / C bring-up)
//
// 零写卡。需要 USBKIT.DLL 已经被部署（运行 usbtest v0.6 / 类似流程）。
// 启动后弹一个全屏窗口（1024x600）：
//   - 4 行核心状态（连接状态、iPhone 状态、流媒体统计）
//   - 8 行诊断事件环，实时显示握手与协议过程
//   - 屏幕触摸 -> Message::Touch 自动上报
//   - 音频通过 WavePlayer (waveOutOpen/Write) 直接流式输出到车机喇叭
//
// 编译：CeGCC 9.3 (arm-mingw32ce-g++)  -std=c++17 -static-libstdc++
//
// CE-only: wrap whole TU in #if defined(__MINGW32CE__) || defined(_WIN32_WCE) so PC builds see an empty TU.

#include "ce_main.h"

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
#include "protocol/message.h"
#include "audio/wave_player.h"
#include "protocol_const.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

// ---------------- GUI (zero-persistence) ----------------
static HWND g_hwnd = nullptr;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;

static char g_lines[4][96] = {
    {"ANTIGRAVITY CARPLAY — BYD TANG (MT3360/WinCE 6.0)"},
    {"[1/3] 初始化 USBKIT 驱动中..."},
    {"[2/3] 等待插入 iPhone (长插盒子即插即用)"},
    {"双击左上角 [EXIT] 退出"}
};

#define EVT_LINES 8
#define EVT_LEN   100
static char g_events[EVT_LINES][EVT_LEN];
static int g_evtHead = 0, g_evtN = 0;

static volatile DWORD g_video_frames = 0;
static volatile DWORD g_audio_frames = 0;
static volatile DWORD g_touch_count = 0;

static carlinkit::WavePlayer g_ce_wave_player;
static carlinkit::Connection *g_conn = nullptr;

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
    case WM_LBUTTONDOWN: {
        int px = LOWORD(lp), py = HIWORD(lp);

        // Top-left corner (80x60) double-tap to safely return to car dashboard
        if (px < 80 && py < 60) {
            static DWORD last_exit_tap = 0;
            DWORD now = GetTickCount();
            if (now - last_exit_tap < 600) {
                PostQuitMessage(0);
                return 0;
            }
            last_exit_tap = now;
        }

        uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
        uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
        if (nx > 10000) nx = 10000;
        if (ny > 10000) ny = 10000;

        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_touch_count));
        if (g_conn) {
            try { g_conn->send(Message::Touch(14, nx, ny)); } catch (...) {}
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (wp & MK_LBUTTON) {
            int px = LOWORD(lp), py = HIWORD(lp);
            uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
            uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
            if (nx > 10000) nx = 10000;
            if (ny > 10000) ny = 10000;

            if (g_conn) {
                try { g_conn->send(Message::Touch(15, nx, ny)); } catch (...) {}
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int px = LOWORD(lp), py = HIWORD(lp);
        uint32_t nx = static_cast<uint32_t>((px * 10000) / 1024);
        uint32_t ny = static_cast<uint32_t>((py * 10000) / 600);
        if (nx > 10000) nx = 10000;
        if (ny > 10000) ny = 10000;

        if (g_conn) {
            try { g_conn->send(Message::Touch(16, nx, ny)); } catch (...) {}
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

        // Background
        HBRUSH br = CreateSolidBrush(RGB(5, 15, 10));
        FillRect(memDC, &rc, br);
        DeleteObject(br);

        // Exit button hint in top-left
        RECT btnRc = {5, 5, 75, 30};
        HBRUSH btnBr = CreateSolidBrush(RGB(30, 50, 40));
        FillRect(memDC, &btnRc, btnBr);
        DeleteObject(btnBr);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(180, 220, 200));
        WCHAR exitW[] = L"EXIT";
        ExtTextOutW(memDC, 20, 9, 0, nullptr, exitW, 4, nullptr);

        // Copy strings under lock
        char l4[4][96], eL[EVT_LINES][EVT_LEN];
        EnterCriticalSection(&g_cs);
        memcpy(l4, g_lines, sizeof(l4));
        memcpy(eL, g_events, sizeof(eL));
        int en = g_evtN, eh = g_evtHead;
        LeaveCriticalSection(&g_cs);

        WCHAR w[120];
        // Header title
        SetTextColor(memDC, RGB(100, 255, 180));
        MultiByteToWideChar(CP_ACP, 0, l4[0], -1, w, 120);
        ExtTextOutW(memDC, 90, 8, 0, nullptr, w, wcslen(w), nullptr);

        // Status lines
        int y = 42;
        for (int i = 1; i < 4; ++i) {
            SetTextColor(memDC, (i == 1) ? RGB(255, 230, 100) : RGB(140, 240, 180));
            MultiByteToWideChar(CP_ACP, 0, l4[i], -1, w, 120);
            ExtTextOutW(memDC, 20, y, 0, nullptr, w, wcslen(w), nullptr);
            y += 28;
        }

        // Diagnostic events log divider
        y += 10;
        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(40, 80, 60));
        HPEN oldPen = static_cast<HPEN>(SelectObject(memDC, linePen));
        MoveToEx(memDC, 20, y, nullptr);
        LineTo(memDC, rc.right - 20, y);
        SelectObject(memDC, oldPen);
        DeleteObject(linePen);
        y += 12;

        SetTextColor(memDC, RGB(160, 200, 180));
        for (int i = 0; i < EVT_LINES; ++i) {
            int idx = (en < EVT_LINES) ? i : ((eh + i) % EVT_LINES);
            MultiByteToWideChar(CP_ACP, 0, eL[idx], -1, w, 120);
            ExtTextOutW(memDC, 20, y, 0, nullptr, w, wcslen(w), nullptr);
            y += 24;
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

// ---------------- Heartbeat Thread ----------------
static DWORD WINAPI HeartbeatThread(LPVOID) {
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        if (g_conn) {
            try { g_conn->send(carlinkit::Cmd::HEARTBEAT); } catch (...) {}
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
        PushEvent("STATE: %d", static_cast<int>(s));
        if (s == ConnState::NO_DEVICE) SetLine(1, "[就绪] 等待插入 iPhone (USB 盒子已连接)");
        else if (s == ConnState::ONLINE) SetLine(1, "[在线] iPhone 已连接！会话保持中");
        else if (s == ConnState::ERROR_STATE) SetLine(1, "[错误] 连接状态异常，正在自动恢复...");
    };

    cb.on_plugged = []() {
        PushEvent("CMD_PLUGGED 收到 -> 会话拉起");
        SetLine(1, "[在线] iPhone 已连接！CarPlay 运行中");
        SetLine(2, "双向触控与音频流已激活");
    };

    cb.on_unplugged = []() {
        PushEvent("CMD_UNPLUGGED 收到 -> iPhone 断开");
        SetLine(1, "[就绪] 等待插入 iPhone (无需拔插盒子)");
        SetLine(2, "下次插上 iPhone 立即自动重连");
        g_ce_wave_player.reset();
    };

    cb.on_any = [](Cmd c, const uint8_t *p, std::size_t n) {
        if (c == Cmd::VIDEO_DATA) {
            InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_video_frames));
            if ((g_video_frames % 60) == 1) {
                SetLine(3, "视频: %u 帧 | 音频: %u 帧 | 触控: %u 次",
                        g_video_frames, g_audio_frames, g_touch_count);
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
            PushEvent("msg type=%u len=%u", static_cast<unsigned>(c), static_cast<unsigned>(n));
        }
    };

    // Resilient auto-reconnect loop
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        try {
            auto t = std::make_unique<UsbTransport_CE>();
            SetLine(1, "[1/3] 正在检测 USB 盒子 (0x1314:0x1520)...");
            t->open();
            SetLine(1, "[2/3] USB 盒子已认领！正在初始化协议...");
            PushEvent("OK: USBTransport claimed");

            g_conn = new Connection(std::move(t), cb);
            g_conn->start();

            // Send initialization burst (1024x600@30, charge_mode=2, CMD_9 discovery)
            PushEvent("发送 sendInit 配置脉冲...");
            g_conn->sendInit(false, 2);
            PushEvent("配置完成！全时监听 iPhone 中...");
            SetLine(1, "[2/3] 盒子就绪！请将 iPhone 插入盒子 USB");
            SetLine(2, "盒子常驻，插拔 iPhone 即可热拉起");

            HANDLE hbeat = CreateThread(nullptr, 256 * 1024, HeartbeatThread,
                                       nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);

            while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
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
            PushEvent("ERR: %s", e.what());
            SetLine(1, "[等待] 未检测到盒子，请插入 USB 盒子...");
            if (g_conn) {
                try { g_conn->stop(); } catch (...) {}
                delete g_conn;
                g_conn = nullptr;
            }
            Sleep(1500);
        } catch (...) {
            PushEvent("ERR: unknown exception");
            Sleep(1500);
        }
    }

    g_ce_wave_player.close();
    g_done = TRUE;
    SetLine(1, "已退出 — 点击屏幕返回车机");
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
    if (sw <= 0) sw = 1024;
    if (sh <= 0) sh = 600;

    InitializeCriticalSection(&g_cs);
    g_hwnd = CreateWindowW(WC_CARLINKIT, L"Carlinkit",
                           WS_POPUP | WS_VISIBLE,
                           0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE ct = CreateThread(nullptr, 256 * 1024, ClientThread, nullptr,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    InterlockedExchange(reinterpret_cast<volatile LONG *>(&g_running), 0);
    if (ct) {
        WaitForSingleObject(ct, 2000);
        CloseHandle(ct);
    }
    DeleteCriticalSection(&g_cs);
    return 0;
}

#endif // _WIN32_WCE / __MINGW32CE__
