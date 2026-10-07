/* usbtest.c — 唐2015 WinCE USB 栈验证器 v0.2 (零写卡版)
 *
 * v0.1 → v0.2 关键变化（吸取 probe v0.1 写坏 FAT 的教训）：
 *   - 删除所有写到 TF 卡的文件操作（日志改屏幕 ring buffer）
 *   - 保留注册表写入 + \Windows DLL 拷贝 —— 这两项 AutoKit 每次运行都在做，
 *     在此车上多年验证安全，且不触碰 TF 卡 FAT
 *
 * 直连路线的验证闭环（基于修正后的认知）：
 *   iPhone 15 USB-C 插上有数据功能的主机即完成 USB 枚举；
 *   之前 usbwatch 看不到事件是因为 Drivers\Active 只显示"已绑定驱动"，
 *   BYD 原厂没有 Apple VID 的客户端驱动 → 没人认领 → 无事件。
 *   本工具装上 CEUSBKWrapper（LoadClients\1452 = Apple VID 0x05AC）后：
 *   iPhone 再插入就会被 UKW 驱动认领 → Ukw API 可枚举/控制传输/抢占接口
 *
 * 验证项（全部屏幕显示）：
 *   UKW_STACK  : UkwOpenDriver OK = 内核驱动加载成功
 *   DEVICE_ENUM: 枚举到 VID 0x05AC 设备
 *   CTRL_XFER  : 控制传输读 iProduct/iSerial 字符串成功 = 数据双向通
 *   CLAIM      : 抢占 vendor 接口成功 = iAP2 通道可用
 *
 * 运行要求：EXE 同目录放 CEUSBBOX.dll 和 ceusbkwrapper.dll（来自 AutoKit BIN）
 * 流程：启动（自动写注册表）→ 屏幕提示 PLUG IPHONE NOW → 插 iPhone → 看结论
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

/* ---------------- GUI：顶部固定行 + 事件滚动区 ---------------- */
static HWND g_hwnd = NULL;
static volatile BOOL g_running = TRUE;
static volatile BOOL g_done = FALSE;
static CRITICAL_SECTION g_cs;
static char g_lines[4][96];            /* 顶部 4 行固定状态 */

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

/* dump 配置描述符里的接口/端点（只往屏幕事件区输出） */
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
                      buf[off + 2], buf[off + 3], buf[off + 4],
                      buf[off + 5], buf[off + 6], buf[off + 7]);
        } else if (bt == 5 && bl >= 7) {
            const char *dir = (buf[off + 2] & 0x80) ? "IN " : "OUT";
            const char *typ = ((buf[off + 3] & 3) == 2) ? "bulk" :
                              ((buf[off + 3] & 3) == 3) ? "int " :
                              ((buf[off + 3] & 3) == 1) ? "iso " : "?   ";
            PushEvent("    EP %02X(%s) %s mps=%u",
                      buf[off + 2], dir, typ,
                      buf[off + 4] | (buf[off + 5] << 8));
        }
        off += bl;
    }
}

/* 读字符串描述符（证明控制传输双向可用） */
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
    PushEvent("=== usbtest v0.2 zero-write start ===");

    /* A. 部署内核驱动 DLL：拷到 \Windows（RAM 盘，不碰 TF 卡 FAT） */
    SetLine(0, "A: deploy DLL");
    WCHAR src2[MAX_PATH];
    _snwprintf(src2, MAX_PATH - 1, L"%s\\CEUSBBOX.dll", dir);
    BOOL winOk = FALSE;
    if (CopyFile(src2, L"\\Windows\\CEUSBBOX.dll", FALSE)) {
        winOk = TRUE;
        PushEvent("A: copy CEUSBBOX.dll -> \\Windows OK");
    } else {
        PushEvent("A: \\Windows copy fail err=%u (use SD path)", GetLastError());
    }
    const WCHAR *drvPath = winOk ? L"CEUSBBOX.dll" : src2;

    /* B. 注册表：UKW 驱动 + Apple(1452) / MagicComm(1314) 绑定 */
    SetLine(0, "B: registry");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    WriteRegDword(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Flags", 8);
    /* 接口过滤：不抢 U盘/RNDIS/CDC-NCM（保护原车功能） */
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_MASS_STORAGE", L"8:*:*");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_RNDIS_STANDARD", L"2:2:255:!1057");
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper",
        L"InterfaceFilter_CDC_NCM", L"2:13:*");
    /* Apple VID 0x05AC = 1452；Magic Comm 盒子 VID 0x1314 = 4884 */
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\1452\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    WriteRegStr(HKEY_LOCAL_MACHINE,
        L"Drivers\\USB\\LoadClients\\4884\\Default\\Default\\Usb_Kernel_Wrapper",
        L"Dll", drvPath);
    PushEvent("B: registry written (Dll=%s)", winOk ? "WinCopy" : "SDPath");

    /* C. 加载用户库（从卡上只读加载） */
    SetLine(1, "C: load ukw lib");
    WCHAR src1[MAX_PATH];
    _snwprintf(src1, MAX_PATH - 1, L"%s\\ceusbkwrapper.dll", dir);
    HMODULE hLib = LoadLibraryW(src1);
    if (!hLib) hLib = LoadLibraryW(L"ceusbkwrapper.dll");
    if (!hLib) {
        PushEvent("C: LoadLibrary FAILED err=%u", GetLastError());
        SetLine(1, "C: LOAD FAILED");
        goto finish;
    }
    pOpen       = (PFN_Open)GetProcAddress(hLib, L"UkwOpenDriver");
    pGetDevList = (PFN_GetDevList)GetProcAddress(hLib, L"UkwGetDeviceList");
    pRelDevList = (PFN_RelDevList)GetProcAddress(hLib, L"UkwReleaseDeviceList");
    pGetDevDesc = (PFN_GetDevDesc)GetProcAddress(hLib, L"UkwGetDeviceDescriptor");
    pGetCfgDesc = (PFN_GetCfgDesc)GetProcAddress(hLib, L"UkwGetConfigDescriptor");
    pClaim      = (PFN_Claim)GetProcAddress(hLib, L"UkwClaimInterface");
    pReleaseIface = (PFN_Release)GetProcAddress(hLib, L"UkwReleaseInterface");
    pCtrlXfer   = (PFN_CtrlXfer)GetProcAddress(hLib, L"UkwIssueControlTransfer");
    pClose      = (PFN_Close)GetProcAddress(hLib, L"UkwCloseDriver");
    if (!pOpen || !pGetDevList || !pGetDevDesc || !pGetCfgDesc || !pCtrlXfer) {
        PushEvent("C: missing Ukw exports");
        SetLine(1, "C: missing exports");
        goto finish;
    }
    PushEvent("C: Ukw exports resolved");

    /* D. 打开驱动 */
    HANDLE hDrv = pOpen();
    if (!hDrv || hDrv == INVALID_HANDLE_VALUE) {
        PushEvent("UKW_STACK: UkwOpenDriver FAILED err=%u", GetLastError());
        SetLine(1, "D: UKW open FAILED");
        goto finish;
    }
    PushEvent("UKW_STACK: UkwOpenDriver OK");
    SetLine(1, "D: UKW STACK OK");

    /* E. 轮询设备 180s */
    SetLine(2, "E: PLUG IPHONE NOW!");
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
                    SetLine(3, "E: APPLE %04X", d.idProduct);
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
        if (round % 5 == 4) SetLine(2, "E: waiting %ds...", (round + 1) * 2);
    }

    PushEvent("DEVICE_ENUM: %s", sawApple ? "Apple seen" : "no Apple (check cable/port)");
    PushEvent("CTRL_XFER : %s", ctrlOK ? "control transfer OK" : "failed");
    PushEvent("CLAIM     : %s", claimOK ? "iface claim OK" : "failed");
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
    SetLine(0, "usbtest v0.2 (zero-write) starting...");
    SetLine(1, "deploys UKW driver + binds Apple VID");
    SetLine(2, "then: PLUG IPHONE");

    g_hwnd = CreateWindowW(WC_USBTEST, L"usbtest v0.2", WS_VISIBLE | WS_CAPTION,
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
