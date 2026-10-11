#include "connection.h"
#include "message.h"

#include <cstring>
#include <ctime>

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
            LockGuard lock(mtx_);
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
        LockGuard lock(mtx_);
        state_ = ConnState::NO_DEVICE;
    }
    send(Cmd::OPEN);
    send(Cmd::VERSION);
    if (cbs_.on_state) cbs_.on_state(ConnState::NO_DEVICE);
}

void Connection::sendInit() {
    // Mirrors FastCarPlay Connection::sendInit() with default settings
    // (encryption off, left-hand-drive, night auto, weak charge, wifi5,
    // bt audio off, autoconnect on, micType 1, dpi 120, androidMode 1).
    const int32_t syncTime = static_cast<int32_t>(std::time(nullptr));
    const int drivePosition = 0;  // left-hand drive
    const int nightMode = 2;      // auto

    // androidMode 1 -> 800x480 base, scaled to Settings 1024x576 window:
    // scale = min(800/1024, 480/576) = 0.78125 -> 800x450
    const int autoSizeW = 800, autoSizeH = 450;

    send(Message::File("/tmp/screen_dpi", 120));
    send(Message::File("/etc/android_work_mode", 1));
    send(Message::Init(1024, 576, 60));
    send(Message::String(Cmd::JSON_CONTROL,
        "{\"syncTime\":" + std::to_string(syncTime) + ",\"mediaDelay\":300,"
        "\"drivePosition\":" + std::to_string(drivePosition) + ","
        "\"androidAutoSizeW\":" + std::to_string(autoSizeW) + ","
        "\"androidAutoSizeH\":" + std::to_string(autoSizeH) + ","
        "\"HiCarConnectMode\":0,\"GNSSCapability\":7,\"DashboardInfo\":1,"
        "\"UseBTPhone\":0}"));
    send(Message::String(Cmd::DAYNIGHT,
        "{\"DayNightMode\":" + std::to_string(nightMode) + "}"));

    send(Message::File("/tmp/night_mode", nightMode));
    send(Message::File("/tmp/charge_mode", 0));  // weak charge
    send(Message::File("/etc/box_name", "CarPlay"));
    send(Message::File("/tmp/hand_drive_mode", drivePosition));

    send(Message::Control(7));    // microphone
    send(Message::Control(25));   // wifi 5GHz
    send(Message::Control(23));   // bluetooth audio off
    send(Message::Control(1002)); // autoconnect
}

void Connection::stop() {
    if (transport_) transport_->close();
    {
        LockGuard lock(mtx_);
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
    // FastCarPlay sends the 16-byte header and the payload as two separate
    // bulk transfers; the short header packet terminates the first transfer
    // so the Box's reader always sees a clean header first.
    transport_->write(out.data(), sizeof(Header));
    if (out.size() > sizeof(Header))
        transport_->write(out.data() + sizeof(Header),
                          out.size() - sizeof(Header));
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
            LockGuard lock(mtx_);
            state_ = ConnState::ONLINE;
        }
        if (cbs_.on_state) cbs_.on_state(ConnState::ONLINE);
        break;
    case Cmd::UNPLUGGED:
        if (cbs_.on_unplugged) cbs_.on_unplugged();
        {
            LockGuard lock(mtx_);
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