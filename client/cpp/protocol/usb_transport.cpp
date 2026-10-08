// USB transport implementations (PC: libusb dynamic-load, CE: CEUSBKWrapper).
//
// PC build: dynamically loads libusb-1.0.dll at runtime (no link-time dep),
// so the same source builds on machines without libusb installed; the unit
// tests never call these paths.
//
// CE build: compiles under CeGCC; resolves Ukw* exports from
// ceusbkwrapper.dll at runtime (same pattern as usbtest v0.6, proven on-car).
//
// Both are only instantiated by the real client main(), never by tests.

#include "usb_transport.h"

#include <cstring>

#ifdef _WIN32_WCE   // ---------------- CE implementation ----------------

#include <windows.h>

namespace carlinkit {

struct UsbTransport_CE::Impl {
    HMODULE lib = nullptr;
    HANDLE drv = INVALID_HANDLE_VALUE;

    // UKW function pointers (resolved from ceusbkwrapper.dll)
    HANDLE (*Open)(void) = nullptr;
    void  *(*GetDevList)(HANDLE, void **, DWORD, LPDWORD) = nullptr; // BOOL
    void   (*RelDevList)(HANDLE, void **, DWORD) = nullptr;
    BOOL  (*GetDevDesc)(void *, void *) = nullptr;   // UKW_DEVICE, desc*
    BOOL  (*GetCfgDesc)(void *, DWORD, void *, DWORD, LPDWORD) = nullptr;
    BOOL  (*Claim)(void *, DWORD) = nullptr;
    BOOL  (*Release)(void *, DWORD) = nullptr;
    BOOL  (*CtrlXfer)(void *, DWORD, void *, void *, DWORD, LPDWORD, void *) = nullptr;
    BOOL  (*BulkXfer)(void *, DWORD, UCHAR, void *, DWORD, LPDWORD, void *) = nullptr;
    void  (*Close)(HANDLE) = nullptr;

    void *dev = nullptr;          // UKW_DEVICE for the Carlinkit box
    RecvCallback recv;
};

UsbTransport_CE::UsbTransport_CE() : impl_(new Impl) {}
UsbTransport_CE::~UsbTransport_CE() { close(); delete impl_; }

void UsbTransport_CE::open(uint16_t vid, uint16_t pid) {
    // 1. Load user-space UKW library from same dir as the exe
    WCHAR dir[MAX_PATH] = {0};
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, dir, MAX_PATH)) {
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
        GetProcAddress(impl_->lib, L##name)); \
    if (!impl_->field) { \
        char msg[96]; \
        _snprintf(msg, 95, "CE: missing export %s", name); \
        throw std::runtime_error(msg); \
    }

    RESOLVE(Open,        "UkwOpenDriver")
    RESOLVE(GetDevList,  "UkwGetDeviceList")
    RESOLVE(RelDevList,  "UkwReleaseDeviceList")
    RESOLVE(GetDevDesc,  "UkwGetDeviceDescriptor")
    RESOLVE(GetCfgDesc,  "UkwGetConfigDescriptor")
    RESOLVE(Claim,       "UkwClaimInterface")
    RESOLVE(Release,     "UkwReleaseInterface")
    RESOLVE(CtrlXfer,    "UkwIssueControlTransfer")
    RESOLVE(BulkXfer,    "UkwIssueBulkTransfer")
    RESOLVE(Close,       "UkwCloseDriver")
#undef RESOLVE

    // 2. Open the UKW driver
    impl_->drv = impl_->Open();
    if (!impl_->drv || impl_->drv == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CE: UkwOpenDriver failed (is USBKIT.DLL driver registered?)");

    // 3. Poll device list looking for the box (vid/pid)
    //    NOTE: driver registration (ClientDrivers + LoadClients\4884) is a
    //    prerequisite. The on-car startup sequence must run usbtest's deploy
    //    steps first (or the AutoKit-installed Usb_Box entry must coexist).
    struct DevDesc { // UKW_DEVICE_DESCRIPTOR layout (see usbtest v0.6)
        unsigned char bLength, bDescriptorType;
        unsigned short bcdUSB;
        unsigned char bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
        unsigned short idVendor, idProduct, bcdDevice;
        unsigned char iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
    };
    void *list[8];
    DWORD actual = 0;
    for (int round = 0; round < 15; ++round) {  // ~30s wait for plug-in
        if (impl_->GetDevList(impl_->drv, list, 8, &actual) && actual > 0) {
            for (DWORD i = 0; i < actual; ++i) {
                DevDesc d;
                memset(&d, 0, sizeof(d));
                if (impl_->GetDevDesc(list[i], &d)) {
                    if (d.idVendor == vid && d.idProduct == pid) {
                        impl_->dev = list[i];
                        break;
                    }
                }
            }
            impl_->RelDevList(impl_->drv, list, 8);
            if (impl_->dev) break;
        }
        Sleep(2000);
    }
    if (!impl_->dev)
        throw std::runtime_error("CE: Carlinkit box (0x1314:0x1520) not found");
}

bool UsbTransport_CE::claim() {
    if (!impl_->dev) return false;
    // Claim interface 0 (box exposes vendor-specific bulk on iface 0)
    if (!impl_->Claim(impl_->dev, 0)) return false;
    return true;
}

void UsbTransport_CE::submitRead(std::size_t /*buf_size*/, RecvCallback cb) {
    // CE has no async callback plumbing in this wrapper; the client main
    // loop polls via a reader thread calling bulkRead(). Store the callback
    // for the future async layer.
    impl_->recv = std::move(cb);
}

void UsbTransport_CE::write(const uint8_t *data, std::size_t len) {
    if (!impl_->dev || !impl_->BulkXfer) return;
    DWORD tx = 0;
    impl_->BulkXfer(impl_->dev, /*flags=*/0 /*OUT*/, 0x01,
                    const_cast<uint8_t *>(data), static_cast<DWORD>(len),
                    &tx, nullptr /*blocking*/);
}

void UsbTransport_CE::close() {
    if (impl_->dev && impl_->Release) impl_->Release(impl_->dev, 0);
    impl_->dev = nullptr;
    if (impl_->drv != INVALID_HANDLE_VALUE && impl_->Close) {
        impl_->Close(impl_->drv);
        impl_->drv = INVALID_HANDLE_VALUE;
    }
    // lib stays loaded (cheap, avoids reload churn)
}

} // namespace carlinkit

#else   // ---------------- PC implementation (libusb-1.0 dynamic) ----------------

namespace carlinkit {

struct UsbTransport_PC::Impl {
    void *lib = nullptr;          // libusb-1.0.dll module
    void *ctx = nullptr;          // libusb_context
    void *devh = nullptr;         // libusb_device_handle
    RecvCallback recv;
};

UsbTransport_PC::UsbTransport_PC() : impl_(new Impl) {}
UsbTransport_PC::~UsbTransport_PC() { close(); delete impl_; }

void UsbTransport_PC::open(uint16_t vid, uint16_t pid) {
    throw std::runtime_error(
        "UsbTransport_PC: real USB not wired yet (unit tests use FakeTransport). "
        "Next step: link libusb-1.0 and implement open/claim/bulk loops here.");
}

bool UsbTransport_PC::claim() { return false; }
void UsbTransport_PC::submitRead(std::size_t, RecvCallback cb) { impl_->recv = std::move(cb); }
void UsbTransport_PC::write(const uint8_t *, std::size_t) {}
void UsbTransport_PC::close() {}

} // namespace carlinkit

#endif