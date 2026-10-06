/* usbtest.c — 唐2015 WinCE USB 栈验证器 v0.1 (Phase 1 实弹测试)
 *
 * 思路大改：不再从零写 USB 客户端驱动，而是复用 AutoKit 发行包里
 * 已在唐上跑通的 CEUSBKWrapper（VNC Automotive, LGPL）：
 *   CEUSBBOX.dll   = ceusbkwrapperdrv.dll（USBD 内核驱动，你车上验证过）
 *   ceusbkwrapper.dll = 用户态库（Ukw* API）
 *
 * 本程序做 Phase 1 的完整闭环验证：
 *   A. 把两个 DLL 部署到 \Windows（失败则退化为 SD 卡全路径）
 *   B. 写注册表：ClientDrivers\Usb_Kernel_Wrapper + LoadClients\1452(Apple)/1314(盒子)
 *   C. UkwOpenDriver 打开驱动（证明内核驱动可加载）
 *   D. 提示插 iPhone，轮询 UkwGetDeviceList 180s：
 *      - 新设备 → dump 设备描述符 + 配置描述符（解析接口/端点）
 *      - Apple(0x05AC) → UkwIssueControlTransfer 读 iProduct/iSerial 字符串
 *                        （证明数据传输真正可用，不只是枚举）
 *                      → UkwClaimInterface 抢占 vendor 接口（iAP2 用）
 *   E. 结论行：UKW_STACK / DEVICE_ENUM / CTRL_XFER / CLAIM
 *
 * 运行要求：EXE 同目录放 ceusbkwrapper.dll 和 CEUSBBOX.dll（来自 AutoKit BIN）
 * 流程：先启动本程序（写注册表）→ 屏幕提示后再插 iPhone
 */

#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ---------------- 日志 ---------------- */
static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static DWORD  g_startTick = 0;
static void LogInit(void) {
    WCHAR path[MAX_PATH] = {0};
    g_startTick = GetTickCount();
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR *dot = wcsrchr(path, L'.');
        if (dot) *dot = 0;
        wcscat(path, L"_log.txt");
    } else wcscpy(path, L"usbtest_log.txt");
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
    int m = _snprintf(line, sizeof(line) - 1, "[t+%u] %s\r\n",
                      GetTickCount() - g_startTick, msg);
    if (m < 0) m = 0;
    DWORD wr; WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    FlushFileBuffers(g_hLog);
}

/* ---------------- UKW 类型和函数指针（动态加载，无头文件依赖） ---------------- */
typedef void *UKW_DEVICE;
typedef struct {
    UINT8  bLength, bDescriptorType;
    UINT16 bcdUSB;
    UINT8  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    UINT16 idVendor, idProduct, bcdDevice;
    UINT8  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} UKW_DEVICE_DESCRIPTOR;
typedef struct {
    UINT8  bmRequestType, bRequest;
    UINT16 wValue, wIndex, wLength;
} UKW_CONTROL_HEADER;

#define UKW_TF_IN_TRANSFER       0x00000001
#define UKW_TF_NO_WAIT           0x00000100
#define UKW_TF_SHORT_TRANSFER_OK 0x00000200
#define UKW_TF_SEND_TO_DEVICE    0x00010000
#define UKW_ACTIVE_CONFIGURATION ((DWORD)-1)

typedef HANDLE (*PFN_Open)(void);
typedef BOOL  (*PFN_GetDevList)(HANDLE, UKW_DEVICE*, DWORD, LPDWORD);
typedef void  (*PFN_RelDevList)(HANDLE, UKW_DEVICE*, DWORD);
typedef BOOL  (*PFN_GetDevDesc)(UKW_DEVICE, UKW_DEVICE_DESCRIPTOR*);
typedef BOOL  (*PFN_GetCfgDesc)(UKW_DEVICE, DWORD, LPVOID, DWORD, LPDWORD);
typedef BOOL  (*PFN_Claim)(UKW_DEVICE, DWORD);
typedef BOOL  (*PFN_Release)(UKW_DEVICE, DWORD);
typedef BOOL  (*PFN_CtrlXfer)(UKW_DEVICE, DWORD, UKW_CONTROL_HEADER*, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef void  (*PFN_Close)(HANDLE);

static PFN_Open        pOpen;
static PFN_GetDevList  pGetDevList;
static PFN_RelDevList  pRelDevList;
static PFN_GetDevDesc  pGetDevDesc;
static PFN_GetCfgDesc  pGetCfgDesc;
static PFN_Claim       pClaim;
static PFN_Release     pReleaseIface;
static PFN_CtrlXfer    pCtrlXfer;
static PFN_Close       pClose;

/* ---------------- GUI ---------------- */
static HWND g_hwnd = NULL;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;
static char g_lines[7][96];

static void SetLine(int i, const char *fmt, ...) {
    if (i < 0 || i > 6) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_lines[i], 95, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

#define WC_USBTEST L"USBTEST"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_done) { PostQuitMessage(0); return 0; }
        Log("USER tap (ignore; tap again after DONE to exit)");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0, 30, 0));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(120, 255, 120));
        char tmp[7][96];
        EnterCriticalSection(&g_cs);
        memcpy(tmp, g_lines, sizeof(tmp));
        LeaveCriticalSection(&g_cs);
        for (int i = 0; i < 7; i++) {
            WCHAR w[96];
            MultiByteToWideChar(CP_ACP, 0, tmp[i], -1, w, 96);
            ExtTextOutW(hdc, 10, 10 + i * 26, 0, NULL, w, wcslen(w), NULL);
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

/* ---------------- 工具 ---------------- */
static void ExeDir(WCHAR *dir, DWORD cap) {
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
        wcsncpy(dir, path, cap - 1);
        dir[cap - 1] = 0;
    } else wcsncpy(dir, L".", cap - 1);
}

static BOOL WriteRegStr(HKEY root, const WCHAR *sub, const WCHAR *val, const WCHAR *data) {
    HKEY hk;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, 0, NULL, &hk, NULL) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegSetValueExW(hk, val, 0, REG_SZ, (const BYTE *)data,
                            (DWORD)(wcslen(data) + 1) * sizeof(WCHAR));
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}
static BOOL WriteRegDword(HKEY root, const WCHAR *sub, const WCHAR *val, DWORD data) {
    HKEY hk;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, 0, NULL, &hk, NULL) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegSetValueExW(hk, val, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

/* dump 配置描述符里的接口/端点 */
static void DumpConfigDescriptor(UKW_DEVICE dev) {
    BYTE buf[1024];
    DWORD len = 0;
    if (!pGetCfgDesc(dev, UKW_ACTIVE_CONFIGURATION, buf, sizeof(buf), &len)) {
        Log("  cfgdesc: FAILED err=%u", GetLastError());
        return;
    }
    Log("  cfgdesc: %u bytes", len);
    /* 原始 hex 前 64 字节 */
    char hex[200]; int o = 0;
    for (DWORD i = 0; i < len && i < 64; i++)
        o += _snprintf(hex + o, sizeof(hex) - o - 1, "%02X ", buf[i]);
    Log("  hex64: %s", hex);
    /* 遍历描述符链 */
    DWORD off = 0;
    while (off + 2 <= len) {
        BYTE bl = buf[off], bt = buf[off + 1];
        if (bl < 2 || off + bl > len) break;
        if (bt == 4 && bl >= 9) {   /* interface */
            Log("  IF#%u alt=%u nEP=%u class=0x%02X sub=0x%02X proto=0x%02X",
                buf[off + 2], buf[off + 3], buf[off + 4],
                buf[off + 5], buf[off + 6], buf[off + 7]);
        } else if (bt == 5 && bl >= 7) {   /* endpoint */
            Log("    EP addr=0x%02X(%s) attr=0x%02X(%s) mps=%u",
                buf[off + 2],
                (buf[off + 2] & 0x80) ? "IN" : "OUT",
                buf[off + 3],
                (buf[off + 3] & 3) == 2 ? "bulk" :
                (buf[off + 3] & 3) == 3 ? "int" :
                (buf[off + 3] & 3) == 1 ? "iso" : "?",
                buf[off + 4] | (buf[off + 5] << 8));
        }
        off += bl;
    }
}

/* 读字符串描述符（证明控制传输双向可用） */
static BOOL ReadStringDesc(UKW_DEVICE dev, UINT8 idx, char *out, DWORD outCap) {
    BYTE buf[256];
    UKW_CONTROL_HEADER h;
    h.bmRequestType = 0x80;      /* Device-to-host, standard, device */
    h.bRequest = 0x06;           /* GET_DESCRIPTOR */
    h.wValue = (UINT16)((3 << 8) | idx);  /* String */
    h.wIndex = 0x0409;           /* English (US) */
    h.wLength = sizeof(buf);
    DWORD xfer = 0;
    memset(buf, 0, sizeof(buf));
    if (!pCtrlXfer(dev, UKW_TF_IN_TRANSFER | UKW_TF_NO_WAIT |
                       UKW_TF_SHORT_TRANSFER_OK | UKW_TF_SEND_TO_DEVICE,
                   &h, buf, sizeof(buf), &xfer, NULL)) {
        Log("  strdesc[%u]: FAILED err=%u", idx, GetLastError());
        return FALSE;
    }
    if (xfer < 2 || buf[0] < 2 || buf[1] != 3) {
        Log("  strdesc[%u]: bad reply len=%u", idx, xfer);
        return FALSE;
    }
    /* UTF-16LE → 窄字符（非 ASCII 降 '?'） */
    DWORD n = (buf[0] - 2) / 2;
    if (n >= outCap) n = outCap - 1;
    for (DWORD i = 0; i < n; i++) {
        unsigned short c = buf[2 + i * 2] | (buf[3 + i * 2] << 8);
        out[i] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = 0;
    Log("  strdesc[%u]: \"%s\" (%u bytes)", idx, out, xfer);
    return TRUE;
}

/* ---------------- 主流程 ---------------- */
static DWORD WINAPI TestThread(LPVOID arg) {
    (void)arg;
    WCHAR dir[MAX_PATH];
    ExeDir(dir, MAX_PATH);
    Log("==== usbtest v0.1 start ====");
    {
        char a[MAX_PATH]; DWORD wa = 0;
        WideCharToMultiByte(CP_ACP, 0, dir, -1, a, sizeof(a), NULL, &wa);
        Log("exedir: %s", a);
    }

    /* A. 部署 DLL */
    SetLine(0, "A: deploy DLLs");
    WCHAR src1[MAX_PATH], src2[MAX_PATH], dst1[MAX_PATH];
    _snwprintf(src1, MAX_PATH - 1, L"%s\\ceusbkwrapper.dll", dir);
    _snwprintf(src2, MAX_PATH - 1, L"%s\\CEUSBBOX.dll", dir);
    BOOL winOk = FALSE;
    if (CopyFile(src2, L"\\Windows\\CEUSBBOX.dll", FALSE)) {
        winOk = TRUE;
        Log("A: copied CEUSBBOX.dll -> \\Windows OK");
    } else {
        Log("A: copy to \\Windows failed err=%u (readonly FS? use SD path)", GetLastError());
    }
    /* 用户库直接从 EXE 目录 LoadLibrary（全路径） */

    /* B. 注册表 */
    SetLine(0, "B: registry");
    const WCHAR *drvPath = winOk ? L"CEUSBBOX.dll" : src2;
    BOOL r1 = WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    WriteRegDword(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Flags", 8);
    /* 接口过滤：别抢 mass storage / RNDIS / CDC-NCM 等系统已有驱动 */
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_MASS_STORAGE", L"8:*:*");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_RNDIS_STANDARD", L"2:2:255:!1057");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_CDC_NCM", L"2:13:*");
    /* LoadClients：Apple 0x05AC=1452，盒子 Magic Comm 0x0522=1314 */
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\1452\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\1314\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    Log("B: registry written (Prefix=%s r1=%d)",
        winOk ? "WinCopy" : "SDPath", r1);
    SetLine(0, "B: registry done(%s)", winOk ? "Win" : "SD");

    /* C. 加载用户库 */
    SetLine(1, "C: load ceusbkwrapper.dll");
    HMODULE hLib = LoadLibraryW(src1);
    if (!hLib) hLib = LoadLibraryW(L"ceusbkwrapper.dll");
    if (!hLib) {
        Log("C VERDICT: LoadLibrary ceusbkwrapper.dll FAILED err=%u", GetLastError());
        SetLine(1, "C: LOAD FAILED");
        goto finish;
    }
    pOpen      = (PFN_Open)GetProcAddress(hLib, L"UkwOpenDriver");
    pGetDevList= (PFN_GetDevList)GetProcAddress(hLib, L"UkwGetDeviceList");
    pRelDevList=(PFN_RelDevList)GetProcAddress(hLib, L"UkwReleaseDeviceList");
    pGetDevDesc= (PFN_GetDevDesc)GetProcAddress(hLib, L"UkwGetDeviceDescriptor");
    pGetCfgDesc= (PFN_GetCfgDesc)GetProcAddress(hLib, L"UkwGetConfigDescriptor");
    pClaim     = (PFN_Claim)GetProcAddress(hLib, L"UkwClaimInterface");
    pReleaseIface=(PFN_Release)GetProcAddress(hLib, L"UkwReleaseInterface");
    pCtrlXfer  = (PFN_CtrlXfer)GetProcAddress(hLib, L"UkwIssueControlTransfer");
    pClose     = (PFN_Close)GetProcAddress(hLib, L"UkwCloseDriver");
    if (!pOpen || !pGetDevList || !pGetDevDesc || !pGetCfgDesc || !pCtrlXfer) {
        Log("C VERDICT: missing Ukw exports (open=%p list=%p desc=%p cfg=%p ctrl=%p)",
            pOpen, pGetDevList, pGetDevDesc, pGetCfgDesc, pCtrlXfer);
        SetLine(1, "C: missing exports");
        goto finish;
    }
    Log("C: all Ukw exports resolved");

    /* D. 打开驱动 */
    HANDLE hDrv = pOpen();
    if (!hDrv || hDrv == INVALID_HANDLE_VALUE) {
        Log("UKW_STACK VERDICT: UkwOpenDriver FAILED err=%u", GetLastError());
        SetLine(1, "D: UKW open FAILED");
        goto finish;
    }
    Log("UKW_STACK VERDICT: UkwOpenDriver OK -> 内核驱动可用");
    SetLine(1, "D: UKW STACK OK");

    /* E. 轮询设备 180s */
    SetLine(2, "E: PLUG IPHONE NOW!");
    Log("E: waiting for devices (plug iPhone now), poll 2s x 90");
    BOOL sawApple = FALSE, ctrlOK = FALSE, claimOK = FALSE;
    UKW_DEVICE seen[16];
    DWORD seenN = 0;
    for (int round = 0; round < 90 && g_running; round++) {
        UKW_DEVICE list[8];
        DWORD actual = 0;
        if (pGetDevList(hDrv, list, 8, &actual) && actual > 0) {
            for (DWORD i = 0; i < actual; i++) {
                /* 新设备判定：指针去重 */
                DWORD j = 0;
                for (; j < seenN; j++) if (seen[j] == list[i]) break;
                if (j < seenN) continue;
                if (seenN < 16) seen[seenN++] = list[i];

                UKW_DEVICE_DESCRIPTOR d;
                memset(&d, 0, sizeof(d));
                if (!pGetDevDesc(list[i], &d)) {
                    Log("DEV: (desc failed err=%u)", GetLastError());
                    continue;
                }
                Log("DEV FOUND: VID=0x%04X PID=0x%04X class=0x%02X usb=%X.%02X",
                    d.idVendor, d.idProduct, d.bDeviceClass,
                    d.bcdUSB >> 8, d.bcdUSB & 0xFF);
                DumpConfigDescriptor(list[i]);

                if (d.idVendor == 0x05AC) {
                    sawApple = TRUE;
                    SetLine(3, "E: APPLE 0x%04X", d.idProduct);
                    char s[128];
                    if (ReadStringDesc(list[i], d.iProduct, s, sizeof(s)))
                        ctrlOK = TRUE;
                    ReadStringDesc(list[i], d.iSerialNumber, s, sizeof(s));
                    /* 抢第一个 vendor-specific 接口（配置 dump 里 class=0xFF 的 IF#） */
                    if (pClaim && pReleaseIface) {
                        if (pClaim(list[i], 0)) {
                            claimOK = TRUE;
                            Log("CLAIM VERDICT: interface 0 claimed OK");
                            pReleaseIface(list[i], 0);
                        } else {
                            Log("CLAIM VERDICT: claim IF0 failed err=%u (可能被其他驱动占用)",
                                GetLastError());
                        }
                    }
                }
            }
            pRelDevList(hDrv, list, 8);
        }
        if (sawApple) {
            /* 苹果设备出现即可提前收工（字符串读完） */
            if (ctrlOK || round > 20) break;
        }
        Sleep(2000);
        if (round % 5 == 4) SetLine(2, "E: waiting... %ds", (round + 1) * 2);
    }

    Log("DEVICE_ENUM VERDICT: %s", sawApple ? "Apple 设备已枚举" : "未见 Apple 设备(检查线/口/时机)");
    Log("CTRL_XFER VERDICT: %s", ctrlOK ? "控制传输成功(读字符串OK)" : "控制传输未成功");
    Log("CLAIM VERDICT: %s", claimOK ? "接口抢占成功" : "接口未抢占");
    SetLine(4, "ENUM:%s", sawApple ? "YES" : "NO");
    SetLine(5, "XFER:%s CLAIM:%s", ctrlOK ? "YES" : "NO", claimOK ? "YES" : "NO");
    pClose(hDrv);

finish:
    Log("==== usbtest done %u ms, tap to exit ====", GetTickCount() - g_startTick);
    SetLine(6, "DONE - tap to exit");
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    g_hLog = INVALID_HANDLE_VALUE;
    g_done = TRUE;
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WC_USBTEST;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    LogInit();
    strcpy(g_lines[0], "usbtest starting...");

    g_hwnd = CreateWindowW(WC_USBTEST, L"usbtest", WS_VISIBLE | WS_CAPTION,
                           0, 0, sw, sh, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 256 * 1024, TestThread, NULL,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_running = FALSE;
    if (th) { WaitForSingleObject(th, 45000); CloseHandle(th); }
    if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    return 0;
}
