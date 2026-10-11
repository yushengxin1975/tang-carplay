#pragma once
// Connection state machine for Carlinkit CPC2000 box.
//
// Models the PC side: open USB, send CMD_OPEN / VERSION / ENCRYPTION, then
// process inbound messages (PLUGGED/UNPLUGGED/VIDEO/AUDIO/CONTROL/HEARTBEAT).
//
// Derived from FastCarPlay/src/protocol/connection.cpp but stripped of
// libusb dependencies — uses UsbTransport abstraction so the same code can
// run on PC (test) and CE (deploy).

#ifndef CARLINKIT_CONNECTION_H
#define CARLINKIT_CONNECTION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include "ce_compat.h"
#include "aes_cipher.h"
#include "message.h"
#include "protocol_const.h"
#include "usb_transport.h"

namespace carlinkit {

enum class ConnState : int32_t {
    UNKNOWN        = PROTOCOL_STATUS_UNKNOWN,
    INITIALISING   = PROTOCOL_STATUS_INITIALISING,
    NO_DEVICE      = PROTOCOL_STATUS_NO_DEVICE,
    LINKING        = PROTOCOL_STATUS_LINKING,
    ERROR_STATE = PROTOCOL_STATUS_ERROR,
    ONLINE         = PROTOCOL_STATUS_ONLINE,
    CONNECTED      = PROTOCOL_STATUS_CONNECTED,
};

// Inbound payload callbacks (register what you want to receive).
struct InboundCallbacks {
    std::function<void(Cmd cmd, const uint8_t *payload, std::size_t len)> on_any;
    std::function<void()> on_plugged;
    std::function<void()> on_unplugged;
    std::function<void(Cmd cmd, const uint8_t *payload, std::size_t len)> on_control;
    std::function<void(Cmd cmd, const uint8_t *payload, std::size_t len)> on_video;
    std::function<void(Cmd cmd, const uint8_t *payload, std::size_t len)> on_audio;
    std::function<void(ConnState new_state)> on_state;
};

class Connection {
public:
    explicit Connection(std::unique_ptr<UsbTransport> t,
                       InboundCallbacks cb = {})
        : transport_(std::move(t)), cbs_(std::move(cb)) {}

    // Start session: open transport, claim, send CMD_OPEN.
    // Throws on transport failure.
    void start();

    // Send the FastCarPlay-style config burst (encryption-disabled path):
    // screen_dpi, android_work_mode, CMD_OPEN(28B), JSON_CONTROL, DAYNIGHT,
    // night/charge/box_name/hand_drive files, then CONTROL mic/wifi/bt/auto.
    void sendInit();

    // Stop session: cancel reads, release endpoints, close transport.
    void stop();

    // Send an outbound packet. Encrypts payload if cipher enabled.
    // Pass empty payload for ping-style messages.
    void send(Cmd cmd, const std::vector<uint8_t> &payload = {});

    // Re-key after Box accepts our key.
    void setKey(const uint8_t key[16], const uint8_t iv[16]) {
        cipher_.init(key, iv);
    }

    // State state from probe (Box status query).
    ConnState state() const {
        // LockGuard resolves to std::lock_guard<std::mutex> on PC, custom
    // RAII wrapper on CE (no std::mutex). Unqualified to avoid std lookup.
    LockGuard lock(mtx_);
        return state_;
    }

private:
    // Called by UsbTransport's RecvCallback when data arrives.
    void onRecv(const uint8_t *data, std::size_t len);

    // Dispatch an inbound message to registered callbacks.
    void dispatch(const Message &msg);

    std::unique_ptr<UsbTransport> transport_;
    AESCipher cipher_;
    bool cipher_enabled_ = false;
    InboundCallbacks cbs_;
    ConnState state_ = ConnState::INITIALISING;
    mutable Mutex mtx_;
};

} // namespace carlinkit

#endif // CARLINKIT_CONNECTION_H