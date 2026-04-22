#include "pch.h"
#include "FrameQueue.h"

namespace MagicStudio::Native {

FrameQueue::FrameQueue(size_t capacity) : _capacity(capacity) {}

void FrameQueue::Push(VideoFrame&& frame) {
    std::unique_lock<std::mutex> lock(_mutex);
    _notFull.wait(lock, [this] { return _queue.size() < _capacity || _shutdown; });
    if (_shutdown) return;
    _queue.push(std::move(frame));
}

std::optional<VideoFrame> FrameQueue::Pop() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_queue.empty()) return std::nullopt;
    auto frame = std::move(_queue.front());
    _queue.pop();
    _notFull.notify_one();
    return frame;
}

std::optional<int64_t> FrameQueue::PeekFrontPts() const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_queue.empty()) return std::nullopt;
    return _queue.front().ptsUs;
}

void FrameQueue::Flush() {
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_queue.empty()) _queue.pop();
    _notFull.notify_all();
}

void FrameQueue::Shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    _shutdown = true;
    _notFull.notify_all();
}

bool FrameQueue::IsShutdown() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _shutdown;
}

size_t FrameQueue::Size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.size();
}

} // namespace MagicStudio::Native
