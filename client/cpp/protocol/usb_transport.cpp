// usb_transport.cpp — platform implementations.
//
// CE: CEUSBKWrapper (Ukw* resolved at runtime, pattern proven by usbtest v0.6)
// PC: libusb-1.0 loaded dynamically (dlopen/LoadLibrary) — zero build-time
//     dependency, so CI compiles it everywhere and users run it on any
//     machine that has libusb-1.0 installed (Linux: libusb-1.0.so.0,
//     Windows: libusb-1.0.dll from libusb.info releases).

#include "usb_transport.h"

#include <cstdio>
#include <cstring>

#if defined(__MINGW32CE__) || defined(_WIN32_WCE) || defined(CARLINKIT_SIM_CE_TRANSPORT)
// ==================== CE implementation ====================
// CARLINKIT_SIM_CE_TRANSPORT: compile this branch for desktop MinGW so the
// REAL production transport code (libBoxUSB load + trailing-byte strip fix
// + reader loop) can be exercised on a PC against libBoxUSB_sim.dll /
// box_sim.exe. The only CE-only APIs in this branch are ActivateDevice /
// ActivateDeviceEx (driver pre-activation), guarded out below.

#include <windows.h>

#if defined(CARLINKIT_SIM_CE_TRANSPORT) && !defined(__MINGW32CE__) && !defined(_WIN32_WCE)
// Desktop compile of the CE branch (simulator rig): desktop Windows has no
// GetProcAddressW (CE-only export name) — map it to GetProcAddress with a
// narrow-string conversion. Real WinCE builds are unaffected.
#include <cstdint>
static FARPROC GetProcAddressW(HMODULE mod, LPCWSTR name) {
    char narrow[128];
    _snprintf(narrow, sizeof(narrow), "%ls", name);
    return GetProcAddress(mod, narrow);
}
#endif

extern void PushEventW(const WCHAR *fmt, ...);

namespace carlinkit {

struct UsbTransport_CE::Impl {
    // 1. 官方 libBoxUSB 驱动后端（生产环境首选）
    HMODULE box_lib = nullptr;
    int  (*host_init)(void) = nullptr;
    int  (*host_send)(void *, int, int) = nullptr;
    int  (*host_recv)(void *, int, int) = nullptr;
    void (*host_close)(void) = nullptr;
    bool use_box_lib = false;

    // 2. 原生 ceusbkwrapper 驱动后端（备用）
    HMODULE lib = nullptr;
    HANDLE drv = INVALID_HANDLE_VALUE;
    HANDLE reader_thread = nullptr;
    volatile LONG reader_running = 0;
    RecvCallback recv;
    // Read buffer size. Must be >= 64KB: the box's initial IDR keyframe
    // (20-50KB at 1024x600@30) arrives as one USB burst. A 16KB buffer
    // truncated it, desyncing the H.264 stream — the parser then dropped
    // every subsequent message (magic check failed) and CarPlay never
    // launched. libBoxUSB's host_recv has no length limit (only host_send
    // caps at 0x4000), so a 64KB read buffer is safe.
    std::size_t read_buf_size = 64 * 1024;

    HANDLE (*Open)(void) = nullptr;
    BOOL  (*GetDevList)(HANDLE, void **, DWORD, LPDWORD) = nullptr;
    void  (*RelDevList)(HANDLE, void **, DWORD) = nullptr;
    BOOL  (*GetDevDesc)(void *, void *) = nullptr;
    BOOL  (*GetCfgDesc)(void *, DWORD, void *, DWORD, LPDWORD) = nullptr;
    BOOL  (*Claim)(void *, DWORD) = nullptr;
    BOOL  (*Release)(void *, DWORD) = nullptr;
    BOOL  (*BulkXfer)(void *, DWORD, UCHAR, void *, DWORD, LPDWORD, void *) = nullptr;
    BOOL  (*ClearHaltHost)(void *, UCHAR) = nullptr;
    BOOL  (*ClearHaltDev)(void *, UCHAR) = nullptr;
    void  (*Close)(HANDLE) = nullptr;

    void *dev = nullptr;
    bool claimed = false;          // interface 0 claimed (idempotent claim)
};

UsbTransport_CE::UsbTransport_CE() : impl_(new Impl) {}
UsbTransport_CE::~UsbTransport_CE() { close(); delete impl_; }

static bool EnsureDriverInstalled(const WCHAR *appDir) {
    WCHAR srcBox[MAX_PATH] = {0}, srcKit[MAX_PATH] = {0};
    _snwprintf(srcBox, MAX_PATH - 1, L"%s\\CEUSBBOX.dll", appDir);
    _snwprintf(srcKit, MAX_PATH - 1, L"%s\\USBKIT.DLL", appDir);

    // Copy to \Windows under all possible aliases
    BOOL c1 = CopyFileW(srcBox, L"\\Windows\\ceusbbox.dll", FALSE);
    CopyFileW(srcBox, L"\\Windows\\CEUSBBOX.DLL", FALSE);
    CopyFileW(srcKit, L"\\Windows\\USBKIT.DLL", FALSE);
    CopyFileW(srcBox, L"\\Windows\\ceusbkwrapperdrv.dll", FALSE);

    const WCHAR *drvPath = c1 ? L"ceusbbox.dll" : srcBox;
    PushEventW(L"驱动复制到 \\Windows: %s", c1 ? L"成功" : L"卡路径备用");

    auto regStr = [](HKEY root, const WCHAR *sub, const WCHAR *val, const WCHAR *data) {
        HKEY hk; DWORD disp;
        if (RegCreateKeyExW(root, sub, 0, NULL, REG_OPTION_NON_VOLATILE, 0, NULL, &hk, &disp) == ERROR_SUCCESS) {
            RegSetValueExW(hk, val, 0, REG_SZ, (const BYTE*)data, (DWORD)(wcslen(data) + 1) * sizeof(WCHAR));
            RegCloseKey(hk);
        }
    };
    auto regDw = [](HKEY root, const WCHAR *sub, const WCHAR *val, DWORD data) {
        HKEY hk; DWORD disp;
        if (RegCreateKeyExW(root, sub, 0, NULL, REG_OPTION_NON_VOLATILE, 0, NULL, &hk, &disp) == ERROR_SUCCESS) {
            RegSetValueExW(hk, val, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
            RegCloseKey(hk);
        }
    };

    // 1. Usb_Box (Real key expected by ceusbkwrapper.dll)
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"Prefix", L"CBX");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"Dll", drvPath);
    regDw(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"Flags", 8);
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"InterfaceFilter_MASS_STORAGE", L"8:*:*");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"InterfaceFilter_RNDIS_STANDARD", L"2:2:255:!1057");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Box", L"InterfaceFilter_CDC_NCM", L"2:13:*");

    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\LoadClients\\4884_5408\\Default\\Default\\Usb_Box", L"Dll", drvPath);
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\LoadClients\\4884\\Default\\Default\\Usb_Box", L"Dll", drvPath);

    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Box", L"Prefix", L"CBX");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Box", L"Dll", drvPath);
    regDw(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Box", L"Flags", 16);
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Box", L"Index", L"0");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Box", L"Order", L"0");

    // 2. Usb_Kernel_Wrapper (Fallback)
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    regDw(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper", L"Flags", 8);

    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Prefix", L"UKW");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Dll", drvPath);
    regDw(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Flags", 16);
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Index", L"0");
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\Usb_Kernel_Wrapper", L"Order", L"0");

    // 3. Pre-activate driver (CE-only APIs; skipped in the desktop sim build)
#ifndef CARLINKIT_SIM_CE_TRANSPORT
    ActivateDevice(L"Drivers\\BuiltIn\\Usb_Box", 0);
    ActivateDevice(L"Drivers\\USB\\ClientDrivers\\Usb_Box", 0);
    ActivateDeviceEx(L"Drivers\\BuiltIn\\Usb_Box", NULL, 0, 0);
#endif

    // 4. CoreChip QF9700 USB 2.0 Fast Ethernet Driver (VID 0x0FE6 = 4070, PID 0x9700 = 38656)
    WCHAR srcQf[MAX_PATH] = {0};
    _snwprintf(srcQf, MAX_PATH - 1, L"%s\\QF9700USB.dll", appDir);
    BOOL cQf = CopyFileW(srcQf, L"\\Windows\\qf9700usb.dll", FALSE);
    CopyFileW(srcQf, L"\\Windows\\QF9700USB.dll", FALSE);
    const WCHAR *qfDrvPath = cQf ? L"qf9700usb.dll" : srcQf;

    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\LoadClients\\4070_38656\\Default\\Default\\QF9700USB", L"DLL", qfDrvPath);
    regStr(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers\\QF9700USB", L"DLL", qfDrvPath);

    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB", L"DisplayName", L"CoreChip Fast Ethernet Adapter over USB");
    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB", L"Group", L"NDIS");
    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB", L"ImagePath", qfDrvPath);

    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1", L"DisplayName", L"CoreChip Fast Ethernet Adapter over USB");
    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1", L"Group", L"NDIS");
    regStr(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1", L"ImagePath", qfDrvPath);

    regDw(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1\\Parms", L"BusNumber", 0);
    regDw(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1\\Parms", L"BusType", 1);

    // DHCP enabled for car router automatic IP assignment
    regDw(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1\\Parms\\TCPIP", L"EnableDHCP", 1);
    regDw(HKEY_LOCAL_MACHINE, L"Comm\\QF9700USB1\\Parms\\TCPIP", L"UseZeroBroadcast", 0);

    PushEventW(L"网卡驱动配置: QF9700 (0x0FE6:0x9700) 已注册");

    return true;
}

void UsbTransport_CE::open(uint16_t vid, uint16_t pid) {
    if (impl_->use_box_lib || impl_->dev) return;
    WCHAR dir[MAX_PATH] = {0}, path[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
        WCHAR *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        _snwprintf(path, MAX_PATH - 1, L"%s\\ceusbkwrapper.dll", dir);
    } else {
        wcscpy(path, L"ceusbkwrapper.dll");
    }

    // Step 1: Pre-install & activate driver
    EnsureDriverInstalled(dir);
    Sleep(200);

    // Step 2: 优先尝试官方 libBoxUSB.dll 驱动
    WCHAR boxLibPath[MAX_PATH] = {0};
    _snwprintf(boxLibPath, MAX_PATH - 1, L"%s\\libBoxUSB.dll", dir);
    HMODULE bMod = LoadLibraryW(boxLibPath);
    if (!bMod) bMod = LoadLibraryW(L"libBoxUSB.dll");
    if (bMod) {
        impl_->host_init  = reinterpret_cast<decltype(impl_->host_init)>(GetProcAddressW(bMod, L"host_init"));
        impl_->host_send  = reinterpret_cast<decltype(impl_->host_send)>(GetProcAddressW(bMod, L"host_send"));
        impl_->host_recv  = reinterpret_cast<decltype(impl_->host_recv)>(GetProcAddressW(bMod, L"host_recv"));
        impl_->host_close = reinterpret_cast<decltype(impl_->host_close)>(GetProcAddressW(bMod, L"host_close"));
        if (impl_->host_init && impl_->host_send && impl_->host_recv && impl_->host_close) {
            PushEventW(L"加载官方 libBoxUSB.dll 驱动...");
            // Retry host_init: the USB device (0x1314:0x1520) may not be
            // fully enumerated right after EnsureDriverInstalled activates
            // the driver. A single call races the enumeration and returns -1.
            // Retry up to 6 times with 500ms spacing (~3s total window).
            int hrc = -1;
            for (int attempt = 0; attempt < 6; ++attempt) {
                hrc = impl_->host_init();
                if (hrc == 0) break;
                PushEventW(L"libBoxUSB init 尝试 %d/6 返回 %d%s",
                           attempt + 1, hrc, attempt < 5 ? L"，等待重试..." : L"");
                if (attempt < 5) Sleep(500);
            }
            if (hrc == 0) {
                impl_->box_lib = bMod;
                impl_->use_box_lib = true;
                impl_->claimed = true;
                PushEventW(L"OK: libBoxUSB 握手成功 (0x1314:0x1520)");
                return;
            } else {
                PushEventW(L"libBoxUSB init 未检测到盒子 (最后 %d)，保持重试...", hrc);
                FreeLibrary(bMod);
                bMod = nullptr;
                throw std::runtime_error("libBoxUSB: 盒子未插入或未就绪 (0x1314:0x1520)");
            }
        }
    }

    // Step 3: 原生 ceusbkwrapper.dll 备用驱动链
    impl_->lib = LoadLibraryW(path);
    if (!impl_->lib) impl_->lib = LoadLibraryW(L"ceusbkwrapper.dll");
    if (!impl_->lib) {
        char msg[96];
        _snprintf(msg, sizeof(msg), "LoadLibrary(ceusbkwrapper.dll) failed err=%u", GetLastError());
        throw std::runtime_error(msg);
    }

#define RESOLVE(field, name) \
    impl_->field = reinterpret_cast<decltype(impl_->field)>( \
        GetProcAddressW(impl_->lib, L##name)); \
    if (!impl_->field) { \
        char msg[96]; \
        _snprintf(msg, 95, "CE: missing export %s", name); \
        throw std::runtime_error(msg); \
    }

    RESOLVE(Open,       "UkwOpenDriver")
    RESOLVE(GetDevList, "UkwGetDeviceList")
    RESOLVE(RelDevList, "UkwReleaseDeviceList")
    RESOLVE(GetDevDesc, "UkwGetDeviceDescriptor")
    RESOLVE(GetCfgDesc, "UkwGetConfigDescriptor")
    RESOLVE(Claim,      "UkwClaimInterface")
    RESOLVE(Release,    "UkwReleaseInterface")
    RESOLVE(BulkXfer,   "UkwIssueBulkTransfer")
    RESOLVE(Close,      "UkwCloseDriver")
#undef RESOLVE

    impl_->ClearHaltHost = reinterpret_cast<decltype(impl_->ClearHaltHost)>(
        GetProcAddressW(impl_->lib, L"UkwClearHaltHost"));
    impl_->ClearHaltDev  = reinterpret_cast<decltype(impl_->ClearHaltDev)>(
        GetProcAddressW(impl_->lib, L"UkwClearHaltDevice"));

    // Step 4: 打开驱动 (Try CBX0: directly first, then UkwOpenDriver)
    HANDLE hDriver = CreateFileW(L"CBX0:", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 INVALID_HANDLE_VALUE);
    if (hDriver != INVALID_HANDLE_VALUE) {
        impl_->drv = hDriver;
        PushEventW(L"OK: CBX0: 直接打开成功 (0x%08X)", (DWORD)(uintptr_t)hDriver);
    } else {
        impl_->drv = impl_->Open();
        PushEventW(L"UkwOpenDriver 打开结果: 0x%08X (e=%u)", (DWORD)(uintptr_t)impl_->drv, GetLastError());
    }

    if (!impl_->drv || impl_->drv == INVALID_HANDLE_VALUE) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "UkwOpenDriver failed (err=%u)", GetLastError());
        throw std::runtime_error(msg);
    }

    struct DevDesc {
        unsigned char bLength, bDescriptorType;
        unsigned short bcdUSB;
        unsigned char bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
        unsigned short idVendor, idProduct, bcdDevice;
        unsigned char iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
    };
    void *list[8];
    DWORD actual = 0;
    for (int round = 0; round < 15 && !impl_->dev; ++round) {
        if (impl_->GetDevList(impl_->drv, list, 8, &actual) && actual > 0) {
            for (DWORD i = 0; i < actual; ++i) {
                DevDesc d;
                memset(&d, 0, sizeof(d));
                if (impl_->GetDevDesc(list[i], &d)) {
                    PushEventW(L"USB设备: VID=0x%04X PID=0x%04X", d.idVendor, d.idProduct);
                    if (d.idVendor == vid && d.idProduct == pid) {
                        impl_->dev = list[i];
                        list[i] = nullptr; // 重要：从列表中移出，防止被下方 RelDevList 释放！
                        break;
                    }
                }
            }
            // 释放除已匹配盒子之外的其它设备
            impl_->RelDevList(impl_->drv, list, actual);
        }
        if (!impl_->dev) Sleep(1000);
    }
    if (!impl_->dev)
        throw std::runtime_error("CE: Carlinkit box (0x1314:0x1520) not found");
}

bool UsbTransport_CE::claim() {
    if (impl_->use_box_lib) return true;
    if (!impl_->dev) {
        PushEventW(L"ERR: claim失败 (设备句柄为空)");
        return false;
    }
    if (impl_->claimed) return true;  // already claimed by us
    if (impl_->Claim(impl_->dev, 0)) {
        impl_->claimed = true;
        PushEventW(L"OK: UkwClaimInterface 接口0 认领成功");
        if (impl_->ClearHaltHost) {
            impl_->ClearHaltHost(impl_->dev, 0x02);
            impl_->ClearHaltHost(impl_->dev, 0x81);
        }
        if (impl_->ClearHaltDev) {
            impl_->ClearHaltDev(impl_->dev, 0x02);
            impl_->ClearHaltDev(impl_->dev, 0x81);
        }
        return true;
    }
    DWORD err = GetLastError();
    PushEventW(L"ERR: UkwClaimInterface 失败 err=%u", err);
    return false;
}

void UsbTransport_CE::submitRead(std::size_t buf_size, RecvCallback cb) {
    if (impl_->reader_thread) return;
    impl_->recv = std::move(cb);
    if (buf_size < 64 * 1024) buf_size = 64 * 1024;
    impl_->read_buf_size = buf_size;
    InterlockedExchange(&impl_->reader_running, 1);
    impl_->reader_thread = CreateThread(nullptr, 256 * 1024,
        [](LPVOID arg) -> DWORD {
            Impl *im = static_cast<Impl *>(arg);
            std::vector<uint8_t> buf(im->read_buf_size);
            // UKW_TF_IN_TRANSFER (0x01) | UKW_TF_SHORT_TRANSFER_OK (0x200)
            const DWORD in_flags = 0x00000001 | 0x00000200;
            // Read-path health telemetry: reveals whether the client keeps
            // reading after the first frame (throttled to avoid log spam).
            DWORD empty_reads = 0, ok_reads = 0;
            while (InterlockedCompareExchange(&im->reader_running, 1, 1) == 1) {
                if (im->use_box_lib && im->host_recv) {
                    int rx = im->host_recv(buf.data(), static_cast<int>(buf.size()), 100);
                    if (rx > 0 && im->recv) {
                        ++ok_reads;
                        // Diagnostic: first 8 bytes of the first 10 reads.
                        // libBoxUSB host_recv returns pure, unmodified USB bulk transfer data
                        // (proven by disassembling host_recv -> libusb_bulk_transfer).
                        if (ok_reads <= 10) {
                            unsigned b0 = buf[0], b1 = (rx>1)?buf[1]:0;
                            unsigned b2 = (rx>2)?buf[2]:0, b3 = (rx>3)?buf[3]:0;
                            unsigned b4 = (rx>4)?buf[4]:0, b5 = (rx>5)?buf[5]:0;
                            unsigned b6 = (rx>6)?buf[6]:0, b7 = (rx>7)?buf[7]:0;
                            PushEventW(L"读[libBoxUSB] #%u: %d 字节 "
                                       L"head=%02X %02X %02X %02X %02X %02X %02X %02X",
                                       (unsigned)ok_reads, rx,
                                       b0,b1,b2,b3,b4,b5,b6,b7);
                        }
                        empty_reads = 0;
                        im->recv(buf.data(), static_cast<std::size_t>(rx));
                    } else {
                        ++empty_reads;
                        // After 100 consecutive empty reads (~10s), log once per 500 more
                        if (empty_reads == 100 || (empty_reads > 100 && (empty_reads % 500) == 0)) {
                            PushEventW(L"读[libBoxUSB] 空读 x%u (累计成功 %u 次)",
                                       (unsigned)empty_reads, (unsigned)ok_reads);
                        }
                        Sleep(10);
                    }
                } else if (im->dev && im->BulkXfer) {
                    DWORD rx = 0;
                    BOOL ok = im->BulkXfer(im->dev, in_flags, 0x81,
                                           buf.data(),
                                           static_cast<DWORD>(buf.size()),
                                           &rx, nullptr);
                    if (ok && rx > 0 && im->recv) {
                        ++ok_reads;
                        if (ok_reads <= 5) {
                            PushEventW(L"读[UKW] #%u: %u 字节", (unsigned)ok_reads, (unsigned)rx);
                        }
                        empty_reads = 0;
                        im->recv(buf.data(), rx);
                    } else {
                        ++empty_reads;
                        if (empty_reads == 100 || (empty_reads > 100 && (empty_reads % 500) == 0)) {
                            PushEventW(L"读[UKW] 空读 x%u (累计成功 %u 次) ok=%d e=%u",
                                       (unsigned)empty_reads, (unsigned)ok_reads, (int)ok, GetLastError());
                        }
                        Sleep(20);
                    }
                } else {
                    Sleep(50);
                }
            }
            return 0;
        }, impl_, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
}

void UsbTransport_CE::write(const uint8_t *data, std::size_t len) {
    static int s_write_count = 0;
    if (impl_->use_box_lib && impl_->host_send) {
        int tx = impl_->host_send(const_cast<uint8_t *>(data), static_cast<int>(len), 3000);
        // Log first 40 writes (covers sendInit + initial heartbeats) to
        // carplay.log on TF card — shows exactly which message fails.
        if (s_write_count < 40) {
            ++s_write_count;
            uint32_t magic = 0, cmd = 0;
            if (len >= 8) {
                magic = static_cast<uint32_t>(data[0]) | (data[1] << 8) |
                        (data[2] << 16) | (data[3] << 24);
                cmd   = static_cast<uint32_t>(data[4]) | (data[5] << 8) |
                        (data[6] << 16) | (data[7] << 24);
            }
            PushEventW(L"写#%d len=%u tx=%d magic=%08X cmd=%u",
                       s_write_count, (unsigned)len, tx, magic, cmd);
        }
        if (tx <= 0) {
            PushEventW(L"写[libBoxUSB]失败: len=%u tx=%d", (unsigned)len, tx);
            if (error_cb_) error_cb_(tx);  // let the app self-heal (reconnect)
        }
        return;
    }
    if (!impl_->dev || !impl_->BulkXfer) return;
    DWORD tx = 0;
    // UKW_TF_OUT_TRANSFER (0x00)
    const DWORD out_flags = 0x00000000;
    BOOL ok = impl_->BulkXfer(impl_->dev, out_flags, 0x02,
                              const_cast<uint8_t *>(data), static_cast<DWORD>(len),
                              &tx, nullptr);
    if (!ok) {
        DWORD err = GetLastError();
        PushEventW(L"写[UKW]失败: len=%u e=%u", (unsigned)len, err);
        if (error_cb_) error_cb_(static_cast<int>(err));
    }
}

void UsbTransport_CE::close() {
    if (impl_->reader_thread) {
        InterlockedExchange(&impl_->reader_running, 0);
        WaitForSingleObject(impl_->reader_thread, 3000);
        CloseHandle(impl_->reader_thread);
        impl_->reader_thread = nullptr;
    }
    if (impl_->use_box_lib) {
        if (impl_->host_close) impl_->host_close();
        if (impl_->box_lib) {
            FreeLibrary(impl_->box_lib);
            impl_->box_lib = nullptr;
        }
        impl_->use_box_lib = false;
        impl_->claimed = false;
        return;
    }
    if (impl_->dev && impl_->Release && impl_->claimed) {
        impl_->Release(impl_->dev, 0);
        impl_->claimed = false;
    }
    if (impl_->dev && impl_->RelDevList && impl_->drv != INVALID_HANDLE_VALUE) {
        void *one[1] = { impl_->dev };
        impl_->RelDevList(impl_->drv, one, 1);
        impl_->dev = nullptr;
    }
    if (impl_->drv != INVALID_HANDLE_VALUE && impl_->Close) {
        impl_->Close(impl_->drv);
        impl_->drv = INVALID_HANDLE_VALUE;
    }
}

} // namespace carlinkit

#else
// ==================== PC implementation (libusb-1.0 dynamic) ====================

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif
#include <chrono>
#include <thread>

namespace carlinkit {

// Minimal libusb-1.0 surface we use, resolved at runtime.
struct UsbTransport_PC::Impl {
    void *so = nullptr;
    void *ctx = nullptr;
    void *devh = nullptr;          // libusb_device_handle*
    bool claimed = false;          // interface 0 claimed (idempotent claim)
    uint16_t vid = 0, pid = 0;     // last opened device (for reset-reopen)
    RecvCallback recv;
    volatile bool reader_running = false;
    std::thread reader_thread;

    int  (*init)(void **) = nullptr;
    void (*exit_fn)(void *) = nullptr;
    ssize_t (*get_device_list)(void *, void ***) = nullptr;
    void (*free_device_list)(void **, int) = nullptr;
    int  (*get_device_descriptor)(void *, void *) = nullptr;
    int  (*open)(void *, void **) = nullptr;
    void (*close_fn)(void *) = nullptr;
    int  (*kernel_driver_active)(void *, int) = nullptr;
    int  (*detach_kernel_driver)(void *, int) = nullptr;
    int  (*claim_interface)(void *, int) = nullptr;
    int  (*release_interface)(void *, int) = nullptr;
    int  (*bulk_transfer)(void *, unsigned char, unsigned char *,
                          int, int *, unsigned) = nullptr;
    int  (*set_configuration)(void *, int) = nullptr;
    int  (*control_transfer)(void *, unsigned char, unsigned char,
                             unsigned short, unsigned short, unsigned char *,
                             unsigned short, unsigned) = nullptr;
    int  (*reset_device)(void *) = nullptr;
    int  (*set_interface_alt_setting)(void *, int, int) = nullptr;
    int  (*clear_halt)(void *, unsigned char) = nullptr;
};

UsbTransport_PC::UsbTransport_PC() : impl_(new Impl) {}
UsbTransport_PC::~UsbTransport_PC() { close(); delete impl_; }

static void *load_sym(void *so, const char *name, char *err, size_t errlen) {
#ifdef _WIN32
    void *p = reinterpret_cast<void *>(GetProcAddress(
        reinterpret_cast<HMODULE>(so), name));
#else
    void *p = dlsym(so, name);
#endif
    if (!p && err) {
        snprintf(err, errlen, "libusb symbol missing: %s", name);
    }
    return p;
}

void UsbTransport_PC::loadLibusb() {
    if (impl_->so) return;  // already loaded
    char err[128] = {0};

#ifdef _WIN32
    impl_->so = LoadLibraryA("libusb-1.0.dll");
    if (!impl_->so)
        throw std::runtime_error(
            "PC: libusb-1.0.dll not found. Install libusb from libusb.info "
            "(extract next to this exe or onto PATH).");
#else
    impl_->so = dlopen("libusb-1.0.so.0", RTLD_NOW);
    if (!impl_->so) impl_->so = dlopen("libusb-1.0.so", RTLD_NOW);
    if (!impl_->so)
        throw std::runtime_error("PC: libusb-1.0 not found (apt install libusb-1.0-0)");
#endif

#define SYM(field, name) do { \
    impl_->field = reinterpret_cast<decltype(impl_->field)>( \
        load_sym(impl_->so, name, err, sizeof(err))); \
    if (!impl_->field) throw std::runtime_error(err); \
} while (0)

    SYM(init,               "libusb_init");
    SYM(exit_fn,            "libusb_exit");
    SYM(get_device_list,    "libusb_get_device_list");
    SYM(free_device_list,   "libusb_free_device_list");
    SYM(get_device_descriptor, "libusb_get_device_descriptor");
    SYM(open,               "libusb_open");
    SYM(close_fn,           "libusb_close");
    SYM(kernel_driver_active, "libusb_kernel_driver_active");
    SYM(detach_kernel_driver, "libusb_detach_kernel_driver");
    SYM(claim_interface,    "libusb_claim_interface");
    SYM(release_interface,  "libusb_release_interface");
    SYM(bulk_transfer,      "libusb_bulk_transfer");
    SYM(set_configuration,  "libusb_set_configuration");
    SYM(control_transfer,   "libusb_control_transfer");
    SYM(reset_device,       "libusb_reset_device");
    SYM(set_interface_alt_setting, "libusb_set_interface_alt_setting");
    SYM(clear_halt,                "libusb_clear_halt");
#undef SYM

    if (impl_->init(&impl_->ctx) != 0)
        throw std::runtime_error("PC: libusb_init failed");
}

void UsbTransport_PC::findAndOpen() {
    // Enumerate and find the Carlinkit box by impl_->vid/pid, open a handle.
    void **list = nullptr;
    ssize_t n = impl_->get_device_list(impl_->ctx, &list);
    struct MiniDevDesc { uint8_t bLength, bType; uint16_t bcdUSB;
        uint8_t cls, sub, proto, mps0; uint16_t vid, pid, bcd; uint8_t rest[6]; };
    void *found = nullptr;
    for (ssize_t i = 0; i < n; ++i) {
        MiniDevDesc d;
        memset(&d, 0, sizeof(d));
        if (impl_->get_device_descriptor(list[i], &d) == 0 &&
            d.vid == impl_->vid && d.pid == impl_->pid) {
            found = list[i];
            break;
        }
    }
    if (!found) {
        impl_->free_device_list(list, 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "PC: Carlinkit box %04x:%04x not found (plug it in)",
                 impl_->vid, impl_->pid);
        throw std::runtime_error(msg);
    }
    if (impl_->open(found, &impl_->devh) != 0) {
        impl_->free_device_list(list, 1);
        throw std::runtime_error("PC: libusb_open failed (permissions? try sudo)");
    }
    impl_->free_device_list(list, 1);
}

void UsbTransport_PC::open(uint16_t vid, uint16_t pid) {
    // Idempotent: pc_main opens+claims, then Connection::start() calls
    // open() again. Re-opening here would leak the first handle (its claim
    // still held) and make the second claim_interface fail.
    if (impl_->devh) return;
    impl_->vid = vid;
    impl_->pid = pid;
    loadLibusb();
    findAndOpen();
}

bool UsbTransport_PC::claim() {
    if (!impl_->devh) return false;
    if (impl_->claimed) return true;  // already claimed by us
    const ClaimOptions &o = claim_opts;

#ifdef __linux__
    auto detachKernel = [&] {
        if (impl_->kernel_driver_active(impl_->devh, 0) == 1)
            impl_->detach_kernel_driver(impl_->devh, 0);
    };
    detachKernel();
#endif

    // --reset-reopen: FastCarPlay link() does reset_device first. On Linux
    // the handle stays valid; on Windows/WinUSB reset invalidates the handle
    // so we close + reopen after. Resets the box to a clean state.
    if (o.reset_reopen) {
        int rc = impl_->reset_device(impl_->devh);
        fprintf(stderr, "[USB-RESET] reset_device rc=%d\n", rc);
        impl_->close_fn(impl_->devh);
        impl_->devh = nullptr;
        impl_->claimed = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        try {
            findAndOpen();
        } catch (const std::exception &e) {
            fprintf(stderr, "[USB-RESET] reopen failed: %s\n", e.what());
            return false;
        }
        fprintf(stderr, "[USB-RESET] reopened handle ok\n");
#ifdef __linux__
        detachKernel();
#endif
    }

    // NOTE: libusb_set_configuration(1) resets gadget FIFOs on the box,
    // causing bulk OUT (0x02) to permanently NAK/timeout rc=-7.
    // The OS USB stack already configured the device during PnP.
    // libBoxUSB.dll (official Carlinkit driver) only calls claim_interface(0).
    if (o.set_config) {
        int rc = impl_->set_configuration(impl_->devh, 1);
        fprintf(stderr, "[USB-CFG] set_configuration(1) rc=%d\n", rc);
    }

    int clrc = impl_->claim_interface(impl_->devh, 0);
    fprintf(stderr, "[USB-CLAIM] claim_interface(0) rc=%d\n", clrc);
    if (clrc != 0) return false;
    impl_->claimed = true;

    if (o.alt_setting) {
        int r = impl_->set_interface_alt_setting(impl_->devh, 0, 0);
        fprintf(stderr, "[USB-ALT] set_interface_alt_setting(0,0) rc=%d\n", r);
    }
    if (o.clear_halt) {
        int r1 = impl_->clear_halt(impl_->devh, 0x02);
        int r2 = impl_->clear_halt(impl_->devh, 0x81);
        fprintf(stderr, "[USB-HALT] clear_halt OUT(0x02) rc=%d IN(0x81) rc=%d\n", r1, r2);
    }
    return true;
}

void UsbTransport_PC::submitRead(std::size_t, RecvCallback cb) {
    impl_->recv = std::move(cb);
    if (!impl_->reader_running) {
        impl_->reader_running = true;
        impl_->reader_thread = std::thread([this]() {
            while (impl_->reader_running) {
                if (!impl_->devh) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                unsigned char buf[64 * 1024];
                int rx = 0;
                int rc = impl_->bulk_transfer(impl_->devh, 0x81, buf, sizeof(buf),
                                              &rx, 100);
                if (rc == 0 && rx > 0) {
                    fprintf(stderr, "[USB-READ] rx=%d\n", rx);
                    if (impl_->recv) {
                        try {
                            impl_->recv(buf, static_cast<std::size_t>(rx));
                        } catch (const std::exception &e) {
                            fprintf(stderr, "[USB-READ] callback exception: %s\n", e.what());
                        } catch (...) {
                            fprintf(stderr, "[USB-READ] callback unknown exception\n");
                        }
                    }
                } else if (rc == -9) {
                    fprintf(stderr, "[USB-READ] pipe stalled (rc=-9), clearing halt on 0x81\n");
                    if (impl_->clear_halt && impl_->devh) {
                        impl_->clear_halt(impl_->devh, 0x81);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc == -1) {
                    fprintf(stderr, "[USB-READ] transient I/O error (rc=-1), retrying\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc != 0 && rc != -7 && rc != -8) {
                    fprintf(stderr, "[USB-READ] fatal err rc=%d\n", rc);
                    break;
                }
            }
            impl_->reader_running = false;
        });
    }
}

void UsbTransport_PC::write(const uint8_t *data, std::size_t len) {
    if (!impl_->devh) return;
    int tx = 0;
    // Box bulk OUT is 0x02 (confirmed via GET_DESCRIPTOR dump: 0x81 IN,
    // 0x02 OUT, both bulk mps=512). Writing 0x01 fails rc=-5 NOT_FOUND.
    int rc = impl_->bulk_transfer(impl_->devh, 0x02,
                         const_cast<unsigned char *>(data),
                         static_cast<int>(len), &tx, 3000);
    // Diagnostic: surface write failures (previously silently ignored).
    if (rc != 0 || tx != static_cast<int>(len))
        fprintf(stderr, "[USB-WRITE] ep=0x02 len=%zu tx=%d rc=%d\n",
                len, tx, rc);
}

void UsbTransport_PC::close() {
    impl_->reader_running = false;
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }
    if (impl_->devh) {
        if (impl_->claimed) impl_->release_interface(impl_->devh, 0);
        impl_->claimed = false;
        impl_->close_fn(impl_->devh);
        impl_->devh = nullptr;
    }
    if (impl_->ctx) {
        impl_->exit_fn(impl_->ctx);
        impl_->ctx = nullptr;
    }
}

// Blocking pump: when background reader_thread is active, acts as liveness check + sleep.
// Otherwise performs a single bulk read. Returns false on fatal error.
bool UsbTransport_PC::readPump(int timeout_ms) {
    if (impl_->reader_thread.joinable()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return impl_->reader_running;
    }
    if (!impl_->devh) return false;
    unsigned char buf[64 * 1024];
    int rx = 0;
    int rc = impl_->bulk_transfer(impl_->devh, 0x81, buf, sizeof(buf),
                                  &rx, timeout_ms);
    if (rc == 0 && rx > 0) {
        fprintf(stderr, "[USB-READ] rx=%d\n", rx);
        if (impl_->recv) impl_->recv(buf, static_cast<std::size_t>(rx));
        return true;
    }
    if (rc == -9) {
        fprintf(stderr, "[USB-READ] pipe stalled (rc=-9), clearing halt on 0x81\n");
        if (impl_->clear_halt && impl_->devh) {
            impl_->clear_halt(impl_->devh, 0x81);
        }
        return true;
    }
    if (rc == -1) {
        fprintf(stderr, "[USB-READ] transient I/O error (rc=-1), retrying\n");
        return true;
    }
    if (rc != 0 && rc != -7 && rc != -8) {
        fprintf(stderr, "[USB-READ] err rc=%d\n", rc);
    }
    return (rc == 0 || rc == -7 || rc == -8);  // error -> false
}

} // namespace carlinkit

#endif