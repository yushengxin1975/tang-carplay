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

## Next layer (TODO)

- `client/cpp/media/`: H.264 decode via OMX (CE), AAC decode via fdk-aac, DirectDraw render
- `client/cpp/main.cpp`: PC main loop with libusbdotnet
- CE port: replace UsbTransport_PC placeholder with real CEUSBKWrapper Ukw* calls, compile via CeGCC
- On-car integration: copy compiled CE binary to BYD SD card, replace AutoKit startup.exe