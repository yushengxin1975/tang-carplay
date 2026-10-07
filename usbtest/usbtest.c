/* usbtest.c — 唐2015 WinCE USB 栈验证器 v0.6 (BuiltIn 注册路径)
 *
 * v0.5 → v0.6 关键变化（修复 v0.3/v0.4/v0.5 E 步骤 err=87）：
 *   - 把 USB 客户端驱动注册路径从 Drivers\USB\ClientDrivers 改为 Drivers\BuiltIn
 *     CE 启动时 Device Manager 强制扫描 BuiltIn 路径加载所有 Active 设备
 *     （USB client drivers 通过 BuiltIn 加载后 USBD 一定能找到 `UKW1:`）
 *   - 同时保留 Drivers\USB\ClientDrivers 路径（万一某种 BuiltIn 不被 USB 注册）
 *   - 注册表 BuiltIn 用最佳实践：Flags=16 (DMFLAGS_IODEVICE|DEVFLAGS_LOAD_ASYNCHRONOUS)
 *   - DLL 路径：用 `\\Windows\\USBKIT.DLL`（persistent）
 *   - ActivateDeviceEx 失败 err=2 时不再当致命错——很可能驱动已经由 BuiltIn 加载
 *
 * 验证流程（v0.6）：
 *   A: copy USBKIT.DLL to \Windows
 *   B: write registry — Dual-path (Drivers\USB\ClientDrivers\ + Drivers\BuiltIn\)
 *   C: ActivateDeviceEx (USB client drivers path - expected err=2 if BuiltIn loaded it)
 *   D: load ukw lib from SD card
 *   E: UkwOpenDriver (should succeed now - either via ActivateEx or BuiltIn)
 *   F: poll + enum iPhone
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

/* ---------------- GUI ---------------- */
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

static BOOL WriteRegStrFull(HKEY r, const WCHAR *sub, const WCHAR *val, const WCHAR *data) {
    HKEY hk;
    DWORD disp;
    LONG rc = RegCreateKeyExW(r, sub, 0, NULL, REG_OPTION_NON_VOLATILE, 0, NULL, &hk, &disp);
    if (rc != ERROR_SUCCESS) return FALSE;
    rc = RegSetValueExW(hk, val, 0, REG_SZ, (const BYTE *)data,
                        (DWORD)(wcslen(data) + 1) * sizeof(WCHAR));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}
static BOOL WriteRegDwordFull(HKEY r, const WCHAR *sub, const WCHAR *val, DWORD data) {
    HKEY hk;
    DWORD disp;
    LONG rc = RegCreateKeyExW(r, sub, 0, NULL, REG_OPTION_NON_VOLATILE, 0, NULL, &hk, &disp);
    if (rc != ERROR_SUCCESS) return FALSE;
    rc = RegSetValueExW(hk, val, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
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
    PushEvent("=== usbtest v0.6 (BuiltIn path) start ===");

    /* A. 部署 DLL：拷到 \Windows\USBKIT.DLL（persistent 路径） */
    SetLine(0, "A: deploy USBKIT.DLL");
    WCHAR srcKrnl[MAX_PATH];
    _snwprintf(srcKrnl, MAX_PATH - 1, L"%s\\USBKIT.DLL", dir);
    BOOL winOk = FALSE;
    if (CopyFile(srcKrnl, L"\\Windows\\USBKIT.DLL", FALSE)) {
        winOk = TRUE;
        PushEvent("A: copy USBKIT.DLL -> \\Windows OK");
    } else {
        DWORD e = GetLastError();
        PushEvent("A: \\Windows copy FAIL err=%u (using SD path)", e);
    }
    const WCHAR *drvPath = winOk ? L"USBKIT.DLL" : srcKrnl;

    /* B. 注册表：双路径同时写——BuiltIn（强制启动加载） + ClientDrivers（USBD 绑定） */
    SetLine(0, "B: registry dual-path");
    /* Path 1: Drivers\USB\ClientDrivers\Usb_Kernel_Wrapper (USBD 客户端驱动注册) */
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    WriteRegDwordFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Flags", 8);
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_MASS_STORAGE", L"8:*:*");
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_RNDIS_STANDARD", L"2:2:255:!1057");
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_CDC_NCM", L"2:13:*");
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\1452\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\4884\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    /* Path 2: Drivers\BuiltIn\Usb_Kernel_Wrapper (CE 启动时 DM 强制扫描——保证激活) */
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    WriteRegDwordFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Flags", 16); /* DEVFLAGS_LOAD_ASYNCHRONOUS */
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Index", L"1");
    WriteRegStrFull(HKEY_LOCAL_MACHINE,
        L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Order", L"0");
    PushEvent("B: dual-path written (USB-CL + BuiltIn)");

    /* B.5 verify BuiltIn entry */
    {
        HKEY hkV;
        WCHAR LWIN[64]; DWORD sz = sizeof(LWIN); DWORD type;
        LONG rcs = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", 0, NULL,
            REG_OPTION_NON_VOLATILE, 0, NULL, &hkV, NULL);
        if (rcs == ERROR_SUCCESS) {
            sz = sizeof(LWIN);
            LONG rqv = RegQueryValueExW(hkV, L"Dll", NULL, &type, (LPBYTE)LWIN, &sz);
            if (rqv == ERROR_SUCCESS) {
                char a[80]; W2A_local(LWIN, a, sizeof(a));
                PushEvent("B5 verify BuiltIn: Dll=%s (len=%u)", a, sz);
            } else {
                PushEvent("B5 verify: FAIL err=%u", rqv);
            }
            RegCloseKey(hkV);
        } else {
            PushEvent("B5 verify: open FAIL err=%u", rcs);
        }
    }

    /* C. ActivateDeviceEx 两条路径都试 */
    SetLine(0, "C: ActivateDeviceEx");
    {
        HANDLE h1 = ActivateDeviceEx(L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", NULL, 0, 0);
        DWORD e1 = GetLastError();
        if (h1 == NULL || h1 == INVALID_HANDLE_VALUE) {
            if (e1 == 2)
                PushEvent("C1: USB-CL ActivateEx err=2 (OK if BuiltIn loaded)");
            else
                PushEvent("C1: USB-CL ActivateEx FAIL err=%u", e1);
        } else {
            CloseHandle(h1);
            PushEvent("C1: USB-CL ActivateEx OK");
        }
        HANDLE h2 = ActivateDeviceEx(L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", NULL, 0, 0);
        DWORD e2 = GetLastError();
        if (h2 == NULL || h2 == INVALID_HANDLE_VALUE) {
            PushEvent("C2: BuiltIn ActivateEx err=%u (built-in usually loaded at boot)", e2);
        } else {
            CloseHandle(h2);
            PushEvent("C2: BuiltIn ActivateEx OK (driver activated now)");
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

    /* E. UkwOpenDriver */
    HANDLE hDrv = pOpen();
    DWORD oe = GetLastError();
    if (!hDrv || hDrv == INVALID_HANDLE_VALUE) {
        PushEvent("E: UkwOpenDriver FAIL err=%u (hDrv=%d)", oe, (DWORD)(DWORD_PTR)hDrv);
        SetLine(1, "E: UKW OPEN FAIL err=%u", oe);
        goto finish;
    }
    PushEvent("E: UkwOpenDriver OK h=0x%08X", (DWORD)hDrv);
    SetLine(1, "E: UKW STACK OK");

    /* F. 轮询 */
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
    SetLine(0, "usbtest v0.6 (BuiltIn) starting...");
    SetLine(1, "writes both USB-CL and BuiltIn");
    SetLine(2, "then: PLUG IPHONE");

    g_hwnd = CreateWindowW(WC_USBTEST, L"usbtest v0.6", WS_VISIBLE | WS_CAPTION,
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