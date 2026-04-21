#include "pch.h"
#include "PacketQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace MagicStudio::Native {

PacketQueue::PacketQueue(size_t capacity) : _capacity(capacity) {}

PacketQueue::~PacketQueue() {
    Flush();
}

bool PacketQueue::Push(AVPacket* pkt) {
    std::unique_lock<std::mutex> lock(_mutex);
    _notFull.wait(lock, [this] { return _queue.size() < _capacity || _shutdown; });
    if (_shutdown) return false;
    _queue.push(pkt);
    _notEmpty.notify_one();
    return true;
}

AVPacket* PacketQueue::Pop() {
    std::unique_lock<std::mutex> lock(_mutex);
    _notEmpty.wait(lock, [this] { return !_queue.empty() || _shutdown; });
    if (_queue.empty()) return nullptr;
    AVPacket* pkt = _queue.front();
    _queue.pop();
    _notFull.notify_one();
    return pkt;
}

void PacketQueue::Shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    _shutdown = true;
    _notFull.notify_all();
    _notEmpty.notify_all();
}

void PacketQueue::Flush() {
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_queue.empty()) {
        AVPacket* pkt = _queue.front();
        _queue.pop();
        av_packet_free(&pkt);
    }
    _notFull.notify_all();
}

} // namespace MagicStudio::Native
