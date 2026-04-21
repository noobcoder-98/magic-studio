#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>

struct AVPacket;

namespace MagicStudio::Native {

// Thread-safe bounded queue for AVPacket*: one producer (demux thread),
// one consumer (video or audio decode thread). The queue owns the packets;
// callers must NOT free packets they successfully Push().
class PacketQueue {
public:
    explicit PacketQueue(size_t capacity);
    ~PacketQueue();

    // Takes ownership of pkt. Blocks when full or until Shutdown().
    // Returns false on shutdown — caller must free pkt in that case.
    bool Push(AVPacket* pkt);

    // Blocks until a packet is available or the queue is shut down.
    // Returns nullptr when shut down and empty.
    AVPacket* Pop();

    void Shutdown();
    void Flush();

private:
    size_t _capacity;
    std::queue<AVPacket*> _queue;
    mutable std::mutex _mutex;
    std::condition_variable _notFull;
    std::condition_variable _notEmpty;
    bool _shutdown = false;
};

} // namespace MagicStudio::Native
