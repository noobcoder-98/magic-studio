#pragma once
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

// Forward-declare D3D11 types
struct ID3D11Texture2D;

namespace MagicStudio::Native {

struct VideoFrame {
    int64_t ptsUs = 0;
    int width = 0, height = 0;

    // CPU-side BGRA data (fallback for CPU rendering)
    std::vector<uint8_t> bgra;

    // GPU-side BGRA texture (D3D11, preferred for Win2D interop)
    // If non-null, Win2D can use this directly via interop
    ID3D11Texture2D* gpuBgraTexture = nullptr;

    // Helper: Returns true if GPU texture is available and should be preferred
    bool HasGpuTexture() const { return gpuBgraTexture != nullptr; }
};

// Thread-safe bounded queue: one producer (decode thread), one consumer (UI thread).
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 8);

    // Blocks until space is available or Shutdown() is called.
    void Push(VideoFrame&& frame);

    // Returns nullopt if empty (non-blocking).
    std::optional<VideoFrame> Pop();

    // Peeks the PTS of the front frame without removing it (non-blocking).
    std::optional<int64_t> PeekFrontPts() const;

    void Flush();
    void Shutdown();
    bool IsShutdown() const;
    size_t Size() const;

private:
    size_t _capacity;
    std::queue<VideoFrame> _queue;
    mutable std::mutex _mutex;
    std::condition_variable _notFull;
    bool _shutdown = false;
};

} // namespace MagicStudio::Native
