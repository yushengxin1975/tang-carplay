// sim_main.cpp — PC test harness that runs the REAL production CE transport
// code path (UsbTransport_CE + libBoxUSB.dll load + trailing-byte strip fix
// + reader loop + Connection state machine + write-failure self-heal)
// against box_sim.exe via libBoxUSB_sim.dll.
//
// No head unit, no physical dongle, no WinCE emulator needed: the exact
// code that runs on the BYD head unit (usb_transport.cpp CE branch) is
// compiled here for desktop MinGW (-DCARLINKIT_SIM_CE_TRANSPORT) and the
// vendor DLL is replaced by a TCP-tunneling drop-in.
//
// Setup (after downloading the CI artifacts into ONE directory):
//   box_sim.exe   — box-side simulator (start first)
//   libBoxUSB.dll — libBoxUSB_sim.dll RENAMED (production code loads this name)
//   sim_main.exe  — this harness
//
// Usage:
//   1. box_sim.exe --port 9123
//   2. sim_main.exe --port 9123 [--duration 30] [--enc]
//
// What to expect on success:
//   OK: libBoxUSB 握手成功 (0x1314:0x1520)
//   读[libBoxUSB] #1: 17 字节 head=AA 55 AA 55 ... tail=..   ← strip fix live
//   STATE: 4 (ONLINE)                                        ← PLUGGED parsed
//   写#.. cmd=170                                            ← heartbeat echo
//   no "[USB-IN] lost sync" lines                            ← stream stable
//
// Fault drills (run box_sim with flags, or set sim-DLL env vars):
//   box_sim.exe --rst-on-write 12    → write failures + self-heal reconnect
//   box_sim.exe --teardown-after 20  → UNPLUGGED + re-plug + re-init
//   box_sim.exe --garbage 64         → parser resync (lost-sync recovery)
//   box_sim.exe --no-heartbeat       → keepalive starvation
//   CARLINKIT_SIM_FAIL_WRITES=7      → every 7th host_send fails (sim DLL)
//   CARLINKIT_SIM_DROP_READS=5       → first 5 host_recv calls return -1

#include <windows.h>

#include "protocol/connection.h"
#include "protocol/usb_transport.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>

// PushEventW is declared extern in usb_transport.cpp (CE branch) and is
// normally defined by ce_main.cpp. The sim harness provides a console
// implementation so the production transport's telemetry prints here.
void PushEventW(const WCHAR *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfwprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ---- harness counters ----
static std::atomic<long> g_rx_msgs{0};
static std::atomic<long> g_video_frames{0};
static std::atomic<long> g_audio_frames{0};
static std::atomic<long> g_plug_count{0};
static std::atomic<long> g_unplug_count{0};
static std::atomic<long> g_write_failures{0};
static std::atomic<long> g_selfheals{0};
static std::atomic<bool> g_selfheal_requested{false};
static std::atomic<bool> g_exit{false};

static const char *state_name(carlinkit::ConnState s) {
    using carlinkit::ConnState;
    switch (s) {
    case ConnState::INITIALISING: return "INITIALISING";
    case ConnState::NO_DEVICE:    return "NO_DEVICE";
    case ConnState::LINKING:      return "LINKING";
    case ConnState::ERROR_STATE:  return "ERROR";
    case ConnState::ONLINE:       return "ONLINE";
    case ConnState::CONNECTED:    return "CONNECTED";
    default:                      return "UNKNOWN";
    }
}

int main(int argc, char **argv) {
    int port = 9123;
    int duration = 30;   // seconds
    bool enc = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next_int = [&]() -> int {
            return (i + 1 < argc) ? std::atoi(argv[++i]) : 0;
        };
        if (a == "--port" || a == "-p") port = next_int();
        else if (a == "--duration" || a == "-d") duration = next_int();
        else if (a == "--enc") enc = true;
        else if (a == "--help" || a == "-h") {
            std::printf("Usage: sim_main.exe [--port 9123] [--duration 30] [--enc]\n");
            return 0;
        }
    }

    // The sim DLL reads its port from the environment.
    char port_env[32];
    std::snprintf(port_env, sizeof(port_env), "%d", port);
    SetEnvironmentVariableA("CARLINKIT_SIM_PORT", port_env);

    std::printf("sim_main: production CE transport code path vs box_sim :%d "
                "(enc=%s duration=%ds)\n", port, enc ? "on" : "off", duration);

    auto transport = std::make_unique<carlinkit::UsbTransport_CE>();

    // App-level error callback: counts consecutive write failures to drive
    // the self-heal reconnect — mirrors ce_main.cpp's g_write_failures loop.
    transport->setErrorCallback([](int err) {
        long f = ++g_write_failures;
        PushEventW(L"传输层错误回调: err=%d (连续写失败 %ld)", err, f);
        if (f >= 5) g_selfheal_requested.store(true);
    });

    carlinkit::InboundCallbacks cbs;
    cbs.on_state = [](carlinkit::ConnState s) {
        PushEventW(L"STATE: %hs (%d)", state_name(s), static_cast<int>(s));
    };
    cbs.on_plugged = []() {
        PushEventW(L"OK: CMD_PLUGGED 收到 (第 %ld 次) -> 会话拉起", ++g_plug_count);
    };
    cbs.on_unplugged = []() {
        PushEventW(L"CMD_UNPLUGGED 收到 (第 %ld 次) -> iPhone 断开", ++g_unplug_count);
    };
    cbs.on_video = [](carlinkit::Cmd, const uint8_t *, std::size_t n) {
        ++g_video_frames;
        if (g_video_frames.load() <= 3)
            PushEventW(L"VIDEO frame #%ld len=%lu", g_video_frames.load(),
                       static_cast<unsigned long>(n));
    };
    cbs.on_audio = [](carlinkit::Cmd, const uint8_t *, std::size_t n) {
        ++g_audio_frames;
        if (g_audio_frames.load() <= 3)
            PushEventW(L"AUDIO frame #%ld len=%lu", g_audio_frames.load(),
                       static_cast<unsigned long>(n));
    };
    cbs.on_any = [](carlinkit::Cmd cmd, const uint8_t *, std::size_t n) {
        ++g_rx_msgs;
        if (g_rx_msgs.load() <= 12)
            PushEventW(L"msg type=%u len=%lu", static_cast<unsigned>(cmd),
                       static_cast<unsigned long>(n));
    };

    carlinkit::Connection co(std::move(transport), cbs);

    try {
        co.start();
    } catch (const std::exception &e) {
        std::printf("start failed: %s\n", e.what());
        std::printf("(hint: start box_sim.exe first; libBoxUSB.dll = renamed "
                    "libBoxUSB_sim.dll must sit next to sim_main.exe)\n");
        return 1;
    }

    try {
        PushEventW(L"发送 sendInit 配置脉冲...");
        co.sendInit(enc, 2);
        PushEventW(L"配置完成！全时监听中...");
    } catch (const std::exception &e) {
        std::printf("sendInit failed: %s\n", e.what());
    }

    auto t0 = std::chrono::steady_clock::now();
    auto last_status = t0;
    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto now = std::chrono::steady_clock::now();
        if (now - last_status >= std::chrono::seconds(2)) {
            last_status = now;
            std::printf("[status] state=%s rx=%ld video=%ld audio=%ld "
                        "plugs=%ld unplugs=%ld write_fails=%ld selfheals=%ld\n",
                        state_name(co.state()), g_rx_msgs.load(),
                        g_video_frames.load(), g_audio_frames.load(),
                        g_plug_count.load(), g_unplug_count.load(),
                        g_write_failures.load(), g_selfheals.load());
            std::fflush(stdout);
        }
        if (g_selfheal_requested.exchange(false)) {
            ++g_selfheals;
            PushEventW(L"自愈: 传输层连续写失败，自动重建连接 (第 %ld 次)...",
                       g_selfheals.load());
            try {
                co.stop();
                co.start();
                co.sendInit(enc, 2);
                g_write_failures.store(0);
                PushEventW(L"自愈完成，会话已重建");
            } catch (const std::exception &e) {
                std::printf("self-heal reconnect failed: %s\n", e.what());
            }
        }
        if (duration > 0 && now - t0 >= std::chrono::seconds(duration)) break;
    }

    std::printf("[done] rx=%ld video=%ld audio=%ld plugs=%ld unplugs=%ld "
                "write_fails=%ld selfheals=%ld\n",
                g_rx_msgs.load(), g_video_frames.load(), g_audio_frames.load(),
                g_plug_count.load(), g_unplug_count.load(),
                g_write_failures.load(), g_selfheals.load());
    co.stop();
    return 0;
}
