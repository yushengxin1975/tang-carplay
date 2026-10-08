// Unit tests for Phase B.1 protocol layer.
// Build via top-level Makefile (make run).
#include "protocol/message.h"
#include "protocol/aes_cipher.h"
#include "protocol/connection.h"
#include "protocol_const.h"
#include "fake_transport.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using carlinkit::test::FakeTransport;
using namespace carlinkit;

static int g_passed = 0;
static int g_failed = 0;

static void expect_(bool cond, const char *msg) {
    if (cond) { g_passed++; std::printf("  [PASS] %s\n", msg); }
    else      { g_failed++; std::printf("  [FAIL] %s\n", msg); }
}

#define EXPECT(cond, msg) expect_(!!(cond), msg)

// --- 1. AES-128-CBC encrypt/decrypt roundtrip ---
static void test_aes_roundtrip() {
    std::printf("[test_aes_roundtrip]\n");
    uint8_t key[16] = {0}, iv[16] = {0};
    uint8_t pt[32];
    for (int i = 0; i < 32; ++i) pt[i] = (uint8_t)(0x10 * (i % 16));
    AESCipher c;
    c.init(key, iv);
    uint8_t ct[32], rt[32];
    EXPECT(c.encrypt(pt, 32, ct), "AES encrypt ok");
    EXPECT(c.decrypt(ct, 32, rt), "AES decrypt ok");
    EXPECT(std::memcmp(rt, pt, 32) == 0, "roundtrip plaintext matches");
    EXPECT(std::memcmp(ct, pt, 32) != 0, "ciphertext differs from plaintext");
}

// --- 1b. NIST FIPS-197 Appendix C.1 known-answer test ---
// key=000102...0f, PT=00112233...ff, CT=69c4e0d86a7b0430d8cdb78070b4c55a
// (CBC with zero IV: first block == ECB vector)
static void test_aes_nist_kat() {
    std::printf("[test_aes_nist_kat]\n");
    const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                             0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    const uint8_t pt[16]  = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                             0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    const uint8_t want[16]= {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                             0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    const uint8_t iv0[16] = {0};
    uint8_t ct[16], rt[16];
    AESCipher c;
    c.init(key, iv0);
    EXPECT(c.encrypt(pt, 16, ct), "NIST: encrypt runs");
    EXPECT(std::memcmp(ct, want, 16) == 0, "NIST C.1 ciphertext matches KAT");
    EXPECT(c.decrypt(ct, 16, rt), "NIST: decrypt runs");
    EXPECT(std::memcmp(rt, pt, 16) == 0, "NIST C.1 decrypt returns plaintext");
}

// --- 2. Message serialize / parse roundtrip (plain) ---
static void test_message_roundtrip() {
    std::printf("[test_message_roundtrip]\n");
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50};
    Message m(Cmd::VIDEO_DATA, false, (int)payload.size(), payload.data());
    auto bytes = m.serialize();
    EXPECT(bytes.size() == sizeof(Header) + payload.size(), "serialize size");
    EXPECT(bytes[0] == 0xAA && bytes[1] == 0x55, "magic byte0-1 = MAGIC LE");
    Message m2 = Message::parse(bytes.data(), bytes.size());
    EXPECT(m2.type() == Cmd::VIDEO_DATA, "roundtrip cmd preserved");
    EXPECT(m2.length() == (int)payload.size(), "roundtrip length");
    EXPECT(std::memcmp(m2.payload(), payload.data(), payload.size()) == 0,
           "roundtrip payload bytes");
    EXPECT(!m2.encrypted(), "roundtrip not encrypted");
}

// --- 3. Message encrypted path (manual CBC + mark_encrypted) ---
static void test_message_encrypted() {
    std::printf("[test_message_encrypted]\n");
    AESCipher c;
    uint8_t k[16];
    for (int i = 0; i < 16; ++i) k[i] = (uint8_t)(0x11 * (i + 1));
    uint8_t iv[16] = {0};
    c.init(k, iv);
    std::vector<uint8_t> payload = {0xAA, 0xBB, 0xCC};
    std::vector<uint8_t> padded = payload;
    while (padded.size() % 16 != 0) padded.push_back(0);
    std::vector<uint8_t> enc(padded.size());
    c.encrypt(padded.data(), padded.size(), enc.data());
    Message msg(Cmd::AUDIO_DATA, true, (int)enc.size(), enc.data());
    msg.mark_encrypted();
    auto bytes = msg.serialize();
    EXPECT(bytes[0] == 0xBB && bytes[1] == 0x55, "MAGIC_ENC after mark_encrypted");

    Message parsed = Message::parse(bytes.data(), bytes.size());
    EXPECT(parsed.encrypted(), "parsed msg is encrypted");
    std::vector<uint8_t> plain(parsed.payload_size());
    c.decrypt(parsed.payload(), parsed.payload_size(), plain.data());
    EXPECT(std::memcmp(plain.data(), payload.data(), payload.size()) == 0,
           "decrypted plaintext matches original");
}

// --- 4. Connection lifecycle via FakeTransport ---
static int s_state_changes = 0;
static ConnState s_last_state = ConnState::UNKNOWN;
static int s_plugged = 0, s_unplugged = 0;

static void on_state(ConnState s) { s_state_changes++; s_last_state = s; }
static void on_p() { s_plugged++; }
static void on_u() { s_unplugged++; }

static void makeMsg(std::vector<uint8_t> &out, Cmd type) {
    out.assign(16, 0);
    Message::write_u32_le(out.data(), MAGIC);
    Message::write_u32_le(out.data() + 4, 0);
    Message::write_u32_le(out.data() + 8, static_cast<uint32_t>(type));
    Message::write_u32_le(out.data() + 12, ~static_cast<uint32_t>(type));
}

static void test_lifecycle() {
    std::printf("[test_lifecycle]\n");
    s_state_changes = 0; s_plugged = 0; s_unplugged = 0;
    s_last_state = ConnState::UNKNOWN;
    InboundCallbacks cb;
    cb.on_state = on_state;
    cb.on_plugged = on_p;
    cb.on_unplugged = on_u;

    // We need a Connection that we can also drive via FakeTransport.
    // Trick: instantiate FakeTransport directly, take its address, pass
    // &as UsbTransport*. Connection stores the ptr; lifetime is OK because
    // FakeTransport outlives the Connection in this scope.
    FakeTransport ft;
    {
        InboundCallbacks cb2 = cb;
        std::unique_ptr<UsbTransport> up(reinterpret_cast<UsbTransport *>(&ft));
        Connection conn(std::move(up), cb2);
        conn.start();
        EXPECT(conn.state() == ConnState::NO_DEVICE, "after start: NO_DEVICE");

        // CMD_PLUGGED
        std::vector<uint8_t> b; makeMsg(b, Cmd::PLUGGED);
        ft.deliver(b);
        EXPECT(s_plugged == 1, "PLUGGED fired");
        EXPECT(s_last_state == ConnState::ONLINE, "state -> ONLINE");

        // CMD_UNPLUGGED
        makeMsg(b, Cmd::UNPLUGGED);
        ft.deliver(b);
        EXPECT(s_unplugged == 1, "UNPLUGGED fired");

        // HEARTBEAT -> auto reply (16 bytes more sent)
        size_t before = ft.sent().size();
        makeMsg(b, Cmd::HEARTBEAT);
        ft.deliver(b);
        EXPECT(ft.sent().size() - before == 16, "heartbeat replied with 16-byte empty msg");
        // Verify the reply is HEARTBEAT type (bytes 8-11 little-endian)
        if (ft.sent().size() >= before + 12) {
            uint32_t rpl_type = Message::read_u32_le(ft.sent().data() + before + 8);
            EXPECT(rpl_type == static_cast<uint32_t>(Cmd::HEARTBEAT),
                   "reply type == HEARTBEAT");
        }
    }
    // Connection destructs first, FakeTransport still alive in scope
    EXPECT(!ft.is_open(), "after scope: ft.is_open()? (auto close via dtor?)");
    // FakeTransport.close() was called by Connection.stop() (in dtor via close()? - we rely on Connection.stop())
}

int main() {
    std::printf("=== Carlinkit protocol unit tests ===\n");
    test_aes_roundtrip();
    test_aes_nist_kat();
    test_message_roundtrip();
    test_message_encrypted();
    test_lifecycle();
    std::printf("=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}