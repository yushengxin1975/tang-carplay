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

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
// ==================== CE implementation ====================

#include <windows.h>

namespace carlinkit {

struct UsbTransport_CE::Impl {
    HMODULE lib = nullptr;
    HANDLE drv = INVALID_HANDLE_VALUE;
    HANDLE reader_thread = nullptr;
    volatile LONG reader_running = 0;
    RecvCallback recv;

    HANDLE (*Open)(void) = nullptr;
    BOOL  (*GetDevList)(HANDLE, void **, DWORD, LPDWORD) = nullptr;
    void  (*RelDevList)(HANDLE, void **, DWORD) = nullptr;
    BOOL  (*GetDevDesc)(void *, void *) = nullptr;
    BOOL  (*GetCfgDesc)(void *, DWORD, void *, DWORD, LPDWORD) = nullptr;
    BOOL  (*Claim)(void *, DWORD) = nullptr;
    BOOL  (*Release)(void *, DWORD) = nullptr;
    BOOL  (*BulkXfer)(void *, DWORD, UCHAR, void *, DWORD, LPDWORD, void *) = nullptr;
    void  (*Close)(HANDLE) = nullptr;

    void *dev = nullptr;
    bool claimed = false;          // interface 0 claimed (idempotent claim)
};

UsbTransport_CE::UsbTransport_CE() : impl_(new Impl) {}
UsbTransport_CE::~UsbTransport_CE() { close(); delete impl_; }

void UsbTransport_CE::open(uint16_t vid, uint16_t pid) {
    // Idempotent: caller may open() again via Connection::start().
    if (impl_->dev) return;
    WCHAR dir[MAX_PATH] = {0}, path[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
        WCHAR *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        _snwprintf(path, MAX_PATH - 1, L"%s\\ceusbkwrapper.dll", dir);
    } else {
        wcscpy(path, L"ceusbkwrapper.dll");
    }
    impl_->lib = LoadLibraryW(path);
    if (!impl_->lib)
        throw std::runtime_error("CE: LoadLibrary(ceusbkwrapper.dll) failed");

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

    impl_->drv = impl_->Open();
    if (!impl_->drv || impl_->drv == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CE: UkwOpenDriver failed (USBKIT.DLL registered?)");

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
                if (impl_->GetDevDesc(list[i], &d) &&
                    d.idVendor == vid && d.idProduct == pid) {
                    impl_->dev = list[i];
                    break;
                }
            }
            impl_->RelDevList(impl_->drv, list, 8);
        }
        if (!impl_->dev) Sleep(2000);
    }
    if (!impl_->dev)
        throw std::runtime_error("CE: Carlinkit box (0x1314:0x1520) not found");
}

bool UsbTransport_CE::claim() {
    if (!impl_->dev) return false;
    if (impl_->claimed) return true;  // already claimed by us
    if (impl_->Claim(impl_->dev, 0)) {
        impl_->claimed = true;
        return true;
    }
    return false;
}

void UsbTransport_CE::submitRead(std::size_t, RecvCallback cb) {
    if (impl_->reader_thread) return;
    if (!impl_->BulkXfer || !impl_->dev) return;
    impl_->recv = std::move(cb);
    InterlockedExchange(&impl_->reader_running, 1);
    impl_->reader_thread = CreateThread(nullptr, 256 * 1024,
        [](LPVOID arg) -> DWORD {
            Impl *im = static_cast<Impl *>(arg);
            std::vector<uint8_t> buf(64 * 1024);
            while (InterlockedCompareExchange(&im->reader_running, 1, 1) == 1) {
                DWORD rx = 0;
                BOOL ok = im->BulkXfer(im->dev, 1 /*IN*/, 0x81,
                                       buf.data(),
                                       static_cast<DWORD>(buf.size()),
                                       &rx, nullptr);
                if (ok && rx > 0 && im->recv) im->recv(buf.data(), rx);
                else if (!ok) Sleep(50);
            }
            return 0;
        }, impl_, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
}

void UsbTransport_CE::write(const uint8_t *data, std::size_t len) {
    if (!impl_->dev || !impl_->BulkXfer) return;
    DWORD tx = 0;
    impl_->BulkXfer(impl_->dev, 0 /*OUT*/, 0x01,
                    const_cast<uint8_t *>(data), static_cast<DWORD>(len),
                    &tx, nullptr);
}

void UsbTransport_CE::close() {
    if (impl_->reader_thread) {
        InterlockedExchange(&impl_->reader_running, 0);
        WaitForSingleObject(impl_->reader_thread, 3000);
        CloseHandle(impl_->reader_thread);
        impl_->reader_thread = nullptr;
    }
    if (impl_->dev && impl_->Release && impl_->claimed) impl_->Release(impl_->dev, 0);
    impl_->claimed = false;
    impl_->dev = nullptr;
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

namespace carlinkit {

// Minimal libusb-1.0 surface we use, resolved at runtime.
struct UsbTransport_PC::Impl {
    void *so = nullptr;
    void *ctx = nullptr;
    void *devh = nullptr;          // libusb_device_handle*
    bool claimed = false;          // interface 0 claimed (idempotent claim)
    RecvCallback recv;
    volatile bool reader_running = false;

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

void UsbTransport_PC::open(uint16_t vid, uint16_t pid) {
    // Idempotent: pc_main opens+claims, then Connection::start() calls
    // open() again. Re-opening here would leak the first handle (its claim
    // still held) and make the second claim_interface fail.
    if (impl_->devh) return;
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
#undef SYM

    if (impl_->init(&impl_->ctx) != 0)
        throw std::runtime_error("PC: libusb_init failed");

    // Enumerate and find the Carlinkit box
    void **list = nullptr;
    ssize_t n = impl_->get_device_list(impl_->ctx, &list);
    struct MiniDevDesc { uint8_t bLength, bType; uint16_t bcdUSB;
        uint8_t cls, sub, proto, mps0; uint16_t vid, pid, bcd; uint8_t rest[6]; };
    void *found = nullptr;
    for (ssize_t i = 0; i < n; ++i) {
        MiniDevDesc d;
        memset(&d, 0, sizeof(d));
        if (impl_->get_device_descriptor(list[i], &d) == 0 &&
            d.vid == vid && d.pid == pid) {
            found = list[i];
            break;
        }
    }
    if (!found) {
        impl_->free_device_list(list, 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "PC: Carlinkit box %04x:%04x not found (plug it in)",
                 vid, pid);
        throw std::runtime_error(msg);
    }
    if (impl_->open(found, &impl_->devh) != 0) {
        impl_->free_device_list(list, 1);
        throw std::runtime_error("PC: libusb_open failed (permissions? try sudo)");
    }
    impl_->free_device_list(list, 1);
}

bool UsbTransport_PC::claim() {
    if (!impl_->devh) return false;
    if (impl_->claimed) return true;  // already claimed by us
#ifdef __linux__
    if (impl_->kernel_driver_active(impl_->devh, 0) == 1)
        impl_->detach_kernel_driver(impl_->devh, 0);
#endif
    if (impl_->set_configuration(impl_->devh, 1) != 0) {
        // Box may already be configured; ignore and try claim anyway
    }
    if (impl_->claim_interface(impl_->devh, 0) == 0) {
        impl_->claimed = true;
        // Diagnostic: dump the active config descriptor's endpoints so we
        // can verify the bulk OUT (0x01) / IN (0x81) addresses are right.
        unsigned char cfg[512] = {0};
        int crc = impl_->control_transfer(impl_->devh, 0x80, 0x06, 0x0200, 0,
                                          cfg, sizeof(cfg), 1000);
        if (crc > 0) {
            int total = cfg[2] | (cfg[3] << 8);
            fprintf(stderr, "[USB-CFG] total=%d numInterfaces=%d\n", total, cfg[4]);
            int off = 0;
            while (off + 2 <= crc && off < total) {
                int len = cfg[off];
                int type = cfg[off + 1];
                if (len < 2 || off + len > total) break;
                if (type == 0x04) {
                    fprintf(stderr, "[USB-IF] num=%d alt=%d numEp=%d class=%02x\n",
                            cfg[off + 2], cfg[off + 3], cfg[off + 4], cfg[off + 5]);
                } else if (type == 0x05) {
                    int addr = cfg[off + 2];
                    int attr = cfg[off + 3];
                    int mps = cfg[off + 4] | (cfg[off + 5] << 8);
                    fprintf(stderr, "[USB-EP] addr=0x%02x attr=0x%02x(%s) mps=%d\n",
                            addr, attr,
                            (attr & 3) == 2 ? "bulk" :
                            (attr & 3) == 3 ? "int" : "ctrl", mps);
                }
                off += len;
            }
        } else {
            fprintf(stderr, "[USB-CFG] GET_DESCRIPTOR failed rc=%d\n", crc);
        }
        return true;
    }
    return false;
}

void UsbTransport_PC::submitRead(std::size_t, RecvCallback cb) {
    impl_->recv = std::move(cb);
    // PC uses a polling read loop started by the caller (pc_main) for
    // deterministic Ctrl+C handling; see readPump().
}

void UsbTransport_PC::write(const uint8_t *data, std::size_t len) {
    if (!impl_->devh) return;
    int tx = 0;
    // Box bulk OUT is 0x02 (confirmed via GET_DESCRIPTOR dump: 0x81 IN,
    // 0x02 OUT, both bulk mps=512). Writing 0x01 fails rc=-5 NOT_FOUND.
    int rc = impl_->bulk_transfer(impl_->devh, 0x02,
                         const_cast<unsigned char *>(data),
                         static_cast<int>(len), &tx, 1000);
    // Diagnostic: surface write failures (previously silently ignored).
    if (rc != 0 || tx != static_cast<int>(len))
        fprintf(stderr, "[USB-WRITE] ep=0x02 len=%zu tx=%d rc=%d\n",
                len, tx, rc);
}

void UsbTransport_PC::close() {
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

// Blocking pump: one bulk read, invoke callback, return.
// Returns false on fatal error. pc_main calls this in a loop.
bool UsbTransport_PC::readPump(int timeout_ms) {
    if (!impl_->devh) return false;
    unsigned char buf[64 * 1024];
    int rx = 0;
    int rc = impl_->bulk_transfer(impl_->devh, 0x81, buf, sizeof(buf),
                                  &rx, timeout_ms);
    if (rc == 0 && rx > 0 && impl_->recv) {
        impl_->recv(buf, static_cast<std::size_t>(rx));
        return true;
    }
    // LIBUSB_ERROR_TIMEOUT (-7): no data in window -> keep pumping.
    // LIBUSB_ERROR_OVERFLOW (-8): frame bigger than buffer -> keep pumping
    // (data lost for this read, but the box is still there).
    return (rc == 0 || rc == -7 || rc == -8);  // error -> false
}

} // namespace carlinkit

#endif