#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace MagicStudio::Native {

struct VideoFrame {
    int64_t ptsUs = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
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
