#pragma once
// AES-128-CBC wrapper for Carlinkit protocol payload encryption.
//
// FastCarPlay uses AES-CBC with a hard-coded base key + IV negotiated via
// CMD_ENCRYPTION. We embed a small AES implementation (no mbedTLS dependency)
// so this compiles standalone on Windows CE (CeGCC) without external libs.
//
// Reference: FIPS-197 / TTAU AES. Optimized for ARM/Thumb when -DARM_ASM used.

#ifndef CARLINKIT_AES_CIPHER_H
#define CARLINKIT_AES_CIPHER_H

#include <cstdint>
#include <cstring>
#include <stdexcept>

class AESCipher {
public:
    static constexpr int BLOCK = 16;
    static constexpr int KEYLEN = 16;     // AES-128
    static constexpr int NUM_ROUND = 10;  // AES-128 -> 10 rounds

    AESCipher() : ctx_initialised_(false) {}

    // Initialise with base key (FastCarPlay uses ENCRYPTION_BASE string).
    // Real key derived via CMD_ENCRYPTION exchange; here we just init with base.
    explicit AESCipher(const uint8_t *key16, const uint8_t *iv16 = nullptr) { init(key16, iv16); }

    void init(const uint8_t *key16, const uint8_t *iv16 = nullptr) {
        if (!key16) throw std::runtime_error("AESCipher: null key");
        key_expand(key16, rk_);
        if (iv16) std::memcpy(iv_, iv16, BLOCK);
        else      std::memcpy(iv_, default_iv_, BLOCK);
        ctx_initialised_ = true;
    }

    bool is_ready() const { return ctx_initialised_; }

    // CBC encrypt `in` (len bytes multiple of 16) -> `out` (same length).
    // Caller must ensure len % 16 == 0 (FastCarPlay pads in higher layer).
    bool encrypt(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ctx_initialised_) return error(err, "AESCipher not initialised");
        if (!in || !out || len == 0 || (len % BLOCK) != 0)
            return error(err, "AESCipher encrypt: bad args");
        std::memcpy(buf_, iv_, BLOCK);
        for (std::size_t off = 0; off < len; off += BLOCK) {
            for (int i = 0; i < BLOCK; ++i) buf_[i] ^= in[off + i];
            aes_encrypt_block(buf_, rk_, out + off);
            for (int i = 0; i < BLOCK; ++i) buf_[i] = out[off + i];
        }
        return true;
    }

    bool decrypt(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ctx_initialised_) return error(err, "AESCipher not initialised");
        if (!in || !out || len == 0 || (len % BLOCK) != 0)
            return error(err, "AESCipher decrypt: bad args");
        uint8_t tmp[BLOCK];
        for (std::size_t off = 0; off < len; off += BLOCK) {
            std::memcpy(tmp, in + off, BLOCK);
            aes_decrypt_block(in + off, rk_, out + off);
            for (int i = 0; i < BLOCK; ++i) out[off + i] ^= buf_[i];
            std::memcpy(buf_, tmp, BLOCK);
        }
        return true;
    }

    static bool error(char *err, const char *msg) {
        if (err) { std::strcpy(err, msg); }
        return false;
    }

    // Default IV used when Box doesn't negotiate one.
    // FastCarPlay: ENCRYPTION_BASE = "AutoBoxTransferString" or similar — verify.
    static constexpr uint8_t default_iv_[BLOCK] = {
        0x41, 0x75, 0x74, 0x6f, 0x42, 0x6f, 0x78, 0x00,
        0x41, 0x75, 0x74, 0x6f, 0x42, 0x6f, 0x78, 0x00
    };  // "AutoBox\0" placeholder, real value TBD via reverse

private:
    bool ctx_initialised_;
    uint8_t rk_[176];   // 11 round keys x 16 bytes
    uint8_t iv_[BLOCK];
    uint8_t buf_[BLOCK];

    static const uint8_t sbox_[256];
    static const uint8_t inv_sbox_[256];

    static uint8_t xtime(uint8_t b) {
        return static_cast<uint8_t>(((b << 1) ^ (((b >> 7) & 1) * 0x1B)) & 0xFF);
    }

    void key_expand(const uint8_t key[KEYLEN], uint8_t rk[]) {
        static const uint8_t rcon[11] = {
            0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
        };
        std::memcpy(rk, key, KEYLEN);
        uint8_t temp[4];
        for (int i = KEYLEN; i < (NUM_ROUND + 1) * BLOCK; i += 4) {
            temp[0] = rk[i - 4 + 0];
            temp[1] = rk[i - 4 + 1];
            temp[2] = rk[i - 4 + 2];
            temp[3] = rk[i - 4 + 3];
            if ((i % KEYLEN) == 0) {
                uint8_t t = temp[0];
                temp[0] = sbox_[temp[1]] ^ rcon[i / KEYLEN];
                temp[1] = sbox_[temp[2]];
                temp[2] = sbox_[temp[3]];
                temp[3] = sbox_[t];
            } else if ((i % KEYLEN) == 16) {
                uint8_t t = temp[0];
                temp[0] = sbox_[temp[0]];
                temp[1] = sbox_[temp[1]];
                temp[2] = sbox_[temp[2]];
                temp[3] = sbox_[t];
            }
            for (int j = 0; j < 4; ++j)
                rk[i + j] = rk[i - KEYLEN + j] ^ temp[j];
        }
    }

    void aes_encrypt_block(const uint8_t in[BLOCK], const uint8_t rk[], uint8_t out[BLOCK]) {
        uint8_t s[BLOCK];
        for (int i = 0; i < BLOCK; ++i) s[i] = in[i] ^ rk[i];
        for (int round = 1; round < NUM_ROUND; ++round) {
            for (int i = 0; i < BLOCK; ++i) s[i] = sbox_[s[i]];
            uint8_t t;
            t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
            t = s[2]; s[2] = s[10]; s[10] = t;
            t = s[6]; s[6] = s[14]; s[14] = t;
            for (int i = 0; i < BLOCK; ++i) s[i] ^= rk[round * BLOCK + i];
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    int idx = r * 4 + c;
                    uint8_t a = s[idx];
                    uint8_t b = (c == 0) ? 0 : s[idx - 1];
                    uint8_t cc = (c == 3) ? 0 : s[idx + 1];
                    s[idx] = (a ^ b ^ cc) ^ xtime(a);
                }
            }
        }
        for (int i = 0; i < BLOCK; ++i) s[i] = sbox_[s[i]];
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        for (int i = 0; i < BLOCK; ++i) out[i] = s[i] ^ rk[NUM_ROUND * BLOCK + i];
    }

    void aes_decrypt_block(const uint8_t in[BLOCK], const uint8_t rk[], uint8_t out[BLOCK]) {
        uint8_t s[BLOCK];
        for (int i = 0; i < BLOCK; ++i) s[i] = in[i] ^ rk[NUM_ROUND * BLOCK + i];
        for (int round = NUM_ROUND - 1; round >= 1; --round) {
            for (int i = 0; i < BLOCK; ++i) s[i] = inv_sbox_[s[i]];
            uint8_t t;
            s[1] = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1];
            t = s[2]; s[2] = s[10]; s[10] = t;
            t = s[6]; s[6] = s[14]; s[14] = t;
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    int idx = r * 4 + c;
                    uint8_t a = s[idx];
                    uint8_t b = (c == 0) ? 0 : s[idx - 1];
                    uint8_t cc = (c == 3) ? 0 : s[idx + 1];
                    s[idx] = (a ^ b ^ cc) ^ xtime(a);
                }
            }
            for (int i = 0; i < BLOCK; ++i) s[i] ^= rk[round * BLOCK + i];
        }
        for (int i = 0; i < BLOCK; ++i) s[i] = inv_sbox_[s[i]];
        for (int i = 0; i < BLOCK; ++i) out[i] = s[i] ^ rk[i];
    }
};

#endif // CARLINKIT_AES_CIPHER_H