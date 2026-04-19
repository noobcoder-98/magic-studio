#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <xaudio2.h>

namespace MagicStudio::Native {

struct AudioChunk {
    int64_t pts_us = 0;
    std::vector<uint8_t> pcm_s16; // interleaved signed-16 PCM
};

class AudioVoiceCallback : public IXAudio2VoiceCallback {
public:
    void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override;
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

// Streams decoded audio through XAudio2.
// GetPositionUs() is the master clock for A/V sync.
class AudioRenderer {
public:
    AudioRenderer() = default;
    ~AudioRenderer();

    bool Initialize(int sampleRate, int channels);
    void Shutdown();

    void QueueChunk(AudioChunk&& chunk);
    void FlushQueue();

    // Master clock: stream position in microseconds.
    int64_t GetPositionUs() const;

    bool IsInitialized() const { return _initialized; }

private:
    void RenderThread();

    IXAudio2* _xaudio2 = nullptr;
    IXAudio2MasteringVoice* _masterVoice = nullptr;
    IXAudio2SourceVoice* _sourceVoice = nullptr;
    AudioVoiceCallback _voiceCallback;

    int _sampleRate = 0;
    int _channels = 0;

    std::queue<AudioChunk> _chunkQueue;
    std::mutex _chunkMutex;
    std::condition_variable _chunkCv;

    std::thread _renderThread;
    std::atomic<bool> _running{false};
    bool _initialized = false;

    // Clock baseline: set once on the first submitted chunk.
    std::once_flag _clockInit;
    std::atomic<int64_t> _basePtsUs{0};
    std::atomic<UINT64> _baseSamplesPlayed{0};
};

} // namespace MagicStudio::Native
