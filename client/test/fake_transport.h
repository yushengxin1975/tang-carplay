#pragma once
// In-memory transport for unit tests. Records sends, replays scripted
// inbound bytes, no USB / OS code.
#include "protocol/usb_transport.h"

#include <stdexcept>
#include <vector>

namespace carlinkit::test {

class FakeTransport : public UsbTransport {
public:
    FakeTransport() = default;

    void open(uint16_t vid, uint16_t pid) override {
        if (vid != CARLINKIT_VENDOR_ID || pid != CARLINKIT_PRODUCT_ID)
            throw std::runtime_error("FakeTransport: VID/PID mismatch");
        open_ = true;
    }
    bool claim() override { claim_ = true; return true; }
    void submitRead(std::size_t /*buf_size*/, RecvCallback cb) override {
        recv_cb_ = cb;
    }
    void write(const uint8_t *data, std::size_t len) override {
        sent_.insert(sent_.end(), data, data + len);
    }
    void close() override {
        open_ = claim_ = false;
        recv_cb_ = nullptr;
    }

    bool is_open() const { return open_; }
    bool is_claimed() const { return claim_; }

    const std::vector<uint8_t> &sent() const { return sent_; }

    // Deliver scripted bytes to registered recv callback.
    void deliver(const uint8_t *data, std::size_t len) {
        if (recv_cb_) recv_cb_(data, len);
    }
    void deliver(const std::vector<uint8_t> &v) { deliver(v.data(), v.size()); }

    // Re-assert sent log (for test reset between cases).
    void reset_sent() { sent_.clear(); }

private:
    bool open_ = false;
    bool claim_ = false;
    RecvCallback recv_cb_;
    std::vector<uint8_t> sent_;
};

} // namespace carlinkit::test