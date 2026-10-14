#pragma once
// Packet header struct (packed, 16 bytes)
//
// Memory layout (little-endian):
//  offset 0  : u32  magic       0x55AA55AA plain / 0x55BB55BB encrypted
//  offset 4  : s32  length       payload size in bytes (may be 0)
//  offset 8  : u32  type         Cmd (see protocol_const.h)
//  offset 12 : u32  typecheck    ~type  (used by Box to validate)
//
// See docs/CARLINKIT_PROTOCOL.md section 3.

#ifndef CARLINKIT_MESSAGE_H
#define CARLINKIT_MESSAGE_H

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "protocol_const.h"

// Message/Header live at global scope (matches FastCarPlay layout);
// pull the carlinkit constants (Cmd/MAGIC/MAGIC_ENC) into view.
using namespace carlinkit;

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

struct Header {
    uint32_t magic;       // MAGIC or MAGIC_ENC
    int32_t  length;      // payload size (signed to match FastCarPlay)
    uint32_t type;        // Cmd
    uint32_t typecheck;   // ~type
};

#ifdef _MSC_VER
#pragma pack(pop)
#endif

static_assert(sizeof(Header) == 16, "Header must be 16 bytes");

class Message {
public:
    Message() : hdr_{MAGIC, 0, 0, 0}, encrypted_(false) {}

    Message(Cmd cmd, bool encrypt = true, int32_t size = 0, uint8_t *buffer = nullptr)
        : hdr_{MAGIC, size, static_cast<uint32_t>(cmd), ~static_cast<uint32_t>(cmd)},
          encrypted_(encrypt)
    {
        if (size > 0 && buffer) {
            payload_.assign(buffer, buffer + size);
        } else if (size > 0) {
            payload_.resize(size);
        }
    }

    explicit Message(Cmd cmd, uint32_t value, bool encrypt = true)
        : Message(cmd, encrypt, 4)
    {
        if (payload_.size() >= 4) {
            payload_[0] = static_cast<uint8_t>(value & 0xFF);
            payload_[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            payload_[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            payload_[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        }
    }

    // ----- Static factories (mirror FastCarPlay message.h) -----

    // CMD_OPEN with the 28-byte host-info payload the Box expects:
    // [0]=width [4]=height [8]=fps [12]=5 [16]=49152 [20]=2 [24]=2 (all LE)
    static Message Init(int width, int height, int fps) {
        Message m(Cmd::OPEN, /*encrypt=*/true, 28);
        m.write_u32(0, static_cast<uint32_t>(width));
        m.write_u32(4, static_cast<uint32_t>(height));
        m.write_u32(8, static_cast<uint32_t>(fps));
        m.write_u32(12, 5);
        m.write_u32(16, 49152);  // 0xC000 matching FastCarPlay specification
        m.write_u32(20, 2);      // 2 matching FastCarPlay specification
        m.write_u32(24, 2);
        return m;
    }

    // CMD_SEND_FILE: [fn_len LE][filename incl '\0'][content_len LE][content]
    static Message File(const char *filename, const uint8_t *data, uint32_t length) {
        const uint32_t fn_len = static_cast<uint32_t>(std::strlen(filename)) + 1;
        Message m(Cmd::SEND_FILE, /*encrypt=*/true,
                  static_cast<int32_t>(4 + fn_len + 4 + length));
        uint8_t *p = m.mutable_payload();
        write_u32_le(p, fn_len);
        p += 4;
        std::memcpy(p, filename, fn_len);
        p += fn_len;
        write_u32_le(p, length);
        p += 4;
        if (length > 0 && data)
            std::memcpy(p, data, length);
        return m;
    }

    // String value: ASCII bytes only, no trailing '\0' (max 16)
    static Message File(const char *filename, const char *value) {
        const uint32_t len = static_cast<uint32_t>(std::strlen(value));
        if (len > 16)
            throw std::invalid_argument("Message::File: string too long (max 16)");
        return File(filename, reinterpret_cast<const uint8_t *>(value), len);
    }

    // Single 32-bit integer value
    static Message File(const char *filename, int value) {
        uint8_t buf[4];
        write_u32_le(buf, static_cast<uint32_t>(value));
        return File(filename, buf, 4);
    }

    // Raw string payload (no trailing '\0')
    static Message String(Cmd cmd, const char *str) {
        const int32_t len = static_cast<int32_t>(std::strlen(str));
        Message m(cmd, /*encrypt=*/true, len);
        if (len > 0)
            std::memcpy(m.mutable_payload(), str, static_cast<std::size_t>(len));
        return m;
    }

    // CMD_CONTROL: 4-byte LE value, sent unencrypted (FastCarPlay default)
    static Message Control(uint32_t value) {
        return Message(Cmd::CONTROL, value, /*encrypt=*/false);
    }

    // CMD_ENCRYPTION: 4-byte LE seed, sent unencrypted
    static Message Encryption(uint32_t seed) {
        return Message(Cmd::ENCRYPTION, seed, /*encrypt=*/false);
    }

    // CMD_TOUCH: 16-byte payload (action, x, y, 0), sent unencrypted
    // action: 14 = DOWN, 15 = MOVE, 16 = UP
    // x, y: 0..10000 (normalized * 10000)
    static Message Touch(uint32_t action, uint32_t x, uint32_t y) {
        Message m(Cmd::TOUCH, /*encrypt=*/false, 16);
        write_u32_le(m.mutable_payload(), action);
        write_u32_le(m.mutable_payload() + 4, x);
        write_u32_le(m.mutable_payload() + 8, y);
        write_u32_le(m.mutable_payload() + 12, 0);
        return m;
    }

    // Accessors
    Cmd type() const { return static_cast<Cmd>(hdr_.type); }
    bool encrypted() const { return encrypted_; }
    int32_t length() const { return hdr_.length; }
    const uint8_t *payload() const { return payload_.data(); }
    uint8_t *mutable_payload() { return payload_.data(); }
    std::size_t payload_size() const { return payload_.size(); }
    const Header &header() const { return hdr_; }

    // Read/write primitives (little-endian)
    static uint32_t read_u32_le(const uint8_t *p) {
        return static_cast<uint32_t>(p[0])        |
               (static_cast<uint32_t>(p[1]) << 8)  |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    static void write_u32_le(uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
    uint32_t read_u32(std::size_t off) const {
        return read_u32_le(payload_.data() + off);
    }
    void write_u32(std::size_t off, uint32_t v) {
        write_u32_le(payload_.data() + off, v);
    }

    // Set header magic after encryption (must call before serialize())
    void mark_encrypted() { hdr_.magic = MAGIC_ENC; }

    // Serialize to bytes for USB write (header + payload)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(sizeof(Header) + payload_.size());
        std::memcpy(out.data(), &hdr_, sizeof(Header));
        if (!payload_.empty())
            std::memcpy(out.data() + sizeof(Header), payload_.data(), payload_.size());
        return out;
    }

    // Parse from raw USB recv. Throws on bad header.
    // Caller is responsible for decrypting payload BEFORE constructing Message.
    static Message parse(const uint8_t *buf, std::size_t n) {
        if (n < sizeof(Header))
            throw std::runtime_error("Message: buffer too small");
        Message m;
        std::memcpy(&m.hdr_, buf, sizeof(Header));
        if (m.hdr_.magic != MAGIC && m.hdr_.magic != MAGIC_ENC)
            throw std::runtime_error("Message: bad magic");
        if (m.hdr_.length < 0)
            throw std::runtime_error("Message: negative length");
        // typecheck mismatch is non-fatal: FastCarPlay logs warning but does not discard
        if (sizeof(Header) + static_cast<std::size_t>(m.hdr_.length) > n)
            throw std::runtime_error("Message: incomplete buffer");
        m.encrypted_ = (m.hdr_.magic == MAGIC_ENC);
        m.payload_.assign(buf + sizeof(Header), buf + sizeof(Header) + m.hdr_.length);
        return m;
    }

private:
    Header hdr_;
    bool encrypted_;
    std::vector<uint8_t> payload_;
};

namespace carlinkit {
using ::Message;
using ::Header;
}

#endif // CARLINKIT_MESSAGE_H