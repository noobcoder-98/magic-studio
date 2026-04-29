#include "pch.h"
#include "MediaPlayerWrapper.h"

// Give C++/CLI a complete type for each handle so it can emit valid metadata.
// The actual layout lives in the Native static lib; we only pass pointers here.
struct MagicPlayerHandle  {};
struct MagicFFplayHandle  {};

using namespace msclr::interop;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

// ============================================================================
// MediaPlayer  (wraps MagicFFmpegPlayer / magic_player_* API)
// ============================================================================

static inline ::MagicPlayerHandle* HP(void* p) {
    return reinterpret_cast<::MagicPlayerHandle*>(p);
}

MediaPlayer::MediaPlayer() : _handle(nullptr) {}
MediaPlayer::~MediaPlayer()  { this->!MediaPlayer(); }
MediaPlayer::!MediaPlayer()  {
    if (_handle) { magic_player_close(HP(_handle)); _handle = nullptr; }
}

bool MediaPlayer::Open(String^ path) {
    if (_handle) { magic_player_close(HP(_handle)); _handle = nullptr; }
    std::string s = marshal_as<std::string>(path);
    _handle = magic_player_open(s.c_str());
    return _handle != nullptr;
}

void MediaPlayer::Play()  { if (_handle && magic_player_is_paused(HP(_handle)))  magic_player_toggle_pause(HP(_handle)); }
void MediaPlayer::Pause() { if (_handle && !magic_player_is_paused(HP(_handle))) magic_player_toggle_pause(HP(_handle)); }
void MediaPlayer::Stop()  { if (!_handle) return; Pause(); magic_player_seek_us(HP(_handle), 0); }
void MediaPlayer::Seek(Int64 us) { if (_handle) magic_player_seek_us(HP(_handle), static_cast<int64_t>(us)); }

Int64 MediaPlayer::GetAudioPositionUs() {
    return _handle ? static_cast<Int64>(magic_player_master_clock_us(HP(_handle))) : 0;
}

IntPtr MediaPlayer::AcquireDxgiDevice() {
    IDXGIDevice* dev = nullptr;
    return (magic_player_acquire_dxgi_device(&dev) && dev) ? IntPtr(dev) : IntPtr::Zero;
}

IntPtr MediaPlayer::TryAcquireCurrentTexture(UInt64% version, int% width, int% height) {
    version = 0; width = 0; height = 0;
    if (!_handle) return IntPtr::Zero;
    UInt64 v = magic_player_current_frame_version(HP(_handle));
    if (v == 0) return IntPtr::Zero;
    ID3D11Texture2D* tex = nullptr;
    if (!magic_player_acquire_current_texture(HP(_handle), &tex) || !tex) return IntPtr::Zero;
    D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
    version = v; width = (int)d.Width; height = (int)d.Height;
    return IntPtr(tex);
}

int MediaPlayer::VideoWidth::get()  { int w=0,h=0; return _handle && magic_player_video_size(HP(_handle),&w,&h) ? w : 0; }
int MediaPlayer::VideoHeight::get() { int w=0,h=0; return _handle && magic_player_video_size(HP(_handle),&w,&h) ? h : 0; }
double MediaPlayer::Duration::get() { return _handle ? magic_player_duration_seconds(HP(_handle)) : 0; }

// ============================================================================
// FFplayPlayer  (wraps MagicFFplay / magic_ffplay_* API)
// ============================================================================

static inline ::MagicFFplayHandle* HF(void* p) {
    return reinterpret_cast<::MagicFFplayHandle*>(p);
}

FFplayPlayer::FFplayPlayer() : _handle(nullptr) {}
FFplayPlayer::~FFplayPlayer()  { this->!FFplayPlayer(); }
FFplayPlayer::!FFplayPlayer()  {
    if (_handle) { magic_ffplay_close(HF(_handle)); _handle = nullptr; }
}

bool FFplayPlayer::Open(String^ path) {
    if (_handle) { magic_ffplay_close(HF(_handle)); _handle = nullptr; }
    std::string s = marshal_as<std::string>(path);
    _handle = magic_ffplay_open(s.c_str());
    return _handle != nullptr;
}

void FFplayPlayer::Play()  { if (_handle && magic_ffplay_is_paused(HF(_handle)))  magic_ffplay_toggle_pause(HF(_handle)); }
void FFplayPlayer::Pause() { if (_handle && !magic_ffplay_is_paused(HF(_handle))) magic_ffplay_toggle_pause(HF(_handle)); }
void FFplayPlayer::Stop()  { if (!_handle) return; Pause(); magic_ffplay_seek_us(HF(_handle), 0); }
void FFplayPlayer::Seek(Int64 us)  { if (_handle) magic_ffplay_seek_us(HF(_handle), static_cast<int64_t>(us)); }
void FFplayPlayer::StepFrame()     { if (_handle) magic_ffplay_step_frame(HF(_handle)); }

Int64 FFplayPlayer::GetAudioPositionUs() {
    return _handle ? static_cast<Int64>(magic_ffplay_master_clock_us(HF(_handle))) : 0;
}

IntPtr FFplayPlayer::AcquireDxgiDevice() {
    if (!_handle) return IntPtr::Zero;
    IDXGIDevice* dev = nullptr;
    return (magic_ffplay_acquire_dxgi_device(HF(_handle), &dev) && dev) ? IntPtr(dev) : IntPtr::Zero;
}

IntPtr FFplayPlayer::TryAcquireCurrentTexture(UInt64% version, int% width, int% height) {
    version = 0; width = 0; height = 0;
    if (!_handle) return IntPtr::Zero;
    UInt64 v = magic_ffplay_current_frame_version(HF(_handle));
    if (v == 0) return IntPtr::Zero;
    ID3D11Texture2D* tex = nullptr;
    if (!magic_ffplay_acquire_current_texture(HF(_handle), &tex) || !tex) return IntPtr::Zero;
    D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
    version = v; width = (int)d.Width; height = (int)d.Height;
    return IntPtr(tex);
}

UInt64 FFplayPlayer::PeekFrameVersion() {
    return _handle ? static_cast<UInt64>(magic_ffplay_current_frame_version(HF(_handle))) : 0;
}

int FFplayPlayer::VideoWidth::get()  { int w=0,h=0; return _handle && magic_ffplay_video_size(HF(_handle),&w,&h) ? w : 0; }
int FFplayPlayer::VideoHeight::get() { int w=0,h=0; return _handle && magic_ffplay_video_size(HF(_handle),&w,&h) ? h : 0; }
double FFplayPlayer::Duration::get() { return _handle ? magic_ffplay_duration_seconds(HF(_handle)) : 0; }

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
