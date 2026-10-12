#pragma once
// h264_decoder_ce.h — Dynamic H.264 video decoding using FFmpeg ARM DLLs on WinCE 6.0
// Dynamically loads:
//   - avutil-54.dll
//   - swscale-3.dll
//   - avcodec-56.dll
// Zero static dependencies. Graceful fallback on desktop / unit tests.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

#if defined(__MINGW32CE__) || defined(_WIN32_WCE)
#include <windows.h>

namespace carlinkit {

class H264Decoder_CE {
public:
    H264Decoder_CE() = default;
    ~H264Decoder_CE() { close(); }

    H264Decoder_CE(const H264Decoder_CE &) = delete;
    H264Decoder_CE &operator=(const H264Decoder_CE &) = delete;

    bool is_ready() const { return ready_; }

    bool init() {
        if (ready_) return true;

        hAvutil_ = load_dll(L"avutil-54.dll");
        hSwscale_ = load_dll(L"swscale-3.dll");
        hAvcodec_ = load_dll(L"avcodec-56.dll");

        if (!hAvutil_ || !hSwscale_ || !hAvcodec_) {
            close();
            return false;
        }

        p_avcodec_register_all = (PFN_avcodec_register_all)GetProcAddressW(hAvcodec_, L"avcodec_register_all");
        p_avcodec_find_decoder_by_name = (PFN_avcodec_find_decoder_by_name)GetProcAddressW(hAvcodec_, L"avcodec_find_decoder_by_name");
        p_avcodec_find_decoder = (PFN_avcodec_find_decoder)GetProcAddressW(hAvcodec_, L"avcodec_find_decoder");
        p_avcodec_alloc_context3 = (PFN_avcodec_alloc_context3)GetProcAddressW(hAvcodec_, L"avcodec_alloc_context3");
        p_avcodec_open2 = (PFN_avcodec_open2)GetProcAddressW(hAvcodec_, L"avcodec_open2");
        p_avcodec_decode_video2 = (PFN_avcodec_decode_video2)GetProcAddressW(hAvcodec_, L"avcodec_decode_video2");
        p_avcodec_close = (PFN_avcodec_close)GetProcAddressW(hAvcodec_, L"avcodec_close");
        p_av_init_packet = (PFN_av_init_packet)GetProcAddressW(hAvcodec_, L"av_init_packet");

        p_av_frame_alloc = (PFN_av_frame_alloc)GetProcAddressW(hAvutil_, L"av_frame_alloc");
        p_av_frame_free = (PFN_av_frame_free)GetProcAddressW(hAvutil_, L"av_frame_free");
        p_av_get_pix_fmt = (PFN_av_get_pix_fmt)GetProcAddressW(hAvutil_, L"av_get_pix_fmt");

        p_sws_getContext = (PFN_sws_getContext)GetProcAddressW(hSwscale_, L"sws_getContext");
        p_sws_scale = (PFN_sws_scale)GetProcAddressW(hSwscale_, L"sws_scale");
        p_sws_freeContext = (PFN_sws_freeContext)GetProcAddressW(hSwscale_, L"sws_freeContext");

        if (!p_avcodec_register_all || !p_avcodec_alloc_context3 || !p_avcodec_open2 ||
            !p_avcodec_decode_video2 || !p_av_init_packet || !p_av_frame_alloc ||
            !p_sws_getContext || !p_sws_scale || !p_sws_freeContext) {
            close();
            return false;
        }

        p_avcodec_register_all();

        void *codec = nullptr;
        if (p_avcodec_find_decoder_by_name) {
            codec = p_avcodec_find_decoder_by_name("h264");
        }
        if (!codec && p_avcodec_find_decoder) {
            codec = p_avcodec_find_decoder(27); // AV_CODEC_ID_H264
        }
        if (!codec) {
            close();
            return false;
        }

        codec_ctx_ = p_avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            close();
            return false;
        }

        if (p_avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            close();
            return false;
        }

        frame_ = p_av_frame_alloc();
        if (!frame_) {
            close();
            return false;
        }

        pix_fmt_rgb565_ = p_av_get_pix_fmt ? p_av_get_pix_fmt("rgb565le") : 44;
        if (pix_fmt_rgb565_ < 0) pix_fmt_rgb565_ = 44;

        ready_ = true;
        return true;
    }

    bool decode_frame(const uint8_t *h264, size_t len, uint8_t *out_rgb565, int stride,
                      int *out_w = nullptr, int *out_h = nullptr) {
        if (!ready_ || !h264 || len == 0 || !out_rgb565) return false;

        uint8_t pkt_buf[128];
        std::memset(pkt_buf, 0, sizeof(pkt_buf));
        p_av_init_packet(pkt_buf);
        *reinterpret_cast<uint8_t**>(pkt_buf + 0x18) = const_cast<uint8_t*>(h264);
        *reinterpret_cast<int*>(pkt_buf + 0x1C) = static_cast<int>(len);

        int got_picture = 0;
        int ret = p_avcodec_decode_video2(codec_ctx_, frame_, &got_picture, pkt_buf);
        if (ret < 0 || !got_picture) {
            return false;
        }

        int fw = *reinterpret_cast<int*>(reinterpret_cast<char*>(frame_) + 0x44);
        int fh = *reinterpret_cast<int*>(reinterpret_cast<char*>(frame_) + 0x48);
        if (fw <= 0) fw = 1024;
        if (fh <= 0) fh = 600;

        if (out_w) *out_w = fw;
        if (out_h) *out_h = fh;

        if (!sws_ctx_ || sws_w_ != fw || sws_h_ != fh) {
            if (sws_ctx_) p_sws_freeContext(sws_ctx_);
            sws_ctx_ = p_sws_getContext(fw, fh, 0 /* YUV420P */,
                                        1024, 600, pix_fmt_rgb565_,
                                        1 /* SWS_FAST_BILINEAR */,
                                        nullptr, nullptr, nullptr);
            sws_w_ = fw;
            sws_h_ = fh;
        }

        if (!sws_ctx_) return false;

        uint8_t **srcData = reinterpret_cast<uint8_t**>(frame_);
        int *srcStride = reinterpret_cast<int*>(reinterpret_cast<char*>(frame_) + 0x20);

        uint8_t *dstData[4] = { out_rgb565, nullptr, nullptr, nullptr };
        int dstStride[4] = { stride, 0, 0, 0 };

        p_sws_scale(sws_ctx_, srcData, srcStride, 0, fh, dstData, dstStride);
        return true;
    }

    void close() {
        if (sws_ctx_ && p_sws_freeContext) {
            p_sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }
        if (frame_ && p_av_frame_free) {
            p_av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (codec_ctx_ && p_avcodec_close) {
            p_avcodec_close(codec_ctx_);
            codec_ctx_ = nullptr;
        }
        if (hAvcodec_) { FreeLibrary(hAvcodec_); hAvcodec_ = nullptr; }
        if (hSwscale_) { FreeLibrary(hSwscale_); hSwscale_ = nullptr; }
        if (hAvutil_)  { FreeLibrary(hAvutil_);  hAvutil_ = nullptr; }
        ready_ = false;
        sws_w_ = 0;
        sws_h_ = 0;
    }

private:
    static HMODULE load_dll(const WCHAR *dll_name) {
        HMODULE h = LoadLibraryW(dll_name);
        if (h) return h;
        const WCHAR *paths[] = {
            L"\\Storage Card\\gps_byd\\",
            L"\\StorageCard\\gps_byd\\",
            L"\\GPS_BYD\\",
            L"\\SDMMC\\gps_byd\\",
            L"\\Storage Card\\",
            L"\\StorageCard\\",
            L"\\SDMMC\\"
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            WCHAR full[MAX_PATH];
            _snwprintf(full, MAX_PATH - 1, L"%s%s", paths[i], dll_name);
            full[MAX_PATH - 1] = 0;
            h = LoadLibraryW(full);
            if (h) return h;
        }
        return nullptr;
    }

    typedef void (*PFN_avcodec_register_all)(void);
    typedef void* (*PFN_avcodec_find_decoder_by_name)(const char *name);
    typedef void* (*PFN_avcodec_find_decoder)(int id);
    typedef void* (*PFN_avcodec_alloc_context3)(const void *codec);
    typedef int (*PFN_avcodec_open2)(void *avctx, const void *codec, void **options);
    typedef int (*PFN_avcodec_decode_video2)(void *avctx, void *picture, int *got_picture_ptr, const void *avpkt);
    typedef int (*PFN_avcodec_close)(void *avctx);
    typedef void (*PFN_av_init_packet)(void *pkt);

    typedef void* (*PFN_av_frame_alloc)(void);
    typedef void (*PFN_av_frame_free)(void **frame);
    typedef int (*PFN_av_get_pix_fmt)(const char *name);

    typedef void* (*PFN_sws_getContext)(int srcW, int srcH, int srcFormat,
                                        int dstW, int dstH, int dstFormat,
                                        int flags, void *srcFilter, void *dstFilter, const double *param);
    typedef int (*PFN_sws_scale)(void *context, const uint8_t *const srcSlice[],
                                 const int srcStride[], int srcSliceY, int srcSliceH,
                                 uint8_t *const dst[], const int dstStride[]);
    typedef void (*PFN_sws_freeContext)(void *swsContext);

    HMODULE hAvutil_ = nullptr;
    HMODULE hSwscale_ = nullptr;
    HMODULE hAvcodec_ = nullptr;

    PFN_avcodec_register_all p_avcodec_register_all = nullptr;
    PFN_avcodec_find_decoder_by_name p_avcodec_find_decoder_by_name = nullptr;
    PFN_avcodec_find_decoder p_avcodec_find_decoder = nullptr;
    PFN_avcodec_alloc_context3 p_avcodec_alloc_context3 = nullptr;
    PFN_avcodec_open2 p_avcodec_open2 = nullptr;
    PFN_avcodec_decode_video2 p_avcodec_decode_video2 = nullptr;
    PFN_avcodec_close p_avcodec_close = nullptr;
    PFN_av_init_packet p_av_init_packet = nullptr;

    PFN_av_frame_alloc p_av_frame_alloc = nullptr;
    PFN_av_frame_free p_av_frame_free = nullptr;
    PFN_av_get_pix_fmt p_av_get_pix_fmt = nullptr;

    PFN_sws_getContext p_sws_getContext = nullptr;
    PFN_sws_scale p_sws_scale = nullptr;
    PFN_sws_freeContext p_sws_freeContext = nullptr;

    void *codec_ctx_ = nullptr;
    void *frame_ = nullptr;
    void *sws_ctx_ = nullptr;
    int sws_w_ = 0;
    int sws_h_ = 0;
    int pix_fmt_rgb565_ = 44;
    bool ready_ = false;
};

} // namespace carlinkit

#else

namespace carlinkit {

class H264Decoder_CE {
public:
    bool init() { return true; }
    bool is_ready() const { return true; }
    bool decode_frame(const uint8_t*, size_t, uint8_t*, int, int* = nullptr, int* = nullptr) { return false; }
    void close() {}
};

} // namespace carlinkit

#endif
