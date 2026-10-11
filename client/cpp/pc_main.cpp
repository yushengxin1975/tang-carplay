// pc_main.cpp — Carlinkit PC console client (Phase B.1 end-to-end bring-up)
//
// Purpose: verify the protocol layer against the REAL Carlinkit box from a
// desktop, no car needed. Plug the box into a PC USB port (it enumerates as
// 0x1314:0x1520), run this exe, and watch the handshake + stream stats.
//
// Requirements: libusb-1.0 runtime only (loaded dynamically):
//   Linux:   apt install libusb-1.0-0
//   Windows: libusb-1.0.dll from libusb.info next to this exe
//
// What it does:
//   1. open box, claim interface
//   2. Connection::start() -> CMD_OPEN + CMD_VERSION
//   3. heartbeat every 1s
//   4. prints every inbound message: type, size, first bytes
//   5. counts video/audio frames; dumps first 2MB of each to
//      sample_video.bin / sample_audio.bin for offline ffmpeg analysis
//   6. Ctrl+C to stop cleanly

#include "protocol/connection.h"
#include "protocol/usb_transport.h"

#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>

using namespace carlinkit;

static std::atomic<bool> g_run{true};

static void onSigint(int) { g_run = false; }
static std::atomic<uint64_t> g_video_bytes{0}, g_audio_bytes{0};
static std::atomic<uint32_t> g_video_frames{0}, g_audio_frames{0};
static std::atomic<uint32_t> g_msg_counts[32];

static const char *cmdName(uint32_t c) {
    switch (c) {
    case 1: return "OPEN";       case 2: return "PLUGGED";
    case 3: return "STATE";      case 4: return "UNPLUGGED";
    case 5: return "TOUCH";      case 6: return "VIDEO";
    case 7: return "AUDIO";      case 8: return "CONTROL";
    case 10: return "APP_INFO";  case 13: return "BT_INFO";
    case 14: return "WIFI_INFO"; case 18: return "DEV_LIST";
    case 20: return "MANUFACTURER"; case 23: return "MULTI_TOUCH";
    case 25: return "JSON_CTRL"; case 42: return "MEDIA_INFO";
    case 153: return "SEND_FILE"; case 162: return "DAYNIGHT";
    case 170: return "HEARTBEAT"; case 204: return "VERSION";
    case 240: return "ENCRYPTION";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    std::signal(SIGINT, onSigint);
    // Unbuffered stdout: when redirected to a file, printf output must
    // land immediately or a killed process loses the whole log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 2018.06.20 CPC200-CMAP dongle runs unencrypted protocol.
    // Encrypted protocol is only used on 2024+ firmware (FastCarPlay settings.txt).
    // Default to plain mode; use --enc to enable CFB128 encryption.
    bool encrypted = false;
    UsbTransport_PC::ClaimOptions co;
    // On Windows/WinUSB, set_interface_alt_setting(0,0) + clear_halt are
    // configured for the box to accept bulk OUT writes.
    co.alt_setting = true;
    co.clear_halt = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::printf("Usage: pc_main.exe [--plain] [--enc] [--no-alt] [--no-set-config] [--reset-reopen]\n");
            return 0;
        }
        else if (!std::strcmp(argv[i], "--plain")) encrypted = false;
        else if (!std::strcmp(argv[i], "--enc")) encrypted = true;
        else if (!std::strcmp(argv[i], "--reset-reopen")) co.reset_reopen = true;
        else if (!std::strcmp(argv[i], "--alt-setting")) co.alt_setting = true;
        else if (!std::strcmp(argv[i], "--clear-halt")) co.clear_halt = true;
        else if (!std::strcmp(argv[i], "--no-set-config")) co.no_set_config = true;
        else if (!std::strcmp(argv[i], "--no-alt")) { co.alt_setting = false; co.clear_halt = false; }
    }
    std::printf("=== Carlinkit PC client (protocol bring-up, %s) ===\n",
                encrypted ? "ENCRYPTED (CFB128)" : "plain");
    if (co.reset_reopen || co.alt_setting || co.clear_halt || co.no_set_config) {
        std::printf("claim options:%s%s%s%s\n",
                    co.reset_reopen  ? " reset-reopen" : "",
                    co.alt_setting   ? " alt-setting" : "",
                    co.clear_halt    ? " clear-halt" : "",
                    co.no_set_config ? " no-set-config" : "");
    }

    std::ofstream vout("sample_video.bin", std::ios::binary);
    std::ofstream aout("sample_audio.bin", std::ios::binary);
    uint64_t vdump = 0, adump = 0;

    InboundCallbacks cb;
    cb.on_state = [](ConnState s) {
        std::printf("[STATE] %d\n", static_cast<int>(s));
    };
    cb.on_plugged = []() { std::printf("[EVENT] iPhone PLUGGED into box\n"); };
    cb.on_unplugged = []() { std::printf("[EVENT] iPhone UNPLUGGED\n"); };
    cb.on_any = [&](Cmd c, const uint8_t *p, std::size_t n) {
        uint32_t idx = static_cast<uint32_t>(c) % 32;
        g_msg_counts[idx]++;
        if (c == Cmd::VIDEO_DATA) {
            g_video_bytes += n; g_video_frames++;
            if (vdump < 2 * 1024 * 1024 && vout.is_open()) {
                vout.write(reinterpret_cast<const char *>(p),
                           static_cast<std::streamsize>(n));
                vdump += n;
            }
        } else if (c == Cmd::AUDIO_DATA) {
            g_audio_bytes += n; g_audio_frames++;
            if (adump < 2 * 1024 * 1024 && aout.is_open()) {
                aout.write(reinterpret_cast<const char *>(p),
                           static_cast<std::streamsize>(n));
                adump += n;
            }
        } else {
            std::printf("[MSG %-11s] %4zu bytes", cmdName(static_cast<uint32_t>(c)), n);
            if (n >= 4) std::printf("  %02x %02x %02x %02x", p[0], p[1], p[2], p[3]);
            std::printf("\n");
        }
    };

    auto *t = new UsbTransport_PC();
    t->claim_opts = co;
    Connection *conn = nullptr;
    try {
        std::printf("opening box (0x1314:0x1520)...\n");
        t->open();
        if (!t->claim()) throw std::runtime_error("claim interface 0 failed");
        std::printf("claimed. starting protocol...\n");
        conn = new Connection(
            std::unique_ptr<UsbTransport>(reinterpret_cast<UsbTransport *>(t)), cb);
        conn->start();
        std::printf("sending config burst (sendInit, %s)...\n",
                    encrypted ? "encrypted" : "plain");
        conn->sendInit(encrypted);
        std::printf("config burst sent.\n");
    } catch (const std::exception &e) {
        std::printf("FATAL: %s\n", e.what());
        return 1;
    }

    std::printf("protocol up. plug iPhone into the box. Ctrl+C to stop.\n\n");

    // Main read pump + periodic stats + heartbeat
    uint64_t last_v = 0, last_a = 0;
    int stat_ticks = 0;
    int hb_ticks = 0;
    while (g_run) {
        if (!t->readPump(200)) {
            std::printf("USB error, exiting\n");
            break;
        }
        // Heartbeat every ~3s (FastCarPlay PROTOCOL_HEARTBEAT_DELAY) keeps
        // the Box session alive even before it starts talking.
        if (++hb_ticks >= 15) {
            hb_ticks = 0;
            conn->send(Cmd::HEARTBEAT);
        }
        if (++stat_ticks >= 25) {  // ~5s at 200ms
            stat_ticks = 0;
            uint64_t vb = g_video_bytes, ab = g_audio_bytes;
            std::printf("[STAT] video %u fr %llu KB (+%llu) | audio %u fr %llu KB (+%llu)\n",
                        g_video_frames.load(), (unsigned long long)(vb / 1024),
                        (unsigned long long)((vb - last_v) / 1024),
                        g_audio_frames.load(), (unsigned long long)(ab / 1024),
                        (unsigned long long)((ab - last_a) / 1024));
            last_v = vb; last_a = ab;
        }
    }

    std::printf("\n=== summary ===\n");
    for (int i = 0; i < 32; ++i) {
        if (g_msg_counts[i]) std::printf("  %-11s x %u\n", cmdName(i), g_msg_counts[i].load());
    }
    std::printf("video: %u frames, %llu KB dumped %llu KB\n",
                g_video_frames.load(), (unsigned long long)(g_video_bytes / 1024),
                (unsigned long long)(vdump / 1024));
    std::printf("audio: %u frames, %llu KB dumped %llu KB\n",
                g_audio_frames.load(), (unsigned long long)(g_audio_bytes / 1024),
                (unsigned long long)(adump / 1024));
    conn->stop();
    return 0;
}