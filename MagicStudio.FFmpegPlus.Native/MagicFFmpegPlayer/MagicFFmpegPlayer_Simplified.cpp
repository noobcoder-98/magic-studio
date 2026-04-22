/*
 * Simplified FFmpeg Player for Win2D CanvasControl
 * Based on ffplay.c but stripped of unnecessary features
 * 
 * Only keeps:
 * - Packet queue (demux)
 * - Video decoding thread
 * - Frame queue
 * - Basic synchronization
 * 
 * Removes:
 * - SDL rendering (using Win2D instead)
 * - Audio playback (handle separately)
 * - Subtitle support
 * - Visualization (audio waves, RDFT)
 * - Complex filtering
 * - Advanced controls (seeking by bytes, etc.)
 */

#include "config.h"
#include <math.h>
#include <limits.h>
#include <stdint.h>

extern "C" {
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
}

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

/* Packet queue for decoded packets */
typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    // Use mutex/cond for thread-safe operations
    void *mutex;
    void *cond;
} PacketQueue;

/* Video frame with metadata */
typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

typedef struct Frame {
    AVFrame *frame;
    int serial;
    double pts;
    double duration;
    int64_t pos;
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
} Frame;

/* Frame queue for decoded frames */
typedef struct FrameQueue {
    Frame queue[3];  // 3 frames max
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    void *mutex;
    void *cond;
    PacketQueue *pktq;
} FrameQueue;

/* Simple clock for synchronization */
typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int *queue_serial;
} Clock;

/* Video decoder thread state */
typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    void *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    void *decoder_tid;
} Decoder;

/* Main video player state */
typedef struct VideoState {
    void *read_tid;
    const AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;

    AVFormatContext *ic;
    int realtime;

    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;  // Video frame queue

    Decoder viddec;    // Video decoder

    int video_stream;
    double max_frame_duration;

    char *filename;
    int width, height;

    AVStream *video_st;
    PacketQueue videoq;

    struct SwsContext *sws_ctx;
    int sws_src_fmt;

    void *continue_read_thread;

    // Stats
    int frame_drops_early;
    int frame_drops_late;
    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int step;
} VideoState;

/* ==================== Packet Queue Operations ==================== */

static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    // Initialize mutex and condition variable
    // Note: In actual implementation, use proper threading primitives
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
}

static void packet_queue_abort(PacketQueue *q)
{
    q->abort_request = 1;
}

static void packet_queue_start(PacketQueue *q)
{
    q->abort_request = 0;
    q->serial++;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    MyAVPacketList pkt_list;
    pkt_list.pkt = pkt1;
    pkt_list.serial = q->serial;

    if (q->abort_request)
        return -1;

    int ret = av_fifo_write(q->pkt_list, &pkt_list, 1);
    if (ret < 0) {
        av_packet_free(&pkt1);
        return ret;
    }

    q->nb_packets++;
    q->size += pkt_list.pkt->size + sizeof(pkt_list);
    q->duration += pkt_list.pkt->duration;

    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;

    for (;;) {
        if (q->abort_request) {
            return -1;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            return 1;
        } else if (!block) {
            return 0;
        } else {
            // In real implementation, wait on condition variable
            av_usleep(1000);
        }
    }
}

/* ==================== Frame Queue Operations ==================== */

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    memset(f, 0, sizeof(FrameQueue));
    f->pktq = pktq;
    f->max_size = max_size > 3 ? 3 : max_size;  // Max 3 frames
    f->keep_last = keep_last;

    for (int i = 0; i < f->max_size; i++) {
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    }
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    for (int i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* Wait until we have space */
    while (f->size >= f->max_size && !f->pktq->abort_request) {
        av_usleep(1000);
    }

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* Wait until we have a readable frame */
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        av_usleep(1000);
    }

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    f->size++;
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    f->size--;
}

static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* ==================== Decoder Operations ==================== */

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, void *empty_queue_cond)
{
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

static void decoder_destroy(Decoder *d)
{
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame)
{
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                ret = avcodec_receive_frame(d->avctx, frame);
                if (ret >= 0) {
                    frame->pts = frame->best_effort_timestamp;
                }

                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0) {
                // Signal empty queue
            }
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

        if (d->pkt->buf && !d->pkt->opaque_ref) {
            FrameData *fd;
            d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
            if (!d->pkt->opaque_ref)
                return AVERROR(ENOMEM);
            fd = (FrameData *)d->pkt->opaque_ref->data;
            fd->pkt_pos = d->pkt->pos;
        }

        if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
            d->packet_pending = 1;
        } else {
            av_packet_unref(d->pkt);
        }
    }
}

/* ==================== Clock Management ==================== */

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

/* ==================== Core Player Functions ==================== */

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp = frame_queue_peek_writable(&is->pictq);
    if (!vp)
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture = decoder_decode_frame(&is->viddec, frame);
    if (got_picture < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);
    }

    return got_picture;
}

static void update_sws_context(VideoState *is, int src_fmt)
{
    if (is->sws_src_fmt == src_fmt && is->sws_ctx)
        return;

    if (is->sws_ctx) {
        sws_freeContext(is->sws_ctx);
        is->sws_ctx = NULL;
    }

    is->sws_src_fmt = src_fmt;
    is->sws_ctx = sws_getContext(
        is->video_st->codecpar->width, is->video_st->codecpar->height, (AVPixelFormat)src_fmt,
        is->video_st->codecpar->width, is->video_st->codecpar->height, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, NULL, NULL, NULL);
}

static int video_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    double pts, duration;
    int ret;
    AVRational tb = is->video_st->time_base;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        duration = av_q2d(tb);

        ret = queue_picture(is, frame, pts, duration, -1, is->viddec.pkt_serial);
        av_frame_unref(frame);

        if (is->videoq.serial != is->viddec.pkt_serial)
            break;
    }

the_end:
    av_frame_free(&frame);
    return 0;
}

static int read_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;

    memset(st_index, -1, sizeof(st_index));

    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ic = avformat_alloc_context();
    if (!ic) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    err = avformat_open_input(&ic, is->filename, is->iformat, NULL);
    if (err < 0) {
        ret = -1;
        goto fail;
    }

    is->ic = ic;

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        ret = -1;
        goto fail;
    }

    is->realtime = 0;  // Simple check

    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        is->video_stream = st_index[AVMEDIA_TYPE_VIDEO];
        is->video_st = st;
    }

    if (is->video_stream < 0) {
        ret = -1;
        goto fail;
    }

    /* Read packets */
    for (;;) {
        if (is->abort_request)
            break;

        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            ret = avformat_seek_file(is->ic, -1, INT64_MIN, seek_target, INT64_MAX, is->seek_flags);
            if (ret >= 0) {
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
            }
            is->seek_req = 0;
        }

        if (is->videoq.size + is->videoq.nb_packets > MAX_QUEUE_SIZE) {
            av_usleep(10000);
            continue;
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                // End of file
                if (is->video_stream >= 0) {
                    AVPacket null_pkt;
                    av_packet_init(&null_pkt);
                    null_pkt.stream_index = is->video_stream;
                    packet_queue_put(&is->videoq, &null_pkt);
                }
                break;
            }
            av_usleep(10000);
            continue;
        }

        if (pkt->stream_index == is->video_stream) {
            packet_queue_put(&is->videoq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;

fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    return 0;
}

/* ==================== Public API ==================== */

static VideoState *stream_open(const char *filename, const AVInputFormat *iformat)
{
    VideoState *is = (VideoState *)av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;

    is->last_paused = -1;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;

    is->iformat = iformat;

    /* Initialize queues and frames */
    if (frame_queue_init(&is->pictq, &is->videoq, 3, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0)
        goto fail;

    if (!(is->continue_read_thread = av_cond_alloc()))
        goto fail;

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->extclk, &is->extclk.serial);

    /* Start read thread */
    is->read_tid = av_thread_create(read_thread, "read_thread", is);
    if (!is->read_tid)
        goto fail;

    return is;

fail:
    if (is) {
        av_freep(&is->filename);
        av_free(is);
    }
    return NULL;
}

static void stream_close(VideoState *is)
{
    if (!is)
        return;

    is->abort_request = 1;

    if (is->read_tid)
        av_thread_join(is->read_tid, NULL);

    if (is->video_stream >= 0)
        packet_queue_destroy(&is->videoq);

    frame_queue_destroy(&is->pictq);

    if (is->sws_ctx)
        sws_freeContext(is->sws_ctx);

    if (is->ic)
        avformat_close_input(&is->ic);

    av_freep(&is->filename);
    av_free(is);
}

Frame *get_current_frame(VideoState *is)
{
    if (!is || is->pictq.size == 0)
        return NULL;
    return frame_queue_peek(&is->pictq);
}

void seek_frame(VideoState *is, int64_t pos)
{
    if (!is)
        return;
    is->seek_pos = pos;
    is->seek_flags = 0;
    is->seek_req = 1;
}

int is_player_ready(VideoState *is)
{
    if (!is)
        return 0;
    return is->video_st != NULL && is->ic != NULL;
}
