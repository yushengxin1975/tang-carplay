// box_sim.cpp — Carlinkit CPC2000 box simulator (box side of the protocol).
//
// Emulates the box over TCP. libBoxUSB_sim.dll tunnels the client's
// host_send/host_recv calls here, so the REAL production CE transport code
// path (UsbTransport_CE + libBoxUSB load + trailing-byte strip fix + reader
// loop + Connection state machine + self-heal) can be exercised on a PC
// without the head unit or the physical dongle.
//
// Wire format (identical to USB bulk): 16-byte header, little-endian
//   [u32 magic 0x55AA55AA][s32 len][u32 type][u32 ~type] + payload
//
// Session script (default):
//   - client handshake arrives (SEND_FILE / OPEN / JSON_CONTROL / DAYNIGHT /
//     CONTROL...) — logged
//   - on OPEN (type 1): reply PLUGGED (2) + STATE (3, status 4 = ONLINE)
//   - HEARTBEAT (170) every 1s (client must echo — keepalive)
//   - optional synthetic VIDEO/AUDIO frames (--media)
//
// Fault injection:
//   --garbage N        send N non-magic garbage bytes right after connect
//                      (tests parser resync / lost-sync path)
//   --teardown-after N after N received messages send UNPLUGGED and close
//                      gracefully (tests re-plug / re-init path)
//   --rst-on-write N   after N received messages RST the connection
//                      (SO_LINGER 0) — the client's next host_send fails,
//                      tests write-failure counting + self-heal reconnect
//   --no-heartbeat     do not send heartbeats (keepalive starvation)
//
// Usage: box_sim.exe [--port 9123] [flags]. Accepts connections in a loop;
// each new connection models a box re-plug.

#include "protocol_const.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET socket_t;
#  ifndef INADDR_LOOPBACK   // older mingw.org headers lack it
#    define INADDR_LOOPBACK 0x7f000001u
#  endif
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <signal.h>
typedef int socket_t;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define closesocket close
#endif

using carlinkit::MAGIC;

struct Header {
    uint32_t magic;
    int32_t  length;
    uint32_t type;
    uint32_t typecheck;
};

static const char *cmd_name(uint32_t t) {
    switch (t) {
    case 1:   return "OPEN";
    case 2:   return "PLUGGED";
    case 3:   return "STATE";
    case 4:   return "UNPLUGGED";
    case 5:   return "TOUCH";
    case 6:   return "VIDEO_DATA";
    case 7:   return "AUDIO_DATA";
    case 8:   return "CONTROL";
    case 9:   return "CMD_9";
    case 25:  return "JSON_CONTROL";
    case 153: return "SEND_FILE";
    case 162: return "DAYNIGHT";
    case 170: return "HEARTBEAT";
    case 204: return "VERSION";
    case 240: return "ENCRYPTION";
    default:  return "?";
    }
}

static bool recv_all(socket_t s, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = static_cast<int>(recv(s,
            reinterpret_cast<char *>(buf + got), static_cast<int>(n - got), 0));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static bool send_all(socket_t s, const uint8_t *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = static_cast<int>(send(s,
            reinterpret_cast<const char *>(buf + sent), static_cast<int>(n - sent), 0));
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// ---- shared connection state ----
static std::mutex  g_send_mtx;
static socket_t    g_client = INVALID_SOCKET;
static std::atomic<bool> g_run{true};

// scenario flags
static int  g_garbage        = 0;   // --garbage N
static int  g_teardown_after = 0;   // --teardown-after N
static int  g_rst_on_write   = 0;   // --rst-on-write N
static bool g_no_heartbeat   = false;
static bool g_media          = false;

static bool box_send(uint32_t type, const uint8_t *payload, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_send_mtx);
    if (g_client == INVALID_SOCKET) return false;
    Header h;
    h.magic     = MAGIC;
    h.length    = static_cast<int32_t>(len);
    h.type      = type;
    h.typecheck = ~type;
    std::vector<uint8_t> pkt(sizeof(Header) + len);
    std::memcpy(pkt.data(), &h, sizeof(Header));
    if (len) std::memcpy(pkt.data() + sizeof(Header), payload, len);
    if (!send_all(g_client, pkt.data(), pkt.size())) return false;
    std::printf("[BOX->] %s len=%u\n", cmd_name(type), len);
    std::fflush(stdout);
    return true;
}

static void heartbeat_loop() {
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_no_heartbeat) box_send(170 /*HEARTBEAT*/, nullptr, 0);
    }
}

static void media_loop() {
    uint32_t seq = 0;
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Synthetic VIDEO_DATA frame: [seq LE][12 zero bytes] (16B payload)
        uint8_t v[16] = {0};
        v[0] = static_cast<uint8_t>(seq & 0xFF);
        v[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        box_send(6 /*VIDEO_DATA*/, v, sizeof(v));
        // Synthetic AUDIO_DATA frame: 12-byte PCM-ish payload
        uint8_t a[12];
        for (size_t i = 0; i < sizeof(a); ++i) a[i] = static_cast<uint8_t>((seq * 7 + i) & 0xFF);
        box_send(7 /*AUDIO_DATA*/, a, sizeof(a));
        ++seq;
    }
}

static void handle_connection(socket_t c) {
    {
        std::lock_guard<std::mutex> lk(g_send_mtx);
        g_client = c;
    }
    std::printf("[BOX] client connected\n");
    std::fflush(stdout);

    // Optional garbage injection right after connect (parser resync test).
    if (g_garbage > 0) {
        std::vector<uint8_t> junk(static_cast<size_t>(g_garbage));
        for (int i = 0; i < g_garbage; ++i)
            junk[static_cast<size_t>(i)] = static_cast<uint8_t>((i * 31 + 0x11) & 0xFF);
        std::lock_guard<std::mutex> lk(g_send_mtx);
        if (send_all(c, junk.data(), junk.size())) {
            std::printf("[BOX->] garbage %d bytes injected\n", g_garbage);
            std::fflush(stdout);
        }
    }

    std::thread hb(heartbeat_loop);
    std::thread md;
    if (g_media) md = std::thread(media_loop);

    uint32_t msg_count = 0;
    while (g_run) {
        Header h;
        if (!recv_all(c, reinterpret_cast<uint8_t *>(&h), sizeof(Header))) {
            std::printf("[BOX] client read: header EOF/reset\n");
            break;
        }
        if (h.magic != MAGIC && h.magic != 0x55BB55BBu) {
            std::printf("[BOX] client read: bad magic 0x%08X — closing\n", h.magic);
            break;
        }
        std::vector<uint8_t> payload(h.length > 0 ? static_cast<size_t>(h.length) : 0u);
        if (h.length > 0 &&
            !recv_all(c, payload.data(), static_cast<size_t>(h.length))) {
            std::printf("[BOX] client read: payload EOF/reset\n");
            break;
        }
        ++msg_count;
        std::printf("[<-BOX] %s len=%d%s\n", cmd_name(h.type), h.length,
                    h.magic == 0x55BB55BBu ? " (ENC)" : "");
        std::fflush(stdout);

        if (h.type == 1 /*OPEN*/) {
            // Session accepted: PLUGGED + STATE(4 = ONLINE)
            box_send(2 /*PLUGGED*/, nullptr, 0);
            uint8_t st[4] = {4, 0, 0, 0};
            box_send(3 /*STATE*/, st, 4);
        }

        if (g_rst_on_write > 0 && msg_count >= static_cast<uint32_t>(g_rst_on_write)) {
            // RST: the client's next host_send fails (write-failure injection).
            struct linger lg;
            lg.l_onoff  = 1;
            lg.l_linger = 0;
            setsockopt(c, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char *>(&lg), sizeof(lg));
            std::printf("[BOX] RST connection after %u msgs (write-failure injection)\n",
                        msg_count);
            std::fflush(stdout);
            break;
        }
        if (g_teardown_after > 0 && msg_count >= static_cast<uint32_t>(g_teardown_after)) {
            box_send(4 /*UNPLUGGED*/, nullptr, 0);
            std::printf("[BOX] teardown: graceful close after %u msgs\n", msg_count);
            std::fflush(stdout);
            break;
        }
    }

    g_run.store(false);  // stop heartbeat/media threads for this connection
    {
        std::lock_guard<std::mutex> lk(g_send_mtx);
        if (g_client != INVALID_SOCKET) { closesocket(g_client); g_client = INVALID_SOCKET; }
    }
    if (hb.joinable()) hb.join();
    if (md.joinable()) md.join();
    g_run.store(true);   // re-arm for the next connection (box re-plug)
    std::printf("[BOX] connection closed, waiting for re-plug...\n");
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    int port = 9123;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next_int = [&]() -> int {
            return (i + 1 < argc) ? std::atoi(argv[++i]) : 0;
        };
        if (a == "--port" || a == "-p") port = next_int();
        else if (a == "--garbage") g_garbage = next_int();
        else if (a == "--teardown-after") g_teardown_after = next_int();
        else if (a == "--rst-on-write") g_rst_on_write = next_int();
        else if (a == "--no-heartbeat") g_no_heartbeat = true;
        else if (a == "--media") g_media = true;
        else if (a == "--help" || a == "-h") {
            std::printf("Usage: box_sim.exe [--port 9123] [--garbage N] "
                        "[--teardown-after N] [--rst-on-write N] "
                        "[--no-heartbeat] [--media]\n");
            return 0;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { std::printf("socket() failed\n"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::printf("bind(%d) failed\n", port);
        return 1;
    }
    if (listen(srv, 1) != 0) { std::printf("listen() failed\n"); return 1; }

    std::printf("box_sim: listening on 127.0.0.1:%d "
                "(garbage=%d teardown-after=%d rst-on-write=%d heartbeat=%s media=%s)\n",
                port, g_garbage, g_teardown_after, g_rst_on_write,
                g_no_heartbeat ? "OFF" : "1s", g_media ? "on" : "off");
    std::fflush(stdout);

    while (g_run) {
        socket_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        handle_connection(c);
    }

    closesocket(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
