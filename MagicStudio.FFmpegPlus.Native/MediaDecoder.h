#pragma once
#include "AudioRenderer.h"
#include "FrameQueue.h"
#include "PacketQueue.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare FFmpeg types to keep this header clean.
struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct SwsContext;
struct SwrContext;

namespace MagicStudio::Native {

class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    bool Open(const char* path);
    void Play();
    void Pause();
    void Stop();
    void Close();

    // Returns the current audio clock position (master clock for A/V sync).
    int64_t GetAudioPositionUs() const;

    // Called from the render thread each frame.
    // Pops all video frames that are due (pts <= audio_pts) and returns the
    // most recent one. Returns false only when no frame has ever been decoded.
    bool TryGetFrameForTime(int64_t audioPtsUs,
                            std::vector<uint8_t>& outBgra,
                            int& outWidth, int& outHeight);

    int    GetVideoWidth()       const { return _videoWidth; }
    int    GetVideoHeight()      const { return _videoHeight; }
    double GetDurationSeconds()  const;
    bool   IsOpen()              const { return _fmtCtx != nullptr; }

private:
    // three separate threads
    void DemuxLoop();        // reads packets, routes to per-stream queues
    void VideoDecodeLoop();  // decodes video packets → FrameQueue
    void AudioDecodeLoop();  // decodes audio packets → AudioRenderer

    // lazily (re)creates _swsCtx when the source pixel format changes.
    // Only called from VideoDecodeLoop, so no locking needed.
    void UpdateSwsContext(int srcFmt);

    AVFormatContext* _fmtCtx      = nullptr;
    AVCodecContext*  _videoCtx    = nullptr;
    AVCodecContext*  _audioCtx    = nullptr;
    SwsContext*      _swsCtx      = nullptr;
    SwrContext*      _swrCtx      = nullptr;
    AVBufferRef*     _hwDeviceCtx = nullptr; // D3D11VA device

    int _videoStream = -1;
    int _audioStream = -1;
    int _videoWidth  = 0;
    int _videoHeight = 0;

    int _videoTbNum = 1, _videoTbDen = 1;
    int _audioTbNum = 1, _audioTbDen = 1;
    int _audioSampleRate = 0;
    int _audioChannels   = 0;

    int _swsSrcFmt = -1; // AV_PIX_FMT_NONE — tracks current sws source format

    std::unique_ptr<FrameQueue>    _frameQueue;
    std::unique_ptr<PacketQueue>   _videoPktQueue; // Solution 2
    std::unique_ptr<PacketQueue>   _audioPktQueue; // Solution 2
    std::unique_ptr<AudioRenderer> _audioRenderer;

    // demux + per-stream decode threads
    std::thread       _demuxThread;
    std::thread       _videoDecodeThread;
    std::thread       _audioDecodeThread;

    std::atomic<bool> _running{false};
    std::atomic<bool> _paused{false};

    // Current (last displayed) frame — protected by _frameMutex.
    std::mutex           _frameMutex;
    std::vector<uint8_t> _curBgra;
    int                  _curWidth  = 0;
    int                  _curHeight = 0;
};

} // namespace MagicStudio::Native
