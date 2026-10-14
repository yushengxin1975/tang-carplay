// libboxusb_sim.cpp — libBoxUSB_sim.dll: drop-in replacement for the vendor
// libBoxUSB.dll, for PC-side testing of the REAL production CE transport
// code path (UsbTransport_CE loads "libBoxUSB.dll" via LoadLibraryW and
// resolves these exact exports).
//
// Rename this DLL to libBoxUSB.dll and place it next to sim_main.exe; the
// unmodified production code then runs against box_sim.exe over TCP.
//
// Exports (identical signatures to the vendor DLL):
//   int  host_init(void);                       — 0 on success, -1 on failure
//   int  host_send(const void*, int, int);      — (data, len, timeout_ms) → len / negative
//   int  host_recv(void*, int, int);            — (buf, bufsize, timeout_ms) → N+1 / negative
//   void host_close(void);
//
// host_recv faithfully reproduces the vendor behavior discovered from
// carplay.log (07:50 session): it returns [N data bytes][1 trailing status
// byte] — the return value is N+1 and the LAST byte is a status byte
// (0x20), NOT part of the payload. The production strip fix (usb_transport
// "drop trailing status byte") depends on this exact shape.
//
// Fault injection (environment variables, read at host_init):
//   CARLINKIT_SIM_PORT=<port>      box_sim TCP port (default 9123)
//   CARLINKIT_SIM_FAIL_WRITES=<k>  every k-th host_send returns -1 without
//                                  sending (tx failure → self-heal test)
//   CARLINKIT_SIM_DROP_READS=<n>   first n host_recv calls return -1
//                                  (empty-read / teardown simulation)

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- state ----
static SOCKET g_sock = INVALID_SOCKET;
static long   g_send_count  = 0;
static long   g_recv_count  = 0;
static int    g_fail_writes = 0;   // every k-th send fails (0 = off)
static int    g_drop_reads  = 0;   // first n recvs return -1 (0 = off)

extern "C" __declspec(dllexport)
int host_init(void) {
    if (g_sock != INVALID_SOCKET) return 0;  // idempotent

    const char *env_port = std::getenv("CARLINKIT_SIM_PORT");
    int port = env_port ? std::atoi(env_port) : 9123;
    const char *env_fw = std::getenv("CARLINKIT_SIM_FAIL_WRITES");
    if (env_fw) g_fail_writes = std::atoi(env_fw);
    const char *env_dr = std::getenv("CARLINKIT_SIM_DROP_READS");
    if (env_dr) g_drop_reads = std::atoi(env_dr);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) { WSACleanup(); return -1; }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(port));
    if (connect(g_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::printf("[libBoxUSB_sim] connect(127.0.0.1:%d) failed err=%u "
                    "(box_sim.exe running?)\n", port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    // Disable Nagle: the production code sends header and payload as two
    // separate host_send calls; low latency keeps the box_sim stream parse
    // snappy and mirrors USB bulk timing better.
    int nodelay = 1;
    setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
    std::printf("[libBoxUSB_sim] connected to box_sim 127.0.0.1:%d "
                "(fail_writes=%d drop_reads=%d)\n", port, g_fail_writes, g_drop_reads);
    return 0;
}

extern "C" __declspec(dllexport)
int host_send(const void *data, int len, int timeout_ms) {
    (void)timeout_ms;
    if (g_sock == INVALID_SOCKET) return -1;
    ++g_send_count;
    if (g_fail_writes > 0 && g_send_count % g_fail_writes == 0) {
        std::printf("[libBoxUSB_sim] host_send #%ld INJECTED FAILURE (len=%d)\n",
                    g_send_count, len);
        return -1;
    }
    const char *p = static_cast<const char *>(data);
    int left = len;
    while (left > 0) {
        int r = send(g_sock, p + (len - left), left, 0);
        if (r <= 0) {
            std::printf("[libBoxUSB_sim] host_send failed err=%u\n", WSAGetLastError());
            // Connection is dead (reset by box_sim fault injection): drop it
            // so the next host_init reconnects (self-heal path).
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            return -2;
        }
        left -= r;
    }
    return len;
}

extern "C" __declspec(dllexport)
int host_recv(void *buf, int size, int timeout_ms) {
    if (g_sock == INVALID_SOCKET || size < 2) return -1;
    ++g_recv_count;
    if (g_drop_reads > 0 && g_recv_count <= g_drop_reads) {
        std::printf("[libBoxUSB_sim] host_recv #%ld INJECTED EMPTY READ\n", g_recv_count);
        return -1;
    }

    // Wait for readability within timeout (select), then read what's there.
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_sock, &rfds);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select(0, &rfds, nullptr, nullptr, &tv);
    if (ready <= 0) return -1;  // timeout (matches vendor empty read)

    // Reserve one byte for the trailing status byte, exactly like the
    // vendor DLL: payload occupies buf[0..N-1], status byte at buf[N].
    int cap = size - 1;
    std::vector<char> tmp(static_cast<size_t>(cap));
    int n = recv(g_sock, tmp.data(), cap, 0);
    if (n <= 0) {
        // select() said readable but recv failed/EOF: the connection is dead
        // (graceful teardown or RST). Drop it so the next host_init
        // reconnects (self-heal / re-plug path).
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return -1;
    }

    std::memcpy(buf, tmp.data(), static_cast<size_t>(n));
    static_cast<unsigned char *>(buf)[n] = 0x20;  // trailing status byte
    return n + 1;                                  // vendor returns N+1
}

extern "C" __declspec(dllexport)
void host_close(void) {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
    std::printf("[libBoxUSB_sim] closed (sends=%ld recvs=%ld)\n",
                g_send_count, g_recv_count);
}
