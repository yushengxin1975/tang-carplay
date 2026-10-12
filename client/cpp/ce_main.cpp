// ce_main.cpp — Carlinkit client for BYD 唐 2015 (Phase B.1 / C bring-up)
//
// 零写卡。支持冷启动驱动自注入（Usb_Box / ceusbbox.dll）。
// 启动后弹一个全屏窗口（1024x600）：
//   - 顶部适配比亚迪原车状态栏（y >= 48 开始绘制，不被顶栏遮挡）
//   - 右上角 [ 退出 EXIT ] 实体大按钮，单次点击安全退出
//   - 全屏深色质感座舱仪表盘：连接状态卡片 + 实时遥测卡片 + 诊断日志终端
//   - 全原生 Unicode (WCHAR) 绘制，彻底消除中文乱码
//   - 屏幕触摸 -> Message::Touch 自动上报
//   - 音频通过 WavePlayer (waveOutOpen/Write) 直接流式输出到车机喇叭
//
// 编译：CeGCC 9.3 (arm-mingw32ce-g++)  -std=c++17 -static-libstdc++

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

// ---------------- GUI (zero-persistence, Unicode native) ----------------
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
static volatile DWORD g_audio_frames = 0;
static volatile DWORD g_touch_count = 0;
static volatile int   g_conn_state = 0; // 0=init, 1=ready, 2=online, 3=error

static carlinkit::WavePlayer g_ce_wave_player;
static carlinkit::Connection *g_conn = nullptr;

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

        // 1. Top-right prominent [ EXIT ] button (x: 870..1000, y: 48..95)
        if (px >= 860 && px <= 1010 && py >= 45 && py <= 98) {
            PostQuitMessage(0);
            return 0;
        }

        // 2. Top-left corner (80x80) emergency double-tap
        if (px < 80 && py < 80) {
            static DWORD last_exit_tap = 0;
            DWORD now = GetTickCount();
            if (now - last_exit_tap < 600) {
                PostQuitMessage(0);
                return 0;
            }
            last_exit_tap = now;
        }

        // 3. Touch event translation to CarPlay (0..10000 normalized)
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

        // 1. Deep automotive cockpit background (avoids muddy green)
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
        DWORD aFrames = g_audio_frames;
        DWORD tCount = g_touch_count;
        LeaveCriticalSection(&g_cs);

        SetBkMode(memDC, TRANSPARENT);

        // 2. Header Area (starts at y = 48 to comfortably avoid BYD top bar)
        // Title: Antigravity CarPlay
        SetTextColor(memDC, RGB(255, 255, 255));
        WCHAR titleW[] = L"Antigravity CarPlay";
        ExtTextOutW(memDC, 28, 50, 0, nullptr, titleW, wcslen(titleW), nullptr);

        // Subtitle: BYD TANG 2015
        SetTextColor(memDC, RGB(130, 175, 205));
        WCHAR subW[] = L"比亚迪·唐 (2015旗舰版) · 独立流媒体客户端";
        ExtTextOutW(memDC, 28, 74, 0, nullptr, subW, wcslen(subW), nullptr);

        // Top-right EXIT Button: [ 退出 EXIT ]
        RECT btnRc = {870, 50, 995, 92};
        HBRUSH btnBr = CreateSolidBrush(RGB(38, 50, 66));
        FillRect(memDC, &btnRc, btnBr);
        DeleteObject(btnBr);

        HPEN btnPen = CreatePen(PS_SOLID, 1, RGB(80, 110, 145));
        HPEN oldP = static_cast<HPEN>(SelectObject(memDC, btnPen));
        MoveToEx(memDC, btnRc.left, btnRc.top, nullptr);
        LineTo(memDC, btnRc.right, btnRc.top);
        LineTo(memDC, btnRc.right, btnRc.bottom);
        LineTo(memDC, btnRc.left, btnRc.bottom);
        LineTo(memDC, btnRc.left, btnRc.top);
        SelectObject(memDC, oldP);
        DeleteObject(btnPen);

        SetTextColor(memDC, RGB(230, 245, 255));
        WCHAR exitTxt[] = L"退出 EXIT";
        ExtTextOutW(memDC, 898, 60, 0, nullptr, exitTxt, wcslen(exitTxt), nullptr);

        // 3. Card 1: Connection Status Card (Left: x = 24, y = 106, w = 560, h = 184)
        RECT c1Rc = {24, 106, 584, 290};
        HBRUSH c1Br = CreateSolidBrush(RGB(23, 30, 39));
        FillRect(memDC, &c1Rc, c1Br);
        DeleteObject(c1Br);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(45, 60, 78));
        oldP = static_cast<HPEN>(SelectObject(memDC, borderPen));
        MoveToEx(memDC, c1Rc.left, c1Rc.top, nullptr);
        LineTo(memDC, c1Rc.right, c1Rc.top);
        LineTo(memDC, c1Rc.right, c1Rc.bottom);
        LineTo(memDC, c1Rc.left, c1Rc.bottom);
        LineTo(memDC, c1Rc.left, c1Rc.top);

        // Status Badge Pill
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

        // Card 1 body lines
        SetTextColor(memDC, RGB(255, 255, 255));
        ExtTextOutW(memDC, 42, 150, 0, nullptr, l4[1], wcslen(l4[1]), nullptr);

        SetTextColor(memDC, RGB(160, 230, 200));
        ExtTextOutW(memDC, 42, 182, 0, nullptr, l4[2], wcslen(l4[2]), nullptr);

        SetTextColor(memDC, RGB(140, 200, 240));
        ExtTextOutW(memDC, 42, 214, 0, nullptr, l4[3], wcslen(l4[3]), nullptr);

        SetTextColor(memDC, RGB(110, 145, 175));
        WCHAR guideW[] = L"提示: 盒子长插在车机主板USB，上车插上手机即可秒连";
        ExtTextOutW(memDC, 42, 248, 0, nullptr, guideW, wcslen(guideW), nullptr);

        // 4. Card 2: Telemetry Card (Right: x = 600, y = 106, w = 400, h = 184)
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
        _snwprintf(rowW, 95, L"视频流:   %u 帧 (1024x600 30FPS)", vFrames);
        ExtTextOutW(memDC, 618, 152, 0, nullptr, rowW, wcslen(rowW), nullptr);

        _snwprintf(rowW, 95, L"音频流:   %u 帧 (PCM 44.1kHz 喇叭直出)", aFrames);
        ExtTextOutW(memDC, 618, 188, 0, nullptr, rowW, wcslen(rowW), nullptr);

        _snwprintf(rowW, 95, L"触控事件: %u 次 (归一化双向触控)", tCount);
        ExtTextOutW(memDC, 618, 224, 0, nullptr, rowW, wcslen(rowW), nullptr);

        SetTextColor(memDC, RGB(120, 190, 150));
        WCHAR volTip[] = L"音量控制: 直接旋转中控台大旋钮或方向盘滚轮";
        ExtTextOutW(memDC, 618, 256, 0, nullptr, volTip, wcslen(volTip), nullptr);

        // 5. Card 3: Diagnostic Terminal (Bottom: x = 24, y = 302, w = 976, h = 278)
        RECT c3Rc = {24, 302, 1000, 582};
        HBRUSH c3Br = CreateSolidBrush(RGB(10, 13, 17));
        FillRect(memDC, &c3Rc, c3Br);
        DeleteObject(c3Br);

        MoveToEx(memDC, c3Rc.left, c3Rc.top, nullptr);
        LineTo(memDC, c3Rc.right, c3Rc.top);
        LineTo(memDC, c3Rc.right, c3Rc.bottom);
        LineTo(memDC, c3Rc.left, c3Rc.bottom);
        LineTo(memDC, c3Rc.left, c3Rc.top);

        // Terminal header bar
        RECT tHeadRc = {24, 302, 1000, 328};
        HBRUSH tHeadBr = CreateSolidBrush(RGB(18, 24, 32));
        FillRect(memDC, &tHeadRc, tHeadBr);
        DeleteObject(tHeadBr);

        SetTextColor(memDC, RGB(130, 160, 190));
        WCHAR logTitle[] = L"诊断事件日志 (DIAGNOSTIC EVENT LOG) — 驱动加载与会话协议追踪";
        ExtTextOutW(memDC, 38, 308, 0, nullptr, logTitle, wcslen(logTitle), nullptr);

        // Event lines
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
        PushEventW(L"STATE: %d", static_cast<int>(s));
        if (s == ConnState::NO_DEVICE) {
            g_conn_state = 1;
            SetLineW(1, L"[就绪] 等待插入 iPhone (USB 盒子已就绪)");
            SetLineW(2, L"盒子常驻供电，插拔 iPhone 即可热拉起");
        } else if (s == ConnState::ONLINE) {
            g_conn_state = 2;
            SetLineW(1, L"[在线] iPhone 已连接！会话保持中");
            SetLineW(2, L"双向触控与车机喇叭音频流已激活");
        } else if (s == ConnState::ERROR_STATE) {
            g_conn_state = 3;
            SetLineW(1, L"[错误] 连接状态异常，正在自动恢复...");
        }
    };

    cb.on_plugged = []() {
        g_conn_state = 2;
        PushEventW(L"OK: CMD_PLUGGED 收到 -> 会话拉起");
        SetLineW(1, L"[在线] iPhone 已连接！CarPlay 运行中");
        SetLineW(2, L"双向触控与音频流已激活");
    };

    cb.on_unplugged = []() {
        g_conn_state = 1;
        PushEventW(L"CMD_UNPLUGGED 收到 -> iPhone 断开");
        SetLineW(1, L"[就绪] 等待插入 iPhone (无需拔插盒子)");
        SetLineW(2, L"下次插上 iPhone 立即自动重连");
        g_ce_wave_player.reset();
    };

    cb.on_any = [](Cmd c, const uint8_t *p, std::size_t n) {
        if (c == Cmd::VIDEO_DATA) {
            InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_video_frames));
            if ((g_video_frames % 60) == 1) {
                SetLineW(3, L"视频: %u 帧 | 音频: %u 帧 | 触控: %u 次",
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
            PushEventW(L"msg type=%u len=%u", static_cast<unsigned>(c), static_cast<unsigned>(n));
        }
    };

    // Resilient auto-reconnect loop
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_running), 1, 1) == 1) {
        try {
            g_conn_state = 0;
            auto t = std::make_unique<UsbTransport_CE>();
            SetLineW(1, L"[1/3] 正在检测 USB 盒子 (0x1314:0x1520)...");

            g_conn = new Connection(std::move(t), cb);
            g_conn->start();

            SetLineW(1, L"[2/3] USB 盒子已认领！正在初始化协议...");
            PushEventW(L"发送 sendInit 配置脉冲...");
            g_conn->sendInit(false, 2);
            PushEventW(L"配置完成！全时监听 iPhone 中...");
            g_conn_state = 1;
            SetLineW(1, L"[2/3] 盒子就绪！请将 iPhone 插入盒子 USB");
            SetLineW(2, L"盒子常驻，插拔 iPhone 即可热拉起");

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

    g_ce_wave_player.close();
    g_done = TRUE;
    SetLineW(1, L"已退出 — 点击屏幕返回车机");
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
