/* usbwatch.c ????015 WinCE USB/????????v0.1 (Phase 1 ???)
 *
 * ?????????Phase 1 ??????????? USB ???????????????
 *   1. ????????dump: HKLM\Drivers\Active + HKLM\Drivers\USB ???(???3)
 *   2. ??????(400ms): Drivers\Active ??? / ????????? ???????????
 *      ??????: ????????????iPhone ????15s ????????5s ??(??????)
 *               ????U?????) ????10s ??????????????? *   3. ??????????dump ???????????
 *
 * ????????? usbwatch_log.txt??
 *   - ??iPhone ?????? Active ??USB ??? ??USB host ????????????? *   - ??iPhone ???????????????iPhone ?????????/???/??DU hub????? *   - ??????: iPhone ????????????"????????????????????
 *
 * ???????????????????????= ??????????240s ??????????
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ---------------- ?????arlive ?????---------------- */
static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static DWORD  g_startTick = 0;

static void LogInit(void) {
    WCHAR path[MAX_PATH] = {0};
    g_startTick = GetTickCount();
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR *dot = wcsrchr(path, L'.');
        if (dot) *dot = 0;
        wcscat(path, L"_log.txt");
    } else wcscpy(path, L"usbwatch_log.txt");
    g_hLog = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
static void Log(const char *fmt, ...) {
    if (g_hLog == INVALID_HANDLE_VALUE) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0; msg[n] = 0;
    char line[600];
    int m = _snprintf(line, sizeof(line) - 1, "[t+%u.%03u] %s\r\n",
                      (GetTickCount() - g_startTick) / 1000,
                      (GetTickCount() - g_startTick) % 1000, msg);
    if (m < 0) m = 0;
    DWORD wr; WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    FlushFileBuffers(g_hLog);   /* ??????????????? */
}
static void W2A(const WCHAR *w, char *a, unsigned cap) {
    unsigned i = 0;
    if (!a || cap == 0) return;
    while (w && w[i] && i < cap - 1) { unsigned short v = (unsigned short)w[i]; a[i] = (v < 0x80) ? (char)v : '?'; i++; }
    a[i] = 0;
}

/* ---------------- ???????? diff ---------------- */

#define MAX_SNAP 128
typedef struct {
    char key[64];
    char val[200];   /* "Name|Dll" */
} SNAPITEM;
static SNAPITEM g_snap[MAX_SNAP];
static int      g_snapN = 0;

/* ????????rivers\Active ???(key) + Name|Dll|Bus(val)????????? */
static int TakeSnapshot(SNAPITEM *items, int max) {
    int n = 0;
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &hk) == ERROR_SUCCESS) {
        for (DWORD idx = 0; idx < MAX_SNAP && n < max; idx++) {
            WCHAR name[64]; DWORD size = 64;
            if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            char *k = items[n].key;
            char *v = items[n].val;
            W2A(name, k, sizeof(items[0].key));
            v[0] = 0;
            HKEY hk2;
            if (RegOpenKeyEx(hk, name, 0, 0, &hk2) == ERROR_SUCCESS) {
                static const WCHAR *vn[3] = { L"Name", L"Dll", L"Bus" };
                for (int i = 0; i < 3; i++) {
                    WCHAR buf[160]; DWORD sz = sizeof(buf); DWORD type;
                    if (RegQueryValueEx(hk2, vn[i], NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
                        char a[160]; W2A(buf, a, sizeof(a));
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
    /* ????????MOUNT ?????diff */
    static const WCHAR *dirs[] = { L"\\Storage Card", L"\\SDMMC", L"\\SDMEM",
        L"\\USB Storage", L"\\USBHD", L"\\Hard Disk", L"\\SD Card", L"\\SDMemory", L"\\SDMemory2" };
    for (int i = 0; i < 9 && n < max; i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFile(dirs[i], &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            char a[64]; W2A(dirs[i], a, sizeof(a));
            strcpy(items[n].key, "MOUNT");
            _snprintf(items[n].val, sizeof(items[0].val) - 1, "%s", a);
            n++;
        }
    }
    return n;
}

/* ????????sBaseline=1 ???????????????????*/
static void DiffAndReport(int isBaseline, const char *tag) {
    SNAPITEM cur[MAX_SNAP];
    int curN = TakeSnapshot(cur, MAX_SNAP);

    if (isBaseline) {
        Log("--- baseline %s: %d items ---", tag, curN);
        for (int i = 0; i < curN; i++) Log("  B: %s = %s", cur[i].key, cur[i].val);
        memcpy(g_snap, cur, sizeof(SNAPITEM) * curN);
        g_snapN = curN;
        return;
    }
    for (int i = 0; i < curN; i++) {          /* ??? */
        int found = 0;
        for (int j = 0; j < g_snapN; j++)
            if (strcmp(cur[i].key, g_snap[j].key) == 0 &&
                strcmp(cur[i].val, g_snap[j].val) == 0) { found = 1; break; }
        if (!found) Log("+++ ADDED   %s = %s", cur[i].key, cur[i].val);
    }
    for (int j = 0; j < g_snapN; j++) {          /* ??? */
        int found = 0;
        for (int i = 0; i < curN; i++)
            if (strcmp(cur[i].key, g_snap[j].key) == 0 &&
                strcmp(cur[i].val, g_snap[j].val) == 0) { found = 1; break; }
        if (!found) Log("--- REMOVED %s = %s", g_snap[j].key, g_snap[j].val);
    }
    memcpy(g_snap, cur, sizeof(SNAPITEM) * curN);
    g_snapN = curN;
}

/* ---------------- HKLM\Drivers\USB ??? dump????????300??? ---------------- */
static int g_usbLines = 0;
static void DumpUsbTree(HKEY root, const WCHAR *sub, int depth) {
    if (depth > 3 || g_usbLines > 300) return;
    HKEY hk;
    if (RegOpenKeyEx(root, sub, 0, 0, &hk) != ERROR_SUCCESS) return;
    char asub[168]; W2A(sub, asub, sizeof(asub));
    Log("  USBTREE: %s", asub);
    g_usbLines++;
    /* ??????????*/
    static const WCHAR *vn[4] = { L"Dll", L"Prefix", L"VID", L"PID" };
    for (int i = 0; i < 4; i++) {
        WCHAR buf[96]; DWORD sz = sizeof(buf); DWORD type;
        if (RegQueryValueEx(hk, vn[i], NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            char a[96], vn2[8]; W2A(buf, a, sizeof(a)); W2A(vn[i], vn2, sizeof(vn2));
            Log("    .%s = %s", vn2, a);
        }
    }
    for (DWORD idx = 0; idx < 32; idx++) {
        WCHAR name[96]; DWORD size = 96;
        if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        WCHAR child[MAX_PATH];
        _snwprintf(child, MAX_PATH - 1, L"%s\\%s", sub, name);
        DumpUsbTree(HKEY_LOCAL_MACHINE, child, depth + 1);
    }
    RegCloseKey(hk);
}

/* ---------------- GUI ---------------- */
static volatile BOOL g_running = TRUE;
static HWND  g_hwnd = NULL;
static char  g_status[128] = "USB WATCH running... tap screen to stop";
static CRITICAL_SECTION g_csStatus;

#define WC_WATCH L"USBWATCH"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        Log("USER tap -> stopping");
        g_running = FALSE;
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
        char s[160];
        EnterCriticalSection(&g_csStatus);
        strcpy(s, g_status);
        LeaveCriticalSection(&g_csStatus);
        /* CE ??Unicode-only???????????*/
        WCHAR ws[160];
        MultiByteToWideChar(CP_ACP, 0, s, -1, ws, 160);
        ExtTextOutW(hdc, 20, rc.bottom / 2 - 20, 0, NULL, ws, wcslen(ws), NULL);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void SetStatus(const char *s) {
    EnterCriticalSection(&g_csStatus);
    strncpy(g_status, s, sizeof(g_status) - 1);
    LeaveCriticalSection(&g_csStatus);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ???????????????? diff ????? */
static DWORD WINAPI WatchThread(LPVOID arg) {
    (void)arg;
    Log("==== usbwatch v0.1 start ====");
    Log("STEP1: baseline dump");
    DiffAndReport(1, "startup");

    Log("STEP2: USB registry tree");
    DumpUsbTree(HKEY_LOCAL_MACHINE, L"Drivers\\USB", 0);
    if (g_usbLines == 0) Log("  (HKLM\\Drivers\\USB ?????????)");

    Log("STEP3: polling 400ms ????? ONETRIP.md ?????????");
    SetStatus("WATCHING - plug/unplug devices, tap to stop");
    int ticks = 0;
    while (g_running && ticks < 600) {   /* 600*400ms = 240s */
        DiffAndReport(0, "poll");
        Sleep(400);
        ticks++;
        if (ticks % 75 == 0) SetStatus("WATCHING (still alive) - tap to stop");
    }

    Log("STEP4: final dump");
    DiffAndReport(1, "final");
    Log("==== usbwatch done, %u ms ====", GetTickCount() - g_startTick);
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    g_hLog = INVALID_HANDLE_VALUE;
    g_running = FALSE;
    if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_WATCH;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_csStatus);
    LogInit();

    g_hwnd = CreateWindowW(WC_WATCH, L"usbwatch",
        WS_VISIBLE | WS_CAPTION,
        0, sh / 4, sw, sh / 2,
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 256*1024, WatchThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!g_running) break;
    }
    /* ????????????????240s ??????tap ?????g_running=FALSE ????????? */
    if (th) { WaitForSingleObject(th, 3000); CloseHandle(th); }
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    return 0;
}
