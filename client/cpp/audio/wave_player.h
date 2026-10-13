#pragma once
// wave_player.h — Real-time streaming PCM audio playback using WinMM waveOut
// Supports: Windows (winmm.dll) and WinCE 6.0 (coredll.dll / waveapi.dll)
// Automatically falls back to dummy sink if no audio hardware is present (headless CI).
// Safe no-op stub on Linux / macOS for cross-platform unit testing.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <cstdio>

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
#include <windows.h>

namespace carlinkit {
namespace detail {

#pragma pack(push, 1)
struct CE_WAVEFORMATEX {
    WORD        wFormatTag;
    WORD        nChannels;
    DWORD       nSamplesPerSec;
    DWORD       nAvgBytesPerSec;
    WORD        nBlockAlign;
    WORD        wBitsPerSample;
    WORD        cbSize;
};

struct CE_WAVEHDR {
    LPSTR       lpData;
    DWORD       dwBufferLength;
    DWORD       dwBytesRecorded;
    DWORD_PTR   dwUser;
    DWORD       dwFlags;
    DWORD       dwLoops;
    struct CE_WAVEHDR *lpNext;
    DWORD_PTR   reserved;
};
#pragma pack(pop)

typedef UINT (WINAPI *PFN_waveOutOpen)(LPHANDLE, UINT, CE_WAVEFORMATEX*, DWORD, DWORD, DWORD);
typedef UINT (WINAPI *PFN_waveOutPrepareHeader)(HANDLE, CE_WAVEHDR*, UINT);
typedef UINT (WINAPI *PFN_waveOutWrite)(HANDLE, CE_WAVEHDR*, UINT);
typedef UINT (WINAPI *PFN_waveOutUnprepareHeader)(HANDLE, CE_WAVEHDR*, UINT);
typedef UINT (WINAPI *PFN_waveOutReset)(HANDLE);
typedef UINT (WINAPI *PFN_waveOutClose)(HANDLE);
typedef UINT (WINAPI *PFN_waveOutGetNumDevs)(void);
typedef UINT (WINAPI *PFN_waveOutSetVolume)(HANDLE, DWORD);

#define CE_WAVE_FORMAT_PCM 1
#define CE_WAVE_MAPPER ((UINT)-1)
#define CE_WHDR_DONE 0x00000001
#define CE_MMSYSERR_NOERROR 0

struct CeWaveApi {
    HMODULE hMod = nullptr;
    PFN_waveOutOpen pOpen = nullptr;
    PFN_waveOutPrepareHeader pPrep = nullptr;
    PFN_waveOutWrite pWrite = nullptr;
    PFN_waveOutUnprepareHeader pUnprep = nullptr;
    PFN_waveOutReset pReset = nullptr;
    PFN_waveOutClose pClose = nullptr;
    PFN_waveOutGetNumDevs pGetNumDevs = nullptr;
    PFN_waveOutSetVolume pSetVolume = nullptr;

    bool init() {
        if (pOpen && pWrite) return true;
        hMod = LoadLibraryW(L"coredll.dll");
        if (hMod && !GetProcAddressW(hMod, L"waveOutOpen")) {
            hMod = LoadLibraryW(L"waveapi.dll");
        }
        if (!hMod) return false;
        pOpen = (PFN_waveOutOpen)GetProcAddressW(hMod, L"waveOutOpen");
        pPrep = (PFN_waveOutPrepareHeader)GetProcAddressW(hMod, L"waveOutPrepareHeader");
        pWrite = (PFN_waveOutWrite)GetProcAddressW(hMod, L"waveOutWrite");
        pUnprep = (PFN_waveOutUnprepareHeader)GetProcAddressW(hMod, L"waveOutUnprepareHeader");
        pReset = (PFN_waveOutReset)GetProcAddressW(hMod, L"waveOutReset");
        pClose = (PFN_waveOutClose)GetProcAddressW(hMod, L"waveOutClose");
        pGetNumDevs = (PFN_waveOutGetNumDevs)GetProcAddressW(hMod, L"waveOutGetNumDevs");
        pSetVolume = (PFN_waveOutSetVolume)GetProcAddressW(hMod, L"waveOutSetVolume");
        return (pOpen && pPrep && pWrite && pUnprep && pReset && pClose);
    }
};

} // namespace detail
} // namespace carlinkit

#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

namespace carlinkit {

class WavePlayer {
public:
    WavePlayer() = default;
    ~WavePlayer() { close(); }

    WavePlayer(const WavePlayer &) = delete;
    WavePlayer &operator=(const WavePlayer &) = delete;

    void set_volume(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 150) percent = 150;
        volume_percent_ = percent;
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (hWave_ && ce_api_.pSetVolume) {
            DWORD dwVol = static_cast<DWORD>((percent * 0xFFFF) / 100);
            if (dwVol > 0xFFFF) dwVol = 0xFFFF;
            DWORD dwStereo = dwVol | (dwVol << 16);
            ce_api_.pSetVolume(hWave_, dwStereo);
        }
#elif defined(_WIN32)
        if (hWave_) {
            DWORD dwVol = static_cast<DWORD>((percent * 0xFFFF) / 100);
            if (dwVol > 0xFFFF) dwVol = 0xFFFF;
            DWORD dwStereo = dwVol | (dwVol << 16);
            waveOutSetVolume(hWave_, dwStereo);
        }
#endif
    }

    int get_volume() const { return volume_percent_; }

    bool open(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample = 16) {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (hWave_) {
            if (sample_rate_ == sample_rate && channels_ == channels && bits_per_sample_ == bits_per_sample) {
                return true;
            }
            close();
        }
        sample_rate_ = sample_rate;
        channels_ = channels;
        bits_per_sample_ = bits_per_sample;

        if (!ce_api_.init()) {
            is_dummy_ = true;
            return true;
        }

        if (ce_api_.pGetNumDevs && ce_api_.pGetNumDevs() == 0) {
            is_dummy_ = true;
            return true;
        }

        detail::CE_WAVEFORMATEX wfx;
        std::memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag = CE_WAVE_FORMAT_PCM;
        wfx.nChannels = channels;
        wfx.nSamplesPerSec = sample_rate;
        wfx.wBitsPerSample = bits_per_sample;
        wfx.nBlockAlign = static_cast<WORD>((channels * bits_per_sample) / 8);
        wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
        wfx.cbSize = 0;

        HANDLE hw = nullptr;
        UINT res = ce_api_.pOpen(&hw, CE_WAVE_MAPPER, &wfx, 0, 0, 0);
        if (res != CE_MMSYSERR_NOERROR || !hw) {
            hWave_ = nullptr;
            is_dummy_ = true;
            return true;
        }
        hWave_ = hw;
        is_dummy_ = false;
        next_buf_ = 0;
        for (auto &slot : buffers_) {
            slot.in_use = false;
            slot.data.clear();
        }
        return true;

#elif defined(_WIN32)
        if (hWave_) {
            if (sample_rate_ == sample_rate && channels_ == channels && bits_per_sample_ == bits_per_sample) {
                return true;
            }
            close();
        }
        sample_rate_ = sample_rate;
        channels_ = channels;
        bits_per_sample_ = bits_per_sample;

        if (waveOutGetNumDevs() == 0) {
            is_dummy_ = true;
            return true;
        }

        WAVEFORMATEX wfx;
        std::memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = channels;
        wfx.nSamplesPerSec = sample_rate;
        wfx.wBitsPerSample = bits_per_sample;
        wfx.nBlockAlign = static_cast<WORD>((channels * bits_per_sample) / 8);
        wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
        wfx.cbSize = 0;

        MMRESULT res = waveOutOpen(&hWave_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
        if (res != MMSYSERR_NOERROR || !hWave_) {
            hWave_ = nullptr;
            is_dummy_ = true;
            return true;
        }
        is_dummy_ = false;
        next_buf_ = 0;
        for (auto &slot : buffers_) {
            slot.in_use = false;
            slot.data.clear();
        }
        std::printf("[AUDIO] Output started: %u Hz, %u ch, %u-bit PCM\n",
                    sample_rate, channels, bits_per_sample);
        return true;
#else
        sample_rate_ = sample_rate;
        channels_ = channels;
        bits_per_sample_ = bits_per_sample;
        return true;
#endif
    }

    void write(const uint8_t *pcm, std::size_t len) {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (is_dummy_ || !hWave_ || !pcm || len == 0) return;
        cleanup_done_buffers();

        BufferSlot *slot = nullptr;
        for (std::size_t i = 0; i < NUM_BUFFERS; ++i) {
            std::size_t idx = (next_buf_ + i) % NUM_BUFFERS;
            if (!buffers_[idx].in_use) {
                slot = &buffers_[idx];
                next_buf_ = (idx + 1) % NUM_BUFFERS;
                break;
            }
        }
        if (!slot) return;

        slot->data.assign(pcm, pcm + len);
        apply_volume(slot->data);
        std::memset(&slot->ce_hdr, 0, sizeof(detail::CE_WAVEHDR));
        slot->ce_hdr.lpData = reinterpret_cast<LPSTR>(slot->data.data());
        slot->ce_hdr.dwBufferLength = static_cast<DWORD>(slot->data.size());

        if (ce_api_.pPrep(hWave_, &slot->ce_hdr, sizeof(detail::CE_WAVEHDR)) == CE_MMSYSERR_NOERROR) {
            if (ce_api_.pWrite(hWave_, &slot->ce_hdr, sizeof(detail::CE_WAVEHDR)) == CE_MMSYSERR_NOERROR) {
                slot->in_use = true;
            } else {
                ce_api_.pUnprep(hWave_, &slot->ce_hdr, sizeof(detail::CE_WAVEHDR));
            }
        }

#elif defined(_WIN32)
        if (is_dummy_ || !hWave_ || !pcm || len == 0) return;
        cleanup_done_buffers();

        BufferSlot *slot = nullptr;
        for (std::size_t i = 0; i < NUM_BUFFERS; ++i) {
            std::size_t idx = (next_buf_ + i) % NUM_BUFFERS;
            if (!buffers_[idx].in_use) {
                slot = &buffers_[idx];
                next_buf_ = (idx + 1) % NUM_BUFFERS;
                break;
            }
        }
        if (!slot) return;

        slot->data.assign(pcm, pcm + len);
        apply_volume(slot->data);
        std::memset(&slot->hdr, 0, sizeof(WAVEHDR));
        slot->hdr.lpData = reinterpret_cast<LPSTR>(slot->data.data());
        slot->hdr.dwBufferLength = static_cast<DWORD>(slot->data.size());

        MMRESULT res = waveOutPrepareHeader(hWave_, &slot->hdr, sizeof(WAVEHDR));
        if (res == MMSYSERR_NOERROR) {
            res = waveOutWrite(hWave_, &slot->hdr, sizeof(WAVEHDR));
            if (res == MMSYSERR_NOERROR) {
                slot->in_use = true;
            } else {
                waveOutUnprepareHeader(hWave_, &slot->hdr, sizeof(WAVEHDR));
            }
        }
#else
        (void)pcm;
        (void)len;
#endif
    }

    void reset() {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (hWave_) {
            ce_api_.pReset(hWave_);
            for (auto &slot : buffers_) {
                if (slot.in_use) {
                    ce_api_.pUnprep(hWave_, &slot.ce_hdr, sizeof(detail::CE_WAVEHDR));
                    slot.in_use = false;
                }
            }
        }
#elif defined(_WIN32)
        if (hWave_) {
            waveOutReset(hWave_);
            for (auto &slot : buffers_) {
                if (slot.in_use) {
                    waveOutUnprepareHeader(hWave_, &slot.hdr, sizeof(WAVEHDR));
                    slot.in_use = false;
                }
            }
        }
#endif
    }

    void close() {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (hWave_) {
            reset();
            for (auto &slot : buffers_) {
                slot.data.clear();
                slot.data.shrink_to_fit();
            }
            ce_api_.pClose(hWave_);
            hWave_ = nullptr;
        }
        is_dummy_ = false;
#elif defined(_WIN32)
        if (hWave_) {
            reset();
            for (auto &slot : buffers_) {
                slot.data.clear();
                slot.data.shrink_to_fit();
            }
            waveOutClose(hWave_);
            hWave_ = nullptr;
        }
        is_dummy_ = false;
#endif
        sample_rate_ = 0;
        channels_ = 0;
        bits_per_sample_ = 0;
    }

    bool is_open() const {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        return (hWave_ != nullptr) || is_dummy_;
#elif defined(_WIN32)
        return (hWave_ != nullptr) || is_dummy_;
#else
        return sample_rate_ > 0;
#endif
    }

    uint32_t sample_rate() const { return sample_rate_; }
    uint16_t channels() const { return channels_; }

private:
    void cleanup_done_buffers() {
#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
        if (!hWave_) return;
        for (auto &slot : buffers_) {
            if (slot.in_use && (slot.ce_hdr.dwFlags & CE_WHDR_DONE)) {
                ce_api_.pUnprep(hWave_, &slot.ce_hdr, sizeof(detail::CE_WAVEHDR));
                slot.in_use = false;
            }
        }
#elif defined(_WIN32)
        if (!hWave_) return;
        for (auto &slot : buffers_) {
            if (slot.in_use && (slot.hdr.dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(hWave_, &slot.hdr, sizeof(WAVEHDR));
                slot.in_use = false;
            }
        }
#endif
    }

    void apply_volume(std::vector<uint8_t> &buf) {
        if (volume_percent_ != 100 && bits_per_sample_ == 16) {
            int16_t *samples = reinterpret_cast<int16_t*>(buf.data());
            std::size_t n_samples = buf.size() / sizeof(int16_t);
            if (volume_percent_ == 0) {
                std::memset(samples, 0, buf.size());
            } else {
                for (std::size_t i = 0; i < n_samples; ++i) {
                    int32_t val = (static_cast<int32_t>(samples[i]) * volume_percent_) / 100;
                    if (val > 32767) val = 32767;
                    else if (val < -32768) val = -32768;
                    samples[i] = static_cast<int16_t>(val);
                }
            }
        }
    }

    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint16_t bits_per_sample_ = 16;
    int volume_percent_ = 50;  // initial 50% to avoid startup pop
    bool is_dummy_ = false;

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
    detail::CeWaveApi ce_api_;
    HANDLE hWave_ = nullptr;
    struct BufferSlot {
        detail::CE_WAVEHDR ce_hdr{};
        std::vector<uint8_t> data;
        bool in_use = false;
    };
    static constexpr std::size_t NUM_BUFFERS = 16;
    BufferSlot buffers_[NUM_BUFFERS];
    std::size_t next_buf_ = 0;

#elif defined(_WIN32)
    HWAVEOUT hWave_ = nullptr;
    struct BufferSlot {
        WAVEHDR hdr{};
        std::vector<uint8_t> data;
        bool in_use = false;
    };
    static constexpr std::size_t NUM_BUFFERS = 16;
    BufferSlot buffers_[NUM_BUFFERS];
    std::size_t next_buf_ = 0;
#endif
};

} // namespace carlinkit
