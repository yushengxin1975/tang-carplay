#include "connection.h"
#include "message.h"

#include <cstring>
#include <ctime>
#include <cstdio>
#include <string>

namespace carlinkit {

void Connection::start() {
    if (!transport_) throw std::runtime_error("Connection: no transport");
    transport_->open(CARLINKIT_VENDOR_ID, CARLINKIT_PRODUCT_ID);
    if (!transport_->claim()) {
        transport_->close();
        throw std::runtime_error("Connection: claim failed");
    }
    // Chain onto any app-level error callback instead of replacing it.
    // ce_main sets a callback that counts consecutive write failures to
    // drive the self-heal reconnect; clobbering it here (old behavior)
    // meant g_write_failures never incremented and a dead box link looped
    // on write errors forever (carplay.log 07:50 session: 70+ write
    // failures, zero self-heal attempts, until the user clicked EXIT).
    auto app_cb = transport_->errorCallback();
    transport_->setErrorCallback([this, app_cb](int err) {
        if (app_cb) app_cb(err);
        // Surface as state ERROR + fire on_state (best-effort).
        {
            LockGuard lock(mtx_);
            state_ = ConnState::ERROR_STATE;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::ERROR_STATE);
    });
    transport_->submitRead(64 * 1024, [this](const uint8_t *d, std::size_t n) {
        onRecv(d, n);
    });
    // In FastCarPlay, no empty OPEN/VERSION is sent here; sendInit() sends the
    // full handshake (including CMD_OPEN with 28-byte payload).
    {
        LockGuard lock(mtx_);
        state_ = ConnState::NO_DEVICE;
    }
    if (cbs_.on_state) cbs_.on_state(ConnState::NO_DEVICE);
}

void Connection::sendInit(bool encrypted, int work_mode) {
    // Full FastCarPlay-aligned session init. The box needs the complete
    // sequence — especially JSON_CONTROL (syncTime/HiCarConnectMode/
    // GNSSCapability/androidAutoSize…) and DAYNIGHT — to negotiate CarPlay
    // with the iPhone. Without them the box starts a partial session that
    // immediately tears down (audio pop → return to car UI, phone never
    // shows the CarPlay icon).
    if (encrypted) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        const uint32_t seed = static_cast<uint32_t>(std::rand());
        cipher_.init_seed(seed);
        send(Message::Encryption(seed));
    }
    const int drivePosition = 0;  // left-hand drive
    const int nightMode = 2;      // auto
    const int width = 1024;
    const int height = 600;
    const int fps = 30;
    const int syncTime = static_cast<int>(std::time(nullptr));
    const int mediaDelay = 0;

    // FastCarPlay order: dpi + work_mode BEFORE Init.
    // Inter-message delay: give embedded WinCE USB host controller and box gadget FIFO
    // sufficient pacing (80-120ms) to process and latch each config file before the next.
    // Blasting 26 writes back-to-back causes gadget endpoint 0x02 to stall.
    send(Message::File("/tmp/screen_dpi", 120));
    Sleep(80);
    // AutoKit reverse-engineered: 2 = WORK_MODE_IPHONE_CARPLAY, 1 = WORK_MODE_ANDROID_AUTO
    send(Message::File("/etc/android_work_mode", work_mode));
    Sleep(80);
    send(Message::Init(width, height, fps));
    Sleep(120);
    // AutoKit reverse-engineered session activation & phone discovery
    send(Message::Control(13));   // AutoKit standard session control 13
    Sleep(80);
    send(Message(Cmd::CMD_9, 1, /*encrypt=*/false)); // AutoKit phone discovery command (Cmd 9, val 1)
    Sleep(80);

    // JSON_CONTROL: critical session config.
    char json[256];
    std::snprintf(json, sizeof(json),
        "{\"syncTime\":%d,\"mediaDelay\":%d,\"drivePosition\":%d,"
        "\"androidAutoSizeW\":%d,\"androidAutoSizeH\":%d,\"HiCarConnectMode\":0,"
        "\"GNSSCapability\":7,\"DashboardInfo\":1,\"UseBTPhone\":0}",
        syncTime, mediaDelay, drivePosition, width, height);
    send(Message::String(Cmd::JSON_CONTROL, json));
    Sleep(120);

    // Day/night mode JSON.
    char dn[64];
    std::snprintf(dn, sizeof(dn), "{\"DayNightMode\":%d}", nightMode);
    send(Message::String(Cmd::DAYNIGHT, dn));
    Sleep(80);

    send(Message::File("/tmp/night_mode", nightMode));
    Sleep(80);
    send(Message::File("/tmp/charge_mode", 2));  // normal/strong charge
    Sleep(80);
    send(Message::File("/etc/box_name", "CarPlay"));
    Sleep(80);
    send(Message::File("/tmp/hand_drive_mode", drivePosition));
    Sleep(80);

    send(Message::Control(7));     // microphone (micType 1)
    Sleep(80);
    send(Message::Control(24));    // wifi channel (wifi5=false → 24)
    Sleep(80);
    send(Message::Control(23));    // bluetooth audio off (→ 23)
    Sleep(80);
    send(Message::Control(1002));  // autoconnect
    Sleep(400);  // let the box process autoconnect and settle gadget endpoints before heartbeats start
}

void Connection::stop() {
    if (transport_) transport_->close();
    {
        LockGuard lock(mtx_);
        state_ = ConnState::UNKNOWN;
    }
}

void Connection::send(Cmd cmd, const std::vector<uint8_t> &payload) {
    Message msg(cmd, /*encrypt=*/cipher_enabled_, static_cast<int32_t>(payload.size()),
               payload.empty() ? nullptr : const_cast<uint8_t *>(payload.data()));
    send(msg);
}

void Connection::send(const Message &in) {
    if (!transport_) return;
    Message msg = in;
    if (cipher_enabled_ && msg.encrypted() && msg.payload_size() > 0) {
        // CFB128 stream mode: no padding needed, any length.
        std::vector<uint8_t> enc(msg.payload_size());
        char err[128];
        if (!cipher_.encrypt_cfb(msg.payload(), msg.payload_size(), enc.data(), err)) {
            throw std::runtime_error(std::string("Connection::send encrypt: ") + err);
        }
        msg = Message(msg.type(), false, static_cast<int32_t>(enc.size()), enc.data());
        msg.mark_encrypted();  // magic -> MAGIC_ENC (box reads encrypted frames)
    }
    // AutoKit & FastCarPlay protocol specification:
    // Every packet is sent across USB Bulk OUT in two distinct transfers:
    // Transfer 1: 16-byte header
    // Transfer 2: payload (only if payload_size > 0)
    // Sending header + payload combined into one bulk URB causes the dongle firmware's
    // 16-byte header read to truncate/desync the gadget FIFO, stalling endpoint 0x02.
    transport_->write(reinterpret_cast<const uint8_t *>(&msg.header()), sizeof(Header));
    if (msg.payload_size() > 0) {
        Sleep(5);  // 5ms delay to let box firmware complete 16-byte header read before reading payload
        transport_->write(msg.payload(), msg.payload_size());
    }
}

void Connection::onRecv(const uint8_t *data, std::size_t len) {
    if (!data || len == 0) return;

    // Append newly arrived bytes to stream buffer
    rx_buf_.insert(rx_buf_.end(), data, data + len);

    while (rx_buf_.size() >= sizeof(Header)) {
        Header h;
        std::memcpy(&h, rx_buf_.data(), sizeof(Header));

        // Strict header validation:
        // 1. Magic must match MAGIC (plain) or MAGIC_ENC (encrypted)
        // 2. typecheck MUST equal ~type (Carlinkit protocol checksum)
        // 3. length must be non-negative and <= MESSAGE_MAX_PAYLOAD
        bool valid_magic = (h.magic == MAGIC || h.magic == MAGIC_ENC);
        bool valid_checksum = (h.typecheck == ~h.type);
        bool valid_len = (h.length >= 0 && static_cast<uint32_t>(h.length) <= MESSAGE_MAX_PAYLOAD);

        if (!valid_magic || !valid_checksum || !valid_len) {
            std::size_t sync_off = 1;
            while (sync_off + sizeof(Header) <= rx_buf_.size()) {
                Header candidate;
                std::memcpy(&candidate, rx_buf_.data() + sync_off, sizeof(Header));
                if ((candidate.magic == MAGIC || candidate.magic == MAGIC_ENC) &&
                    (candidate.typecheck == ~candidate.type) &&
                    (candidate.length >= 0 && static_cast<uint32_t>(candidate.length) <= MESSAGE_MAX_PAYLOAD)) {
                    break;
                }
                sync_off++;
            }
            if (sync_off + sizeof(Header) > rx_buf_.size()) {
                // Not enough bytes remaining in buffer to confirm a valid header.
                // Keep the trailing tail (up to sizeof(Header) - 1 bytes) and wait for more data.
                std::size_t keep = std::min(rx_buf_.size(), sizeof(Header) - 1);
                if (rx_buf_.size() > keep) {
                    std::fprintf(stderr, "[USB-IN] lost sync, dropped %zu bytes\n", rx_buf_.size() - keep);
                    rx_buf_.erase(rx_buf_.begin(), rx_buf_.end() - keep);
                }
                break;
            }
            std::fprintf(stderr, "[USB-IN] resynced, dropped %zu bytes\n", sync_off);
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + sync_off);
            continue;
        }

        std::size_t total_msg_size = sizeof(Header) + static_cast<std::size_t>(h.length);
        if (rx_buf_.size() < total_msg_size) {
            // Incomplete message; wait for remaining bytes in subsequent reads
            break;
        }

        std::fprintf(stderr, "[USB-IN] magic=0x%08X type=%u len=%d\n",
                     h.magic, h.type, h.length);

        try {
            Message msg = Message::parse(rx_buf_.data(), total_msg_size);
            if (msg.encrypted()) {
                if (msg.payload_size() > 0) {
                    std::vector<uint8_t> plain(msg.payload_size());
                    char err[128];
                    if (cipher_.decrypt_cfb(msg.payload(), msg.payload_size(), plain.data(), err)) {
                        msg = Message(msg.type(), false,
                                      static_cast<int32_t>(plain.size()), plain.data());
                    }
                }
            }
            dispatch(msg);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[USB-IN] parse/dispatch error: %s\n", e.what());
        }

        rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + total_msg_size);
    }
}

void Connection::dispatch(const Message &msg) {
    auto p = msg.payload();
    auto n = msg.payload_size();
    if (cbs_.on_any) cbs_.on_any(msg.type(), p, n);
    switch (msg.type()) {
    case Cmd::PLUGGED: {
        bool need_reinit;
        {
            LockGuard lock(mtx_);
            state_ = ConnState::ONLINE;
            need_reinit = was_unplugged_;
            was_unplugged_ = false;
        }
        if (cbs_.on_plugged) cbs_.on_plugged();
        if (cbs_.on_state) cbs_.on_state(ConnState::ONLINE);
        // Re-send init after a re-plug: the box's USB gadget resets
        // when switching to CarPlay mode, losing its configuration.
        if (need_reinit) {
            try { sendInit(); } catch (...) {}
        }
        break;
    }
    case Cmd::UNPLUGGED:
        if (cbs_.on_unplugged) cbs_.on_unplugged();
        {
            LockGuard lock(mtx_);
            state_ = ConnState::NO_DEVICE;
            was_unplugged_ = true;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::NO_DEVICE);
        break;
    case Cmd::STATE:
        if (cbs_.on_state) {
            int32_t s = (n >= 4) ? static_cast<int32_t>(Message::read_u32_le(p)) : 0;
            ConnState new_state = (s >= 0 && s <= 5)
                                   ? static_cast<ConnState>(s)
                                   : ConnState::UNKNOWN;
            {
                LockGuard lock(mtx_);
                state_ = new_state;
            }
            cbs_.on_state(new_state);
        }
        break;
    case Cmd::CONTROL:
    case Cmd::JSON_CONTROL:
        if (cbs_.on_control) cbs_.on_control(msg.type(), p, n);
        break;
    case Cmd::VIDEO_DATA:
        if (cbs_.on_video) cbs_.on_video(msg.type(), p, n);
        break;
    case Cmd::AUDIO_DATA:
        if (cbs_.on_audio) cbs_.on_audio(msg.type(), p, n);
        break;
    case Cmd::HEARTBEAT:
        send(Cmd::HEARTBEAT);  // immediate reply (keepalive)
        break;
    case Cmd::ENCRYPTION:
        if (n == 0) {
            cipher_enabled_ = true;
            std::fprintf(stderr, "[ENCRYPT] Box acknowledged encryption handshake (len 0)\n");
        }
        break;
    case Cmd::VERSION:
    case Cmd::DEVICE_LIST:
    case Cmd::APP_INFO:
    case Cmd::MEDIA_INFO:
    case Cmd::BLUETOOTH:
    case Cmd::WIFI_INFO:
    case Cmd::DAYNIGHT:
    case Cmd::MANUFACTURER:
    case Cmd::MULTI_TOUCH:
    case Cmd::SEND_FILE:
        // Future: route to negotiation handler.
        break;
    default:
        break;
    }
}

} // namespace carlinkit