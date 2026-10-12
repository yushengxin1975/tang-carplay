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
// Features:
//   1. open box, claim interface
//   2. Connection::start() -> CMD_OPEN + CMD_VERSION
//   3. heartbeat every 1s
//   4. prints every inbound message: type, size, first bytes
//   5. counts video/audio frames; dumps first 2MB to sample_video.bin / sample_audio.bin
//   6. TCP Video Streaming (--stream 18888): broadcast H.264 stream to local viewer
//   7. TCP Control Channel  (--ctrl 18889): receive mouse touch events and inject to iPhone

#include "protocol/connection.h"
#include "protocol/usb_transport.h"
#include "protocol_const.h"

#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define CLOSE_SOCK closesocket
#define SET_NONBLOCK(s) do { u_long m = 1; ioctlsocket(s, FIONBIO, &m); } while(0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCK close
#define SET_NONBLOCK(s) do { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, f | O_NONBLOCK); } while(0)
#endif

using namespace carlinkit;

static std::atomic<bool> g_run{true};

static void onSigint(int) { g_run = false; }
static std::atomic<uint64_t> g_video_bytes{0}, g_audio_bytes{0};
static std::atomic<uint32_t> g_video_frames{0}, g_audio_frames{0};
static std::atomic<uint32_t> g_msg_counts[32];

static socket_t g_video_listen = INVALID_SOCKET;
static socket_t g_ctrl_listen = INVALID_SOCKET;
static socket_t g_video_client = INVALID_SOCKET;
static socket_t g_ctrl_client = INVALID_SOCKET;
static std::vector<uint8_t> g_sps_pps;
static bool g_send_sps_pps = false;

static const char *cmdName(uint32_t c) {
    switch (c) {
    case 1: return "OPEN";       case 2: return "PLUGGED";
    case 3: return "STATE";      case 4: return "UNPLUGGED";
    case 5: return "TOUCH";      case 6: return "VIDEO";
    case 7: return "AUDIO";      case 8: return "CONTROL";
    case 9: return "CMD_9";      case 10: return "APP_INFO";
    case 14: return "WIFI_INFO"; case 18: return "DEV_LIST";
    case 20: return "MANUFACTURER"; case 23: return "MULTI_TOUCH";
    case 25: return "JSON_CTRL"; case 42: return "MEDIA_INFO";
    case 153: return "SEND_FILE"; case 162: return "DAYNIGHT";
    case 170: return "HEARTBEAT"; case 204: return "VERSION";
    case 240: return "ENCRYPTION";
    default: return "?";
    }
}

static socket_t create_tcp_server(int port) {
    if (port <= 0) return INVALID_SOCKET;
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 2) == SOCKET_ERROR) {
        CLOSE_SOCK(s);
        return INVALID_SOCKET;
    }
    SET_NONBLOCK(s);
    return s;
}

int main(int argc, char **argv) {
    std::signal(SIGINT, onSigint);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    bool encrypted = false;
    int work_mode = 2; // Default 2: WORK_MODE_IPHONE_CARPLAY
    int stream_port = 18888;
    int ctrl_port = 18889;
    UsbTransport_PC::ClaimOptions co;
    co.alt_setting = false;
    co.clear_halt = false;
    co.set_config = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::printf("Usage: pc_main.exe [--plain] [--enc] [--carplay] [--android] [--mode <1|2>] [--stream <port>] [--ctrl <port>] [--no-stream]\n");
            return 0;
        }
        else if (!std::strcmp(argv[i], "--plain")) encrypted = false;
        else if (!std::strcmp(argv[i], "--enc")) encrypted = true;
        else if (!std::strcmp(argv[i], "--carplay")) work_mode = 2;
        else if (!std::strcmp(argv[i], "--android")) work_mode = 1;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) work_mode = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) stream_port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ctrl") && i + 1 < argc) ctrl_port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-stream")) { stream_port = 0; ctrl_port = 0; }
        else if (!std::strcmp(argv[i], "--reset-reopen")) co.reset_reopen = true;
        else if (!std::strcmp(argv[i], "--alt-setting")) co.alt_setting = true;
        else if (!std::strcmp(argv[i], "--clear-halt")) co.clear_halt = true;
        else if (!std::strcmp(argv[i], "--set-config")) co.set_config = true;
        else if (!std::strcmp(argv[i], "--no-alt")) { co.alt_setting = false; co.clear_halt = false; }
    }

    std::printf("=== Carlinkit PC client (protocol bring-up, %s, mode=%d %s) ===\n",
                encrypted ? "ENCRYPTED (CFB128)" : "plain",
                work_mode,
                work_mode == 2 ? "CarPlay" : work_mode == 1 ? "AndroidAuto" : "other");

    if (stream_port > 0) {
        g_video_listen = create_tcp_server(stream_port);
        std::printf("[STREAM] TCP video server listening on port %d\n", stream_port);
    }
    if (ctrl_port > 0) {
        g_ctrl_listen = create_tcp_server(ctrl_port);
        std::printf("[CTRL] TCP control server listening on port %d\n", ctrl_port);
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
            // Stream raw Annex B H.264 (strip 20-byte Carlinkit header)
            if (n > 20) {
                const uint8_t *h264 = p + 20;
                std::size_t h264_len = n - 20;
                // If this packet contains SPS (0x27), cache parameter sets
                if (h264_len >= 5 && h264[0] == 0 && h264[1] == 0 && h264[2] == 0 && h264[3] == 1 &&
                    (h264[4] & 0x1f) == 7) {
                    g_sps_pps.assign(h264, h264 + std::min(h264_len, static_cast<std::size_t>(256)));
                }
                if (g_video_client != INVALID_SOCKET) {
                    if (g_send_sps_pps && !g_sps_pps.empty()) {
                        send(g_video_client, reinterpret_cast<const char *>(g_sps_pps.data()),
                             static_cast<int>(g_sps_pps.size()), 0);
                        g_send_sps_pps = false;
                    }
                    int rc = send(g_video_client, reinterpret_cast<const char *>(h264),
                                  static_cast<int>(h264_len), 0);
                    if (rc <= 0) {
                        CLOSE_SOCK(g_video_client);
                        g_video_client = INVALID_SOCKET;
                        std::printf("[STREAM] video viewer disconnected\n");
                    }
                }
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

    while (g_run) {
        Connection *conn = nullptr;
        UsbTransport_PC *t = nullptr;
        bool logged_wait = false;

        // 1. Wait for Carlinkit box to be plugged in (retry every 1s)
        while (g_run) {
            t = new UsbTransport_PC();
            t->claim_opts = co;
            try {
                conn = new Connection(
                    std::unique_ptr<UsbTransport>(reinterpret_cast<UsbTransport *>(t)), cb);
                conn->start();
                std::printf("box found! interface claimed & reader active.\n");
                std::printf("sending config burst (sendInit, %s, mode=%d)...\n",
                            encrypted ? "encrypted" : "plain", work_mode);
                conn->sendInit(encrypted, work_mode);
                std::printf("config burst sent. protocol up. plug iPhone into the box.\n\n");
                break;
            } catch (const std::exception &e) {
                delete conn;
                conn = nullptr;
                t = nullptr;
                if (!logged_wait) {
                    std::printf("waiting for Carlinkit box (0x1314:0x1520)... plug box into PC USB.\n");
                    logged_wait = true;
                }
                // Keep handling viewer TCP connections while waiting for box
                if (g_video_listen != INVALID_SOCKET && g_video_client == INVALID_SOCKET) {
                    socket_t cl = accept(g_video_listen, nullptr, nullptr);
                    if (cl != INVALID_SOCKET) {
                        SET_NONBLOCK(cl);
                        g_video_client = cl;
                        g_send_sps_pps = true;
                        std::printf("[STREAM] video viewer connected (waiting for box)\n");
                    }
                }
                if (g_ctrl_listen != INVALID_SOCKET && g_ctrl_client == INVALID_SOCKET) {
                    socket_t cl = accept(g_ctrl_listen, nullptr, nullptr);
                    if (cl != INVALID_SOCKET) {
                        SET_NONBLOCK(cl);
                        g_ctrl_client = cl;
                        std::printf("[CTRL] control client connected (waiting for box)\n");
                    }
                }
#ifdef _WIN32
                Sleep(1000);
#else
                sleep(1);
#endif
            }
        }

        if (!g_run || !conn) break;

        // If a video viewer connected while waiting for box, request keyframe
        if (g_video_client != INVALID_SOCKET) {
            conn->send(Message::Control(BTN_SCREEN_REFRESH));
        }

        // 2. Main read pump + periodic stats + heartbeat
        uint64_t last_v = 0, last_a = 0;
        int stat_ticks = 0;
        int hb_ticks = 0;

        while (g_run) {
            if (!t->readPump(200)) {
                std::printf("USB disconnected or error, resetting...\n");
                break;
            }

            // Accept video viewer
            if (g_video_listen != INVALID_SOCKET && g_video_client == INVALID_SOCKET) {
                socket_t cl = accept(g_video_listen, nullptr, nullptr);
                if (cl != INVALID_SOCKET) {
                    SET_NONBLOCK(cl);
                    g_video_client = cl;
                    g_send_sps_pps = true;
                    std::printf("[STREAM] video viewer connected\n");
                    conn->send(Message::Control(BTN_SCREEN_REFRESH));
                }
            }

            // Accept control client and poll touch events
            if (g_ctrl_listen != INVALID_SOCKET) {
                if (g_ctrl_client == INVALID_SOCKET) {
                    socket_t cl = accept(g_ctrl_listen, nullptr, nullptr);
                    if (cl != INVALID_SOCKET) {
                        SET_NONBLOCK(cl);
                        g_ctrl_client = cl;
                        std::printf("[CTRL] control client connected\n");
                    }
                } else {
                    uint8_t cbuf[16];
                    int r = recv(g_ctrl_client, reinterpret_cast<char *>(cbuf), sizeof(cbuf), 0);
                    if (r >= 12) {
                        uint32_t act = Message::read_u32_le(cbuf);
                        uint32_t tx  = Message::read_u32_le(cbuf + 4);
                        uint32_t ty  = Message::read_u32_le(cbuf + 8);
                        conn->send(Message::Touch(act, tx, ty));
                        std::printf("[TOUCH] act=%u x=%u y=%u\n", act, tx, ty);
                    } else if (r == 0) {
                        CLOSE_SOCK(g_ctrl_client);
                        g_ctrl_client = INVALID_SOCKET;
                        std::printf("[CTRL] control client disconnected\n");
                    }
                }
            }

            // Heartbeat every ~3s
            if (++hb_ticks >= 15) {
                hb_ticks = 0;
                conn->send(Cmd::HEARTBEAT);
            }

            if (++stat_ticks >= 25) {  // ~5s
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

        conn->stop();
        delete conn;
        conn = nullptr;
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

    if (g_video_client != INVALID_SOCKET) CLOSE_SOCK(g_video_client);
    if (g_ctrl_client != INVALID_SOCKET) CLOSE_SOCK(g_ctrl_client);
    if (g_video_listen != INVALID_SOCKET) CLOSE_SOCK(g_video_listen);
    if (g_ctrl_listen != INVALID_SOCKET) CLOSE_SOCK(g_ctrl_listen);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}