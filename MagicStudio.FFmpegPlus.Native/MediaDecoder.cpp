#include "pch.h"
#include "MediaDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "swresample.lib")

namespace MagicStudio::Native {

static int64_t TsToUs(int64_t ts, int tbNum, int tbDen) {
    if (ts == AV_NOPTS_VALUE) return 0;
    return static_cast<int64_t>(ts) * tbNum * 1'000'000LL / tbDen;
}

// prefer D3D11VA hardware pixel format when available.
static AVPixelFormat GetHwFormat(AVCodecContext*, const AVPixelFormat* pix_fmts) {
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (pix_fmts[i] == AV_PIX_FMT_D3D11)
            return AV_PIX_FMT_D3D11;
    }
    return pix_fmts[0]; // fallback to first software format
}

MediaDecoder::MediaDecoder()
    : _frameQueue(std::make_unique<FrameQueue>(16))
    , _videoPktQueue(std::make_unique<PacketQueue>(64))
    , _audioPktQueue(std::make_unique<PacketQueue>(128))
    , _audioRenderer(std::make_unique<AudioRenderer>())
{}

MediaDecoder::~MediaDecoder() {
    Close();
}

bool MediaDecoder::Open(const char* path) {
    Close();

    if (avformat_open_input(&_fmtCtx, path, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(_fmtCtx, nullptr) < 0) return false;

    _videoStream = av_find_best_stream(_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    _audioStream = av_find_best_stream(_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // --- Video decoder ---
    if (_videoStream >= 0) {
        AVStream* vs      = _fmtCtx->streams[_videoStream];
        const AVCodec* vc = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!vc) return false;

        _videoCtx = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(_videoCtx, vs->codecpar);

        // attempt D3D11VA hardware acceleration.
        // av_hwdevice_ctx_create sets the output pointer to nullptr on failure.
        if (av_hwdevice_ctx_create(&_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                   nullptr, nullptr, 0) >= 0) {
            _videoCtx->hw_device_ctx = av_buffer_ref(_hwDeviceCtx);
            _videoCtx->get_format    = GetHwFormat;
            // HW manages its own parallelism; disable FFmpeg frame threading.
            _videoCtx->thread_count = 1;
            _videoCtx->thread_type  = 0;
        } else {
            // HW unavailable — fall back to multi-threaded software decode.
            _videoCtx->thread_count = 0; // auto-detect core count
            _videoCtx->thread_type  = FF_THREAD_FRAME;
        }

        if (avcodec_open2(_videoCtx, vc, nullptr) < 0) return false;

        _videoWidth  = _videoCtx->width;
        _videoHeight = _videoCtx->height;
        _videoTbNum  = vs->time_base.num;
        _videoTbDen  = vs->time_base.den;
        // _swsCtx is created lazily in VideoDecodeLoop once the actual
        // decoded pixel format is known (needed for HW frames).
    }

    // --- Audio decoder ---
    if (_audioStream >= 0) {
        AVStream* as      = _fmtCtx->streams[_audioStream];
        const AVCodec* ac = avcodec_find_decoder(as->codecpar->codec_id);
        if (ac) {
            _audioCtx = avcodec_alloc_context3(ac);
            avcodec_parameters_to_context(_audioCtx, as->codecpar);
            if (avcodec_open2(_audioCtx, ac, nullptr) >= 0) {
                _audioTbNum      = as->time_base.num;
                _audioTbDen      = as->time_base.den;
                _audioSampleRate = _audioCtx->sample_rate;
                _audioChannels   = std::min(_audioCtx->ch_layout.nb_channels, 2);

                _swrCtx = swr_alloc();
                AVChannelLayout outLayout = (_audioChannels == 1)
                    ? AVChannelLayout AV_CHANNEL_LAYOUT_MONO
                    : AVChannelLayout AV_CHANNEL_LAYOUT_STEREO;

                av_opt_set_chlayout   (_swrCtx, "in_chlayout",    &_audioCtx->ch_layout, 0);
                av_opt_set_chlayout   (_swrCtx, "out_chlayout",   &outLayout,             0);
                av_opt_set_int        (_swrCtx, "in_sample_rate",  _audioSampleRate,       0);
                av_opt_set_int        (_swrCtx, "out_sample_rate", _audioSampleRate,       0);
                av_opt_set_sample_fmt (_swrCtx, "in_sample_fmt",   _audioCtx->sample_fmt,  0);
                av_opt_set_sample_fmt (_swrCtx, "out_sample_fmt",  AV_SAMPLE_FMT_S16,      0);
                swr_init(_swrCtx);

                _audioRenderer->Initialize(_audioSampleRate, _audioChannels);
            }
        }
    }

    return _videoStream >= 0;
}

void MediaDecoder::Play() {
    if (_running.exchange(true)) {
        _paused = false; // already running — just un-pause
        return;
    }
    _paused = false;
    // launch three independent threads.
    _demuxThread       = std::thread(&MediaDecoder::DemuxLoop,       this);
    _videoDecodeThread = std::thread(&MediaDecoder::VideoDecodeLoop, this);
    _audioDecodeThread = std::thread(&MediaDecoder::AudioDecodeLoop, this);
}

void MediaDecoder::Pause() {
    _paused = true;
}

void MediaDecoder::Stop() {
    _running = false;
    _paused  = false;

    // Unblock every thread waiting on a queue.
    _videoPktQueue->Shutdown();
    _audioPktQueue->Shutdown();
    _frameQueue->Shutdown();
    if (_audioRenderer) _audioRenderer->FlushQueue();

    if (_demuxThread.joinable())       _demuxThread.join();
    if (_videoDecodeThread.joinable()) _videoDecodeThread.join();
    if (_audioDecodeThread.joinable()) _audioDecodeThread.join();

    // Reset queues so the decoder can be Play()'d again.
    _videoPktQueue = std::make_unique<PacketQueue>(64);
    _audioPktQueue = std::make_unique<PacketQueue>(128);
    _frameQueue    = std::make_unique<FrameQueue>(16);
}

void MediaDecoder::Close() {
    Stop();
    if (_audioRenderer) _audioRenderer->Shutdown();
    if (_swsCtx)      { sws_freeContext(_swsCtx);        _swsCtx      = nullptr; }
    if (_swrCtx)      { swr_free(&_swrCtx);                                      }
    if (_videoCtx)    { avcodec_free_context(&_videoCtx);                        }
    if (_audioCtx)    { avcodec_free_context(&_audioCtx);                        }
    if (_hwDeviceCtx) { av_buffer_unref(&_hwDeviceCtx);                          }
    if (_fmtCtx)      { avformat_close_input(&_fmtCtx);                          }
    _videoStream = _audioStream = -1;
    _videoWidth  = _videoHeight = 0;
    _swsSrcFmt   = -1; // AV_PIX_FMT_NONE
    std::lock_guard<std::mutex> lock(_frameMutex);
    _curBgra.clear();
    _curWidth = _curHeight = 0;
}

double MediaDecoder::GetDurationSeconds() const {
    if (!_fmtCtx || _fmtCtx->duration == AV_NOPTS_VALUE) return 0.0;
    return static_cast<double>(_fmtCtx->duration) / AV_TIME_BASE;
}

int64_t MediaDecoder::GetAudioPositionUs() const {
    if (_audioRenderer && _audioRenderer->IsInitialized())
        return _audioRenderer->GetPositionUs();
    return 0;
}

bool MediaDecoder::TryGetFrameForTime(int64_t audioPtsUs,
                                      std::vector<uint8_t>& outBgra,
                                      int& outWidth, int& outHeight) {
    while (true) {
        auto frontPts = _frameQueue->PeekFrontPts();
        if (!frontPts) break;
        if (*frontPts > audioPtsUs) break;

        auto frame = _frameQueue->Pop();
        if (!frame) break;

        std::lock_guard<std::mutex> lock(_frameMutex);
        _curBgra   = std::move(frame->bgra);
        _curWidth  = frame->width;
        _curHeight = frame->height;
    }

    std::lock_guard<std::mutex> lock(_frameMutex);
    if (_curWidth == 0) return false;
    outBgra   = _curBgra;
    outWidth  = _curWidth;
    outHeight = _curHeight;
    return true;
}

// Only called from VideoDecodeLoop — no locking needed.
void MediaDecoder::UpdateSwsContext(int srcFmt) {
    if (_swsSrcFmt == srcFmt && _swsCtx) return;
    if (_swsCtx) { sws_freeContext(_swsCtx); _swsCtx = nullptr; }
    _swsSrcFmt = srcFmt;
    _swsCtx = sws_getContext(
        _videoWidth, _videoHeight, static_cast<AVPixelFormat>(srcFmt),
        _videoWidth, _videoHeight, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Thread 1: demux
// Reads packets from the container and routes them to the per-stream queues.
// ---------------------------------------------------------------------------
void MediaDecoder::DemuxLoop() {
    // Let av_read_frame() be interrupted when _running becomes false
    // (important for network sources; harmless for local files).
    _fmtCtx->interrupt_callback.callback = [](void* opaque) -> int {
        return !static_cast<MediaDecoder*>(opaque)->_running.load() ? 1 : 0;
    };
    _fmtCtx->interrupt_callback.opaque = this;

    AVPacket* pkt = av_packet_alloc();
    while (_running) {
        if (_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        int ret = av_read_frame(_fmtCtx, pkt);
        if (ret == AVERROR_EOF || ret < 0) break;

        if (pkt->stream_index == _videoStream && _videoCtx) {
            AVPacket* vpkt = av_packet_alloc();
            av_packet_ref(vpkt, pkt);
            if (!_videoPktQueue->Push(vpkt))
                av_packet_free(&vpkt); // queue shut down — discard
        } else if (pkt->stream_index == _audioStream && _audioCtx) {
            AVPacket* apkt = av_packet_alloc();
            av_packet_ref(apkt, pkt);
            if (!_audioPktQueue->Push(apkt))
                av_packet_free(&apkt);
        }

        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Signal EOF to both decode threads.
    _videoPktQueue->Shutdown();
    _audioPktQueue->Shutdown();
}

// ---------------------------------------------------------------------------
// Thread 2: video decode
// handles D3D11VA hardware frames via av_hwframe_transfer_data.
// ---------------------------------------------------------------------------
void MediaDecoder::VideoDecodeLoop() {
    if (!_videoCtx) return;

    AVFrame* frame   = av_frame_alloc();
    AVFrame* swFrame = av_frame_alloc(); // used for HW→SW transfer

    const int bgraStride = _videoWidth * 4;
    std::vector<uint8_t> bgraBuf(_videoWidth * _videoHeight * 4);

    // Drains all pending frames from the codec. Returns true when the codec
    // needs more input (EAGAIN), false when fully drained or stopped.
    auto drainFrames = [&]() -> bool {
        while (_running) {
            int ret = avcodec_receive_frame(_videoCtx, frame);
            if (ret == AVERROR(EAGAIN)) return true;  // need more packets
            if (ret == AVERROR_EOF || ret < 0) return false;

            // Solution 1: if the frame lives on the GPU, copy it to CPU first.
            AVFrame* srcFrame = frame;
            if (frame->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(swFrame);
                if (av_hwframe_transfer_data(swFrame, frame, 0) < 0) {
                    av_frame_unref(frame);
                    continue;
                }
                srcFrame = swFrame;
            }

            // Lazily create / recreate SWS context if pixel format changed.
            UpdateSwsContext(srcFrame->format);

            if (_swsCtx) {
                uint8_t* dst[4]    = { bgraBuf.data(), nullptr, nullptr, nullptr };
                int      stride[4] = { bgraStride,     0,       0,       0       };
                sws_scale(_swsCtx, srcFrame->data, srcFrame->linesize,
                          0, _videoHeight, dst, stride);

                VideoFrame vf;
                vf.ptsUs  = TsToUs(frame->best_effort_timestamp, _videoTbNum, _videoTbDen);
                vf.width  = _videoWidth;
                vf.height = _videoHeight;
                vf.bgra   = bgraBuf;
                _frameQueue->Push(std::move(vf));
            }
            av_frame_unref(frame);
        }
        return false;
    };

    while (_running) {
        AVPacket* pkt = _videoPktQueue->Pop(); // blocks until packet or shutdown
        if (pkt) {
            avcodec_send_packet(_videoCtx, pkt);
            av_packet_free(&pkt);
            drainFrames();
        } else {
            // Queue shut down (EOF or Stop) — flush remaining frames.
            avcodec_send_packet(_videoCtx, nullptr);
            while (drainFrames()) {} // keep draining until AVERROR_EOF
            break;
        }
    }

    av_frame_free(&frame);
    av_frame_free(&swFrame);
    _frameQueue->Shutdown(); // notify UI that the video stream has ended
}

// ---------------------------------------------------------------------------
// Thread 3: audio decode
// ---------------------------------------------------------------------------
void MediaDecoder::AudioDecodeLoop() {
    if (!_audioCtx || !_swrCtx) return;

    AVFrame* frame = av_frame_alloc();

    auto drainFrames = [&]() -> bool {
        while (_running) {
            int ret = avcodec_receive_frame(_audioCtx, frame);
            if (ret == AVERROR(EAGAIN)) return true;
            if (ret == AVERROR_EOF || ret < 0) return false;

            int outSamples = av_rescale_rnd(
                swr_get_delay(_swrCtx, _audioSampleRate) + frame->nb_samples,
                _audioSampleRate, _audioSampleRate, AV_ROUND_UP);

            std::vector<uint8_t> pcm(
                static_cast<size_t>(outSamples) * _audioChannels * sizeof(int16_t));

            uint8_t* outPtr   = pcm.data();
            int converted = swr_convert(_swrCtx, &outPtr, outSamples,
                const_cast<const uint8_t**>(frame->data), frame->nb_samples);

            if (converted > 0) {
                pcm.resize(static_cast<size_t>(converted) * _audioChannels * sizeof(int16_t));
                AudioChunk chunk;
                chunk.ptsUs  = TsToUs(frame->pts, _audioTbNum, _audioTbDen);
                chunk.pcmS16 = std::move(pcm);
                _audioRenderer->QueueChunk(std::move(chunk));
            }
            av_frame_unref(frame);
        }
        return false;
    };

    while (_running) {
        AVPacket* pkt = _audioPktQueue->Pop();
        if (pkt) {
            avcodec_send_packet(_audioCtx, pkt);
            av_packet_free(&pkt);
            drainFrames();
        } else {
            avcodec_send_packet(_audioCtx, nullptr);
            while (drainFrames()) {}
            break;
        }
    }

    av_frame_free(&frame);
}

} // namespace MagicStudio::Native
