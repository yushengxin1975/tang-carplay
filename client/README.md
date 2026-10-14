# Carlinkit PC Client (Phase B protocol layer)

C++17 protocol layer for the BYD 唐 2015 carlinkit client replacement project.

## Building & testing (PC)

### Requirements

- g++ 11+ (MinGW-w64 / LLVM-MinGW / Linux g++)
- 7z (Windows) to extract MinGW zip

### Build the test binary

```bash
g++ -std=c++17 -O2 -g -Wall -Iclient/include -Iclient/cpp \
    client/cpp/protocol/aes_sbox_data.cpp \
    client/cpp/protocol/connection.cpp \
    client/test/test_protocol.cpp \
    -o test_protocol
```

### Run the tests

```bash
./test_protocol
```

Expected output:

```
=== Carlinkit protocol unit tests ===
[PASS]  [test_aes_roundtrip] AES encrypt ok
[PASS]  [test_aes_roundtrip] AES decrypt ok
[PASS]  [test_aes_roundtrip] roundtrip plaintext matches
[PASS]  [test_aes_roundtrip] ciphertext differs from plaintext
[PASS]  [test_message_roundtrip] serialize size
[PASS]  [test_message_roundtrip] magic byte0-1 = MAGIC LE
[PASS]  [test_message_roundtrip] roundtrip cmd preserved
[PASS]  [test_message_roundtrip] roundtrip length
[PASS]  [test_message_roundtrip] roundtrip payload bytes
[PASS]  [test_message_roundtrip] roundtrip not encrypted
[PASS]  [test_message_encrypted] MAGIC_ENC after mark_encrypted
[PASS]  [test_message_encrypted] parsed msg is encrypted
[PASS]  [test_message_encrypted] decrypted plaintext matches original
[PASS]  [test_lifecycle] after start: NO_DEVICE
[PASS]  [test_lifecycle] PLUGGED fired
[PASS]  [test_lifecycle] state -> ONLINE
[PASS]  [test_lifecycle] UNPLUGGED fired
[PASS]  [test_lifecycle] heartbeat replied with 16-byte empty msg
[PASS]  [test_lifecycle] reply type == HEARTBEAT
=== 17 passed, 0 failed ===
```

## Layout

```
client/
├── include/
│   └── protocol_const.h      - 20 message types, button IDs, VID/PID
├── cpp/
│   └── protocol/
│       ├── message.h         - Packed Header + Message class
│       ├── aes_cipher.h      - AES-128-CBC (no external deps)
│       ├── aes_sbox_data.cpp  - FIPS-197 sbox/inv_sbox tables
│       ├── connection.h/.cpp  - State machine + dispatch
│       └── usb_transport.h    - Abstract transport + PC/CE placeholders
└── test/
    ├── fake_transport.h     - In-memory transport for unit tests
    └── test_protocol.cpp     - 4 test cases
```

## What's validated (test_protocol.cpp)

1. **AES roundtrip**: Encrypt then decrypt a 32-byte buffer, verify plaintext matches ciphertext differs.
2. **Message roundtrip (plain)**: Build a `Message(VIDEO_DATA, ...)` with 5-byte payload, serialize, parse, verify all fields match.
3. **Message encrypted**: AES-CBC encrypt payload, mark encrypted, serialize, parse, decrypt, verify plaintext matches.
4. **Connection lifecycle**: Open a `Connection` with `FakeTransport`, verify NO_DEVICE initial state, deliver PLUGGED → ON_PLUGGED callback fires + state=ONLINE, deliver UNPLUGGED → fires, deliver HEARTBEAT → auto-reply of 16-byte HEARTBEAT message.

## What's NOT yet validated (deferred)

- **AES vector test**: NIST KAT vectors (would catch subtle off-by-one in MixColumns/SubBytes/Rcon). Add before trusting crypto.
- **Real USB transport**: UsbTransport_PC/UsbTransport_CE are placeholders. Real implementations need libusbdotnet (PC) + CEUSBKWrapper Ukw* bindings (CE).
- **Protocol message parsing of real Carlinkit box traffic**: needs a way to log box messages, then compare to expected fast paths.

These are TODO before connecting to the actual car.

## PC box simulator rig (`client/sim/`)

Run the **real production CE transport code path** (UsbTransport_CE + libBoxUSB
load + trailing-byte strip fix + reader loop + Connection state machine +
write-failure self-heal) on a PC — no head unit, no physical dongle, no WinCE
emulator needed. The exact code that runs on the BYD head unit is compiled for
desktop MinGW (`-DCARLINKIT_SIM_CE_TRANSPORT`) and the vendor DLL is replaced
by a TCP-tunneling drop-in.

### Three pieces

| File | Role |
|------|------|
| `box_sim.exe` | Box-side protocol simulator (TCP server, port 9123). Accepts connections in a loop — each new connection models a box re-plug. |
| `libBoxUSB.dll` | Drop-in replacement for the vendor `libBoxUSB.dll`. Same exports (`host_init`/`host_send`/`host_recv`/`host_close`), TCP-tunnels to `box_sim`. `host_recv` faithfully returns N+1 with a trailing `0x20` status byte. |
| `sim_main.exe` | Test harness. Loads `libBoxUSB.dll`, creates `UsbTransport_CE` + `Connection`, runs `start()` + `sendInit()`, counts write failures → self-heal at ≥5. |

### Getting the binaries

The `sim-pc` CI job (`.github/workflows/pc-unit-tests.yml`) builds all three
on every push. Download the `tang-carplay-pc-sim-<run_id>` artifact from any
green run — it contains `box_sim.exe`, `libBoxUSB.dll`, `sim_main.exe` in one
directory, statically linked (no MinGW runtime needed on the host).

### Basic end-to-end test

```powershell
# 1. Start the box simulator (background)
Start-Process .\box_sim.exe -ArgumentList '--port','9123'

# 2. Run the production CE transport code path against it
.\sim_main.exe --port 9123 --duration 15
```

Expected (key lines):
```
[libBoxUSB_sim] connected to box_sim 127.0.0.1:9123
OK: libBoxUSB 握手成功 (0x1314:0x1520)
读[libBoxUSB] #1: 17 字节 head=AA 55 AA 55 00 00 00 00 tail=FF 20   ← strip fix live
msg type=2 len=0                                                      ← PLUGGED parsed
STATE: ONLINE (4)                                                     ← STATE(4) parsed
msg type=170 len=0                                                    ← heartbeat from box
写#27 len=16 tx=16 magic=55AA55AA cmd=0                               ← heartbeat echoed back
[status] state=ONLINE rx=9 ... write_fails=0 selfheals=0              ← healthy session
```

### Fault injection drills

**Write-failure self-heal** (sim DLL env var — connection stays alive, writes
fail intermittently, failures accumulate past threshold 5 → auto-reconnect):
```powershell
$env:CARLINKIT_SIM_FAIL_WRITES = '3'   # every 3rd host_send returns -1
.\sim_main.exe --port 9123 --duration 20
# → selfheals increments continuously (stop+start+sendInit on each trigger)
```

**Graceful teardown + re-plug** (box sends UNPLUGGED, closes, accepts new
connection):
```powershell
Start-Process .\box_sim.exe -ArgumentList '--port','9123','--teardown-after','8'
.\sim_main.exe --port 9123 --duration 15
# → CMD_PLUGGED (1st) → ONLINE → CMD_UNPLUGGED → NO_DEVICE → self-heal → CMD_PLUGGED (2nd) → ONLINE
```

**RST on write** (box RSTs the socket — client's next write fails, connection
dies, error callback fires):
```powershell
Start-Process .\box_sim.exe -ArgumentList '--port','9123','--rst-on-write','12'
.\sim_main.exe --port 9123 --duration 15
# → write_fails detected, state → ERROR
```

**Parser resync** (box sends garbage bytes right after connect):
```powershell
Start-Process .\box_sim.exe -ArgumentList '--port','9123','--garbage','64'
.\sim_main.exe --port 9123 --duration 15
# → parser skips garbage, resyncs on next AA 55 AA 55 magic
```

**Keepalive starvation**:
```powershell
Start-Process .\box_sim.exe -ArgumentList '--port','9123','--no-heartbeat'
.\sim_main.exe --port 9123 --duration 15
```

### What this validates (that unit tests can't)

- The **trailing-byte strip fix** (`usb_transport.cpp`: `plen = rx - 1`) against
  a faithful `host_recv` that returns N+1 with trailing `0x20`.
- The **chained error-callback fix** (`Connection::start()` no longer overwrites
  the app-level error callback) — write failures reach the self-heal counter.
- **Parser resync** after garbage / lost sync.
- **Re-plug + re-init** after UNPLUGGED / teardown.
- The full **sendInit handshake** sequence (SEND_FILE / OPEN / JSON_CONTROL /
  DAYNIGHT / CONTROL×4) over the real transport.

## Next layer (TODO)

- `client/cpp/media/`: H.264 decode via OMX (CE), AAC decode via fdk-aac, DirectDraw render
- `client/cpp/main.cpp`: PC main loop with libusbdotnet
- CE port: replace UsbTransport_PC placeholder with real CEUSBKWrapper Ukw* calls, compile via CeGCC
- On-car integration: copy compiled CE binary to BYD SD card, replace AutoKit startup.exe