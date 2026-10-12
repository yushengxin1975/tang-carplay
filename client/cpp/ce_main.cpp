// ce_main.cpp — Carlinkit client for BYD 唐 2015 (Phase B.2 Bring-up)
//
// 零写卡。支持冷启动驱动自注入（Usb_Box / ceusbbox.dll）。
// 启动后弹一个全屏窗口（1024x600）：
//   - 顶部适配比亚迪原车状态栏（y >= 48 开始绘制，不被顶栏遮挡）
//   - 右上角 [ 退出 EXIT ] 实体大按钮，单次点击安全退出
//   - 独立音量触控调节 [ 🔉 - ] [ 🔊 + ] 与方向盘/旋钮实体按键映射
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
static volatile int   g_current_volume = 100; // 0..150%

static carlinkit::WavePlayer g_ce_wave_player;
static carlinkit::Connection *g_conn = nullptr;
static carlinkit::H264Decoder_CE g_decoder;

// Video frame rendering surface (1024x600 16bpp RGB565)
// Static 8-byte aligned BSS buffer — zero allocation failure risk, never NULL
static alignas(8) uint16_t g_carplay_pixels[1024 * 600];
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

// Persistent diagnostic file logging
static FILE *g_logFile = nullptr;

static void InitLogging() {
    WCHAR exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        WCHAR *pSlash = wcsrchr(exePath, L'\\');
        if (pSlash) {
            *(pSlash + 1) = L'\0';
            wcscat(exePath, L"carplay.log");
            g_logFile = _wfopen(exePath, L"a+");
            if (g_logFile) {
                fwprintf(g_logFile, L"\n=== CarPlay Startup (Tick=%u) ===\n", GetTickCount());
                fflush(g_logFile);
            }
        }
    }
}

static void LogDebugW(const WCHAR *fmt, ...) {
    WCHAR buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = L'\0';

    if (g_logFile) {
        fwprintf(g_logFile, L"[%u] %s\n", GetTickCount(), buf);
        fflush(g_logFile);
    }
    OutputDebugStringW(buf);
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
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
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
            // [ 退出 EXIT ] button (x: 870..995, y: 50..92)
            if (px >= 860 && px <= 1010 && py >= 45 && py <= 98) {
                LogDebugW(L"[EXIT] Diagnostic screen EXIT button clicked");
                PostQuitMessage(0);
                return 0;
            }
            // Volume [ 🔉 - ] (x: 640..710, y: 50..92)
            if (px >= 640 && px <= 710 && py >= 45 && py <= 98) {
                AdjustVolume(-10);
                return 0;
            }
            // Volume [ 🔊 + ] (x: 770..840, y: 50..92)
            if (px >= 770 && px <= 840 && py >= 45 && py <= 98) {
                AdjustVolume(+10);
                return 0;
            }
        } else {
            // 2. CarPlay Overlay Button Interceptions (top floating bar y <= 44)
            if (py <= 44) {
                // Exit button (x: 880..1010)
                if (px >= 880 && px <= 1010) {
                    LogDebugW(L"[EXIT] CarPlay overlay EXIT button clicked");
                    PostQuitMessage(0);
                    return 0;
                }
                // Volume [-] (x: 640..710)
                if (px >= 640 && px <= 710) {
                    AdjustVolume(-10);
                    return 0;
                }
                // Volume [+] (x: 800..870)
                if (px >= 800 && px <= 870) {
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
            RECT barRc = {630, 4, 1010, 40};
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

            // [ 🔉 - ] button
            SetTextColor(memDC, RGB(180, 220, 255));
            WCHAR vmTxt[] = L"🔉 -";
            ExtTextOutW(memDC, 650, 12, 0, nullptr, vmTxt, wcslen(vmTxt), nullptr);

            // Current Volume
            WCHAR volStr[32];
            _snwprintf(volStr, 31, L"音量 %d%%", g_current_volume);
            SetTextColor(memDC, RGB(255, 255, 255));
            ExtTextOutW(memDC, 725, 12, 0, nullptr, volStr, wcslen(volStr), nullptr);

            // [ 🔊 + ] button
            SetTextColor(memDC, RGB(180, 220, 255));
            WCHAR vpTxt[] = L"🔊 +";
            ExtTextOutW(memDC, 820, 12, 0, nullptr, vpTxt, wcslen(vpTxt), nullptr);

            // [ 退出 EXIT ] button
            RECT exRc = {885, 8, 1000, 36};
            HBRUSH exBr = CreateSolidBrush(RGB(48, 62, 82));
            FillRect(memDC, &exRc, exBr);
            DeleteObject(exBr);

            SetTextColor(memDC, RGB(255, 220, 120));
            WCHAR exTxt[] = L"退出 EXIT";
            ExtTextOutW(memDC, 905, 12, 0, nullptr, exTxt, wcslen(exTxt), nullptr);

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

            // Volume Controls [ 🔉 - ] [ 音量 100% ] [ 🔊 + ]
            RECT vDownRc = {640, 50, 710, 92};
            HBRUSH vDownBr = CreateSolidBrush(RGB(32, 42, 56));
            FillRect(memDC, &vDownRc, vDownBr);
            DeleteObject(vDownBr);

            SetTextColor(memDC, RGB(180, 225, 255));
            WCHAR vDownTxt[] = L"🔉 -";
            ExtTextOutW(memDC, 655, 60, 0, nullptr, vDownTxt, wcslen(vDownTxt), nullptr);

            WCHAR vCurTxt[32];
            _snwprintf(vCurTxt, 31, L"%d%%", curVol);
            SetTextColor(memDC, RGB(255, 255, 255));
            ExtTextOutW(memDC, 725, 60, 0, nullptr, vCurTxt, wcslen(vCurTxt), nullptr);

            RECT vUpRc = {770, 50, 840, 92};
            HBRUSH vUpBr = CreateSolidBrush(RGB(32, 42, 56));
            FillRect(memDC, &vUpRc, vUpBr);
            DeleteObject(vUpBr);

            SetTextColor(memDC, RGB(180, 225, 255));
            WCHAR vUpTxt[] = L"🔊 +";
            ExtTextOutW(memDC, 785, 60, 0, nullptr, vUpTxt, wcslen(vUpTxt), nullptr);

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

            // 3. Card 1: Connection Status Card
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
                bool ok = false;
                try {
                    ok = g_decoder.decode_frame(nal_buffer.data(), actual_len,
                                                reinterpret_cast<uint8_t*>(g_carplay_pixels),
                                                1024 * 2, &fw, &fh, /*flip_vertical=*/true);
                } catch (...) {
                    LogDebugW(L"[VID] EXCEPTION in decode_frame!");
                }
                if (ok) {
                    if (!g_has_carplay_frame) {
                        LogDebugW(L"[VID] First CarPlay frame decoded successfully (%dx%d)!", fw, fh);
                    }
                    g_has_carplay_frame = TRUE;
                    InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_decoded_frames));
                    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
                }
            }
        }
    }
    LogDebugW(L"[VID] VideoDecodeThread stopped");
    return 0;
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
            SetLineW(1, L"[在线] iPhone 已连接！CarPlay 会话已建立");
            SetLineW(2, L"画面解码中，双向触控与车机喇叭已激活");
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
            if (n > 20) {
                EnterCriticalSection(&g_vid_cs);
                const size_t h264_sz = n - 20;
                g_pending_video.resize(h264_sz + 64);
                std::memcpy(g_pending_video.data(), p + 20, h264_sz);
                std::memset(g_pending_video.data() + h264_sz, 0, 64);
                g_has_pending_video = TRUE;
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
                                       nullptr, 0, nullptr);

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

    g_decoder.close();
    g_ce_wave_player.close();
    g_done = TRUE;
    SetLineW(1, L"已退出 — 点击屏幕返回车机");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    InitLogging();
    LogDebugW(L"[MAIN] Application starting");

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
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    return 0;
}

#endif // _WIN32_WCE / __MINGW32CE__
