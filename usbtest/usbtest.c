/* usbtest.c — 唐2015 WinCE USB 栈验证器 v0.3 (零写卡 + ActivateDeviceEx)
 *
 * v0.2 → v0.3 变化（修复 v0.2 实测 err=87 UkwOpenDriver 失败）：
 *   1. CEUSBBOX.dll → \Windows\USBKIT.DLL（新文件名，避免覆盖 AutoKit 自己的 \Windows\CEUSBBOX.dll）
 *   2. 注册表 ClientDrivers\Usb_Kernel_Wrapper.Dll = USBKIT.DLL（bare 名走 \Windows 搜索路径）
 *   3. 加 ActivateDeviceEx() 调用强制 USBD 加载驱动——这是 WinCE 客户端驱动注册的标准两步：
 *      写 ClientDrivers 注册表（声明驱动）→ ActivateDeviceEx 注册（实例化驱动）
 *      没有这一步，UkwOpenDriver 内部 CreateFile("UKW1:") 找不到驱动返回 err=87
 *   4. 错误信息更详细（错误码 + 路径）
 *
 * 验证项（屏幕 ring buffer）：
 *   A: deploy DLL    → push A 事件 + 屏幕行 0
 *   B: registry     → push B 事件 + 屏幕行 0
 *   C: ActivateDev   → push DEVICE EVENT → 如果设备事件触发 → 内核驱动真的活了
 *   D: load ukw lib  → push D
 *   E: UkwOpenDriver → 屏幕行 1 UKW STACK OK/FAIL
 *   F: poll + enum iPhone → 屏幕行 2-3 ENUM/XFER/CLAIM verdicts
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

/* ---------------- UKW 类型和函数指针 ---------------- */
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

/* ---------------- GUI：顶部 4 行 + 滚动事件区 ---------------- */
static HWND g_hwnd = NULL;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;
static char g_lines[4][96];

#define EVT_LINES 9
#define EVT_LEN   110
static char g_events[EVT_LINES][EVT_LEN];
static int  g_evtHead = 0, g_evtN = 0;

static void SetLine(int i, const char *fmt, ...) {
    if (i < 0 || i > 3) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_lines[i], 95, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void PushEvent(const char *fmt, ...) {
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_events[g_evtHead], EVT_LEN - 1, fmt, ap);
    va_end(ap);
    g_evtHead = (g_evtHead + 1) % EVT_LINES;
    if (g_evtN < EVT_LINES) g_evtN++;
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

#define WC_USBTEST L"USBTEST"

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_done) { PostQuitMessage(0); return 0; }
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
        char tmp4[4][96], tmpE[EVT_LINES][EVT_LEN];
        EnterCriticalSection(&g_cs);
        memcpy(tmp4, g_lines, sizeof(tmp4));
        memcpy(tmpE, g_events, sizeof(tmpE));
        int evtN = g_evtN, evtHead = g_evtHead;
        LeaveCriticalSection(&g_cs);
        WCHAR w[120];
        int y = 8;
        for (int i = 0; i < 4; i++) {
            MultiByteToWideChar(CP_ACP, 0, tmp4[i], -1, w, 120);
            ExtTextOutW(hdc, 10, y, 0, NULL, w, wcslen(w), NULL);
            y += 26;
        }
        SetTextColor(hdc, RGB(180, 255, 180));
        for (int i = 0; i < EVT_LINES; i++) {
            int idx = (evtN < EVT_LINES) ? i : ((evtHead + i) % EVT_LINES);
            MultiByteToWideChar(CP_ACP, 0, tmpE[idx], -1, w, 120);
            ExtTextOutW(hdc, 10, y, 0, NULL, w, wcslen(w), NULL);
            y += 22;
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

static BOOL WriteRegStr(HKEY r, const WCHAR *sub, const WCHAR *val, const WCHAR *data) {
    HKEY hk;
    if (RegCreateKeyExW(r, sub, 0, NULL, 0, 0, NULL, &hk, NULL) != ERROR_SUCCESS) return FALSE;
    LONG rc = RegSetValueExW(hk, val, 0, REG_SZ, (const BYTE *)data,
                              (DWORD)(wcslen(data) + 1) * sizeof(WCHAR));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}
static BOOL WriteRegDword(HKEY r, const WCHAR *sub, const WCHAR *val, DWORD data) {
    HKEY hk;
    if (RegCreateKeyExW(r, sub, 0, NULL, 0, 0, NULL, &hk, NULL) != ERROR_SUCCESS) return FALSE;
    LONG rc = RegSetValueExW(hk, val, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static void DumpConfigDescriptor(UKW_DEVICE dev) {
    BYTE buf[1024];
    DWORD len = 0;
    if (!pGetCfgDesc(dev, UKW_ACTIVE_CONFIGURATION, buf, sizeof(buf), &len)) {
        PushEvent("  cfgdesc FAILED err=%u", GetLastError());
        return;
    }
    PushEvent("  cfgdesc %u bytes", len);
    DWORD off = 0;
    while (off + 2 <= len) {
        BYTE bl = buf[off], bt = buf[off + 1];
        if (bl < 2 || off + bl > len) break;
        if (bt == 4 && bl >= 9) {
            PushEvent("  IF#%u alt=%u nEP=%u cls=%02X sub=%02X proto=%02X",
                      buf[off+2], buf[off+3], buf[off+4],
                      buf[off+5], buf[off+6], buf[off+7]);
        } else if (bt == 5 && bl >= 7) {
            const char *dir = (buf[off+2] & 0x80) ? "IN " : "OUT";
            const char *typ = ((buf[off+3] & 3) == 2) ? "bulk" :
                              ((buf[off+3] & 3) == 3) ? "int " :
                              ((buf[off+3] & 3) == 1) ? "iso " : "?   ";
            PushEvent("    EP %02X(%s) %s mps=%u",
                      buf[off+2], dir, typ,
                      buf[off+4] | (buf[off+5] << 8));
        }
        off += bl;
    }
}

static BOOL ReadStringDesc(UKW_DEVICE dev, UINT8 idx) {
    BYTE buf[256];
    UKW_CONTROL_HEADER h;
    h.bmRequestType = 0x80;
    h.bRequest = 0x06;
    h.wValue = (UINT16)((3 << 8) | idx);
    h.wIndex = 0x0409;
    h.wLength = sizeof(buf);
    DWORD xfer = 0;
    memset(buf, 0, sizeof(buf));
    if (!pCtrlXfer(dev, UKW_TF_IN_TRANSFER | UKW_TF_NO_WAIT |
                       UKW_TF_SHORT_TRANSFER_OK | UKW_TF_SEND_TO_DEVICE,
                   &h, buf, sizeof(buf), &xfer, NULL)) {
        PushEvent("  str[%u] FAILED err=%u", idx, GetLastError());
        return FALSE;
    }
    if (xfer < 2 || buf[0] < 2 || buf[1] != 3) {
        PushEvent("  str[%u] bad len=%u", idx, xfer);
        return FALSE;
    }
    char out[128];
    DWORD n = (buf[0] - 2) / 2;
    if (n >= sizeof(out)) n = sizeof(out) - 1;
    for (DWORD i = 0; i < n; i++) {
        unsigned short c = buf[2 + i * 2] | (buf[3 + i * 2] << 8);
        out[i] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = 0;
    PushEvent("  str[%u] \"%s\"", idx, out);
    return TRUE;
}

/* ---------------- 主流程 ---------------- */
static DWORD WINAPI TestThread(LPVOID arg) {
    (void)arg;
    WCHAR dir[MAX_PATH];
    ExeDir(dir, MAX_PATH);
    PushEvent("=== usbtest v0.3 (activate+USBKIT) start ===");

    /* A. 部署 DLL：拷到 \Windows\USBKIT.DLL（不覆盖 AutoKit 原 CEUSBBOX.dll） */
    SetLine(0, "A: deploy USBKIT.DLL");
    WCHAR srcKrnl[MAX_PATH];
    _snwprintf(srcKrnl, MAX_PATH - 1, L"%s\\USBKIT.DLL", dir);
    BOOL winOk = FALSE;
    if (CopyFile(srcKrnl, L"\\Windows\\USBKIT.DLL", FALSE)) {
        winOk = TRUE;
        PushEvent("A: copy USBKIT.DLL -> \\Windows OK");
    } else {
        DWORD e = GetLastError();
        PushEvent("A: \\Windows copy fail err=%u (using SD path)", e);
    }

    /* B. 注册表：UKW 前缀 + USBKIT.DLL bare 名（WinCopy 时走 \Windows 搜索） */
    SetLine(0, "B: registry");
    const WCHAR *drvPath = winOk ? L"USBKIT.DLL" : srcKrnl;
    BOOL r1 = WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    WriteRegDword(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Flags", 8);
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_MASS_STORAGE", L"8:*:*");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_RNDIS_STANDARD", L"2:2:255:!1057");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_CDC_NCM", L"2:13:*");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\1452\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\4884\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    PushEvent("B: registry written (Dll=%s r1=%d)", winOk ? "Good" : "SD", r1);

    /* C. ActivateDeviceEx 强制 USBD 加载驱动（关键步骤，v0.2 缺这个导致 err=87）
     * 注意：如果车机刚重启过（USBD boot-time 已扫 ClientDrivers），驱动已加载，
     * ActivateDeviceEx 会返回 err=2 (ERROR_FILE_NOT_FOUND)，此时跳过即可——驱动已在
     */
    SetLine(0, "C: ActivateDeviceEx");
    {
        HANDLE h = ActivateDeviceEx(L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", NULL, 0, 0);
        DWORD e = GetLastError();
        if (h == NULL || h == INVALID_HANDLE_VALUE) {
            if (e == 2) {
                PushEvent("C: ActivateEx err=2 (驱动可能已激活/重启后无需激活)");
            } else {
                PushEvent("C: ActivateEx FAIL err=%u", e);
            }
        } else {
            PushEvent("h = 0x%08X ok", (DWORD)h);
            CloseHandle(h);
            PushEvent("C: ActivateDeviceEx OK");
        }
    }
    Sleep(500);

    /* D. 加载用户库（卡上只读加载） */
    HMODULE hLib = NULL;
    {
WCHAR src1[MAX_PATH];
    _snwprintf(src1, MAX_PATH - 1, L"%s\\ceusbkwrapper.dll", dir);
        hLib = LoadLibraryW(src1);
        if (!hLib) hLib = LoadLibraryW(L"ceusbkwrapper.dll");
        if (!hLib) {
            PushEvent("D: LoadLibrary FAIL err=%u", GetLastError());
            SetLine(1, "D: LOAD FAILED");
            goto finish;
        }
    }
    pOpen       = (PFN_Open)GetProcAddress(hLib, L"UkwOpenDriver");
    pGetDevList = (PFN_GetDevList)GetProcAddress(hLib, L"UkwGetDeviceList");
    pRelDevList = (PFN_RelDevList)GetProcAddress(hLib, L"UkwReleaseDeviceList");
    pGetDevDesc = (PFN_GetDevDesc)GetProcAddress(hLib, L"UkwGetDeviceDescriptor");
    pGetCfgDesc = (PFN_GetCfgDesc)GetProcAddress(hLib, L"UkwGetConfigDescriptor");
    pClaim      = (PFN_Claim)GetProcAddress(hLib, L"UkwClaimInterface");
    pReleaseIface=(PFN_Release)GetProcAddress(hLib, L"UkwReleaseInterface");
    pCtrlXfer   = (PFN_CtrlXfer)GetProcAddress(hLib, L"UkwIssueControlTransfer");
    pClose      = (PFN_Close)GetProcAddress(hLib, L"UkwCloseDriver");
    PushEvent("D: exports resolved (lib=0x%08X)", (DWORD)hLib);
    if (!pOpen || !pGetDevList || !pGetDevDesc || !pGetCfgDesc || !pCtrlXfer) {
        PushEvent("D: missing exports");
        SetLine(1, "D: missing exports");
        goto finish;
    }

    /* E. UkwOpenDriver（v0.2 失败的步骤，现在 ActivateDeviceEx 之后） */
    HANDLE hDrv = pOpen();
    DWORD oe = GetLastError();
    if (!hDrv || hDrv == INVALID_HANDLE_VALUE) {
        PushEvent("E: UkwOpenDriver FAIL err=%u (hDrv=%d)", oe, (DWORD)(DWORD_PTR)hDrv);
        SetLine(1, "E: UKW OPEN FAIL err=%u", oe);
        goto finish;
    }
    PushEvent("E: UkwOpenDriver OK h=0x%08X", (DWORD)hDrv);
    SetLine(1, "E: UKW STACK OK");

    /* F. 轮询 + 枚举 iPhone */
    SetLine(2, "F: PLUG IPHONE NOW");
    BOOL sawApple = FALSE, ctrlOK = FALSE, claimOK = FALSE;
    UKW_DEVICE seen[16];
    DWORD seenN = 0;
    for (int round = 0; round < 90 && g_running; round++) {
        UKW_DEVICE list[8];
        DWORD actual = 0;
        if (pGetDevList(hDrv, list, 8, &actual) && actual > 0) {
            for (DWORD i = 0; i < actual; i++) {
                DWORD j = 0;
                for (; j < seenN; j++) if (seen[j] == list[i]) break;
                if (j < seenN) continue;
                if (seenN < 16) seen[seenN++] = list[i];

                UKW_DEVICE_DESCRIPTOR d;
                memset(&d, 0, sizeof(d));
                if (!pGetDevDesc(list[i], &d)) {
                    PushEvent("DEV: desc fail err=%u", GetLastError());
                    continue;
                }
                PushEvent("DEV VID=%04X PID=%04X cls=%02X",
                          d.idVendor, d.idProduct, d.bDeviceClass);
                DumpConfigDescriptor(list[i]);

                if (d.idVendor == 0x05AC) {
                    sawApple = TRUE;
                    SetLine(3, "F: APPLE %04X", d.idProduct);
                    if (ReadStringDesc(list[i], d.iProduct)) ctrlOK = TRUE;
                    ReadStringDesc(list[i], d.iSerialNumber);
                    if (pClaim && pReleaseIface) {
                        if (pClaim(list[i], 0)) {
                            claimOK = TRUE;
                            PushEvent("CLAIM: IF0 claimed OK");
                            pReleaseIface(list[i], 0);
                        } else {
                            PushEvent("CLAIM: IF0 fail err=%u", GetLastError());
                        }
                    }
                }
            }
            pRelDevList(hDrv, list, 8);
        }
        if (sawApple && (ctrlOK || round > 20)) break;
        Sleep(2000);
        if (round % 5 == 4) SetLine(2, "F: waiting %ds...", (round + 1) * 2);
    }

    PushEvent("DEVICE_ENUM: %s", sawApple ? "Apple seen" : "no Apple");
    PushEvent("CTRL_XFER : %s", ctrlOK ? "OK" : "failed");
    PushEvent("CLAIM     : %s", claimOK ? "OK" : "failed");
    SetLine(2, "ENUM:%s XFER:%s", sawApple ? "YES" : "NO", ctrlOK ? "YES" : "NO");
    SetLine(3, "CLAIM:%s  tap=exit", claimOK ? "YES" : "NO");
    pClose(hDrv);

finish:
    PushEvent("=== done, tap to exit ===");
    SetLine(0, "DONE - tap to exit");
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
    SetLine(0, "usbtest v0.3 (activate) starting...");
    SetLine(1, "deploys UKW + binds Apple VID");
    SetLine(2, "then: PLUG IPHONE");

    g_hwnd = CreateWindowW(WC_USBTEST, L"usbtest v0.3", WS_VISIBLE | WS_CAPTION,
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
    return 0;
}