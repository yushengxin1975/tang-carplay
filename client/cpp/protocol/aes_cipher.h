#pragma once
// AES-128-CBC cipher, FIPS-197 reference structure.
// State: 16 bytes column-major (s[r + 4*c] == state byte at row r, col c).
// No external dependencies; CE-safe (CeGCC ARMv4T, no NEON assumed).
//
// Tables live in aes_sbox_data.cpp (generated, NIST-verified).
//
// Encryption direction per FIPS-197:
//   AddRoundKey(0)
//   for round=1..9: SubBytes, ShiftRows, MixColumns, AddRoundKey(round)
//   final:          SubBytes, ShiftRows, AddRoundKey(10)
// Decryption is the exact inverse.

#ifndef CARLINKIT_AES_CIPHER_H
#define CARLINKIT_AES_CIPHER_H

#include <cstdint>
#include <cstring>
#include <stdexcept>

// Generated tables (aes_sbox_data.cpp), declared at namespace scope to keep
// the class header-free of the 512 bytes of data.
extern const uint8_t sbox[256];
extern const uint8_t inv_sbox[256];

class AESCipher {
public:
    static constexpr int BLOCK = 16;
    static constexpr int KEYLEN = 16;      // AES-128
    static constexpr int NUM_ROUND = 10;
    static constexpr int RKLEN = (NUM_ROUND + 1) * BLOCK; // 176

    // FastCarPlay ENCRYPTION_BASE (connection.h): 16-byte base key from
    // which the session key is derived by rotation with the seed.
    static constexpr char BASE_KEY[KEYLEN + 1] = "SkBRDy3gmrw1ieH0";

    AESCipher() : ready_(false) {}

    AESCipher(const uint8_t *key16, const uint8_t *iv16 = nullptr) { init(key16, iv16); }

    void init(const uint8_t *key16, const uint8_t *iv16 = nullptr) {
        if (!key16) throw std::runtime_error("AESCipher: null key");
        expand_key(key16);
        memcpy(iv_, iv16 ? iv16 : default_iv, BLOCK);
        memcpy(chain_, iv_, BLOCK);
        ready_ = true;
    }

    bool is_ready() const { return ready_; }

    // CBC encrypt. len must be a multiple of 16 (caller pads).
    bool encrypt(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ready_) return fail(err, "not initialised");
        if (!in || !out || len == 0 || (len % BLOCK) != 0)
            return fail(err, "encrypt: len must be non-zero multiple of 16");
        memcpy(chain_, iv_, BLOCK);
        for (std::size_t off = 0; off < len; off += BLOCK) {
            uint8_t blk[BLOCK];
            for (int i = 0; i < BLOCK; ++i) blk[i] = in[off + i] ^ chain_[i];
            encrypt_block(blk, out + off);
            memcpy(chain_, out + off, BLOCK);
        }
        return true;
    }

    // CBC decrypt. len must be a multiple of 16.
    bool decrypt(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ready_) return fail(err, "not initialised");
        if (!in || !out || len == 0 || (len % BLOCK) != 0)
            return fail(err, "decrypt: len must be non-zero multiple of 16");
        memcpy(chain_, iv_, BLOCK);
        for (std::size_t off = 0; off < len; off += BLOCK) {
            uint8_t tmp[BLOCK];
            memcpy(tmp, in + off, BLOCK);
            decrypt_block(in + off, out + off);
            for (int i = 0; i < BLOCK; ++i) out[off + i] ^= chain_[i];
            memcpy(chain_, tmp, BLOCK);
        }
        return true;
    }

    // ---- CFB128 (OpenSSL EVP_aes_128_cfb compatible) ----
    // Stream mode: any length, no padding. Both directions use the block
    // ENCRYPT function only. Keystream restarts from IV on every call
    // (FastCarPlay creates a fresh EVP context per message).

    // Raw key/IV init for CFB (used by tests / manual keying).
    void init_cfb(const uint8_t *key16, const uint8_t *iv16) {
        if (!key16 || !iv16) throw std::runtime_error("AESCipher: null key/iv");
        expand_key(key16);
        memcpy(iv_, iv16, BLOCK);
        ready_ = true;
    }

    // FastCarPlay seed derivation (aes_cipher.cpp):
    //   key[i] = BASE_KEY[(seed + i) % 16]
    //   IV zero except IV[1]=seed&0xFF, IV[4]=(seed>>8)&0xFF,
    //   IV[9]=(seed>>16)&0xFF, IV[12]=(seed>>24)&0xFF
    void init_seed(uint32_t seed) {
        uint8_t key[KEYLEN];
        for (int i = 0; i < KEYLEN; ++i)
            key[i] = static_cast<uint8_t>(BASE_KEY[(seed + i) % KEYLEN]);
        uint8_t iv[BLOCK] = {0};
        iv[1]  = static_cast<uint8_t>(seed);
        iv[4]  = static_cast<uint8_t>(seed >> 8);
        iv[9]  = static_cast<uint8_t>(seed >> 16);
        iv[12] = static_cast<uint8_t>(seed >> 24);
        init_cfb(key, iv);
    }

    // CFB128 encrypt. Any length.
    bool encrypt_cfb(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ready_) return fail(err, "not initialised");
        if (!in || !out || len == 0) return fail(err, "encrypt_cfb: empty input");
        cfb_crypt(in, len, out);
        return true;
    }

    // CFB128 decrypt. Any length (same keystream path as encrypt).
    bool decrypt_cfb(const uint8_t *in, std::size_t len, uint8_t *out, char *err = nullptr) {
        if (!ready_) return fail(err, "not initialised");
        if (!in || !out || len == 0) return fail(err, "decrypt_cfb: empty input");
        cfb_crypt(in, len, out);
        return true;
    }

    static bool fail(char *err, const char *msg) {
        if (err) strncpy(err, msg, 127), err[127] = 0;
        return false;
    }

    static constexpr uint8_t default_iv[BLOCK] = {
        0x41, 0x75, 0x74, 0x6f, 0x42, 0x6f, 0x78, 0x00,
        0x41, 0x75, 0x74, 0x6f, 0x42, 0x6f, 0x78, 0x00
    }; // "AutoBox\0" placeholder; real IV negotiated via CMD_ENCRYPTION.

private:
    bool ready_;
    uint8_t rk_[RKLEN];
    uint8_t iv_[BLOCK];
    uint8_t chain_[BLOCK];

    // CFB128 core: keystream = E(IV); out = in XOR keystream; IV <- ciphertext.
    // Identical for encrypt and decrypt (CFB uses the block ENCRYPT function
    // in both directions). Fresh IV per call = fresh EVP context per message.
    void cfb_crypt(const uint8_t *in, std::size_t len, uint8_t *out) {
        uint8_t iv[BLOCK];
        memcpy(iv, iv_, BLOCK);
        std::size_t off = 0;
        while (off < len) {
            uint8_t ks[BLOCK];
            encrypt_block(iv, ks);
            std::size_t n = (len - off < BLOCK) ? (len - off) : BLOCK;
            for (std::size_t i = 0; i < n; ++i) {
                uint8_t c = static_cast<uint8_t>(in[off + i] ^ ks[i]);
                out[off + i] = c;
                iv[i] = c;  // feedback ciphertext (encrypt and decrypt alike)
            }
            off += n;
        }
    }

    // ---- GF(2^8) multiply (xtime-based,Russian peasant) ----
    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        while (b) {
            if (b & 1) p ^= a;
            uint8_t hi = a & 0x80;
            a = static_cast<uint8_t>(a << 1);
            if (hi) a ^= 0x1B;
            b >>= 1;
        }
        return p;
    }

    // ---- Key expansion (FIPS-197 Sec 5.2) ----
    void expand_key(const uint8_t key[KEYLEN]) {
        static const uint8_t Rcon[11] = {
            0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
        };
        memcpy(rk_, key, KEYLEN);
        for (int i = KEYLEN; i < RKLEN; i += 4) {
            uint8_t t[4];
            memcpy(t, rk_ + i - 4, 4);
            if (i % KEYLEN == 0) {
                // RotWord + SubWord + Rcon
                uint8_t t0 = t[0];
                t[0] = static_cast<uint8_t>(sbox[t[1]] ^ Rcon[i / KEYLEN]);
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[t0];
            }
            for (int j = 0; j < 4; ++j)
                rk_[i + j] = static_cast<uint8_t>(rk_[i - KEYLEN + j] ^ t[j]);
        }
    }

    // ---- Round ops on state[16], column-major s[r + 4c] ----
    static void add_round_key(uint8_t s[BLOCK], const uint8_t *rk) {
        for (int i = 0; i < BLOCK; ++i) s[i] ^= rk[i];
    }
    static void sub_bytes(uint8_t s[BLOCK]) {
        for (int i = 0; i < BLOCK; ++i) s[i] = sbox[s[i]];
    }
    static void inv_sub_bytes(uint8_t s[BLOCK]) {
        for (int i = 0; i < BLOCK; ++i) s[i] = inv_sbox[s[i]];
    }
    static void shift_rows(uint8_t s[BLOCK]) {
        uint8_t t;
        // row 1: left-rotate by 1 (cols c: s[1+4c])
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        // row 2: left-rotate by 2
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        // row 3: left-rotate by 3 == right-rotate by 1
        t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
    }
    static void inv_shift_rows(uint8_t s[BLOCK]) {
        uint8_t t;
        // row 1: right-rotate by 1
        t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1]  = t;
        // row 2: right-rotate by 2 (same as left 2)
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        // row 3: right-rotate by 3 == left-rotate by 1
        t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;
    }
    static void mix_columns(uint8_t s[BLOCK]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t *p = s + 4 * c;
            uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = static_cast<uint8_t>(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
            p[1] = static_cast<uint8_t>(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
            p[2] = static_cast<uint8_t>(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
            p[3] = static_cast<uint8_t>(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
        }
    }
    static void inv_mix_columns(uint8_t s[BLOCK]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t *p = s + 4 * c;
            uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = static_cast<uint8_t>(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
            p[1] = static_cast<uint8_t>(gmul(a0,9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
            p[2] = static_cast<uint8_t>(gmul(a0,13) ^ gmul(a1,9) ^ gmul(a2,14) ^ gmul(a3,11));
            p[3] = static_cast<uint8_t>(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9) ^ gmul(a3,14));
        }
    }

    void encrypt_block(const uint8_t in[BLOCK], uint8_t out[BLOCK]) {
        uint8_t s[BLOCK];
        memcpy(s, in, BLOCK);
        add_round_key(s, rk_);
        for (int r = 1; r < NUM_ROUND; ++r) {
            sub_bytes(s);
            shift_rows(s);
            mix_columns(s);
            add_round_key(s, rk_ + r * BLOCK);
        }
        sub_bytes(s);
        shift_rows(s);
        add_round_key(s, rk_ + NUM_ROUND * BLOCK);
        memcpy(out, s, BLOCK);
    }

    void decrypt_block(const uint8_t in[BLOCK], uint8_t out[BLOCK]) {
        uint8_t s[BLOCK];
        memcpy(s, in, BLOCK);
        add_round_key(s, rk_ + NUM_ROUND * BLOCK);
        for (int r = NUM_ROUND - 1; r >= 1; --r) {
            inv_shift_rows(s);
            inv_sub_bytes(s);
            add_round_key(s, rk_ + r * BLOCK);
            inv_mix_columns(s);
        }
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, rk_);
        memcpy(out, s, BLOCK);
    }
};

#endif // CARLINKIT_AES_CIPHER_H