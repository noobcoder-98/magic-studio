#include "pch.h"
#include "AudioRenderer.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace MagicStudio::Native {

// Heap-allocated per submitted XAudio2 buffer; freed in OnBufferEnd.
struct BufferContext {
    std::vector<uint8_t> data;
};

void AudioVoiceCallback::OnBufferEnd(void* pBufferContext) {
    delete static_cast<BufferContext*>(pBufferContext);
}

AudioRenderer::~AudioRenderer() {
    Shutdown();
}

bool AudioRenderer::Initialize(int sampleRate, int channels) {
    _sampleRate = sampleRate;
    _channels = channels;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = XAudio2Create(&_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return false;

    hr = _xaudio2->CreateMasteringVoice(&_masterVoice);
    if (FAILED(hr)) return false;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = static_cast<WORD>(channels);
    wfx.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    hr = _xaudio2->CreateSourceVoice(&_sourceVoice, &wfx,
        0, XAUDIO2_DEFAULT_FREQ_RATIO, &_voiceCallback);
    if (FAILED(hr)) return false;

    _sourceVoice->Start(0);
    _initialized = true;
    _running = true;
    _renderThread = std::thread(&AudioRenderer::RenderThread, this);
    return true;
}

void AudioRenderer::Shutdown() {
    if (!_initialized) return;
    _running = false;
    _chunkCv.notify_all();
    if (_renderThread.joinable()) _renderThread.join();

    if (_sourceVoice)  { _sourceVoice->DestroyVoice();  _sourceVoice  = nullptr; }
    if (_masterVoice)  { _masterVoice->DestroyVoice();  _masterVoice  = nullptr; }
    if (_xaudio2)      { _xaudio2->Release();            _xaudio2      = nullptr; }

    _initialized = false;
}

void AudioRenderer::QueueChunk(AudioChunk&& chunk) {
    std::lock_guard<std::mutex> lock(_chunkMutex);
    _chunkQueue.push(std::move(chunk));
    _chunkCv.notify_one();
}

void AudioRenderer::FlushQueue() {
    {
        std::lock_guard<std::mutex> lock(_chunkMutex);
        while (!_chunkQueue.empty()) _chunkQueue.pop();
    }
    if (_sourceVoice) _sourceVoice->FlushSourceBuffers();
}

int64_t AudioRenderer::GetPositionUs() const {
    if (!_sourceVoice) return 0;
    XAUDIO2_VOICE_STATE state{};
    _sourceVoice->GetState(&state);
    UINT64 played = state.SamplesPlayed;
    UINT64 base   = _baseSamplesPlayed.load(std::memory_order_relaxed);
    if (played < base) return _basePtsUs.load(std::memory_order_relaxed);
    int64_t elapsed_us = static_cast<int64_t>((played - base) * 1'000'000LL / _sampleRate);
    return _basePtsUs.load(std::memory_order_relaxed) + elapsed_us;
}

void AudioRenderer::RenderThread() {
    // Limit buffered XAudio2 ahead to ~4 chunks to bound latency.
    constexpr UINT32 kMaxQueued = 4;

    while (_running) {
        AudioChunk chunk;
        {
            std::unique_lock<std::mutex> lock(_chunkMutex);
            _chunkCv.wait(lock, [this] { return !_chunkQueue.empty() || !_running; });
            if (!_running && _chunkQueue.empty()) break;
            chunk = std::move(_chunkQueue.front());
            _chunkQueue.pop();
        }

        // Back-pressure: wait if XAudio2 is still consuming previous buffers.
        while (_running) {
            XAUDIO2_VOICE_STATE state{};
            _sourceVoice->GetState(&state);
            if (state.BuffersQueued < kMaxQueued) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (!_running) break;

        // Capture baseline PTS + sample count on the very first chunk.
        std::call_once(_clockInit, [&] {
            XAUDIO2_VOICE_STATE state{};
            _sourceVoice->GetState(&state);
            _baseSamplesPlayed.store(state.SamplesPlayed, std::memory_order_relaxed);
            _basePtsUs.store(chunk.pts_us, std::memory_order_relaxed);
        });

        auto* ctx    = new BufferContext{std::move(chunk.pcm_s16)};
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(ctx->data.size());
        buf.pAudioData = ctx->data.data();
        buf.pContext   = ctx;

        if (FAILED(_sourceVoice->SubmitSourceBuffer(&buf)))
            delete ctx;
    }
}

} // namespace MagicStudio::Native
