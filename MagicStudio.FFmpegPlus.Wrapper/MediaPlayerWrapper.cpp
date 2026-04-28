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

MediaPlayer::MediaPlayer() : _handle(nullptr) {}

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
    std::string nativePath = marshal_as<std::string>(path);
    _handle = magic_player_open(nativePath.c_str());
    return _handle != nullptr;
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

IntPtr MediaPlayer::AcquireDxgiDevice() {
    IDXGIDevice* dev = nullptr;
    if (!magic_player_acquire_dxgi_device(&dev) || !dev)
        return IntPtr::Zero;
    return IntPtr(dev);
}

IntPtr MediaPlayer::TryAcquireCurrentTexture(UInt64% version,
                                             int% width,
                                             int% height) {
    version = 0;
    width   = 0;
    height  = 0;
    if (!_handle) return IntPtr::Zero;

    // Cheap version check first: if no frame has ever been published, skip
    // the texture acquire/Release dance entirely.
    UInt64 v = magic_player_current_frame_version(H(_handle));
    if (v == 0) return IntPtr::Zero;

    ID3D11Texture2D* tex = nullptr;
    if (!magic_player_acquire_current_texture(H(_handle), &tex) || !tex)
        return IntPtr::Zero;

    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    version = v;
    width   = static_cast<int>(desc.Width);
    height  = static_cast<int>(desc.Height);
    return IntPtr(tex);
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
