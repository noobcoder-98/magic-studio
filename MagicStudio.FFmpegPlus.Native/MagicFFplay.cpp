// ============================================================================
// MagicFFplay.cpp -- C++ port of FFmpeg's ffplay.c
//
// Removed from upstream ffplay.c:
//   - SDL window / renderer / textures (SDL_Window, SDL_Renderer, SDL_Texture)
//   - Subtitle pipeline (subdec / subpq / subtitle_thread / AVSubtitle)
//   - RDFT / waveform audio visualisation (SHOW_MODE_WAVES / SHOW_MODE_RDFT)
//   - libavfilter video + audio graphs (configure_video_filters / audio_filters)
//   - cmdutils.h / opt_common.h / ffplay_renderer.h dependencies
//   - Vulkan / vk_renderer
//   - SDL event loop, keyboard / mouse handlers, main()
//
// Kept verbatim from ffplay.c:
//   - PacketQueue / FrameQueue / Decoder / Clock plumbing
//   - read_thread (demux), video_thread, audio_thread
//   - SDL audio output (sdl_audio_callback + audio_open) + swresample
//   - Full A/V sync: audio master clock, video_refresh, synchronize_audio,
//     framedrop, check_external_clock_speed, stream_seek, step playback
//   - force_refresh, step, EOF handling
//
// Added:
//   - Per-instance D3D11 device + ID3D11VideoProcessor (NV12 -> BGRA).
//   - D3D11VA hardware decode bound to that device.
//   - BGRA ID3D11Texture2D parked in each Frame slot so the UI wraps it as
//     a Win2D CanvasBitmap with zero CPU copy.
//   - magic_ffplay_* C-callable API (parallel to magic_player_* in
//     MagicFFmpegPlayer.cpp; the two are independent instances).
//
// Originally Copyright (c) 2003 Fabrice Bellard, LGPL v2.1+.
// ============================================================================

#include "pch.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// d3d11.h must come before any extern "C" block (operator overloads on
// structs like D3D11_VIEWPORT are C++-only and fail inside C linkage).
#include <d3d11.h>
#include <d3d11_4.h>      // ID3D11VideoContext2 (VideoProcessorSetStreamHDRMetaData)
#include <dxgi.h>
#include <dxgi1_5.h>      // DXGI_HDR_METADATA_HDR10, DXGI_HDR_METADATA_TYPE
#include <wrl/client.h>

extern "C" {
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <atomic>
#include <mutex>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "avfilter.lib")

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Tunables (from ffplay.c).
// ----------------------------------------------------------------------------

#define MAX_QUEUE_SIZE                  (15 * 1024 * 1024)
#define MIN_FRAMES                      25
#define EXTERNAL_CLOCK_MIN_FRAMES       2
#define EXTERNAL_CLOCK_MAX_FRAMES       10

#define SDL_AUDIO_MIN_BUFFER_SIZE       512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#define AV_SYNC_THRESHOLD_MIN           0.04
#define AV_SYNC_THRESHOLD_MAX           0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD      0.1
#define AV_NOSYNC_THRESHOLD             10.0

#define SAMPLE_CORRECTION_PERCENT_MAX   10

#define EXTERNAL_CLOCK_SPEED_MIN        0.900
#define EXTERNAL_CLOCK_SPEED_MAX        1.010
#define EXTERNAL_CLOCK_SPEED_STEP       0.001

#define AUDIO_DIFF_AVG_NB               20
#define REFRESH_RATE                    0.01

#define VIDEO_PICTURE_QUEUE_SIZE        16
#define SAMPLE_QUEUE_SIZE               9
#define FRAME_QUEUE_SIZE                FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

// ----------------------------------------------------------------------------
// Per-instance GPU pipeline (D3D11 device, VideoProcessor for NV12->BGRA).
// Each MagicFFplayHandle owns one; independent from MagicFFmpegPlayer's
// g_gpu singleton so both players can run simultaneously on separate devices.
// ----------------------------------------------------------------------------

struct FfplayGpu {
    ComPtr<ID3D11Device>                   device;
    ComPtr<ID3D11DeviceContext>            context;
    ComPtr<IDXGIDevice>                    dxgiDevice;
    ComPtr<ID3D11VideoDevice>              videoDevice;
    ComPtr<ID3D11VideoContext>             videoContext;
    ComPtr<ID3D11VideoContext2>            videoContext2;   // for ColorSpace1 + HDR metadata
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor>           processor;
    int  procW = 0;
    int  procH = 0;

    // Cached input description so we re-init the processor only when the
    // stream's color space (or size) actually changes.  Output is always
    // RGB BT.709 SDR (so the driver tone-maps HDR PQ/HLG -> SDR for us).
    DXGI_COLOR_SPACE_TYPE  inColorSpace  = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    DXGI_COLOR_SPACE_TYPE  outColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool                   hasHdrMeta    = false;
    DXGI_HDR_METADATA_HDR10 hdrMeta      = {};

    std::mutex          bltMutex;
    std::recursive_mutex ffmpegLock;
};

static void ffplay_gpu_lock_cb  (void* ctx) { reinterpret_cast<FfplayGpu*>(ctx)->ffmpegLock.lock();   }
static void ffplay_gpu_unlock_cb(void* ctx) { reinterpret_cast<FfplayGpu*>(ctx)->ffmpegLock.unlock(); }

static int ffplay_gpu_create(FfplayGpu* g) {
    if (g->device) return 0;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
               | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &g->device, nullptr, &g->context);
    if (FAILED(hr)) return -1;

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g->device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(g->device.As(&g->dxgiDevice)))    return -1;
    if (FAILED(g->device.As(&g->videoDevice)))   return -1;
    if (FAILED(g->context.As(&g->videoContext))) return -1;

    // ID3D11VideoContext2 (Win10 1607+) gives us VideoProcessorSetStream/
    // OutputColorSpace1 plus HDR static-metadata APIs.  Treat as fatal --
    // without it we cannot tone-map HDR -> SDR correctly.
    if (FAILED(g->videoContext.As(&g->videoContext2))) return -1;
    return 0;
}

// Map AVFrame color metadata to a DXGI_COLOR_SPACE_TYPE understood by the
// VideoProcessor.  When the input is HDR (PQ/HLG with BT.2020 primaries) the
// driver tone-maps it down to the SDR output color space we set below.
static DXGI_COLOR_SPACE_TYPE frame_to_dxgi_color_space(const AVFrame* f) {
    const bool full = (f->color_range == AVCOL_RANGE_JPEG);

    // HDR transfer functions take priority: signal them explicitly so the
    // driver knows to apply ST.2084 / HLG decoding before tone mapping.
    if (f->color_trc == AVCOL_TRC_SMPTE2084) {
        // HDR10 PQ -- DXGI only defines the studio-range variant.  Full-range
        // PQ is non-standard and broadcast/streaming content is always studio.
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (f->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        // HLG -- DXGI exposes only studio range; full-range HLG is non-standard.
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
    }

    // SDR.  Pick gamut + matrix from primaries / colorspace.
    const bool isBt2020 = (f->color_primaries == AVCOL_PRI_BT2020) ||
                          (f->colorspace      == AVCOL_SPC_BT2020_NCL) ||
                          (f->colorspace      == AVCOL_SPC_BT2020_CL);
    if (isBt2020) {
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                    : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }

    const bool isBt601 = (f->colorspace == AVCOL_SPC_BT470BG) ||
                         (f->colorspace == AVCOL_SPC_SMPTE170M);
    if (isBt601) {
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                    : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }

    // Default to BT.709 (covers AVCOL_SPC_BT709 and unknown).
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

// Pull HDR10 mastering-display + content-light side data off the frame and
// pack it into a DXGI_HDR_METADATA_HDR10.  Returns false if not present.
static bool frame_to_hdr10_metadata(const AVFrame* f, DXGI_HDR_METADATA_HDR10& out) {
    AVFrameSideData* mdSide =
        av_frame_get_side_data(const_cast<AVFrame*>(f), AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (!mdSide) return false;

    auto* md = reinterpret_cast<AVMasteringDisplayMetadata*>(mdSide->data);
    out = {};

    // FFmpeg stores primaries as AVRational in [0..1] (CIE xy * 50000 implied
    // by spec), but the docs say "normalized" -- we convert to the 0.00002
    // unit DXGI expects.
    if (md->has_primaries) {
        out.RedPrimary[0]   = (UINT16)(av_q2d(md->display_primaries[0][0]) * 50000.0);
        out.RedPrimary[1]   = (UINT16)(av_q2d(md->display_primaries[0][1]) * 50000.0);
        out.GreenPrimary[0] = (UINT16)(av_q2d(md->display_primaries[1][0]) * 50000.0);
        out.GreenPrimary[1] = (UINT16)(av_q2d(md->display_primaries[1][1]) * 50000.0);
        out.BluePrimary[0]  = (UINT16)(av_q2d(md->display_primaries[2][0]) * 50000.0);
        out.BluePrimary[1]  = (UINT16)(av_q2d(md->display_primaries[2][1]) * 50000.0);
        out.WhitePoint[0]   = (UINT16)(av_q2d(md->white_point[0])          * 50000.0);
        out.WhitePoint[1]   = (UINT16)(av_q2d(md->white_point[1])          * 50000.0);
    }
    if (md->has_luminance) {
        // DXGI: max in 1.0 nit units, min in 0.0001 nit units.
        out.MaxMasteringLuminance = (UINT)(av_q2d(md->max_luminance) * 1.0);
        out.MinMasteringLuminance = (UINT)(av_q2d(md->min_luminance) * 10000.0);
    }

    AVFrameSideData* clSide =
        av_frame_get_side_data(const_cast<AVFrame*>(f), AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (clSide) {
        auto* cl = reinterpret_cast<AVContentLightMetadata*>(clSide->data);
        out.MaxContentLightLevel      = (UINT16)cl->MaxCLL;
        out.MaxFrameAverageLightLevel = (UINT16)cl->MaxFALL;
    }
    return true;
}

static int ffplay_gpu_ensure_processor(FfplayGpu* g, int w, int h,
                                       DXGI_COLOR_SPACE_TYPE inCs,
                                       const DXGI_HDR_METADATA_HDR10* hdrMeta) {
    const bool sizeChanged   = !g->processor || g->procW != w || g->procH != h;
    const bool colorChanged  = inCs != g->inColorSpace;
    const bool metaChanged   = (hdrMeta != nullptr) != g->hasHdrMeta ||
                               (hdrMeta && memcmp(hdrMeta, &g->hdrMeta, sizeof(*hdrMeta)) != 0);

    if (sizeChanged) {
        g->processor.Reset();
        g->enumerator.Reset();
        g->procW = w;
        g->procH = h;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
        desc.InputFrameFormat           = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate.Numerator   = 60;
        desc.InputFrameRate.Denominator = 1;
        desc.InputWidth                 = (UINT)w;
        desc.InputHeight                = (UINT)h;
        desc.OutputFrameRate.Numerator  = 60;
        desc.OutputFrameRate.Denominator= 1;
        desc.OutputWidth                = (UINT)w;
        desc.OutputHeight               = (UINT)h;
        desc.Usage                      = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(g->videoDevice->CreateVideoProcessorEnumerator(&desc, &g->enumerator))) return -1;
        if (FAILED(g->videoDevice->CreateVideoProcessor(g->enumerator.Get(), 0, &g->processor))) return -1;

        // A fresh processor has no color-space / HDR state -- the inner
        // blocks below re-apply both because sizeChanged is true.
        g->hasHdrMeta = false;
    }

    if (sizeChanged || colorChanged) {
        g->videoContext2->VideoProcessorSetStreamColorSpace1(g->processor.Get(), 0, inCs);
        g->videoContext2->VideoProcessorSetOutputColorSpace1(g->processor.Get(),
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        g->inColorSpace  = inCs;
        g->outColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    if (sizeChanged || metaChanged) {
        if (hdrMeta) {
            g->videoContext2->VideoProcessorSetStreamHDRMetaData(
                g->processor.Get(), 0,
                DXGI_HDR_METADATA_TYPE_HDR10, sizeof(*hdrMeta), hdrMeta);
            g->hdrMeta    = *hdrMeta;
            g->hasHdrMeta = true;
        } else if (g->hasHdrMeta) {
            // Clear stale metadata when switching from HDR to SDR.
            g->videoContext2->VideoProcessorSetStreamHDRMetaData(
                g->processor.Get(), 0, DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
            g->hasHdrMeta = false;
        }
    }
    return 0;
}

// Convert one NV12 array-texture slice to a fresh BGRA ID3D11Texture2D.
// Returns AddRef'd; caller releases.
static ID3D11Texture2D* ffplay_gpu_nv12_to_bgra(FfplayGpu* g, ID3D11Texture2D* src, UINT slice) {
    if (!g->processor || !src) return nullptr;

    D3D11_TEXTURE2D_DESC outDesc = {};
    outDesc.Width            = (UINT)g->procW;
    outDesc.Height           = (UINT)g->procH;
    outDesc.MipLevels        = 1;
    outDesc.ArraySize        = 1;
    outDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage            = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> out;
    if (FAILED(g->device->CreateTexture2D(&outDesc, nullptr, &out))) return nullptr;

    std::lock_guard<std::mutex>           blt(g->bltMutex);
    std::lock_guard<std::recursive_mutex> ff(g->ffmpegLock);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(g->videoDevice->CreateVideoProcessorInputView(
            src, g->enumerator.Get(), &ivDesc, &inView))) return nullptr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(g->videoDevice->CreateVideoProcessorOutputView(
            out.Get(), g->enumerator.Get(), &ovDesc, &outView))) return nullptr;

    RECT r = { 0, 0, g->procW, g->procH };
    g->videoContext->VideoProcessorSetStreamSourceRect(g->processor.Get(), 0, TRUE, &r);
    g->videoContext->VideoProcessorSetStreamDestRect  (g->processor.Get(), 0, TRUE, &r);
    g->videoContext->VideoProcessorSetOutputTargetRect(g->processor.Get(),    TRUE, &r);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable        = TRUE;
    stream.OutputIndex   = 0;
    stream.pInputSurface = inView.Get();

    if (FAILED(g->videoContext->VideoProcessorBlt(
            g->processor.Get(), outView.Get(), 0, 1, &stream))) return nullptr;

    out->AddRef();
    return out.Get();
}

// ----------------------------------------------------------------------------
// ffplay-derived data structures (C++ port, subtitle fields removed).
// ----------------------------------------------------------------------------

struct MyAVPacketList {
    AVPacket* pkt;
    int       serial;
};

struct PacketQueue {
    AVFifo*    pkt_list;
    int        nb_packets;
    int        size;
    int64_t    duration;
    int        abort_request;
    int        serial;
    SDL_mutex* mutex;
    SDL_cond*  cond;
};

struct AudioParams {
    int                 freq;
    AVChannelLayout     ch_layout;
    enum AVSampleFormat fmt;
    int                 frame_size;
    int                 bytes_per_sec;
};

struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int    serial;
    int    paused;
    int*   queue_serial;
};

struct Frame {
    AVFrame*         frame;
    int              serial;
    double           pts;
    double           duration;
    int64_t          pos;
    int              width;
    int              height;
    int              format;
    AVRational       sar;
    ID3D11Texture2D* tex;      // BGRA output; AddRef'd; released by frame_queue_unref_item
    uint64_t         version;  // monotonic stamp; 0 = no texture yet
};

struct FrameQueue {
    Frame        queue[FRAME_QUEUE_SIZE];
    int          rindex;
    int          windex;
    int          size;
    int          max_size;
    int          keep_last;
    int          rindex_shown;
    SDL_mutex*   mutex;
    SDL_cond*    cond;
    PacketQueue* pktq;
};

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

struct Decoder {
    AVPacket*       pkt;
    PacketQueue*    queue;
    AVCodecContext* avctx;
    int             pkt_serial;
    int             finished;
    int             packet_pending;
    SDL_cond*       empty_queue_cond;
    int64_t         start_pts;
    AVRational      start_pts_tb;
    int64_t         next_pts;
    AVRational      next_pts_tb;
    SDL_Thread*     decoder_tid;
};

struct VideoState {
    SDL_Thread*     read_tid;
    SDL_Thread*     refresh_tid;
    int             abort_request;
    int             force_refresh;
    int             paused;
    int             last_paused;
    int             queue_attachments_req;
    int             seek_req;
    int             seek_flags;
    int64_t         seek_pos;
    int64_t         seek_rel;
    int             read_pause_return;
    AVFormatContext* ic;
    int             realtime;

    Clock           audclk;
    Clock           vidclk;
    Clock           extclk;

    FrameQueue      pictq;
    FrameQueue      sampq;

    Decoder         auddec;
    Decoder         viddec;

    int             audio_stream;
    int             av_sync_type;

    double          audio_clock;
    int             audio_clock_serial;
    double          audio_diff_cum;
    double          audio_diff_avg_coef;
    double          audio_diff_threshold;
    int             audio_diff_avg_count;
    AVStream*       audio_st;
    PacketQueue     audioq;
    int             audio_hw_buf_size;
    uint8_t*        audio_buf;
    uint8_t*        audio_buf1;
    unsigned int    audio_buf_size;
    unsigned int    audio_buf1_size;
    int             audio_buf_index;
    int             audio_write_buf_size;
    int             audio_volume;
    int             muted;
    AudioParams     audio_src;
    AudioParams     audio_tgt;
    SwrContext*     swr_ctx;
    int             frame_drops_early;
    int             frame_drops_late;

    double          frame_timer;
    int             video_stream;
    AVStream*       video_st;
    PacketQueue     videoq;
    double          max_frame_duration;
    int             eof;
    int             step;

    char*           filename;

    SDL_cond*       continue_read_thread;

    // Synchronous open handshake: read_thread sets prepared (1=ok / -1=err)
    // once avformat_open_input + stream components are ready, signals prep_cond.
    SDL_mutex*      prep_mutex;
    SDL_cond*       prep_cond;
    int             prepared;

    SDL_AudioDeviceID audio_dev;
    int64_t           audio_callback_time;

    // Per-instance GPU pipeline (NV12->BGRA).
    FfplayGpu*      gpu;

    // Frame version counter -- monotonically increases each time a new BGRA
    // texture is parked in pictq.  The UI compares against its last-seen
    // value to avoid redundant CanvasBitmap wrapping.
    std::atomic<uint64_t>* frame_version_seq;

    // ---- Playback speed & audio filter (libavfilter atempo) ----
    double            playback_speed;    // [0.1, 100.0], default 1.0
    bool              pitch_correction;  // true = atempo (preserve pitch);
                                         // false = asetrate+aresample (tape-like)
    std::atomic<int>* speed_changed;     // 1 = rebuild filter on next callback
    // Target-PTS clock: video frame selection at speed != 1.0 uses
    // target_pts = speed_base_pts + wall_elapsed * playback_speed
    // instead of audio-master A/V sync, which breaks at high speeds.
    double            speed_base_pts;      // timeline PTS snapshot
    int64_t           speed_base_wall_us;  // av_gettime_relative() snapshot

    // avfilter graph: abuffer → atempo/asetrate chain → abuffersink
    AVFilterGraph*    af_graph;
    AVFilterContext*  af_src;            // abuffer
    AVFilterContext*  af_sink;           // abuffersink
    int               af_serial;         // last audioq serial pushed into graph
};

// File-scope tunables (analogous to the static globals in ffplay.c).
static int    s_framedrop           = -1;   // -1=auto, 0=off, 1=always
static int    s_decoder_reorder_pts = -1;
static int    s_av_sync_type        = AV_SYNC_AUDIO_MASTER;

// ----------------------------------------------------------------------------
// PacketQueue (verbatim from ffplay.c, ported to C++).
// ----------------------------------------------------------------------------

static int packet_queue_put_private(PacketQueue* q, AVPacket* pkt) {
    if (q->abort_request) return -1;
    MyAVPacketList pkt1 = { pkt, q->serial };
    int ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0) return ret;
    q->nb_packets++;
    q->size     += pkt1.pkt->size + (int)sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue* q, AVPacket* pkt) {
    AVPacket* pkt1 = av_packet_alloc();
    if (!pkt1) { av_packet_unref(pkt); return -1; }
    av_packet_move_ref(pkt1, pkt);
    SDL_LockMutex(q->mutex);
    int ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);
    if (ret < 0) av_packet_free(&pkt1);
    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index) {
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static int packet_queue_init(PacketQueue* q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list) return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCond();
    if (!q->mutex || !q->cond) return AVERROR(ENOMEM);
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue* q) {
    MyAVPacketList pkt1;
    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size       = 0;
    q->duration   = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue* q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue* q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 1;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue* q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial) {
    MyAVPacketList pkt1;
    int ret;
    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) { ret = -1; break; }
        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size     -= pkt1.pkt->size + (int)sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial) *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0; break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// ----------------------------------------------------------------------------
// Decoder (subtitle branch removed).
// ----------------------------------------------------------------------------

static int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt) return AVERROR(ENOMEM);
    d->avctx            = avctx;
    d->queue            = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts        = AV_NOPTS_VALUE;
    d->pkt_serial       = -1;
    return 0;
}

static int decoder_decode_frame(Decoder* d, AVFrame* frame) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) return -1;

                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        if (s_decoder_reorder_pts == -1)
                            frame->pts = frame->best_effort_timestamp;
                        else if (!s_decoder_reorder_pts)
                            frame->pts = frame->pkt_dts;
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        AVRational tb = AVRational{ 1, frame->sample_rate };
                        if (frame->pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        else if (d->next_pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        if (frame->pts != AV_NOPTS_VALUE) {
                            d->next_pts    = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb;
                        }
                    }
                    break;
                default:
                    break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0) return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished    = 0;
                    d->next_pts    = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial) break;
            av_packet_unref(d->pkt);
        } while (1);

        if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
            av_log(d->avctx, AV_LOG_ERROR,
                "Receive_frame and send_packet both returned EAGAIN, API violation.\n");
            d->packet_pending = 1;
        } else {
            av_packet_unref(d->pkt);
        }
    }
}

static void decoder_destroy(Decoder* d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

// ----------------------------------------------------------------------------
// FrameQueue (subtitle avsubtitle_free removed; tex release added).
// ----------------------------------------------------------------------------

static void frame_queue_unref_item(Frame* vp) {
    av_frame_unref(vp->frame);
    if (vp->tex) { vp->tex->Release(); vp->tex = nullptr; }
}

static int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last) {
    memset(f, 0, sizeof(FrameQueue));
    f->mutex = SDL_CreateMutex();
    f->cond  = SDL_CreateCond();
    if (!f->mutex || !f->cond) return AVERROR(ENOMEM);
    f->pktq      = pktq;
    f->max_size  = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (int i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc())) return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue* f) {
    for (int i = 0; i < f->max_size; i++) {
        frame_queue_unref_item(&f->queue[i]);
        av_frame_free(&f->queue[i].frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue* f) {
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame* frame_queue_peek(FrameQueue* f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame* frame_queue_peek_next(FrameQueue* f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame* frame_queue_peek_last(FrameQueue* f) {
    return &f->queue[f->rindex];
}

static Frame* frame_queue_peek_writable(FrameQueue* f) {
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size && !f->pktq->abort_request)
        SDL_CondWait(f->cond, f->mutex);
    SDL_UnlockMutex(f->mutex);
    if (f->pktq->abort_request) return nullptr;
    return &f->queue[f->windex];
}

static Frame* frame_queue_peek_readable(FrameQueue* f) {
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
        SDL_CondWait(f->cond, f->mutex);
    SDL_UnlockMutex(f->mutex);
    if (f->pktq->abort_request) return nullptr;
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue* f) {
    if (++f->windex == f->max_size) f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue* f) {
    if (f->keep_last && !f->rindex_shown) { f->rindex_shown = 1; return; }
    // Hold mutex across unref so tex->Release() never races with
    // acquire_current_texture's tex->AddRef() (both now require the lock).
    SDL_LockMutex(f->mutex);
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size) f->rindex = 0;
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static int frame_queue_nb_remaining(FrameQueue* f) {
    return f->size - f->rindex_shown;
}

static int64_t frame_queue_last_pos(FrameQueue* f) {
    Frame* fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial) return fp->pos;
    return -1;
}

static void decoder_abort(Decoder* d, FrameQueue* fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, nullptr);
    d->decoder_tid = nullptr;
    packet_queue_flush(d->queue);
}

// ----------------------------------------------------------------------------
// Clocks (verbatim from ffplay.c).
// ----------------------------------------------------------------------------

static double get_clock(Clock* c) {
    if (*c->queue_serial != c->serial) return NAN;
    if (c->paused) return c->pts;
    double t = av_gettime_relative() / 1000000.0;
    return c->pts_drift + t - (t - c->last_updated) * (1.0 - c->speed);
}

static void set_clock_at(Clock* c, double pts, int serial, double time) {
    c->pts          = pts;
    c->last_updated = time;
    c->pts_drift    = c->pts - time;
    c->serial       = serial;
}

static void set_clock(Clock* c, double pts, int serial) {
    set_clock_at(c, pts, serial, av_gettime_relative() / 1000000.0);
}

static void set_clock_speed(Clock* c, double speed) {
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock* c, int* queue_serial) {
    c->speed        = 1.0;
    c->paused       = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock* c, Clock* slave) {
    double clk  = get_clock(c);
    double slv  = get_clock(slave);
    if (!isnan(slv) && (isnan(clk) || fabs(clk - slv) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slv, slave->serial);
}

static int get_master_sync_type(VideoState* is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
        return is->video_st ? AV_SYNC_VIDEO_MASTER : AV_SYNC_AUDIO_MASTER;
    if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
        return is->audio_st ? AV_SYNC_AUDIO_MASTER : AV_SYNC_EXTERNAL_CLOCK;
    return AV_SYNC_EXTERNAL_CLOCK;
}

static double get_master_clock(VideoState* is) {
    switch (get_master_sync_type(is)) {
    case AV_SYNC_VIDEO_MASTER: return get_clock(&is->vidclk);
    case AV_SYNC_AUDIO_MASTER: return get_clock(&is->audclk);
    default:                   return get_clock(&is->extclk);
    }
}

// Adjust external-clock speed to compensate for buffer under/over-run.
// Present in ffplay.c, omitted in MagicFFmpegPlayer.cpp.
static void check_external_clock_speed(VideoState* is) {
    if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
        set_clock_speed(&is->extclk,
            FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk,
            FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk,
                speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

// ----------------------------------------------------------------------------
// Pause / seek / step.
// ----------------------------------------------------------------------------

static void stream_seek(VideoState* is, int64_t pos, int64_t rel, int by_bytes) {
    if (is->seek_req) return;
    is->seek_pos    = pos;
    is->seek_rel    = rel;
    is->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes) is->seek_flags |= AVSEEK_FLAG_BYTE;
    is->seek_req = 1;
    SDL_CondSignal(is->continue_read_thread);
}

static void stream_toggle_pause(VideoState* is) {
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS))
            is->vidclk.paused = 0;
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState* is) {
    stream_toggle_pause(is);
    is->step = 0;
}

static void step_to_next_frame(VideoState* is) {
    if (is->paused) stream_toggle_pause(is);
    is->step = 1;
}

// ----------------------------------------------------------------------------
// A/V sync helpers.
// ----------------------------------------------------------------------------

static double compute_target_delay(double delay, VideoState* is) {
    double sync_threshold, diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }
    return delay;
}

static double vp_duration(VideoState* is, Frame* vp, Frame* nextvp) {
    if (vp->serial == nextvp->serial) {
        double dur = nextvp->pts - vp->pts;
        if (isnan(dur) || dur <= 0 || dur > is->max_frame_duration)
            return vp->duration;
        return dur;
    }
    return 0.0;
}

static void update_video_pts(VideoState* is, double pts, int serial) {
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

// Advance pictq's read-index past frames whose display deadline has passed.
// Does NOT touch any SDL display API.  The UI reads frame_queue_peek_last()
// at vsync cadence and wraps the tex as a CanvasBitmap.
static void video_refresh(VideoState* is, double* remaining_time) {
    if (!is->video_st) return;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

retry:
    if (frame_queue_nb_remaining(&is->pictq) == 0) {
        // Nothing to show yet; just return.
        return;
    }

    Frame* lastvp = frame_queue_peek_last(&is->pictq);
    Frame* vp     = frame_queue_peek(&is->pictq);

    if (vp->serial != is->videoq.serial) {
        frame_queue_next(&is->pictq);
        goto retry;
    }

    if (lastvp->serial != vp->serial) {
        is->frame_timer = av_gettime_relative() / 1000000.0;
        // Re-anchor speed clock to first frame after a seek/serial change.
        if (is->playback_speed != 1.0) {
            is->speed_base_pts     = isnan(vp->pts) ? 0.0 : vp->pts;
            is->speed_base_wall_us = av_gettime_relative();
        }
    }

    double last_duration;
    double delay;
    double time;

    if (is->paused) goto display;

    // -----------------------------------------------------------------------
    // Speed != 1.0: target-PTS clock drives frame selection.
    // Compute target_pts = snapshot_pts + elapsed_wall * speed, then advance
    // pictq until the current frame's PTS reaches that target.  This is the
    // same strategy used by professional editors (Premiere/CapCut): pick the
    // frame at the right timeline position, don't fight audio-master A/V sync.
    // -----------------------------------------------------------------------
    if (is->playback_speed != 1.0) {
        double wall_elapsed = (av_gettime_relative() - is->speed_base_wall_us) / 1e6;
        double target_pts   = is->speed_base_pts + wall_elapsed * is->playback_speed;

        for (;;) {
            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                is->force_refresh = 1;
                if (frame_queue_nb_remaining(&is->pictq) == 0) goto display;
                vp = frame_queue_peek(&is->pictq);
                continue;
            }

            // If there is a NEXT frame that is still at-or-before target_pts,
            // skip the current vp in favour of a closer frame.
            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame* nextvp = frame_queue_peek_next(&is->pictq);
                if (nextvp->serial == is->videoq.serial && nextvp->pts <= target_pts) {
                    frame_queue_next(&is->pictq);
                    is->force_refresh = 1;
                    vp = frame_queue_peek(&is->pictq);
                    continue;
                }
            }

            // vp is the best frame: show it if its PTS has been reached.
            if (!isnan(vp->pts) && vp->pts <= target_pts) {
                frame_queue_next(&is->pictq);   // mark vp as shown (becomes peek_last)
                is->force_refresh = 1;
                SDL_LockMutex(is->pictq.mutex);
                update_video_pts(is, vp->pts, vp->serial);
                SDL_UnlockMutex(is->pictq.mutex);
            }
            break;
        }
        goto display;
    }

    // -----------------------------------------------------------------------
    // Speed == 1.0: standard audio-master A/V sync (original ffplay logic).
    // -----------------------------------------------------------------------
    last_duration = vp_duration(is, lastvp, vp);
    delay         = compute_target_delay(last_duration, is);

    time = av_gettime_relative() / 1000000.0;
    if (time < is->frame_timer + delay) {
        *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
        goto display;
    }

    is->frame_timer += delay;
    if (time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
        is->frame_timer = time;

    SDL_LockMutex(is->pictq.mutex);
    if (!isnan(vp->pts))
        update_video_pts(is, vp->pts, vp->serial);
    SDL_UnlockMutex(is->pictq.mutex);

    if (frame_queue_nb_remaining(&is->pictq) > 1) {
        Frame* nextvp = frame_queue_peek_next(&is->pictq);
        double duration = vp_duration(is, vp, nextvp);
        if (!is->step &&
            (s_framedrop > 0 || (s_framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) &&
            time > is->frame_timer + duration) {
            is->frame_drops_late++;
            frame_queue_next(&is->pictq);
            goto retry;
        }
    }

    frame_queue_next(&is->pictq);
    is->force_refresh = 1;

    if (is->step && !is->paused)
        stream_toggle_pause(is);

display:
    if (is->force_refresh && is->pictq.rindex_shown) {
        // Lazy NV12→BGRA conversion: only the frame that is actually about to
        // be shown pays the VideoProcessor cost.  Skipped frames (fast-forward)
        // never reach here, so their GPU conversion is avoided entirely.
        Frame* fp = frame_queue_peek_last(&is->pictq);
        if (fp->tex == nullptr && fp->frame->format == AV_PIX_FMT_D3D11) {
            auto* nv12  = reinterpret_cast<ID3D11Texture2D*>(fp->frame->data[0]);
            UINT  slice = (UINT)(intptr_t)fp->frame->data[1];
            DXGI_COLOR_SPACE_TYPE inCs = frame_to_dxgi_color_space(fp->frame);
            DXGI_HDR_METADATA_HDR10 meta = {};
            const bool hasMeta = frame_to_hdr10_metadata(fp->frame, meta);
            ID3D11Texture2D* bgra = nullptr;
            if (ffplay_gpu_ensure_processor(is->gpu, fp->width, fp->height,
                                            inCs, hasMeta ? &meta : nullptr) == 0)
                bgra = ffplay_gpu_nv12_to_bgra(is->gpu, nv12, slice);
            SDL_LockMutex(is->pictq.mutex);
            fp->tex     = bgra;
            fp->version = bgra ? ++(*is->frame_version_seq) : 0;
            SDL_UnlockMutex(is->pictq.mutex);
        }
        is->force_refresh = 0;
    }
}

// ----------------------------------------------------------------------------
// Video pipeline: D3D11VA NV12 frame -> BGRA ID3D11Texture2D -> pictq.
// ----------------------------------------------------------------------------

static int queue_picture(VideoState* is, AVFrame* src_frame,
                         double pts, double duration, int64_t pos, int serial) {
    Frame* vp = frame_queue_peek_writable(&is->pictq);
    if (!vp) return -1;

    vp->sar      = src_frame->sample_aspect_ratio;
    vp->width    = src_frame->width;
    vp->height   = src_frame->height;
    vp->format   = src_frame->format;
    vp->pts      = pts;
    vp->duration = duration;
    vp->pos      = pos;
    vp->serial   = serial;

    // Release the BGRA tex from the previous occupant of this slot.
    if (vp->tex) {
        SDL_LockMutex(is->pictq.mutex);
        vp->tex->Release();
        vp->tex = nullptr;
        SDL_UnlockMutex(is->pictq.mutex);
    }

    // NV12→BGRA conversion is deferred to video_refresh (display section).
    // Frames that are skipped during fast-forward never reach the display path,
    // so their VideoProcessor cost is eliminated entirely.  The raw NV12 data
    // stays live in vp->frame until frame_queue_unref_item releases it.
    vp->tex     = nullptr;
    vp->version = 0;

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState* is, AVFrame* frame) {
    // Scale decoder effort with playback speed to reduce CPU usage.
    // skip_loop_filter: skip deblocking at > 2x (saves ~30% decode time).
    // skip_frame: NV12→BGRA conversion is now lazy (display path only), so the
    //   decode thread is no longer bottlenecked by VideoProcessor.  GPU-only H.264
    //   decode typically exceeds 500fps, comfortably covering x10 (300fps needed).
    //   AVDISCARD_NONKEY is only required at extreme speeds where raw GPU decode
    //   itself becomes the limit (above ~16x for typical 1080p content).
    {
        const double spd = is->playback_speed;
        is->viddec.avctx->skip_loop_filter =
            (spd > 2.0) ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
        is->viddec.avctx->skip_frame =
            (spd > 30.0) ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
    }

    int got = decoder_decode_frame(&is->viddec, frame);
    if (got < 0) return -1;
    if (!got)    return 0;

    double dpts = NAN;
    if (frame->pts != AV_NOPTS_VALUE)
        dpts = av_q2d(is->video_st->time_base) * frame->pts;

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

    // At speed != 1.0 the target-PTS selection in video_refresh handles frame
    // dropping.  Don't also drop here against an audio clock that advances Nx
    // faster than real time — that would starve pictq of every non-I-frame.
    if (is->playback_speed == 1.0 &&
        (s_framedrop > 0 || (s_framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))) {
        if (frame->pts != AV_NOPTS_VALUE) {
            double diff = dpts - get_master_clock(is);
            if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                diff < 0 &&
                is->viddec.pkt_serial == is->vidclk.serial &&
                is->videoq.nb_packets) {
                is->frame_drops_early++;
                av_frame_unref(frame);
                got = 0;
            }
        }
    }
    return got;
}

static int video_thread(void* arg) {
    VideoState* is         = (VideoState*)arg;
    AVFrame*    frame      = av_frame_alloc();
    AVRational  tb         = is->video_st->time_base;
    AVRational  frame_rate = av_guess_frame_rate(is->ic, is->video_st, nullptr);
    if (!frame) return AVERROR(ENOMEM);

    for (;;) {
        int ret = get_video_frame(is, frame);
        if (ret < 0) break;
        if (!ret)    continue;

        double duration = (frame_rate.num && frame_rate.den)
                          ? av_q2d(AVRational{ frame_rate.den, frame_rate.num }) : 0;
        double pts      = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        if (queue_picture(is, frame, pts, duration, -1, is->viddec.pkt_serial) < 0) {
            av_frame_unref(frame);
            break;
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    return 0;
}

// ----------------------------------------------------------------------------
// Audio: decode -> sampq -> SDL audio callback -> swr -> S16 stream.
// ----------------------------------------------------------------------------

static int audio_thread(void* arg) {
    VideoState* is    = (VideoState*)arg;
    AVFrame*    frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);

    int ret = 0;
    do {
        int got = decoder_decode_frame(&is->auddec, frame);
        if (got < 0) { ret = got; break; }
        if (!got) continue;

        Frame* af = frame_queue_peek_writable(&is->sampq);
        if (!af) break;

        AVRational tb = AVRational{ 1, frame->sample_rate };
        af->pts      = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->pos      = -1;
        af->serial   = is->auddec.pkt_serial;
        af->duration = av_q2d(AVRational{ frame->nb_samples, frame->sample_rate });

        av_frame_move_ref(af->frame, frame);
        frame_queue_push(&is->sampq);
    } while (ret >= 0 || ret == AVERROR(EAGAIN));

    av_frame_free(&frame);
    return ret;
}

// ----------------------------------------------------------------------------
// Audio filter graph (libavfilter atempo time-stretching).
// ----------------------------------------------------------------------------

// Build a filter-chain string for [speed] using atempo (range 0.5–100.0).
// For speed < 0.5 chain multiple atempo=0.5 stages until the remainder fits.
static std::string build_atempo_chain(double speed) {
    std::string chain;
    double rem = speed;
    while (rem < 0.5 - 1e-9) {
        chain += "atempo=0.5,";
        rem /= 0.5;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "atempo=%.6f", rem);
    chain += buf;
    return chain;
}

static void audio_filter_teardown(VideoState* is) {
    avfilter_graph_free(&is->af_graph);
    is->af_src  = nullptr;
    is->af_sink = nullptr;
}

// Build the avfilter graph for the current playback_speed / pitch_correction.
// ref supplies the decoded-frame format so abuffer can be configured correctly.
// pitch_correction is forced ON when speed >= 5.0 regardless of the flag.
static int audio_filter_init(VideoState* is, const AVFrame* ref) {
    const AVFilter* abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersrc || !abuffersink) return AVERROR(ENOSYS);

    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) return AVERROR(ENOMEM);

    const double speed    = is->playback_speed;
    const bool   pitch_on = is->pitch_correction || (speed >= 5.0);

    // Middle filter string (between abuffer and abuffersink).
    std::string filter_str;
    if (pitch_on) {
        filter_str = build_atempo_chain(speed);
    } else {
        // Tape-like: pretend the samples are at rate*speed, then resample back.
        // Effect: tempo and pitch both change proportionally (like a cassette).
        char buf[128];
        snprintf(buf, sizeof(buf), "asetrate=%d,aresample=%d",
                 (int)(ref->sample_rate * speed + 0.5), ref->sample_rate);
        filter_str = buf;
    }

    // abuffer source args.
    char ch_layout_str[64] = {};
    av_channel_layout_describe(&ref->ch_layout, ch_layout_str, sizeof(ch_layout_str));
    char src_args[256];
    snprintf(src_args, sizeof(src_args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d",
             ref->sample_rate,
             av_get_sample_fmt_name((AVSampleFormat)ref->format),
             ch_layout_str,
             ref->sample_rate);

    AVFilterContext* src_ctx  = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    int ret;

    ret = avfilter_graph_create_filter(&src_ctx,  abuffersrc,  "in",
                                       src_args, nullptr, graph);
    if (ret < 0) goto fail;
    ret = avfilter_graph_create_filter(&sink_ctx, abuffersink, "out",
                                       nullptr, nullptr, graph);
    if (ret < 0) goto fail;

    {
        // outputs = the SOURCE side of the parsed chain (feeds data IN).
        // inputs  = the SINK   side of the parsed chain (receives data OUT).
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = src_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = nullptr;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = nullptr;

        ret = avfilter_graph_parse_ptr(graph, filter_str.c_str(),
                                       &inputs, &outputs, nullptr);
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        if (ret < 0) goto fail;
    }

    ret = avfilter_graph_config(graph, nullptr);
    if (ret < 0) goto fail;

    is->af_graph = graph;
    is->af_src   = src_ctx;
    is->af_sink  = sink_ctx;
    return 0;

fail:
    avfilter_graph_free(&graph);
    return ret;
}

// Feed raw decoded frames into the filter graph and return one S16 output
// buffer.  Called from audio_decode_frame when playback_speed != 1.0.
// The inner loop reads from sampq until the filter produces output.
static int audio_decode_frame_filtered(VideoState* is) {
    AVFrame* filt = av_frame_alloc();
    if (!filt) return AVERROR(ENOMEM);

    for (;;) {
        // 1. Try to pull a filtered frame from the graph.
        int ret = is->af_graph ? av_buffersink_get_frame(is->af_sink, filt)
                               : AVERROR(EAGAIN);

        if (ret >= 0) {
            // ---- Filtered output available ----
            AVRational tb = av_buffersink_get_time_base(is->af_sink);
            // atempo/asetrate output PTS is in output-time coordinates.
            // Scale by playback_speed to convert to original-timeline coordinates
            // so audclk stays in sync with vidclk (which uses original PTS).
            if (filt->pts != AV_NOPTS_VALUE) {
                const double spd = is->playback_speed;
                is->audio_clock = filt->pts * av_q2d(tb) * spd
                                + (double)filt->nb_samples / filt->sample_rate * spd;
            } else {
                is->audio_clock = NAN;
            }
            is->audio_clock_serial = is->af_serial;

            // Re-init swr if the filtered-frame format differs from last time.
            const bool fmt_changed =
                !is->swr_ctx
                || filt->format      != is->audio_src.fmt
                || filt->sample_rate != is->audio_src.freq
                || av_channel_layout_compare(&filt->ch_layout,
                                             &is->audio_src.ch_layout);
            if (fmt_changed) {
                swr_free(&is->swr_ctx);
                if (swr_alloc_set_opts2(&is->swr_ctx,
                        &is->audio_tgt.ch_layout, is->audio_tgt.fmt,
                        is->audio_tgt.freq,
                        &filt->ch_layout, (AVSampleFormat)filt->format,
                        filt->sample_rate, 0, nullptr) < 0
                    || swr_init(is->swr_ctx) < 0) {
                    swr_free(&is->swr_ctx);
                    av_frame_free(&filt);
                    return -1;
                }
                av_channel_layout_copy(&is->audio_src.ch_layout, &filt->ch_layout);
                is->audio_src.freq = filt->sample_rate;
                is->audio_src.fmt  = (AVSampleFormat)filt->format;
            }

            int out_count = filt->nb_samples + 256;
            int out_size  = av_samples_get_buffer_size(nullptr,
                is->audio_tgt.ch_layout.nb_channels, out_count,
                is->audio_tgt.fmt, 0);
            if (out_size < 0) { av_frame_free(&filt); return -1; }

            av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size,
                           (size_t)out_size);
            if (!is->audio_buf1) { av_frame_free(&filt); return AVERROR(ENOMEM); }

            int len2 = swr_convert(is->swr_ctx, &is->audio_buf1, out_count,
                                   (const uint8_t**)filt->extended_data,
                                   filt->nb_samples);
            av_frame_free(&filt);
            if (len2 < 0) return -1;

            is->audio_buf = is->audio_buf1;
            return len2 * is->audio_tgt.ch_layout.nb_channels
                        * av_get_bytes_per_sample(is->audio_tgt.fmt);
        }

        if (ret != AVERROR(EAGAIN)) { av_frame_free(&filt); return -1; }

        // 2. Filter needs more input — read the next raw frame from sampq.
        Frame* af;
        do {
#if defined(_WIN32)
            while (frame_queue_nb_remaining(&is->sampq) == 0) {
                if ((av_gettime_relative() - is->audio_callback_time) >
                    1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec)
                { av_frame_free(&filt); return -1; }
                av_usleep(1000);
            }
#endif
            af = frame_queue_peek_readable(&is->sampq);
            if (!af) { av_frame_free(&filt); return -1; }
            frame_queue_next(&is->sampq);
        } while (af->serial != is->audioq.serial);

        // 3. Serial mismatch after seek — rebuild filter for the new segment.
        if (af->serial != is->af_serial) {
            audio_filter_teardown(is);
            is->af_serial = af->serial;
        }

        // 4. Lazy init: build the graph on the first frame (format now known).
        if (!is->af_graph) {
            if (audio_filter_init(is, af->frame) < 0) {
                av_frame_free(&filt);
                return -1;
            }
        }

        // 5. Push the raw frame into the filter.
        //    KEEP_REF: the filter takes its own reference; sampq keeps ownership.
        if (av_buffersrc_add_frame_flags(is->af_src, af->frame,
                                         AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            av_frame_free(&filt);
            return -1;
        }
    }
}

static int synchronize_audio(VideoState* is, int nb_samples) {
    int wanted = nb_samples;

    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff = get_clock(&is->audclk) - get_master_clock(is);
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                double avg = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                if (fabs(avg) >= is->audio_diff_threshold) {
                    wanted = nb_samples + (int)(diff * is->audio_src.freq);
                    int mn = nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    int mx = nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    wanted = av_clip(wanted, mn, mx);
                }
            }
        } else {
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }
    return wanted;
}

static int audio_decode_frame(VideoState* is) {
    if (is->paused) return -1;

    // Tear down the filter graph when speed or pitch_correction changed.
    // The filtered path rebuilds it lazily on the next frame.
    if (is->speed_changed->exchange(0))
        audio_filter_teardown(is);

    // Delegate to the filter path whenever speed != 1.0.
    if (is->playback_speed != 1.0)
        return audio_decode_frame_filtered(is);

    Frame* af;
    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - is->audio_callback_time) >
                1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep(1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq))) return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    int data_size = av_samples_get_buffer_size(
        nullptr, af->frame->ch_layout.nb_channels,
        af->frame->nb_samples, (AVSampleFormat)af->frame->format, 1);

    int wanted = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format       != is->audio_src.fmt ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate  != is->audio_src.freq ||
        (wanted != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        int ret = swr_alloc_set_opts2(&is->swr_ctx,
            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
            &af->frame->ch_layout, (AVSampleFormat)af->frame->format, af->frame->sample_rate,
            0, nullptr);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt  = (AVSampleFormat)af->frame->format;
    }

    int resampled_size;
    if (is->swr_ctx) {
        const uint8_t** in  = (const uint8_t**)af->frame->extended_data;
        uint8_t**       out = &is->audio_buf1;
        int out_count = (int64_t)wanted * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(
            nullptr, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        if (out_size < 0) return -1;

        if (wanted != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx,
                    (wanted - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                    wanted * is->audio_tgt.freq / af->frame->sample_rate) < 0)
                return -1;
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) return AVERROR(ENOMEM);
        int len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) return -1;
        if (len2 == out_count) {
            if (swr_init(is->swr_ctx) < 0) swr_free(&is->swr_ctx);
        }
        is->audio_buf       = is->audio_buf1;
        resampled_size      = len2 * is->audio_tgt.ch_layout.nb_channels *
                              av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf  = af->frame->data[0];
        resampled_size = data_size;
    }

    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
    return resampled_size;
}

static void sdl_audio_callback(void* opaque, Uint8* stream, int len) {
    VideoState* is = (VideoState*)opaque;
    is->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= (int)is->audio_buf_size) {
            int audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                is->audio_buf      = nullptr;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE /
                                     is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        int len1 = (int)is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME) {
            memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, is->audio_buf + is->audio_buf_index,
                                   AUDIO_S16SYS, len1, is->audio_volume);
        }
        len    -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = (int)is->audio_buf_size - is->audio_buf_index;

    if (!isnan(is->audio_clock)) {
        // Buffer compensation must be scaled by playback_speed: each byte in the
        // SDL audio buffer represents speed-times more original-timeline content.
        const double spd = is->playback_speed;
        set_clock_at(&is->audclk,
            is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                              is->audio_tgt.bytes_per_sec * spd,
            is->audio_clock_serial,
            is->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(VideoState* is, AVChannelLayout* wanted_ch_layout,
                      int wanted_sample_rate, AudioParams* audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    static const int next_nb_channels[]  = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sr_idx      = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_chans  = wanted_ch_layout->nb_channels;

    const char* env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_chans = atoi(env);
        av_channel_layout_uninit(wanted_ch_layout);
        av_channel_layout_default(wanted_ch_layout, wanted_nb_chans);
    }
    if (wanted_ch_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_ch_layout);
        av_channel_layout_default(wanted_ch_layout, wanted_nb_chans);
    }
    wanted_nb_chans = wanted_ch_layout->nb_channels;
    wanted_spec.channels = wanted_nb_chans;
    wanted_spec.freq     = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) return -1;

    while (next_sr_idx && next_sample_rates[next_sr_idx] >= wanted_spec.freq)
        next_sr_idx--;

    wanted_spec.format   = AUDIO_S16SYS;
    wanted_spec.silence  = 0;
    wanted_spec.samples  = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = is;

    while (!(is->audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq     = next_sample_rates[next_sr_idx--];
            wanted_spec.channels = wanted_nb_chans;
            if (!wanted_spec.freq) return -1;
        }
        av_channel_layout_default(wanted_ch_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) return -1;
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_ch_layout);
        av_channel_layout_default(wanted_ch_layout, spec.channels);
        if (wanted_ch_layout->order != AV_CHANNEL_ORDER_NATIVE) return -1;
    }

    audio_hw_params->fmt  = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_ch_layout) < 0) return -1;
    audio_hw_params->frame_size    = av_samples_get_buffer_size(
        nullptr, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(
        nullptr, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq,
        audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) return -1;
    return spec.size;
}

// ----------------------------------------------------------------------------
// D3D11VA hardware decode.
// ----------------------------------------------------------------------------

static enum AVPixelFormat get_d3d11_format(AVCodecContext* /*ctx*/, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    return fmts[0];
}

static int create_hwaccel(FfplayGpu* g, AVBufferRef** device_ctx) {
    if (ffplay_gpu_create(g) < 0) return AVERROR(ENOSYS);

    *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!*device_ctx) return AVERROR(ENOMEM);

    auto* dev = reinterpret_cast<AVHWDeviceContext*>((*device_ctx)->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(dev->hwctx);

    g->device->AddRef();
    d3d->device   = g->device.Get();
    d3d->lock     = ffplay_gpu_lock_cb;
    d3d->unlock   = ffplay_gpu_unlock_cb;
    d3d->lock_ctx = g;

    int ret = av_hwdevice_ctx_init(*device_ctx);
    if (ret < 0) av_buffer_unref(device_ctx);
    return ret;
}

// ----------------------------------------------------------------------------
// Stream open / close.
// ----------------------------------------------------------------------------

static int stream_component_open(VideoState* is, int stream_index) {
    AVFormatContext* ic = is->ic;
    AVCodecContext*  avctx;
    const AVCodec*   codec;
    AVDictionary*    opts      = nullptr;
    AVChannelLayout  ch_layout = {};
    int ret = 0;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) return -1;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) { ret = AVERROR(EINVAL); goto fail; }
    avctx->codec_id = codec->id;

    av_dict_set(&opts, "threads", "auto", 0);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(is->gpu, &avctx->hw_device_ctx);
        if (ret < 0) goto fail;
        avctx->get_format   = get_d3d11_format;
        avctx->thread_count = 1;
        avctx->thread_type  = 0;
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) goto fail;

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
        int sample_rate = avctx->sample_rate;
        if (av_channel_layout_copy(&ch_layout, &avctx->ch_layout) < 0) {
            ret = AVERROR(ENOMEM); goto fail;
        }
        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0) goto fail;
        is->audio_hw_buf_size  = ret;
        is->audio_src          = is->audio_tgt;
        is->audio_buf_size     = 0;
        is->audio_buf_index    = 0;
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        is->audio_diff_threshold = (double)is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;
        is->audio_stream = stream_index;
        is->audio_st     = ic->streams[stream_index];
        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
            goto fail;
        if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts    = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        packet_queue_start(is->auddec.queue);
        is->auddec.decoder_tid = SDL_CreateThread(audio_thread, "ffplay_audio", is);
        if (!is->auddec.decoder_tid) { ret = AVERROR(ENOMEM); goto out; }
        SDL_PauseAudioDevice(is->audio_dev, 0);
        break;
    }
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st     = ic->streams[stream_index];
        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
            goto fail;
        packet_queue_start(is->viddec.queue);
        is->viddec.decoder_tid = SDL_CreateThread(video_thread, "ffplay_video", is);
        if (!is->viddec.decoder_tid) { ret = AVERROR(ENOMEM); goto out; }
        is->queue_attachments_req = 1;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);
    return ret;
}

static void stream_component_close(VideoState* is, int stream_index) {
    AVFormatContext*   ic = is->ic;
    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) return;
    AVCodecParameters* par = ic->streams[stream_index]->codecpar;

    switch (par->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudioDevice(is->audio_dev);
        decoder_destroy(&is->auddec);
        audio_filter_teardown(is);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf       = nullptr;
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    if (par->codec_type == AVMEDIA_TYPE_AUDIO) { is->audio_st = nullptr; is->audio_stream = -1; }
    else if (par->codec_type == AVMEDIA_TYPE_VIDEO) { is->video_st = nullptr; is->video_stream = -1; }
}

// ----------------------------------------------------------------------------
// Demux / read thread.
// ----------------------------------------------------------------------------

static int decode_interrupt_cb(void* ctx) {
    return ((VideoState*)ctx)->abort_request;
}

static int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* q) {
    return stream_id < 0 ||
           q->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           (q->nb_packets > MIN_FRAMES &&
            (!q->duration || av_q2d(st->time_base) * q->duration > 1.0));
}

static int is_realtime(AVFormatContext* s) {
    if (!strcmp(s->iformat->name, "rtp") ||
        !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp"))
        return 1;
    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

static int read_thread(void* arg) {
    VideoState*      is         = (VideoState*)arg;
    AVFormatContext* ic         = nullptr;
    AVPacket*        pkt        = nullptr;
    SDL_mutex*       wait_mutex = SDL_CreateMutex();
    int ret = 0;

    if (!wait_mutex) { ret = AVERROR(ENOMEM); goto fail; }

    pkt = av_packet_alloc();
    if (!pkt) { ret = AVERROR(ENOMEM); goto fail; }

    ic = avformat_alloc_context();
    if (!ic) { ret = AVERROR(ENOMEM); goto fail; }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque   = is;

    if (avformat_open_input(&ic, is->filename, nullptr, nullptr) < 0) { ret = -1; goto fail; }
    is->ic = ic;

    if (avformat_find_stream_info(ic, nullptr) < 0) { ret = -1; goto fail; }
    if (ic->pb) ic->pb->eof_reached = 0;

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    for (unsigned i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    {
        int v_idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int a_idx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, v_idx, nullptr, 0);

        if (a_idx >= 0) stream_component_open(is, a_idx);
        if (v_idx >= 0) stream_component_open(is, v_idx);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) { ret = -1; goto fail; }

    is->realtime = is_realtime(ic);

    // Signal magic_ffplay_open that the file is ready.
    SDL_LockMutex(is->prep_mutex);
    is->prepared = 1;
    SDL_CondSignal(is->prep_cond);
    SDL_UnlockMutex(is->prep_mutex);

    for (;;) {
        if (is->abort_request) break;

        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) is->read_pause_return = av_read_pause(ic);
            else            av_read_play(ic);
        }

        if (is->seek_req) {
            int64_t target = is->seek_pos;
            int64_t mn = is->seek_rel > 0 ? target - is->seek_rel + 2 : INT64_MIN;
            int64_t mx = is->seek_rel < 0 ? target - is->seek_rel - 2 : INT64_MAX;
            int sret = avformat_seek_file(ic, -1, mn, target, mx, is->seek_flags);
            if (sret >= 0) {
                if (is->audio_stream >= 0) packet_queue_flush(&is->audioq);
                if (is->video_stream >= 0) packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE)
                    set_clock(&is->extclk, NAN, 0);
                else
                    set_clock(&is->extclk, target / (double)AV_TIME_BASE, 0);
            }
            is->seek_req              = 0;
            is->queue_attachments_req = 1;
            is->eof                   = 0;
            if (is->paused) is->force_refresh = 1;
        }

        if (is->queue_attachments_req) {
            if (is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                if (av_packet_ref(pkt, &is->video_st->attached_pic) >= 0) {
                    packet_queue_put(&is->videoq, pkt);
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                }
            }
            is->queue_attachments_req = 0;
        }

        if (is->audioq.size + is->videoq.size > MAX_QUEUE_SIZE ||
            (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
             stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq))) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }

        if (pkt->stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream &&
                   !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic) avformat_close_input(&ic);
    av_packet_free(&pkt);
    if (wait_mutex) SDL_DestroyMutex(wait_mutex);

    SDL_LockMutex(is->prep_mutex);
    if (is->prepared == 0) is->prepared = -1;
    SDL_CondSignal(is->prep_cond);
    SDL_UnlockMutex(is->prep_mutex);
    return 0;
}

// Worker thread driving video_refresh so the UI can poll at vsync cadence.
static int refresh_thread(void* arg) {
    VideoState* is = (VideoState*)arg;
    while (!is->abort_request) {
        double remaining = REFRESH_RATE;
        if (!is->paused) video_refresh(is, &remaining);
        if (remaining > 0) av_usleep((int64_t)(remaining * 1000000.0));
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Stream lifecycle.
// ----------------------------------------------------------------------------

static void stream_close(VideoState* is) {
    is->abort_request = 1;
    if (is->read_tid)    SDL_WaitThread(is->read_tid, nullptr);
    if (is->refresh_tid) SDL_WaitThread(is->refresh_tid, nullptr);

    if (is->audio_stream >= 0) stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0) stream_component_close(is, is->video_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);

    SDL_DestroyCond(is->continue_read_thread);
    if (is->prep_cond)  SDL_DestroyCond(is->prep_cond);
    if (is->prep_mutex) SDL_DestroyMutex(is->prep_mutex);

    av_free(is->filename);
    audio_filter_teardown(is);
    delete is->speed_changed;
    delete is->gpu;
    delete is->frame_version_seq;
    av_free(is);
}

static VideoState* stream_open(const char* filename) {
    VideoState* is = (VideoState*)av_mallocz(sizeof(VideoState));
    if (!is) return nullptr;

    is->gpu               = new FfplayGpu();
    is->frame_version_seq = new std::atomic<uint64_t>(0);
    is->video_stream      = -1;
    is->audio_stream      = -1;
    is->filename          = av_strdup(filename);
    if (!is->filename) goto fail;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE,        1) < 0) goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0) goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) goto fail;
    if (!(is->prep_mutex = SDL_CreateMutex()))           goto fail;
    if (!(is->prep_cond  = SDL_CreateCond()))            goto fail;

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_volume       = SDL_MIX_MAXVOLUME;
    is->muted              = 0;
    is->av_sync_type       = s_av_sync_type;

    is->playback_speed      = 1.0;
    is->pitch_correction    = true;
    is->speed_changed       = new std::atomic<int>(0);
    is->speed_base_pts      = 0.0;
    is->speed_base_wall_us  = av_gettime_relative();
    is->af_graph          = nullptr;
    is->af_src            = nullptr;
    is->af_sink           = nullptr;
    is->af_serial         = -1;

    is->read_tid    = SDL_CreateThread(read_thread,    "ffplay_read",    is);
    is->refresh_tid = SDL_CreateThread(refresh_thread, "ffplay_refresh", is);
    if (!is->read_tid || !is->refresh_tid) {
fail:
        if (is) stream_close(is);
        return nullptr;
    }
    return is;
}

// ============================================================================
// Public C API.
// ============================================================================

extern "C" {

struct MagicFFplayHandle {
    VideoState*             is;
    ComPtr<ID3D11Texture2D> staging;
    int                     staging_w = 0;
    int                     staging_h = 0;
    std::mutex              staging_mutex;
};

MagicFFplayHandle* magic_ffplay_open(const char* path) {
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return nullptr;

    VideoState* is = stream_open(path);
    if (!is) return nullptr;

    SDL_LockMutex(is->prep_mutex);
    while (is->prepared == 0)
        SDL_CondWait(is->prep_cond, is->prep_mutex);
    int prepared = is->prepared;
    SDL_UnlockMutex(is->prep_mutex);

    if (prepared < 0) {
        stream_close(is);
        return nullptr;
    }

    auto* h = new MagicFFplayHandle();
    h->is = is;
    return h;
}

void magic_ffplay_close(MagicFFplayHandle* h) {
    if (!h) return;
    if (h->is) stream_close(h->is);
    delete h;
}

void magic_ffplay_toggle_pause(MagicFFplayHandle* h) {
    if (h && h->is) toggle_pause(h->is);
}

int magic_ffplay_is_paused(MagicFFplayHandle* h) {
    return h && h->is ? h->is->paused : 0;
}

void magic_ffplay_seek_us(MagicFFplayHandle* h, int64_t position_us) {
    if (!h || !h->is) return;
    int64_t target = av_rescale(position_us, AV_TIME_BASE, 1'000'000LL);
    stream_seek(h->is, target, 0, 0);
}

void magic_ffplay_step_frame(MagicFFplayHandle* h) {
    if (h && h->is) step_to_next_frame(h->is);
}

int64_t magic_ffplay_master_clock_us(MagicFFplayHandle* h) {
    if (!h || !h->is) return 0;
    double t = get_master_clock(h->is);
    if (isnan(t)) return 0;
    return (int64_t)(t * 1'000'000.0);
}

double magic_ffplay_duration_seconds(MagicFFplayHandle* h) {
    if (!h || !h->is || !h->is->ic || h->is->ic->duration == AV_NOPTS_VALUE) return 0;
    return (double)h->is->ic->duration / AV_TIME_BASE;
}

int magic_ffplay_video_size(MagicFFplayHandle* h, int* w, int* out_h) {
    if (!h || !h->is || !h->is->video_st) return 0;
    *w     = h->is->video_st->codecpar->width;
    *out_h = h->is->video_st->codecpar->height;
    return 1;
}

int magic_ffplay_acquire_current_texture(MagicFFplayHandle* h, ID3D11Texture2D** out) {
    if (!h || !h->is || !out) return 0;
    *out = nullptr;
    SDL_LockMutex(h->is->pictq.mutex);
    if (h->is->pictq.rindex_shown) {
        Frame* fp = &h->is->pictq.queue[h->is->pictq.rindex];
        if (fp->tex) {
            fp->tex->AddRef();
            *out = fp->tex;
        }
    }
    SDL_UnlockMutex(h->is->pictq.mutex);
    return *out ? 1 : 0;
}

uint64_t magic_ffplay_current_frame_version(MagicFFplayHandle* h) {
    if (!h || !h->is) return 0;
    uint64_t v = 0;
    SDL_LockMutex(h->is->pictq.mutex);
    if (h->is->pictq.rindex_shown)
        v = h->is->pictq.queue[h->is->pictq.rindex].version;
    SDL_UnlockMutex(h->is->pictq.mutex);
    return v;
}

int magic_ffplay_copy_current_bgra(MagicFFplayHandle* h,
                                   uint8_t* dst, int dst_capacity_bytes,
                                   int* out_w, int* out_h) {
    if (!h || !dst || !out_w || !out_h) return 0;

    ID3D11Texture2D* src = nullptr;
    if (!magic_ffplay_acquire_current_texture(h, &src) || !src) return 0;

    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    const int w      = (int)desc.Width;
    const int hh     = (int)desc.Height;
    const int needed = w * hh * 4;
    if (dst_capacity_bytes < needed) { src->Release(); return 0; }

    std::lock_guard<std::mutex> lk(h->staging_mutex);

    if (!h->staging || h->staging_w != w || h->staging_h != hh) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width            = (UINT)w;
        sd.Height           = (UINT)hh;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        h->staging.Reset();
        FfplayGpu* g = h->is->gpu;
        if (FAILED(g->device->CreateTexture2D(&sd, nullptr, &h->staging))) {
            src->Release(); return 0;
        }
        h->staging_w = w;
        h->staging_h = hh;
    }

    FfplayGpu*            g = h->is->gpu;
    D3D11_MAPPED_SUBRESOURCE map = {};
    HRESULT hr;
    {
        std::lock_guard<std::mutex> blt(g->bltMutex);
        g->context->CopyResource(h->staging.Get(), src);
        hr = g->context->Map(h->staging.Get(), 0, D3D11_MAP_READ, 0, &map);
    }
    src->Release();
    if (FAILED(hr)) return 0;

    const int row_bytes = w * 4;
    const uint8_t* srcRow = (const uint8_t*)map.pData;
    uint8_t*       dstRow = dst;
    for (int y = 0; y < hh; ++y, srcRow += map.RowPitch, dstRow += row_bytes)
        memcpy(dstRow, srcRow, row_bytes);
    g->context->Unmap(h->staging.Get(), 0);

    *out_w = w;
    *out_h = hh;
    return 1;
}

void magic_ffplay_set_speed(MagicFFplayHandle* h, double speed) {
    if (!h || !h->is) return;
    double cur = get_master_clock(h->is);
    h->is->speed_base_pts     = isnan(cur) ? 0.0 : cur;
    h->is->speed_base_wall_us = av_gettime_relative();
    h->is->playback_speed     = av_clipd(speed, 0.1, 100.0);
    h->is->speed_changed->store(1);
    // Wake the read thread so it refills queues immediately at the new rate.
    SDL_CondSignal(h->is->continue_read_thread);
}

double magic_ffplay_get_speed(MagicFFplayHandle* h) {
    return (h && h->is) ? h->is->playback_speed : 1.0;
}

// enabled: 1 = preserve pitch (atempo); 0 = tape-like pitch shift (asetrate).
// At speed >= 5.0 pitch correction is always on regardless of this flag.
void magic_ffplay_set_pitch_correction(MagicFFplayHandle* h, int enabled) {
    if (!h || !h->is) return;
    h->is->pitch_correction = (enabled != 0);
    h->is->speed_changed->store(1);
}

int magic_ffplay_get_pitch_correction(MagicFFplayHandle* h) {
    return (h && h->is) ? (h->is->pitch_correction ? 1 : 0) : 1;
}

// Returns the AddRef'd IDXGIDevice* of this handle's own D3D11 device.
int magic_ffplay_acquire_dxgi_device(MagicFFplayHandle* h, IDXGIDevice** out) {
    if (!h || !out) return 0;
    *out = nullptr;
    FfplayGpu* g = h->is ? h->is->gpu : nullptr;
    if (!g) return 0;
    if (ffplay_gpu_create(g) < 0) return 0;
    g->dxgiDevice->AddRef();
    *out = g->dxgiDevice.Get();
    return 1;
}

} // extern "C"
