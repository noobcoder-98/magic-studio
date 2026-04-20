#include "pch.h"
#include "MediaDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

MediaDecoder::MediaDecoder()
    : _frameQueue(std::make_unique<FrameQueue>(8))
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

    // Video decoder
    if (_videoStream >= 0) {
        AVStream* vs      = _fmtCtx->streams[_videoStream];
        const AVCodec* vc = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!vc) return false;

        _videoCtx = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(_videoCtx, vs->codecpar);
        // Enable multi-threaded decoding
        _videoCtx->thread_count = 0; // auto
        _videoCtx->thread_type  = FF_THREAD_FRAME;
        if (avcodec_open2(_videoCtx, vc, nullptr) < 0) return false;

        _videoWidth  = _videoCtx->width;
        _videoHeight = _videoCtx->height;
        _videoTbNum  = vs->time_base.num;
        _videoTbDen  = vs->time_base.den;

        _swsCtx = sws_getContext(
            _videoWidth, _videoHeight, _videoCtx->pix_fmt,
            _videoWidth, _videoHeight, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!_swsCtx) return false;
    }

    // Audio decoder
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

                // Resample to stereo S16 at the original sample rate.
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
    _decodeThread = std::thread(&MediaDecoder::DecodeLoop, this);
}

void MediaDecoder::Pause() {
    _paused = true;
}

void MediaDecoder::Stop() {
    _running = false;
    _paused  = false;
    _frameQueue->Shutdown();
    if (_audioRenderer) _audioRenderer->FlushQueue();
    if (_decodeThread.joinable()) _decodeThread.join();
    // Reset queue for potential re-play.
    _frameQueue = std::make_unique<FrameQueue>(8);
}

void MediaDecoder::Close() {
    Stop();
    if (_audioRenderer) _audioRenderer->Shutdown();
    if (_swsCtx)  { sws_freeContext(_swsCtx);     _swsCtx   = nullptr; }
    if (_swrCtx)  { swr_free(&_swrCtx);            }
    if (_videoCtx){ avcodec_free_context(&_videoCtx); }
    if (_audioCtx){ avcodec_free_context(&_audioCtx); }
    if (_fmtCtx)  { avformat_close_input(&_fmtCtx); }
    _videoStream = _audioStream = -1;
    _videoWidth  = _videoHeight = 0;
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
    // Pop every frame whose presentation time has passed.
    // The last one popped is the correct frame to display.
    while (true) {
        auto frontPts = _frameQueue->PeekFrontPts();
        if (!frontPts) break;
        if (*frontPts > audioPtsUs) break; // frame is in the future

        auto frame = _frameQueue->Pop();
        if (!frame) break;

        std::lock_guard<std::mutex> lock(_frameMutex);
        _curBgra   = std::move(frame->bgra);
        _curWidth  = frame->width;
        _curHeight = frame->height;
    }

    std::lock_guard<std::mutex> lock(_frameMutex);
    if (_curWidth == 0) return false;
    outBgra   = _curBgra; // copy to caller
    outWidth  = _curWidth;
    outHeight = _curHeight;
    return true;
}

void MediaDecoder::DecodeLoop() {
    AVPacket* pkt  = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    const int bgraStride = _videoWidth * 4;
    std::vector<uint8_t> bgraBuf(_videoWidth * _videoHeight * 4);

    while (_running) {
        if (_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        int ret = av_read_frame(_fmtCtx, pkt);
        if (ret == AVERROR_EOF || ret < 0) break;

        if (pkt->stream_index == _videoStream && _videoCtx && _swsCtx) {
            avcodec_send_packet(_videoCtx, pkt);
            while (_running && avcodec_receive_frame(_videoCtx, frame) == 0) {
                uint8_t* dst[4]     = { bgraBuf.data(), nullptr, nullptr, nullptr };
                int      stride[4]  = { bgraStride,     0,       0,       0       };
                sws_scale(_swsCtx, frame->data, frame->linesize,
                          0, _videoHeight, dst, stride);

                VideoFrame vf;
                vf.ptsUs = TsToUs(frame->best_effort_timestamp, _videoTbNum, _videoTbDen);
                vf.width  = _videoWidth;
                vf.height = _videoHeight;
                vf.bgra   = bgraBuf; // copy

                // Blocks when the queue is full — naturally throttles decode
                // so we never buffer too many frames ahead.
                _frameQueue->Push(std::move(vf));
                av_frame_unref(frame);
            }

        } else if (pkt->stream_index == _audioStream && _audioCtx && _swrCtx) {
            avcodec_send_packet(_audioCtx, pkt);
            while (_running && avcodec_receive_frame(_audioCtx, frame) == 0) {
                int outSamples = av_rescale_rnd(
                    swr_get_delay(_swrCtx, _audioSampleRate) + frame->nb_samples,
                    _audioSampleRate, _audioSampleRate, AV_ROUND_UP);

                std::vector<uint8_t> pcm(
                    static_cast<size_t>(outSamples) * _audioChannels * sizeof(int16_t));

                uint8_t* outPtr = pcm.data();
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
        }

        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

} // namespace MagicStudio::Native
