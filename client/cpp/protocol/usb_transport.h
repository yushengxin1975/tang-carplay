#pragma once
// USB transport interface for Carlinkit CPC200 box.
//
// Concrete classes:
//   UsbTransport_PC      — libusb (Windows .NET libusbdotnet OR native libusb)
//   UsbTransport_CE      — ENLYZE CEUSBKWrapper (UsbDeviceList + Ukw* functions)
//
// Both expose the same minimal API used by protocol/connection.h:
//   open(), claim(), close(), submitRead(), write(packet_buffer)
//
// On BYD 唐 2015 (Windows CE 6.0 ARM) we link to CEUSBKWrapper's
// ceusbkwrapper.dll; libusbdotnet for PC unit tests.

#ifndef CARLINKIT_USB_TRANSPORT_H
#define CARLINKIT_USB_TRANSPORT_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

#include "protocol_const.h"

namespace carlinkit {

struct USBDeviceDescriptor {
    uint16_t vid;
    uint16_t pid;
    uint8_t  bus;
    uint8_t  addr;
};

// Abstract transport. Subclasses concrete impl per platform.
class UsbTransport {
public:
    using RecvCallback = std::function<void(const uint8_t *data, std::size_t len)>;
    using ErrorCallback = std::function<void(int err)>;

    virtual ~UsbTransport() = default;

    // Open/find by VID/PID. On failure throws std::runtime_error.
    virtual void open(uint16_t vid = CARLINKIT_VENDOR_ID,
                      uint16_t pid = CARLINKIT_PRODUCT_ID) = 0;

    // Claim endpoints, ready for I/O. Returns true if claimed successfully.
    virtual bool claim() = 0;

    // Submit a bulk read. RecvCallback fires when data arrives.
    virtual void submitRead(std::size_t buf_size, RecvCallback cb) = 0;

    // Write a packet (header + payload) over bulk OUT.
    virtual void write(const uint8_t *data, std::size_t len) = 0;

    // Cleanup endpoints and close device.
    virtual void close() = 0;

    // Optional: set error callback. Default: ignored.
    void setErrorCallback(ErrorCallback cb) { error_cb_ = std::move(cb); }

protected:
    ErrorCallback error_cb_;
};

// ----- PC implementation (libusbdotnet) -----
// Concrete impl uses libusbdotnet.LibUsbDotNet.UsbDevice.Open;
// declared in separate header to avoid dragging libusbdotnet into CE build.

class UsbTransport_PC : public UsbTransport {
public:
    UsbTransport_PC();
    ~UsbTransport_PC() override;
    void open(uint16_t vid = CARLINKIT_VENDOR_ID,
              uint16_t pid = CARLINKIT_PRODUCT_ID) override;
    bool claim() override;
    void submitRead(std::size_t buf_size, RecvCallback cb) override;
    void write(const uint8_t *data, std::size_t len) override;
    void close() override;
private:
    struct Impl;
    Impl *impl_;
};

// ----- CE implementation (CEUSBKWrapper) -----
// Concrete impl uses LoadLibrary("ceusbkwrapper.dll") + GetProcAddress("Ukw*");
// + ActivateDeviceEx on "Drivers\\USB\\ClientDrivers\\Usb_Kernel_Wrapper"
// to load our bundled USBKIT.DLL driver (or rely on AutoKit-installed one).
class UsbTransport_CE : public UsbTransport {
public:
    UsbTransport_CE();
    ~UsbTransport_CE() override;
    void open(uint16_t vid = CARLINKIT_VENDOR_ID,
              uint16_t pid = CARLINKIT_PRODUCT_ID) override;
    bool claim() override;
    void submitRead(std::size_t buf_size, RecvCallback cb) override;
    void write(const uint8_t *data, std::size_t len) override;
    void close() override;
private:
    struct Impl;
    Impl *impl_;
};

} // namespace carlinkit

#endif // CARLINKIT_USB_TRANSPORT_H