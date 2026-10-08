#include "connection.h"
#include "message.h"

#include <cstring>

namespace carlinkit {

void Connection::start() {
    if (!transport_) throw std::runtime_error("Connection: no transport");
    transport_->open(CARLINKIT_VENDOR_ID, CARLINKIT_PRODUCT_ID);
    if (!transport_->claim()) {
        transport_->close();
        throw std::runtime_error("Connection: claim failed");
    }
    transport_->setErrorCallback([this](int err) {
        // Surface as state ERROR + fire on_state (best-effort).
        {
            std::lock_guard<Mutex> lock(mtx_);
            state_ = ConnState::ERROR_STATE;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::ERROR_STATE);
    });
    transport_->submitRead(64 * 1024, [this](const uint8_t *d, std::size_t n) {
        onRecv(d, n);
    });
    // OPEN message: payload can be empty to probe basic; real impl carries
    // host info (FastCarPlay sends 0x88 byte setup blob).
    {
        std::lock_guard<Mutex> lock(mtx_);
        state_ = ConnState::NO_DEVICE;
    }
    send(Cmd::OPEN);
    send(Cmd::VERSION);
    if (cbs_.on_state) cbs_.on_state(ConnState::NO_DEVICE);
}

void Connection::stop() {
    if (transport_) transport_->close();
    {
        std::lock_guard<Mutex> lock(mtx_);
        state_ = ConnState::UNKNOWN;
    }
}

void Connection::send(Cmd cmd, const std::vector<uint8_t> &payload) {
    if (!transport_) return;
    Message msg(cmd, /*encrypt=*/cipher_enabled_, static_cast<int32_t>(payload.size()),
               payload.empty() ? nullptr : const_cast<uint8_t *>(payload.data()));
    if (cipher_enabled_ && !payload.empty()) {
        // Pad to 16-byte boundary (AES block)
        std::vector<uint8_t> padded = payload;
        while (padded.size() % 16 != 0) padded.push_back(0);
        std::vector<uint8_t> enc(padded.size());
        char err[128];
        if (!cipher_.encrypt(padded.data(), padded.size(), enc.data(), err)) {
            throw std::runtime_error(std::string("Connection::send encrypt: ") + err);
        }
        msg = Message(cmd, cipher_enabled_, padded.size(), enc.data());
    }
    auto out = msg.serialize();
    transport_->write(out.data(), out.size());
}

void Connection::onRecv(const uint8_t *data, std::size_t len) {
    if (!data || len < 16) return;
    // A single bulk read may contain multiple frames back-to-back.
    std::size_t off = 0;
    while (off + sizeof(Header) <= len) {
        Header h;
        std::memcpy(&h, data + off, sizeof(Header));
        if (h.magic != MAGIC && h.magic != MAGIC_ENC)
            break;  // lost sync; drop the rest of this read
        if (h.length < 0)
            break;
        std::size_t frame = sizeof(Header) + static_cast<std::size_t>(h.length);
        if (off + frame > len)
            break;  // incomplete tail; dropped (no cross-read buffering yet)

        Message msg = Message::parse(data + off, frame);
        if (msg.encrypted()) {
            if (msg.payload_size() == 0 || msg.payload_size() % 16 != 0)
                break;  // malformed encrypted length; drop rest
            std::vector<uint8_t> plain(msg.payload_size());
            char err[128];
            if (!cipher_.decrypt(msg.payload(), msg.payload_size(), plain.data(), err))
                break;
            msg = Message(msg.type(), false,
                          static_cast<int32_t>(plain.size()), plain.data());
        }
        dispatch(msg);
        off += frame;
    }
}

void Connection::dispatch(const Message &msg) {
    auto p = msg.payload();
    auto n = msg.payload_size();
    if (cbs_.on_any) cbs_.on_any(msg.type(), p, n);
    switch (msg.type()) {
    case Cmd::PLUGGED:
        if (cbs_.on_plugged) cbs_.on_plugged();
        {
            std::lock_guard<Mutex> lock(mtx_);
            state_ = ConnState::ONLINE;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::ONLINE);
        break;
    case Cmd::UNPLUGGED:
        if (cbs_.on_unplugged) cbs_.on_unplugged();
        {
            std::lock_guard<Mutex> lock(mtx_);
            state_ = ConnState::ONLINE;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::ONLINE);
        break;
    case Cmd::STATE:
        if (cbs_.on_state) {
            int32_t s = (n >= 4) ? static_cast<int32_t>(Message::read_u32_le(p)) : 0;
            ConnState new_state = (s >= 0 && s <= 5)
                                   ? static_cast<ConnState>(s)
                                   : ConnState::UNKNOWN;
            {
                std::lock_guard<Mutex> lock(mtx_);
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
    case Cmd::VERSION:
    case Cmd::ENCRYPTION:
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