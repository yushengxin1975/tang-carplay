#pragma once
// Carlinkit protocol constants - FIPS-197 + FastCarPlay-derived.
// Wrapped in namespace carlinkit so callers can use carlinkit::Cmd etc.

namespace carlinkit {

#pragma once
// Carlinkit CPC2000/CP2A protocol constants
// Sourced from niellun/FastCarPlay (C++) + AutoKit_WinCE reverse
#ifndef CARLINKIT_PROTOCOL_CONST_H
#define CARLINKIT_PROTOCOL_CONST_H

#include <cstdint>

// Connection state (Box status)
constexpr int32_t PROTOCOL_STATUS_UNKNOWN       = -1;
constexpr int32_t PROTOCOL_STATUS_INITIALISING  = 0;
constexpr int32_t PROTOCOL_STATUS_NO_DEVICE     = 1;
constexpr int32_t PROTOCOL_STATUS_LINKING       = 2;
constexpr int32_t PROTOCOL_STATUS_ERROR         = 3;
constexpr int32_t PROTOCOL_STATUS_ONLINE        = 4;
constexpr int32_t PROTOCOL_STATUS_CONNECTED     = 5;

// Magic numbers
constexpr uint32_t MAGIC      = 0x55AA55AAu;   // plain text message
constexpr uint32_t MAGIC_ENC  = 0x55BB55BBu;   // AES-CBC encrypted payload

// Command types (PC <-> Box)
enum class Cmd : uint32_t {
    OPEN         = 1,
    PLUGGED      = 2,
    STATE        = 3,
    UNPLUGGED    = 4,
    TOUCH        = 5,
    VIDEO_DATA   = 6,
    AUDIO_DATA   = 7,
    CONTROL      = 8,
    CMD_9        = 9,
    APP_INFO     = 10,
    BLUETOOTH    = 13,
    WIFI_INFO    = 14,
    DEVICE_LIST  = 18,
    MANUFACTURER  = 20,
    MULTI_TOUCH  = 23,
    JSON_CONTROL = 25,
    MEDIA_INFO   = 42,
    SEND_FILE    = 153,
    DAYNIGHT     = 162,
    HEARTBEAT    = 170,
    VERSION      = 204,
    ENCRYPTION   = 240,
};

// Control button IDs (Box->PC, CMD_CONTROL=8 payload)
constexpr uint8_t BTN_SIRI             = 5;
constexpr uint8_t BTN_MICROPHONE       = 7;
constexpr uint8_t BTN_SCREEN_REFRESH   = 12;
// Navigation
constexpr uint8_t BTN_LEFT             = 100;
constexpr uint8_t BTN_RIGHT            = 101;
constexpr uint8_t BTN_SELECT_DOWN      = 104;
constexpr uint8_t BTN_SELECT_UP        = 105;
constexpr uint8_t BTN_BACK             = 106;
constexpr uint8_t BTN_DOWN             = 114;
constexpr uint8_t BTN_HOME             = 200;
// Playback
constexpr uint8_t BTN_PLAY             = 201;
constexpr uint8_t BTN_PAUSE            = 202;
constexpr uint8_t BTN_NEXT_TRACK       = 204;
constexpr uint8_t BTN_PREVIOUS_TRACK   = 205;

// Media sizing
constexpr uint32_t AUDIO_BUFFER_SIZE    = 2560;
constexpr uint32_t AUDIO_BUFFER_OFFSET  = 12;
constexpr uint32_t MESSAGE_MAX_PAYLOAD  = 2 * 1024 * 1024;  // 2 MiB

// USB device identification (Carlinkit CPC2000/CP2A)
constexpr uint16_t CARLINKIT_VENDOR_ID   = 0x1314;
constexpr uint16_t CARLINKIT_PRODUCT_ID  = 0x1520;

// Endpoint addresses (typical Carlinkit CPC2000 layout, verify on hardware)
constexpr uint8_t EP_OUT_BULK  = 0x01;   // PC -> Box
constexpr uint8_t EP_IN_BULK   = 0x81;   // Box -> PC
constexpr uint8_t EP_IN_INT     = 0x83;   // Box -> PC, interrupt (heartbeat)

#endif // CARLINKIT_PROTOCOL_CONST_H
} // namespace carlinkit
