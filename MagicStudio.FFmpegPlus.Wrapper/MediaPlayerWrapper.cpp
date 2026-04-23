#include "pch.h"
#include "MediaPlayerWrapper.h"

// Native owns the real layout of MagicPlayerHandle. We never instantiate or
// sizeof it here -- only pass pointers through -- but C++/CLI needs a complete
// type in this TU to avoid LNK4248 ("unresolved typeref") in the metadata.
struct MagicPlayerHandle {};

using namespace msclr::interop;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

static inline ::MagicPlayerHandle* H(void* p) {
    return reinterpret_cast<::MagicPlayerHandle*>(p);
}

MediaPlayer::MediaPlayer()
    : _handle(nullptr), _width(0), _height(0), _lastVersion(0), _bgraBuffer(nullptr)
{}

MediaPlayer::~MediaPlayer() {
    this->!MediaPlayer();
}

MediaPlayer::!MediaPlayer() {
    if (_handle) {
        magic_player_close(H(_handle));
        _handle = nullptr;
    }
}

bool MediaPlayer::Open(String^ path) {
    if (_handle) {
        magic_player_close(H(_handle));
        _handle = nullptr;
    }
    _lastVersion = 0;
    _bgraBuffer  = nullptr;

    std::string nativePath = marshal_as<std::string>(path);
    _handle = magic_player_open(nativePath.c_str());
    if (!_handle) return false;

    // Cache the video size up front so TryGetFrame can size the readback
    // buffer without a per-frame native call.
    int w = 0, h = 0;
    if (magic_player_video_size(H(_handle), &w, &h)) {
        _width  = w;
        _height = h;
    }
    return true;
}

void MediaPlayer::Play() {
    if (!_handle) return;
    if (magic_player_is_paused(H(_handle)))
        magic_player_toggle_pause(H(_handle));
}

void MediaPlayer::Pause() {
    if (!_handle) return;
    if (!magic_player_is_paused(H(_handle)))
        magic_player_toggle_pause(H(_handle));
}

void MediaPlayer::Stop() {
    if (!_handle) return;
    Pause();
    magic_player_seek_us(H(_handle), 0);
}

void MediaPlayer::Seek(Int64 positionUs) {
    if (!_handle) return;
    magic_player_seek_us(H(_handle), static_cast<int64_t>(positionUs));
}

Int64 MediaPlayer::GetAudioPositionUs() {
    if (!_handle) return 0;
    return static_cast<Int64>(magic_player_master_clock_us(H(_handle)));
}

bool MediaPlayer::TryGetFrame(Int64 /*audioPtsUs*/, [Out] FrameData^% frame) {
    frame = nullptr;
    if (!_handle) return false;

    // Cheap check: same frame as last time? Avoid the GPU readback + managed
    // allocation entirely. The host should keep using its cached bitmap.
    UInt64 version = magic_player_current_frame_version(H(_handle));
    if (version == 0 || version == _lastVersion)
        return false;

    // Re-query dimensions in case the stream wasn't ready when Open() returned.
    if (_width <= 0 || _height <= 0) {
        int w = 0, h = 0;
        if (!magic_player_video_size(H(_handle), &w, &h) || w <= 0 || h <= 0)
            return false;
        _width  = w;
        _height = h;
    }

    const int needed = _width * _height * 4;
    if (_bgraBuffer == nullptr || _bgraBuffer->Length != needed)
        _bgraBuffer = gcnew array<Byte>(needed);

    int outW = 0, outH = 0;
    {
        pin_ptr<Byte> pin = &_bgraBuffer[0];
        if (!magic_player_copy_current_bgra(H(_handle),
                                            pin, needed,
                                            &outW, &outH))
            return false;
    }

    // The frame size could change mid-stream (e.g. resolution switch); if so,
    // grow the cached dims and ask the next Draw cycle to allocate again.
    if (outW != _width || outH != _height) {
        _width      = outW;
        _height     = outH;
        _bgraBuffer = nullptr;
        return false;
    }

    _lastVersion = version;

    auto fd    = gcnew FrameData();
    fd->Width  = outW;
    fd->Height = outH;
    fd->BgraData = _bgraBuffer;
    frame = fd;
    return true;
}

int MediaPlayer::VideoWidth::get() {
    if (!_handle) return 0;
    int w = 0, h = 0;
    return magic_player_video_size(H(_handle), &w, &h) ? w : 0;
}

int MediaPlayer::VideoHeight::get() {
    if (!_handle) return 0;
    int w = 0, h = 0;
    return magic_player_video_size(H(_handle), &w, &h) ? h : 0;
}

double MediaPlayer::Duration::get() {
    if (!_handle) return 0;
    return magic_player_duration_seconds(H(_handle));
}

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
