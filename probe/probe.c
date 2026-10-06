/* probe.c — 唐2015 WinCE 车机环境探针 v0.1 (Phase 0)
 *
 * 目标：上车一次跑完，回答直连 CarPlay 的全部前提问题：
 *   1. CE 内核版本（期望 6.0；5.0/7.0 也能继续但需调整）
 *   2. CPU 架构/型号（期望 ARM；ARMv6/ARMv7 决定 ffmpeg 优化档位）
 *   3. 内存（H.264 软解至少要 64MB 可用）
 *   4. 屏幕分辨率/色深（决定 CarPlay 请求的 canvas）
 *   5. USB host 驱动是否加载（注册表 Drivers\Active + BuiltIn 扫描）— Phase 1 生死判定
 *   6. 存储卡挂载点（U盘启动路径）+ 关键 DLL 存在性（ddraw/ws2 等）
 *   7. 当前进程列表（找原车 launcher/导航进程名，后续集成入口）
 *
 * 输出：
 *   - EXE 同目录 probe_log.txt（ASCII，带回车换行）
 *   - 弹窗显示关键结论，点确定退出
 *
 * 编码约定（继承 carlive 真机经验）：
 *   - 日志一律窄字符（char），宽字符降级 '?' 再落盘，防 CeGCC %ls 崩溃
 *   - 日志前 3 秒强制 FlushFileBuffers，防"启动即崩、日志全丢"
 *   - 全部宽 API（CE 是 Unicode-only）
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ---------------- 日志（照抄 carlive 验证过的模式，精简版） ---------------- */

static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static DWORD  g_startTick = 0;

static void LogInit(void) {
    WCHAR path[MAX_PATH] = {0};
    g_startTick = GetTickCount();
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR *dot = wcsrchr(path, L'.');
        if (dot) *dot = 0;
        wcscat(path, L"_log.txt");
    } else {
        wcscpy(path, L"probe_log.txt");
    }
    g_hLog = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void Log(const char *fmt, ...) {
    if (g_hLog == INVALID_HANDLE_VALUE) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(msg) - 1) n = (int)(sizeof(msg) - 1);
    msg[n] = 0;

    char line[600];
    int m = _snprintf(line, sizeof(line) - 1, "[t+%u] %s\r\n",
                      GetTickCount() - g_startTick, msg);
    if (m < 0) m = 0;
    DWORD wr = 0;
    WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    /* 启动 3 秒内每条都刷盘：探测过程短，不能丢任何一行 */
    if (GetTickCount() - g_startTick < 3000) FlushFileBuffers(g_hLog);
}

/* 宽→窄降级（非 ASCII 变 '?'，carlive R28 教训） */
static void W2A(const WCHAR *w, char *a, unsigned cap) {
    unsigned i = 0;
    if (!a || cap == 0) return;
    while (w && w[i] && i < cap - 1) {
        unsigned short v = (unsigned short)w[i];
        a[i] = (v < 0x80) ? (char)v : '?';
        i++;
    }
    a[i] = 0;
}

/* ---------------- 探测项 ---------------- */

static void DumpVersion(void) {
    OSVERSIONINFO ovi;
    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (GetVersionEx(&ovi)) {
        Log("OS: Windows CE %u.%u (build %u) platform=%u",
            ovi.dwMajorVersion, ovi.dwMinorVersion,
            ovi.dwBuildNumber, ovi.dwPlatformId);
        /* CE6 = 6.0x；5.x 需重新评估 API 差异；7.x(WEC7) 反而更好办 */
        if (ovi.dwMajorVersion == 6)      Log("OS VERDICT: CE 6.0 -> OK, 按计划走");
        else if (ovi.dwMajorVersion == 5) Log("OS VERDICT: CE 5.0 -> 注意: API 差异需评估");
        else                              Log("OS VERDICT: %u -> 重新评估", ovi.dwMajorVersion);
    } else {
        Log("OS: GetVersionEx FAILED err=%u", GetLastError());
    }
}

static void DumpCpu(void) {
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    Log("CPU: arch=%u (5=ARM) procType=%u level=%u rev=%u numCPUs=%u pageSize=%u",
        si.wProcessorArchitecture, si.dwProcessorType,
        si.wProcessorLevel, si.wProcessorRevision,
        si.dwNumberOfProcessors, si.dwPageSize);
    /* 常见值: 2577=ARM720 2578=ARM820 2579=ARM920 2580=ARM1020(ARM10/11 系) 0x0C24? */
    if (si.wProcessorArchitecture == 5)
        Log("CPU VERDICT: ARM -> OK (procType 见上, 决定 ffmpeg armv4/armv6 档位)");
    else
        Log("CPU VERDICT: 非 ARM(=%u) -> 直连方案需重新评估!", si.wProcessorArchitecture);
}

static void DumpMemory(void) {
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    Log("MEM: phys total=%uKB avail=%uKB virt total=%uKB avail=%uKB",
        ms.dwTotalPhys / 1024, ms.dwAvailPhys / 1024,
        ms.dwTotalVirtual / 1024, ms.dwAvailVirtual / 1024);
    if (ms.dwAvailPhys >= 48 * 1024 * 1024)
        Log("MEM VERDICT: 可用>=48MB -> H.264 软解内存 OK");
    else
        Log("MEM VERDICT: 可用<48MB -> 软解需按 %uKB 预算裁剪", ms.dwAvailPhys / 1024);
}

static void DumpScreen(void) {
    HDC hdc = GetDC(NULL);
    if (!hdc) { Log("SCREEN: GetDC FAILED"); return; }
    int w = GetDeviceCaps(hdc, HORZRES);
    int h = GetDeviceCaps(hdc, VERTRES);
    int bpp = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
    Log("SCREEN: %dx%d @ %dbpp", w, h, bpp);
    ReleaseDC(NULL, hdc);
    Log("SCREEN VERDICT: CarPlay canvas 按 %dx%d 请求", w, h);
}

/* OEM/平台名：CE 专属 SPI，两档值都试（不同 SDK 定义不一） */
static void DumpOem(void) {
    WCHAR buf[256];
    char a[256];
    memset(buf, 0, sizeof(buf));
    if (SystemParametersInfo(0x0100 /*SPI_GETOEMINFO*/, 256, buf, 0)) {
        W2A(buf, a, sizeof(a)); Log("OEMINFO: %s", a);
    } else Log("OEMINFO: not available (err=%u)", GetLastError());
    memset(buf, 0, sizeof(buf));
    if (SystemParametersInfo(0x0101 /*SPI_GETPLATFORMTYPE*/, 256, buf, 0)) {
        W2A(buf, a, sizeof(a)); Log("PLATFORMTYPE: %s", a);
    } else Log("PLATFORMTYPE: not available (err=%u)", GetLastError());
}

static void DumpPlatformRegistry(void) {
    HKEY hk;
    char a[256];
    /* HKLM\Platform: Name/Vendor — 车机方案商线索 */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Platform", 0, 0, &hk) == ERROR_SUCCESS) {
        static const WCHAR *names[] = { L"Name", L"Vendor" };
        for (int i = 0; i < 2; i++) {
            WCHAR buf[256]; DWORD size = sizeof(buf); DWORD type = 0;
            if (RegQueryValueEx(hk, names[i], NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
                W2A(buf, a, sizeof(a)); Log("HKLM\\Platform\\%s = %s", i ? "Vendor" : "Name", a);
            }
        }
        RegCloseKey(hk);
    } else Log("HKLM\\Platform: absent");
    /* AkuVersion: CE6 的 QFE/维护版本 */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Versions", 0, 0, &hk) == ERROR_SUCCESS) {
        WCHAR buf[64]; DWORD size = sizeof(buf); DWORD type = 0;
        if (RegQueryValueEx(hk, L"AkuVersion", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            W2A(buf, a, sizeof(a)); Log("AkuVersion: %s", a);
        }
        RegCloseKey(hk);
    }
}

/* 枚举注册表子键名（宽转窄打印）。返回枚举到的个数 */
static int EnumSubKeys(HKEY root, const WCHAR *sub, int maxKeys) {
    HKEY hk;
    if (RegOpenKeyEx(root, sub, 0, 0, &hk) != ERROR_SUCCESS) {
        char asub[128]; W2A(sub, asub, sizeof(asub));
        Log("REG: %s absent", asub);
        return -1;
    }
    char a[180];
    int count = 0;
    for (DWORD idx = 0; idx < (DWORD)maxKeys; idx++) {
        WCHAR name[128]; DWORD size = 128;
        if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        W2A(name, a, sizeof(a));
        Log("  [%u] %s", idx, a);
        count++;
    }
    RegCloseKey(hk);
    return count;
}

/* USB host 生死判定：BuiltIn 里的 HCD（主机控制器驱动） + Active 里的已加载项 */
static void DumpUsb(void) {
    Log("--- HKLM\\Drivers\\BuiltIn (最多96项, 找 *usb*/*hcd*/*ohci*/*ehci*) ---");
    int n = EnumSubKeys(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn", 96);
    Log("BuiltIn total shown: %d", n);

    Log("--- HKLM\\Drivers\\Active (已加载驱动, 最多64项) ---");
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &hk) == ERROR_SUCCESS) {
        char a[160], adll[160];
        int usbCount = 0;
        for (DWORD idx = 0; idx < 64; idx++) {
            WCHAR name[64]; DWORD size = 64;
            if (RegEnumKeyEx(hk, idx, name, &size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            W2A(name, a, sizeof(a));
            /* 每项里的 Dll/Name 值才是重点 */
            HKEY hk2;
            WCHAR dll[MAX_PATH] = L"", dev[128] = L"";
            if (RegOpenKeyEx(hk, name, 0, 0, &hk2) == ERROR_SUCCESS) {
                DWORD sz2 = sizeof(dll);
                RegQueryValueEx(hk2, L"Dll", NULL, NULL, (LPBYTE)dll, &sz2);
                sz2 = sizeof(dev);
                RegQueryValueEx(hk2, L"Name", NULL, NULL, (LPBYTE)dev, &sz2);
                RegCloseKey(hk2);
            }
            W2A(dll, adll, sizeof(adll));
            /* 只详列 USB 相关，其余一行带过（防止日志爆炸） */
            char alldll[300];
            W2A(dev, a, sizeof(a));
            _snprintf(alldll, sizeof(alldll) - 1, "%s %s", a, adll);
            if (strstr(alldll, "usb") || strstr(alldll, "hcd") ||
                strstr(alldll, "USB") || strstr(alldll, "HCD")) {
                Log("  USB> [%s] dev=%s dll=%s", a, a, adll);
                usbCount++;
            }
        }
        RegCloseKey(hk);
        Log("USB VERDICT: %d 个 USB 相关驱动已加载 (%s)",
            usbCount, usbCount ? "USB host 存活 -> Phase 1 可行" : "未发现 -> 上车插U盘重测/驱动未加载");
    }
}

static void DumpStorage(void) {
    static const WCHAR *dirs[] = {
        L"\\Storage Card", L"\\SDMMC", L"\\SDMEM", L"\\USB Storage",
        L"\\USBHD", L"\\Hard Disk", L"\\SD Card", L"\\SDMemory", L"\\SDMemory2",
    };
    Log("--- 存储挂载点探测 ---");
    for (int i = 0; i < 9; i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFile(dirs[i], &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            char a[64]; W2A(dirs[i], a, sizeof(a));
            Log("  MOUNT OK: %s", a);
        }
    }
}

static void DumpDlls(void) {
    struct { const WCHAR *dll; const char *why; } items[] = {
        { L"\\Windows\\ddraw.dll",     "DirectDraw (Phase3 渲染)" },
        { L"\\Windows\\ws2.dll",       "Winsock (CarPlay over IP)" },
        { L"\\Windows\\waveapi.dll",   "WaveAPI 音频" },
        { L"\\Windows\\uhcd.dll",      "USB UHCI host" },
        { L"\\Windows\\ohcd.dll",      "USB OHCI host" },
        { L"\\Windows\\ehci.dll",      "USB EHCI host(高速)" },
        { L"\\Windows\\hcd.dll",       "USB HCD 通用" },
        { L"\\Windows\\usbmsc.dll",    "USB mass storage" },
        { L"\\Windows\\aygshell.dll",  "shell 集成" },
        { L"\\Windows\\toolhelp.dll",  "进程快照" },
        { L"\\Windows\\nk.bin",        "内核镜像(binfs)" },
    };
    Log("--- 关键文件存在性 ---");
    for (int i = 0; i < 11; i++) {
        DWORD attr = GetFileAttributes(items[i].dll);
        char a[64]; W2A(items[i].dll, a, sizeof(a));
        Log("  %s %-20s : %s", (attr != INVALID_FILE_ATTRIBUTES) ? "OK" : "--",
            a, items[i].why);
    }
    /* nk.bin 大小（存在时）= 内核规模线索 */
    HANDLE h = CreateFile(L"\\Windows\\nk.bin", GENERIC_READ, FILE_SHARE_READ,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        Log("  nk.bin size = %u KB", GetFileSize(h, NULL) / 1024);
        CloseHandle(h);
    }
}

static void DumpProcesses(void) {
    Log("--- 运行进程（找原车 launcher/导航进程名） ---");
    /* CE 的 CreateToolhelp32Snapshot 在 toolhelp 里；若 DLL 缺失则优雅跳过 */
    typedef HANDLE (*PFN_SNAP)(DWORD, DWORD);
    typedef BOOL (*PFN_FIRST)(HANDLE, void *);
    typedef BOOL (*PFN_NEXT)(HANDLE, void *);
    HMODULE hMod = GetModuleHandle(L"toolhelp.dll");
    if (!hMod) hMod = LoadLibrary(L"toolhelp.dll");
    if (!hMod) { Log("  toolhelp 不可用，跳过进程列表"); return; }
    PFN_SNAP  snap  = (PFN_SNAP)GetProcAddress(hMod, L"CreateToolhelp32Snapshot");
    PFN_FIRST first = (PFN_FIRST)GetProcAddress(hMod, L"Process32First");
    PFN_NEXT  next  = (PFN_NEXT)GetProcAddress(hMod, L"Process32Next");
    if (!snap || !first || !next) { Log("  toolhelp API 不全，跳过"); return; }

#pragma pack(push,1)
    struct PROCENTRY {
        DWORD dwSize;
        DWORD cntUsage;
        DWORD th32ProcessID;
        DWORD th32DefaultHeapID;
        DWORD th32ModuleID;
        DWORD cntThreads;
        DWORD th32ParentProcessID;
        LONG  pcPriClassBase;
        DWORD dwFlags;
        WCHAR szExeFile[MAX_PATH];
    } pe;
#pragma pack(pop)

    HANDLE hSnap = snap(0x00000002 /*TH32CS_SNAPPROCESS*/, 0);
    if (hSnap == INVALID_HANDLE_VALUE) { Log("  snapshot 失败"); return; }
    memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
    char a[64];
    if (first(hSnap, &pe)) {
        do {
            W2A(pe.szExeFile, a, sizeof(a));
            Log("  pid=%-8u ppid=%-8u %s", pe.th32ProcessID, pe.th32ParentProcessID, a);
            memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
        } while (next(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

/* ---------------- 入口 ---------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nShow;

    LogInit();
    Log("==== tang-carplay probe v0.1 ====");
    Log("exe: (见日志同目录) tick=%u", GetTickCount());

    DumpVersion();
    DumpCpu();
    DumpMemory();
    DumpScreen();
    DumpOem();
    DumpPlatformRegistry();
    DumpStorage();
    DumpDlls();
    DumpUsb();
    DumpProcesses();

    Log("==== probe done, 共 %u ms ====", GetTickCount() - g_startTick);
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);

    /* 弹窗给结论（ASCII，避免编码坑）；完整数据看 probe_log.txt */
    MessageBox(NULL,
        L"probe done.\r\n"
        L"See probe_log.txt (same folder as exe).\r\n"
        L"Key lines: 'OS VERDICT' / 'CPU VERDICT' / 'USB VERDICT'.",
        L"tang-carplay probe", MB_OK | MB_ICONINFORMATION);
    return 0;
}
