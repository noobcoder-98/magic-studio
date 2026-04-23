// ============================================================================
// MagicFFmpegPlayer.cpp -- a stripped-down ffplay variant intended to be
// embedded in MagicStudio's Win2D-based UI.
//
// Removed from upstream ffplay:
//   - All SDL UI: window/renderer/textures, event loop, command-line option
//     parsing, main(), do_exit, signal handlers, fullscreen/cursor handling
//   - Subtitle pipeline (subdec / subpq / subtitle_thread / AVSubtitle)
//   - libavfilter video & audio graphs (configure_video_filters /
//     configure_audio_filters / autorotate / aresample), buffersrc/buffersink
//   - RDFT audio visualisation (SHOW_MODE_WAVES / SHOW_MODE_RDFT, sample_array)
//   - cmdutils.h dependencies (codec_opts / format_opts / parse_options /
//     filter_codec_opts / show_help_*), opt_common.h, ffplay_renderer.h,
//     Vulkan / vk_renderer
//
// Kept verbatim from ffplay:
//   - PacketQueue / FrameQueue / Decoder / Clock plumbing
//   - read_thread (demux), video_thread, audio_thread
//   - SDL audio output (sdl_audio_callback + audio_open) + swresample
//   - A/V sync (audio master clock, video_refresh timing, synchronize_audio,
//     framedrop), stream_seek, pause/resume, EOF handling
//
// Added:
//   - Forced D3D11VA hardware decode on a shared ID3D11Device.
//   - GPU-side NV12 -> BGRA conversion via ID3D11VideoProcessor; the BGRA
//     ID3D11Texture2D is parked alongside each Frame so the UI can wrap it
//     as a Win2D CanvasBitmap with zero CPU copy.
//   - magic_player_* C-callable API for the C++/CLI wrapper.
//
// Originally Copyright (c) 2003 Fabrice Bellard, LGPL v2.1+.
// ============================================================================

#include "pch.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// d3d11.h / dxgi.h declare C++ operator overloads (==, !=) on structs such as
// D3D11_VIEWPORT / D3D11_RECT via guiddef.h. They must be included *before* any
// extern "C" block; otherwise libavutil/hwcontext_d3d11va.h pulls d3d11.h in
// with C linkage and the compiler rejects the overloads with C2733.
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
// Native is a static library, so these directives ride with the .lib and
// resolve the FFmpeg + SDL2 imports for whoever links us downstream
// (the C++/CLI Wrapper). Library search path is supplied by the consumer.
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "SDL2.lib")

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Tunables (lifted from ffplay).
// ----------------------------------------------------------------------------

#define MAX_QUEUE_SIZE              (15 * 1024 * 1024)
#define MIN_FRAMES                  25

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE   512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN       0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX       0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD  0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD         10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB           20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE                0.01

#define VIDEO_PICTURE_QUEUE_SIZE    3
#define SAMPLE_QUEUE_SIZE           9
#define FRAME_QUEUE_SIZE            FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

// ----------------------------------------------------------------------------
// Shared D3D11 device + NV12 -> BGRA video processor.
// One instance, reference-counted so the host can fetch the IDXGIDevice and
// hand it to Win2D (CanvasDevice.CreateFromDirect3D11Device) on the same GPU
// as the decoder + converter.
// ----------------------------------------------------------------------------

struct GpuPipeline {
    ComPtr<ID3D11Device>                   device;
    ComPtr<ID3D11DeviceContext>            context;
    ComPtr<IDXGIDevice>                    dxgiDevice;
    ComPtr<ID3D11VideoDevice>              videoDevice;
    ComPtr<ID3D11VideoContext>             videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor>           processor;
    int  width  = 0;
    int  height = 0;
    std::mutex          bltMutex;     // serialises VideoProcessorBlt + view creation
    std::recursive_mutex ffmpegLock;  // hooked into AVD3D11VADeviceContext.lock
};

static GpuPipeline g_gpu;

static void gpu_lock_cb(void* /*ctx*/)   { g_gpu.ffmpegLock.lock();   }
static void gpu_unlock_cb(void* /*ctx*/) { g_gpu.ffmpegLock.unlock(); }

static int gpu_create_device() {
    if (g_gpu.device) return 0;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT      // required by Direct2D / Win2D
               | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;    // required by VideoProcessor
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &g_gpu.device, nullptr, &g_gpu.context);
    if (FAILED(hr)) return -1;

    // FFmpeg's d3d11va decoder and Win2D may both touch the immediate context
    // from different threads -- opt into D3D11's internal serialisation.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g_gpu.device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(g_gpu.device.As(&g_gpu.dxgiDevice)))    return -1;
    if (FAILED(g_gpu.device.As(&g_gpu.videoDevice)))   return -1;
    if (FAILED(g_gpu.context.As(&g_gpu.videoContext))) return -1;
    return 0;
}

static int gpu_ensure_processor(int w, int h) {
    if (g_gpu.processor && g_gpu.width == w && g_gpu.height == h) return 0;

    g_gpu.processor.Reset();
    g_gpu.enumerator.Reset();
    g_gpu.width  = w;
    g_gpu.height = h;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat            = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator    = 60;
    desc.InputFrameRate.Denominator  = 1;
    desc.InputWidth                  = (UINT)w;
    desc.InputHeight                 = (UINT)h;
    desc.OutputFrameRate.Numerator   = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth                 = (UINT)w;
    desc.OutputHeight                = (UINT)h;
    desc.Usage                       = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(g_gpu.videoDevice->CreateVideoProcessorEnumerator(&desc, &g_gpu.enumerator)))
        return -1;
    if (FAILED(g_gpu.videoDevice->CreateVideoProcessor(g_gpu.enumerator.Get(), 0, &g_gpu.processor)))
        return -1;

    // BT.709 limited-range YCbCr -> full-range RGB. Good default for HD;
    // SD content (BT.601) will be slightly off but still display.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs = {};
    inCs.YCbCr_Matrix  = 1;                                                  // BT.709
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    g_gpu.videoContext->VideoProcessorSetStreamColorSpace(g_gpu.processor.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
    outCs.RGB_Range = 0;                                                     // full range
    g_gpu.videoContext->VideoProcessorSetOutputColorSpace(g_gpu.processor.Get(), &outCs);
    return 0;
}

// Convert a NV12 array-texture subresource into a fresh BGRA ID3D11Texture2D.
// Returns AddRef'd; caller releases.
static ID3D11Texture2D* gpu_convert_nv12_to_bgra(ID3D11Texture2D* src, UINT slice) {
    if (!g_gpu.processor || !src) return nullptr;

    D3D11_TEXTURE2D_DESC outDesc = {};
    outDesc.Width            = (UINT)g_gpu.width;
    outDesc.Height           = (UINT)g_gpu.height;
    outDesc.MipLevels        = 1;
    outDesc.ArraySize        = 1;
    outDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage            = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> out;
    if (FAILED(g_gpu.device->CreateTexture2D(&outDesc, nullptr, &out))) return nullptr;

    std::lock_guard<std::mutex>           blt(g_gpu.bltMutex);
    std::lock_guard<std::recursive_mutex> ff(g_gpu.ffmpegLock);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(g_gpu.videoDevice->CreateVideoProcessorInputView(
            src, g_gpu.enumerator.Get(), &ivDesc, &inView)))
        return nullptr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(g_gpu.videoDevice->CreateVideoProcessorOutputView(
            out.Get(), g_gpu.enumerator.Get(), &ovDesc, &outView)))
        return nullptr;

    RECT rect = { 0, 0, g_gpu.width, g_gpu.height };
    g_gpu.videoContext->VideoProcessorSetStreamSourceRect(g_gpu.processor.Get(), 0, TRUE, &rect);
    g_gpu.videoContext->VideoProcessorSetStreamDestRect  (g_gpu.processor.Get(), 0, TRUE, &rect);
    g_gpu.videoContext->VideoProcessorSetOutputTargetRect(g_gpu.processor.Get(),    TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable        = TRUE;
    stream.OutputIndex   = 0;
    stream.pInputSurface = inView.Get();

    if (FAILED(g_gpu.videoContext->VideoProcessorBlt(
            g_gpu.processor.Get(), outView.Get(), 0, 1, &stream)))
        return nullptr;

    out->AddRef();
    return out.Get();
}

// ----------------------------------------------------------------------------
// ffplay-derived data structures.
// ----------------------------------------------------------------------------

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int       serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo  *pkt_list;
    int      nb_packets;
    int      size;
    int64_t  duration;
    int      abort_request;
    int      serial;
    SDL_mutex *mutex;
    SDL_cond  *cond;
} PacketQueue;

typedef struct AudioParams {
    int                 freq;
    AVChannelLayout     ch_layout;
    enum AVSampleFormat fmt;
    int                 frame_size;
    int                 bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int    serial;        /* clock is based on a packet with this serial */
    int    paused;
    int   *queue_serial;  /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data. */
typedef struct Frame {
    AVFrame         *frame;
    int              serial;
    double           pts;
    double           duration;
    int64_t          pos;
    int              width;
    int              height;
    int              format;
    AVRational       sar;
    ID3D11Texture2D *tex;     // BGRA, AddRef'd; released by frame_queue_unref_item
} Frame;

typedef struct FrameQueue {
    Frame       queue[FRAME_QUEUE_SIZE];
    int         rindex;
    int         windex;
    int         size;
    int         max_size;
    int         keep_last;
    int         rindex_shown;
    SDL_mutex  *mutex;
    SDL_cond   *cond;
    PacketQueue*pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

typedef struct Decoder {
    AVPacket       *pkt;
    PacketQueue    *queue;
    AVCodecContext *avctx;
    int             pkt_serial;
    int             finished;
    int             packet_pending;
    SDL_cond       *empty_queue_cond;
    int64_t         start_pts;
    AVRational      start_pts_tb;
    int64_t         next_pts;
    AVRational      next_pts_tb;
    SDL_Thread     *decoder_tid;
} Decoder;

typedef struct VideoState {
    SDL_Thread          *read_tid;
    SDL_Thread          *refresh_tid;
    int                  abort_request;
    int                  paused;
    int                  last_paused;
    int                  queue_attachments_req;
    int                  seek_req;
    int                  seek_flags;
    int64_t              seek_pos;
    int64_t              seek_rel;
    int                  read_pause_return;
    AVFormatContext     *ic;
    int                  realtime;

    Clock                audclk;
    Clock                vidclk;
    Clock                extclk;

    FrameQueue           pictq;
    FrameQueue           sampq;

    Decoder              auddec;
    Decoder              viddec;

    int                  audio_stream;
    int                  av_sync_type;

    double               audio_clock;
    int                  audio_clock_serial;
    double               audio_diff_cum;
    double               audio_diff_avg_coef;
    double               audio_diff_threshold;
    int                  audio_diff_avg_count;
    AVStream            *audio_st;
    PacketQueue          audioq;
    int                  audio_hw_buf_size;
    uint8_t             *audio_buf;
    uint8_t             *audio_buf1;
    unsigned int         audio_buf_size;     /* in bytes */
    unsigned int         audio_buf1_size;
    int                  audio_buf_index;    /* in bytes */
    int                  audio_write_buf_size;
    int                  audio_volume;
    int                  muted;
    AudioParams          audio_src;
    AudioParams          audio_tgt;
    SwrContext          *swr_ctx;
    int                  frame_drops_early;
    int                  frame_drops_late;

    double               frame_timer;
    int                  video_stream;
    AVStream            *video_st;
    PacketQueue          videoq;
    double               max_frame_duration;
    int                  eof;
    int                  step;

    char                *filename;
    SDL_cond            *continue_read_thread;

    // Open() handshake: read_thread sets prepared (=1 ok / =-1 err) once
    // avformat_open_input + find_stream_info + stream_component_open have
    // run, and signals prep_cond. magic_player_open blocks on this so the
    // caller sees a populated ic (duration, video size) on return.
    SDL_mutex           *prep_mutex;
    SDL_cond            *prep_cond;
    int                  prepared;

    SDL_AudioDeviceID    audio_dev;
    int64_t              audio_callback_time;
} VideoState;

static int    framedrop = -1;       /* -1: auto (when slave), 0: off, 1: always */
static int    decoder_reorder_pts = -1;
static int    av_sync_type_default = AV_SYNC_AUDIO_MASTER;

// ----------------------------------------------------------------------------
// PacketQueue.
// ----------------------------------------------------------------------------

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request) return -1;

    pkt1.pkt    = pkt;
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0) return ret;
    q->nb_packets++;
    q->size     += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) { av_packet_unref(pkt); return -1; }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0) av_packet_free(&pkt1);
    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index) {
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list) return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCond();
    if (!q->mutex || !q->cond) return AVERROR(ENOMEM);
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q) {
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

static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 1;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet. */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) { ret = -1; break; }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size     -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial) *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// ----------------------------------------------------------------------------
// Decoder.
// ----------------------------------------------------------------------------

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
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

// Subtitle-free variant of ffplay's decoder_decode_frame.
static int decoder_decode_frame(Decoder *d, AVFrame *frame) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
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
                   "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
            d->packet_pending = 1;
        } else {
            av_packet_unref(d->pkt);
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

// ----------------------------------------------------------------------------
// FrameQueue.
// ----------------------------------------------------------------------------

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    if (vp->tex) { vp->tex->Release(); vp->tex = nullptr; }
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) {
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) return AVERROR(ENOMEM);
    if (!(f->cond  = SDL_CreateCond()))  return AVERROR(ENOMEM);
    f->pktq      = pktq;
    f->max_size  = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size && !f->pktq->abort_request)
        SDL_CondWait(f->cond, f->mutex);
    SDL_UnlockMutex(f->mutex);
    if (f->pktq->abort_request) return nullptr;
    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
        SDL_CondWait(f->cond, f->mutex);
    SDL_UnlockMutex(f->mutex);
    if (f->pktq->abort_request) return nullptr;
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size) f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size) f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

static void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, nullptr);
    d->decoder_tid = nullptr;
    packet_queue_flush(d->queue);
}

// ----------------------------------------------------------------------------
// Clocks (verbatim from ffplay).
// ----------------------------------------------------------------------------

static double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial) return NAN;
    if (c->paused) return c->pts;
    double t = av_gettime_relative() / 1000000.0;
    return c->pts_drift + t - (t - c->last_updated) * (1.0 - c->speed);
}

static void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts          = pts;
    c->last_updated = time;
    c->pts_drift    = c->pts - time;
    c->serial       = serial;
}

static void set_clock(Clock *c, double pts, int serial) {
    set_clock_at(c, pts, serial, av_gettime_relative() / 1000000.0);
}

static void init_clock(Clock *c, int *queue_serial) {
    c->speed        = 1.0;
    c->paused       = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock       = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
        return is->video_st ? AV_SYNC_VIDEO_MASTER : AV_SYNC_AUDIO_MASTER;
    if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
        return is->audio_st ? AV_SYNC_AUDIO_MASTER : AV_SYNC_EXTERNAL_CLOCK;
    return AV_SYNC_EXTERNAL_CLOCK;
}

static double get_master_clock(VideoState *is) {
    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER: return get_clock(&is->vidclk);
        case AV_SYNC_AUDIO_MASTER: return get_clock(&is->audclk);
        default:                   return get_clock(&is->extclk);
    }
}

// ----------------------------------------------------------------------------
// Pause / seek.
// ----------------------------------------------------------------------------

static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes) {
    if (is->seek_req) return;
    is->seek_pos    = pos;
    is->seek_rel    = rel;
    is->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes) is->seek_flags |= AVSEEK_FLAG_BYTE;
    is->seek_req = 1;
    SDL_CondSignal(is->continue_read_thread);
}

static void stream_toggle_pause(VideoState *is) {
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS))
            is->vidclk.paused = 0;
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is) {
    stream_toggle_pause(is);
    is->step = 0;
}

static double compute_target_delay(double delay, VideoState *is) {
    double sync_threshold, diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)                                           delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) delay = delay + diff;
            else if (diff >= sync_threshold)                                       delay = 2 * delay;
        }
    }
    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        return duration;
    }
    return 0.0;
}

static void update_video_pts(VideoState *is, double pts, int serial) {
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

// Drive the video frame queue's rindex forward. No display calls -- the UI
// reads frame_queue_peek_last() each refresh and wraps the texture itself.
static void video_refresh(VideoState *is, double *remaining_time) {
    if (!is->video_st) return;

retry:
    if (frame_queue_nb_remaining(&is->pictq) == 0) return;

    Frame *lastvp = frame_queue_peek_last(&is->pictq);
    Frame *vp     = frame_queue_peek(&is->pictq);

    if (vp->serial != is->videoq.serial) {
        frame_queue_next(&is->pictq);
        goto retry;
    }

    if (lastvp->serial != vp->serial)
        is->frame_timer = av_gettime_relative() / 1000000.0;

    if (is->paused) return;

    double last_duration = vp_duration(is, lastvp, vp);
    double delay         = compute_target_delay(last_duration, is);

    double time = av_gettime_relative() / 1000000.0;
    if (time < is->frame_timer + delay) {
        *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
        return;
    }

    is->frame_timer += delay;
    if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
        is->frame_timer = time;

    SDL_LockMutex(is->pictq.mutex);
    if (!isnan(vp->pts))
        update_video_pts(is, vp->pts, vp->serial);
    SDL_UnlockMutex(is->pictq.mutex);

    if (frame_queue_nb_remaining(&is->pictq) > 1) {
        Frame *nextvp = frame_queue_peek_next(&is->pictq);
        double duration = vp_duration(is, vp, nextvp);
        if (!is->step
            && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
            && time > is->frame_timer + duration) {
            is->frame_drops_late++;
            frame_queue_next(&is->pictq);
            goto retry;
        }
    }

    frame_queue_next(&is->pictq);

    if (is->step && !is->paused)
        stream_toggle_pause(is);
}

// ----------------------------------------------------------------------------
// Video decode pipeline: NV12 D3D11 frame -> BGRA D3D11 texture -> pictq.
// ----------------------------------------------------------------------------

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    Frame *vp;
    if (!(vp = frame_queue_peek_writable(&is->pictq))) return -1;

    vp->sar      = src_frame->sample_aspect_ratio;
    vp->width    = src_frame->width;
    vp->height   = src_frame->height;
    vp->format   = src_frame->format;
    vp->pts      = pts;
    vp->duration = duration;
    vp->pos      = pos;
    vp->serial   = serial;

    // Release any stale BGRA texture that might be sitting in this slot.
    // Must hold pictq.mutex: when the queue drains to size==0 with
    // keep_last==1, windex coincides with rindex and the UI reader
    // (magic_player_acquire_current_texture) may still be inspecting tex.
    if (vp->tex) {
        SDL_LockMutex(is->pictq.mutex);
        vp->tex->Release();
        vp->tex = nullptr;
        SDL_UnlockMutex(is->pictq.mutex);
    }

    if (src_frame->format == AV_PIX_FMT_D3D11) {
        ID3D11Texture2D *nv12 = reinterpret_cast<ID3D11Texture2D*>(src_frame->data[0]);
        UINT             slice = (UINT)(intptr_t)src_frame->data[1];
        if (gpu_ensure_processor(vp->width, vp->height) == 0)
            vp->tex = gpu_convert_nv12_to_bgra(nv12, slice);
    }

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame) {
    int got_picture = decoder_decode_frame(&is->viddec, frame);
    if (got_picture < 0) return -1;
    if (!got_picture)    return 0;

    double dpts = NAN;
    if (frame->pts != AV_NOPTS_VALUE)
        dpts = av_q2d(is->video_st->time_base) * frame->pts;

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

    if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
        if (frame->pts != AV_NOPTS_VALUE) {
            double diff = dpts - get_master_clock(is);
            if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                diff < 0 &&
                is->viddec.pkt_serial == is->vidclk.serial &&
                is->videoq.nb_packets) {
                is->frame_drops_early++;
                av_frame_unref(frame);
                got_picture = 0;
            }
        }
    }
    return got_picture;
}

static int video_thread(void *arg) {
    VideoState *is        = (VideoState*)arg;
    AVFrame    *frame     = av_frame_alloc();
    AVRational  tb        = is->video_st->time_base;
    AVRational  frame_rate= av_guess_frame_rate(is->ic, is->video_st, nullptr);

    if (!frame) return AVERROR(ENOMEM);

    for (;;) {
        int ret = get_video_frame(is, frame);
        if (ret < 0) break;
        if (!ret) continue;

        double duration = (frame_rate.num && frame_rate.den)
                            ? av_q2d(AVRational{ frame_rate.den, frame_rate.num })
                            : 0;
        double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
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

static int audio_thread(void *arg) {
    VideoState *is    = (VideoState*)arg;
    AVFrame    *frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);

    int ret = 0;
    do {
        int got = decoder_decode_frame(&is->auddec, frame);
        if (got < 0) { ret = got; break; }
        if (!got) continue;

        Frame *af = frame_queue_peek_writable(&is->sampq);
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

static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                double avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    int min_nb_samples = nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    int max_nb_samples = nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
            }
        } else {
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }
    return wanted_nb_samples;
}

static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused) return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - is->audio_callback_time)
                > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep(1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq))) return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(nullptr, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           (AVSampleFormat)af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt           ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq          ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
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

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out      = &is->audio_buf1;
        int out_count      = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size       = av_samples_get_buffer_size(nullptr, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        if (out_size < 0) return -1;

        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx,
                    (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                    wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0)
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
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf       = af->frame->data[0];
        resampled_data_size = data_size;
    }

    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
    return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = (VideoState*)opaque;
    is->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= (int)is->audio_buf_size) {
            int audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                is->audio_buf      = nullptr;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        int len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        len    -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk,
            is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec,
            is->audio_clock_serial,
            is->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(VideoState *is, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    static const int next_nb_channels[]   = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[]  = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels   = wanted_channel_layout->nb_channels;

    const char* env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq     = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) return -1;

    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
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
            wanted_spec.freq     = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) return -1;
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) return -1;
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) return -1;
    }

    audio_hw_params->fmt  = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0) return -1;
    audio_hw_params->frame_size    = av_samples_get_buffer_size(nullptr, audio_hw_params->ch_layout.nb_channels, 1,                     audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) return -1;
    return spec.size;
}

// ----------------------------------------------------------------------------
// HW accel: D3D11VA on our shared device.
// ----------------------------------------------------------------------------

static enum AVPixelFormat get_d3d11_format(AVCodecContext * /*ctx*/, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    return pix_fmts[0];
}

static int create_hwaccel(AVBufferRef **device_ctx) {
    if (gpu_create_device() < 0) return AVERROR(ENOSYS);

    *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!*device_ctx) return AVERROR(ENOMEM);

    auto *dev = reinterpret_cast<AVHWDeviceContext*>((*device_ctx)->data);
    auto *d3d = static_cast<AVD3D11VADeviceContext*>(dev->hwctx);

    g_gpu.device->AddRef();
    d3d->device   = g_gpu.device.Get();
    d3d->lock     = gpu_lock_cb;
    d3d->unlock   = gpu_unlock_cb;
    d3d->lock_ctx = nullptr;

    int ret = av_hwdevice_ctx_init(*device_ctx);
    if (ret < 0) av_buffer_unref(device_ctx);
    return ret;
}

// ----------------------------------------------------------------------------
// Stream open / close.
// ----------------------------------------------------------------------------

static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext  *avctx;
    const AVCodec   *codec;
    AVDictionary    *opts = nullptr;
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
        ret = create_hwaccel(&avctx->hw_device_ctx);
        if (ret < 0) goto fail;
        avctx->get_format    = get_d3d11_format;
        avctx->thread_count  = 1;
        avctx->thread_type   = 0;
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
        is->audio_hw_buf_size = ret;
        is->audio_src         = is->audio_tgt;
        is->audio_buf_size    = 0;
        is->audio_buf_index   = 0;

        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        is->audio_diff_threshold = (double)is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st     = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0) goto fail;
        if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts    = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        packet_queue_start(is->auddec.queue);
        is->auddec.decoder_tid = SDL_CreateThread(audio_thread, "audio_decoder", is);
        if (!is->auddec.decoder_tid) { ret = AVERROR(ENOMEM); goto out; }
        SDL_PauseAudioDevice(is->audio_dev, 0);
        break;
    }
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st     = ic->streams[stream_index];
        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0) goto fail;
        packet_queue_start(is->viddec.queue);
        is->viddec.decoder_tid = SDL_CreateThread(video_thread, "video_decoder", is);
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

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) return;
    AVCodecParameters *codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudioDevice(is->audio_dev);
        decoder_destroy(&is->auddec);
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
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { is->audio_st = nullptr; is->audio_stream = -1; }
    else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { is->video_st = nullptr; is->video_stream = -1; }
}

// ----------------------------------------------------------------------------
// Demux thread + stream lifecycle.
// ----------------------------------------------------------------------------

static int decode_interrupt_cb(void *ctx) {
    return ((VideoState*)ctx)->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           (queue->nb_packets > MIN_FRAMES &&
            (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0));
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp")
     || !strcmp(s->iformat->name, "rtsp")
     || !strcmp(s->iformat->name, "sdp"))
        return 1;
    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

static int read_thread(void *arg) {
    VideoState *is = (VideoState*)arg;
    AVFormatContext *ic = nullptr;
    AVPacket *pkt = nullptr;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int ret = 0;

    int v_idx = 0;
    int video_ret = -1;
    int a_idx = 0;

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

    v_idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    a_idx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, v_idx, nullptr, 0);

    if (a_idx >= 0) stream_component_open(is, a_idx);

    video_ret = -1;
    if (v_idx >= 0) video_ret = stream_component_open(is, v_idx);

    if (is->video_stream < 0 && is->audio_stream < 0) { ret = -1; goto fail; }

    is->realtime = is_realtime(ic);

    // Hand-off to magic_player_open: ic + duration + video size + audio
    // device are all populated now, so the synchronous open call can return.
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
            int64_t mn     = is->seek_rel > 0 ? target - is->seek_rel + 2 : INT64_MIN;
            int64_t mx     = is->seek_rel < 0 ? target - is->seek_rel - 2 : INT64_MAX;
            int sret = avformat_seek_file(is->ic, -1, mn, target, mx, is->seek_flags);
            if (sret >= 0) {
                if (is->audio_stream >= 0) packet_queue_flush(&is->audioq);
                if (is->video_stream >= 0) packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE)
                    set_clock(&is->extclk, NAN, 0);
                else
                    set_clock(&is->extclk, target / (double)AV_TIME_BASE, 0);
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
        }

        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if (av_packet_ref(pkt, &is->video_st->attached_pic) >= 0) {
                    packet_queue_put(&is->videoq, pkt);
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                }
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (is->audioq.size + is->videoq.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq))) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0) packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0) packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
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

    // If we exited before the success signal above, unblock magic_player_open
    // with an error so it doesn't hang waiting for a file that never opened.
    SDL_LockMutex(is->prep_mutex);
    if (is->prepared == 0) is->prepared = -1;
    SDL_CondSignal(is->prep_cond);
    SDL_UnlockMutex(is->prep_mutex);
    return 0;
}

// Drives video_refresh on a worker thread so the UI thread can simply poll
// magic_player_get_current_texture() at its own (vsync) cadence.
static int refresh_thread(void *arg) {
    VideoState *is = (VideoState*)arg;
    while (!is->abort_request) {
        double remaining = REFRESH_RATE;
        if (!is->paused) video_refresh(is, &remaining);
        if (remaining > 0) av_usleep((int64_t)(remaining * 1000000.0));
    }
    return 0;
}

static void stream_close(VideoState *is) {
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
    av_free(is);
}

static VideoState *stream_open(const char *filename) {
    VideoState *is = (VideoState*)av_mallocz(sizeof(VideoState));
    if (!is) return nullptr;
    is->video_stream = -1;
    is->audio_stream = -1;
    is->filename     = av_strdup(filename);
    if (!is->filename) goto fail;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)         goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0) goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) goto fail;
    if (!(is->prep_mutex           = SDL_CreateMutex())) goto fail;
    if (!(is->prep_cond            = SDL_CreateCond()))  goto fail;

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_volume       = SDL_MIX_MAXVOLUME;
    is->muted              = 0;
    is->av_sync_type       = av_sync_type_default;

    is->read_tid    = SDL_CreateThread(read_thread,    "read_thread",    is);
    is->refresh_tid = SDL_CreateThread(refresh_thread, "refresh_thread", is);
    if (!is->read_tid || !is->refresh_tid) {
fail:
        if (is) stream_close(is);
        return nullptr;
    }
    return is;
}

// ============================================================================
// Public C API consumed by the C++/CLI wrapper.
// ============================================================================

extern "C" {

struct MagicPlayerHandle {
    VideoState              *is;
    // Lazily allocated, sized to the current frame -- staging texture used by
    // magic_player_copy_current_bgra to read back BGRA pixels into a CPU buffer
    // without bouncing through the wrapper's own ID3D11Device.
    ComPtr<ID3D11Texture2D>  staging;
    int                      staging_w = 0;
    int                      staging_h = 0;
    std::mutex               staging_mutex;
};

MagicPlayerHandle* magic_player_open(const char* path) {
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) return nullptr;
    VideoState *is = stream_open(path);
    if (!is) return nullptr;

    // Block until read_thread has actually opened the file -- otherwise the
    // caller will see Duration == 0 / VideoSize == 0 and trip up sliders /
    // bitmap allocation.
    SDL_LockMutex(is->prep_mutex);
    while (is->prepared == 0)
        SDL_CondWait(is->prep_cond, is->prep_mutex);
    int prepared = is->prepared;
    SDL_UnlockMutex(is->prep_mutex);

    if (prepared < 0) {
        stream_close(is);
        return nullptr;
    }

    auto *h = new MagicPlayerHandle();
    h->is = is;
    return h;
}

void magic_player_close(MagicPlayerHandle *h) {
    if (!h) return;
    if (h->is) stream_close(h->is);
    delete h;
}

void magic_player_toggle_pause(MagicPlayerHandle *h) {
    if (h && h->is) toggle_pause(h->is);
}

int magic_player_is_paused(MagicPlayerHandle *h) {
    return h && h->is ? h->is->paused : 0;
}

void magic_player_seek_us(MagicPlayerHandle *h, int64_t position_us) {
    if (!h || !h->is) return;
    int64_t target = av_rescale(position_us, AV_TIME_BASE, 1'000'000LL);
    stream_seek(h->is, target, 0, 0);
}

int64_t magic_player_master_clock_us(MagicPlayerHandle *h) {
    if (!h || !h->is) return 0;
    double t = get_master_clock(h->is);
    if (isnan(t)) return 0;
    return (int64_t)(t * 1'000'000.0);
}

double magic_player_duration_seconds(MagicPlayerHandle *h) {
    if (!h || !h->is || !h->is->ic || h->is->ic->duration == AV_NOPTS_VALUE) return 0;
    return (double)h->is->ic->duration / AV_TIME_BASE;
}

int magic_player_video_size(MagicPlayerHandle *h, int *w, int *out_h) {
    if (!h || !h->is || !h->is->video_st) return 0;
    *w     = h->is->video_st->codecpar->width;
    *out_h = h->is->video_st->codecpar->height;
    return 1;
}

// Returns AddRef'd; caller releases. The returned BGRA texture is the most
// recent frame whose presentation deadline has passed (driven by video_refresh
// on its worker thread).
int magic_player_acquire_current_texture(MagicPlayerHandle *h, ID3D11Texture2D **out) {
    if (!h || !h->is || !out) return 0;
    *out = nullptr;
    SDL_LockMutex(h->is->pictq.mutex);
    if (h->is->pictq.rindex_shown) {
        Frame *fp = &h->is->pictq.queue[h->is->pictq.rindex];
        if (fp->tex) {
            fp->tex->AddRef();
            *out = fp->tex;
        }
    }
    SDL_UnlockMutex(h->is->pictq.mutex);
    return *out ? 1 : 0;
}

int magic_player_copy_current_bgra(MagicPlayerHandle *h,
                                   uint8_t *dst, int dst_capacity_bytes,
                                   int *out_w, int *out_h) {
    if (!h || !dst || !out_w || !out_h) return 0;

    ID3D11Texture2D *src = nullptr;
    if (!magic_player_acquire_current_texture(h, &src) || !src) return 0;

    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    const int w = (int)desc.Width;
    const int h_ = (int)desc.Height;
    const int needed = w * h_ * 4;
    if (dst_capacity_bytes < needed) { src->Release(); return 0; }

    std::lock_guard<std::mutex> lock(h->staging_mutex);

    // (Re)create the staging texture if size changed.
    if (!h->staging || h->staging_w != w || h->staging_h != h_) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width            = (UINT)w;
        sd.Height           = (UINT)h_;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        h->staging.Reset();
        if (FAILED(g_gpu.device->CreateTexture2D(&sd, nullptr, &h->staging))) {
            src->Release();
            return 0;
        }
        h->staging_w = w;
        h->staging_h = h_;
    }

    // GPU copy + CPU map, serialised with the video processor's blt mutex so
    // we don't race the conversion that produces fp->tex.
    D3D11_MAPPED_SUBRESOURCE map = {};
    HRESULT hr;
    {
        std::lock_guard<std::mutex> blt(g_gpu.bltMutex);
        g_gpu.context->CopyResource(h->staging.Get(), src);
        hr = g_gpu.context->Map(h->staging.Get(), 0, D3D11_MAP_READ, 0, &map);
    }
    src->Release();
    if (FAILED(hr)) return 0;

    const int row_bytes = w * 4;
    const uint8_t *srcRow = (const uint8_t*)map.pData;
    uint8_t       *dstRow = dst;
    for (int y = 0; y < h_; ++y) {
        memcpy(dstRow, srcRow, row_bytes);
        srcRow += map.RowPitch;
        dstRow += row_bytes;
    }
    g_gpu.context->Unmap(h->staging.Get(), 0);

    *out_w = w;
    *out_h = h_;
    return 1;
}

// Returns AddRef'd; caller releases. Stays valid for the lifetime of the
// process (the device is a singleton).
int magic_player_acquire_dxgi_device(IDXGIDevice **out) {
    if (!out) return 0;
    *out = nullptr;
    if (gpu_create_device() < 0) return 0;
    g_gpu.dxgiDevice->AddRef();
    *out = g_gpu.dxgiDevice.Get();
    return 1;
}

} // extern "C"
