/* probe.c — 唐2015 WinCE 车机环境探针 v0.2 (零写盘版)
 *
 * 与 v0.1 关键区别：不创建任何文件到 TF 卡。
 * 所有结论：1. 屏幕常驻显示 5 行关键指标 + VERDICT
 *          2. 退出时无任何写盘动作
 *
 * 触发 BYD 防破解的可能性（v0.1）：
 *   - 在 \gps_byd\ 创建了新文件 probe_log.txt → FAT 写入
 *   - FlushFileBuffers 每行日志都刷盘 → 多次 FAT 元数据更新
 * v0.2 取消所有磁盘写入，包括 log 文件。
 *
 * 代价：用户需拍照屏幕才能保留结论，不再有 log 文件。
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

static void W2A_local(const WCHAR *w, char *a, unsigned cap) {
    unsigned i = 0;
    if (!a || cap == 0) return;
    while (w && w[i] && i < cap - 1) { unsigned short v = (unsigned short)w[i]; a[i] = (v < 0x80) ? (char)v : '?'; i++; }
    a[i] = 0;
}

/* GUI 状态 */
static HWND g_hwnd = NULL;
static CRITICAL_SECTION g_cs;
static char g_lines[8][120];

static void SetLine(int i, const char *fmt, ...) {
    if (i < 0 || i > 7) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_lines[i], 119, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

#define WC_PROBE L"PROBE"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
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
        char tmp[8][120];
        EnterCriticalSection(&g_cs);
        memcpy(tmp, g_lines, sizeof(tmp));
        LeaveCriticalSection(&g_cs);
        for (int i = 0; i < 8; i++) {
            WCHAR w[120];
            MultiByteToWideChar(CP_ACP, 0, tmp[i], -1, w, 120);
            ExtTextOutW(hdc, 10, 10 + i * 28, 0, NULL, w, wcslen(w), NULL);
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

/* 探测结果 */
static const char *g_osVerdict = "?";
static const char *g_cpuVerdict = "?";
static const char *g_usbVerdict = "?";
static char g_screen[64] = "?";
static char g_mem[64] = "?";
static int  g_usbCount = 0;

static void ProbeVersion(void) {
    OSVERSIONINFO ovi;
    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (GetVersionEx(&ovi)) {
        if (ovi.dwMajorVersion == 6)      g_osVerdict = "CE 6.0 OK";
        else if (ovi.dwMajorVersion == 5) g_osVerdict = "CE 5.0 (needs review)";
        else                              g_osVerdict = "UNEXPECTED";
        char b[80];
        _snprintf(b, 79, "OS: CE %u.%u build %u", ovi.dwMajorVersion,
                  ovi.dwMinorVersion, ovi.dwBuildNumber);
        SetLine(0, b);
    }
}

static void ProbeCpu(void) {
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    if (si.wProcessorArchitecture == 5) g_cpuVerdict = "ARM OK";
    else                                g_cpuVerdict = "NOT ARM (!)";
    char b[80];
    _snprintf(b, 79, "CPU: arch=%u type=%u level=%u",
              si.wProcessorArchitecture, si.dwProcessorType, si.wProcessorLevel);
    SetLine(1, b);
}

static void ProbeScreen(void) {
    HDC hdc = GetDC(NULL);
    if (hdc) {
        int w = GetDeviceCaps(hdc, HORZRES);
        int h = GetDeviceCaps(hdc, VERTRES);
        int bpp = GetDeviceCaps(hdc, BITSPIXEL);
        _snprintf(g_screen, 63, "%dx%d@%d", w, h, bpp);
        ReleaseDC(NULL, hdc);
    }
    SetLine(2, "SCREEN: %s", g_screen);
}

static void ProbeMem(void) {
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    _snprintf(g_mem, 63, "%uMB total %uMB free",
              ms.dwTotalPhys / (1024 * 1024),
              ms.dwAvailPhys / (1024 * 1024));
    SetLine(3, "MEM: %s", g_mem);
}

static void ProbeUsb(void) {
    HKEY hk;
    g_usbCount = 0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &hk) != ERROR_SUCCESS) {
        g_usbVerdict = "REG FAIL";
        SetLine(4, "USB: Drivers\\Active open fail");
        return;
    }
    for (DWORD idx = 0; idx < 200; idx++) {
        WCHAR name[64]; DWORD size = 64;
        if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hk2;
        if (RegOpenKeyEx(hk, name, 0, 0, &hk2) != ERROR_SUCCESS) continue;
        char hit[160] = "";
        static const WCHAR *vn[2] = { L"Name", L"Dll" };
        for (int i = 0; i < 2; i++) {
            WCHAR buf[80]; DWORD sz = sizeof(buf); DWORD type;
            if (RegQueryValueEx(hk2, vn[i], NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
                char a[80]; W2A_local(buf, a, sizeof(a));
                if (hit[0]) strcat(hit, "|");
                strncat(hit, a, 79 - strlen(hit));
            }
        }
        RegCloseKey(hk2);
        if (strstr(hit, "usb") || strstr(hit, "USB") ||
            strstr(hit, "hcd") || strstr(hit, "HCD"))
            g_usbCount++;
    }
    RegCloseKey(hk);
    if (g_usbCount >= 1) g_usbVerdict = "USB host ALIVE";
    else                 g_usbVerdict = "no USB drv (need load)";
    SetLine(4, "USB: %d USB-related drv loaded", g_usbCount);
}

static void ProbeStorage(void) {
    static const WCHAR *dirs[] = {
        L"\\Storage Card", L"\\SDMMC", L"\\SDMEM",
        L"\\USB Storage", L"\\USBHD", L"\\Hard Disk", L"\\SD Card"
    };
    char found[200] = "mounts:";
    for (int i = 0; i < 7; i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFile(dirs[i], &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            char a[32]; W2A_local(dirs[i], a, sizeof(a));
            strncat(found, " ", 199 - strlen(found));
            strncat(found, a, 199 - strlen(found));
        }
    }
    SetLine(5, "%s", found);
}

static void ProbeProcList(void) {
    HMODULE hMod = GetModuleHandle(L"toolhelp.dll");
    if (!hMod) hMod = LoadLibrary(L"toolhelp.dll");
    if (!hMod) { SetLine(6, "PROC: toolhelp missing"); return; }
    typedef HANDLE (*PFN_SNAP)(DWORD, DWORD);
    typedef BOOL (*PFN_FIRST)(HANDLE, void *);
    typedef BOOL (*PFN_NEXT)(HANDLE, void *);
    PFN_SNAP  snap  = (PFN_SNAP)GetProcAddress(hMod, L"CreateToolhelp32Snapshot");
    PFN_FIRST first = (PFN_FIRST)GetProcAddress(hMod, L"Process32First");
    PFN_NEXT  next  = (PFN_NEXT)GetProcAddress(hMod, L"Process32Next");
    if (!snap || !first || !next) { SetLine(6, "PROC: api missing"); return; }

#pragma pack(push,1)
    struct PROCENTRY {
        DWORD dwSize, cntUsage, th32ProcessID, th32DefaultHeapID, th32ModuleID,
               cntThreads, th32ParentProcessID;
        LONG  pcPriClassBase; DWORD dwFlags;
        WCHAR szExeFile[MAX_PATH];
    } pe;
#pragma pack(pop)

    HANDLE hSnap = snap(0x00000002, 0);
    if (hSnap == INVALID_HANDLE_VALUE) { SetLine(6, "PROC: snap fail"); return; }
    memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
    int n = 0; char list[100] = "procs:";
    if (first(hSnap, &pe)) {
        do {
            char a[32]; W2A_local(pe.szExeFile, a, sizeof(a));
            strncat(list, " ", 99 - strlen(list));
            strncat(list, a, 99 - strlen(list));
            n++;
            if (n >= 6) { strncat(list, "...", 99 - strlen(list)); break; }
            memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
        } while (next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    SetLine(6, "%s", list);
}

static void ProbeBuiltInUsb(void) {
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn", 0, 0, &hk) != ERROR_SUCCESS) {
        SetLine(7, "BuiltIn: absent"); return;
    }
    char found[200] = "USB built-in:";
    for (DWORD idx = 0; idx < 200; idx++) {
        WCHAR name[80]; DWORD size = 80;
        if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        char a[80]; W2A_local(name, a, sizeof(a));
        if (strstr(a, "USB") || strstr(a, "usb") || strstr(a, "HCD") || strstr(a, "hcd")) {
            strncat(found, " ", 199 - strlen(found));
            strncat(found, a, 199 - strlen(found));
        }
    }
    RegCloseKey(hk);
    SetLine(7, "%s", found);
}

static DWORD WINAPI ProbeThread(LPVOID arg) {
    (void)arg;
    Sleep(500);
    ProbeVersion();
    ProbeCpu();
    ProbeScreen();
    ProbeMem();
    ProbeUsb();
    ProbeStorage();
    ProbeProcList();
    ProbeBuiltInUsb();
    SetLine(0, "[OS]   %s", g_osVerdict);
    SetLine(1, "[CPU]  %s", g_cpuVerdict);
    SetLine(2, "[USB]  %s", g_usbVerdict);
    SetLine(3, "SCREEN %s  MEM %s", g_screen, g_mem);
    SetLine(7, "TAP SCREEN TO EXIT (no file written)");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_PROBE;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    SetLine(0, "probe v0.2 (no-write) starting...");
    SetLine(1, "tapping screen will exit");

    g_hwnd = CreateWindowW(WC_PROBE, L"probe v0.2",
        WS_VISIBLE | WS_CAPTION,
        0, 0, sw, sh, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 256 * 1024, ProbeThread, NULL,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (th) { WaitForSingleObject(th, 30000); CloseHandle(th); }
    /* 关键：此处无 FlushFileBuffers、无 CreateFile、无任何磁盘写入 */
    return 0;
}
