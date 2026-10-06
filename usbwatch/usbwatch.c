/* usbwatch.c — 唐2015 WinCE USB/设备监测器 v0.2 (零写盘版)
 *
 * 与 v0.1 区别：不写任何文件到卡。事件流全部显示在屏幕上。
 *
 * 显示布局（屏幕底部滚动 8 行）：
 *   [0] 状态文字（WAITING / ACTIVE）
 *   [1] 当前时间 + 累积事件数
 *   [2-9] 最近 8 条事件（ring buffer）
 *   - 启动时一次性 dump Active 前 16 项（基线）
 *
 * 退出：屏幕任意位置点击（240s 后自动退出）
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <process.h>

static void W2A_local(const WCHAR *w, char *a, unsigned cap) {
    unsigned i = 0;
    if (!a || cap == 0) return;
    while (w && w[i] && i < cap - 1) { unsigned short v = (unsigned short)w[i]; a[i] = (v < 0x80) ? (char)v : '?'; i++; }
    a[i] = 0;
}

/* ---------------- GUI ---------------- */
static HWND g_hwnd = NULL;
static CRITICAL_SECTION g_cs;
static DWORD g_startTick = 0;
static int  g_added = 0, g_removed = 0;

#define EVT_LINES 10
#define EVT_LEN   120
static char g_events[EVT_LINES][EVT_LEN];
static int  g_evtHead = 0;   /* 下一条写入位置 */
static int  g_evtN = 0;       /* 总已写入条数 */

static void PushEvent(const char *fmt, ...) {
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    char *line = g_events[g_evtHead];
    _vsnprintf(line, EVT_LEN - 1, fmt, ap);
    va_end(ap);
    g_evtHead = (g_evtHead + 1) % EVT_LINES;
    if (g_evtN < EVT_LINES) g_evtN++;
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

#define WC_WATCH L"USBWATCH"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        PushEvent("USER tap -> stopping");
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 40));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 0));
        char tmp[EVT_LINES][EVT_LEN];
        EnterCriticalSection(&g_cs);
        memcpy(tmp, g_events, sizeof(tmp));
        LeaveCriticalSection(&g_cs);
        DWORD now = GetTickCount() - g_startTick;
        /* 头部 2 行：状态 + 提示 */
        WCHAR w[160];
        char line[160];
        _snprintf(line, 159, "[STATUS] t+%u.%u  +%d/-%d  tap to exit", now/1000, (now/100)%10, g_added, g_removed);
        MultiByteToWideChar(CP_ACP, 0, line, -1, w, 160);
        ExtTextOutW(hdc, 10, 10, 0, NULL, w, wcslen(w), NULL);
        _snprintf(line, 159, "[HOW] plug/unplug device to watch diff; tap exit");
        MultiByteToWideChar(CP_ACP, 0, line, -1, w, 160);
        ExtTextOutW(hdc, 10, 36, 0, NULL, w, wcslen(w), NULL);
        /* 事件流：从最早的开始 */
        int y = 62;
        for (int i = 0; i < EVT_LINES; i++) {
            int idx;
            if (g_evtN < EVT_LINES) idx = i;
            else idx = (g_evtHead + i) % EVT_LINES;
            MultiByteToWideChar(CP_ACP, 0, tmp[idx], -1, w, 160);
            ExtTextOutW(hdc, 10, y, 0, NULL, w, wcslen(w), NULL);
            y += 24;
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---------------- 状态快照 ---------------- */
#define MAX_SNAP 128
typedef struct { char key[64]; char val[200]; } SNAPITEM;
static SNAPITEM g_snap[MAX_SNAP];
static int      g_snapN = 0;

static int TakeSnapshot(SNAPITEM *items, int max) {
    int n = 0;
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &hk) == ERROR_SUCCESS) {
        for (DWORD idx = 0; idx < MAX_SNAP && n < max; idx++) {
            WCHAR name[64]; DWORD size = 64;
            if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            char *k = items[n].key; char *v = items[n].val;
            W2A_local(name, k, sizeof(items[0].key));
            v[0] = 0;
            HKEY hk2;
            if (RegOpenKeyEx(hk, name, 0, 0, &hk2) == ERROR_SUCCESS) {
                static const WCHAR *vn[3] = { L"Name", L"Dll", L"Bus" };
                for (int i = 0; i < 3; i++) {
                    WCHAR buf[160]; DWORD sz = sizeof(buf); DWORD type;
                    if (RegQueryValueEx(hk2, vn[i], NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
                        char a[160]; W2A_local(buf, a, sizeof(a));
                        if (v[0]) strncat(v, "|", sizeof(items[0].val) - strlen(v) - 1);
                        strncat(v, a, sizeof(items[0].val) - strlen(v) - 1);
                    }
                }
                RegCloseKey(hk2);
            }
            n++;
        }
        RegCloseKey(hk);
    }
    static const WCHAR *dirs[] = { L"\\Storage Card", L"\\SDMMC", L"\\SDMEM",
        L"\\USB Storage", L"\\USBHD", L"\\Hard Disk", L"\\SD Card", L"\\SDMemory", L"\\SDMemory2" };
    for (int i = 0; i < 9 && n < max; i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFile(dirs[i], &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            char a[64]; W2A_local(dirs[i], a, sizeof(a));
            strcpy(items[n].key, "MOUNT");
            _snprintf(items[n].val, sizeof(items[0].val) - 1, "%s", a);
            n++;
        }
    }
    return n;
}

static void DiffAndReport(int isBaseline) {
    SNAPITEM cur[MAX_SNAP];
    int curN = TakeSnapshot(cur, MAX_SNAP);
    if (isBaseline) {
        PushEvent("baseline: %d items", curN);
        for (int i = 0; i < curN && i < 16; i++) {
            PushEvent("  B: %s = %s", cur[i].key, cur[i].val);
        }
        memcpy(g_snap, cur, sizeof(SNAPITEM) * curN);
        g_snapN = curN;
        return;
    }
    for (int i = 0; i < curN; i++) {
        int found = 0;
        for (int j = 0; j < g_snapN; j++)
            if (strcmp(cur[i].key, g_snap[j].key) == 0 &&
                strcmp(cur[i].val, g_snap[j].val) == 0) { found = 1; break; }
        if (!found) { PushEvent("+++ ADD %s = %s", cur[i].key, cur[i].val); g_added++; }
    }
    for (int j = 0; j < g_snapN; j++) {
        int found = 0;
        for (int i = 0; i < curN; i++)
            if (strcmp(cur[i].key, g_snap[j].key) == 0 &&
                strcmp(cur[i].val, g_snap[j].val) == 0) { found = 1; break; }
        if (!found) { PushEvent("--- DEL %s = %s", g_snap[j].key, g_snap[j].val); g_removed++; }
    }
    memcpy(g_snap, cur, sizeof(SNAPITEM) * curN);
    g_snapN = curN;
}

/* ---------------- Worker ---------------- */
static DWORD WINAPI WatchThread(LPVOID arg) {
    (void)arg;
    g_startTick = GetTickCount();
    PushEvent("=== usbwatch v0.2 (no-write) start ===");
    PushEvent("STEP1: baseline dump");
    DiffAndReport(1);
    PushEvent("STEP2: polling 400ms; PLUG DEVICE NOW");
    int rounds = 0;
    while (rounds < 600) {   /* 600*400ms = 240s */
        DiffAndReport(0);
        Sleep(400);
        rounds++;
        if (rounds % 25 == 0)
            PushEvent("alive %ds (+%d/-%d)", rounds*2/5, g_added, g_removed);
    }
    PushEvent("=== done, tap or auto-exit ===");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_WATCH;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    g_startTick = GetTickCount();
    memset(g_events, 0, sizeof(g_events));

    g_hwnd = CreateWindowW(WC_WATCH, L"usbwatch v0.2",
        WS_VISIBLE | WS_CAPTION,
        0, sh / 4, sw, sh / 2, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 256 * 1024, WatchThread, NULL,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (th) { WaitForSingleObject(th, 3000); CloseHandle(th); }
    /* 无任何磁盘写入 */
    return 0;
}
