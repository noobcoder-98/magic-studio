#pragma once
using namespace System;
using namespace System::Runtime::InteropServices;

// Forward-declare the opaque native handle so this header stays free of
// d3d11/dxgi includes.
struct MagicPlayerHandle;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

public ref class FrameData sealed {
public:
    property array<Byte>^ BgraData;
    property int Width;
    property int Height;
};

/// <summary>
/// Managed wrapper around the native MagicFFmpegPlayer C API.
/// Open() then Play() to start; call TryGetFrame() from your render loop with
/// the master clock returned by GetAudioPositionUs().
/// </summary>
public ref class MediaPlayer sealed {
public:
    MediaPlayer();
    ~MediaPlayer();   // IDisposable.Dispose()
    !MediaPlayer();   // finalizer

    bool   Open(String^ path);
    void   Play();
    void   Pause();
    void   Stop();
    void   Seek(Int64 positionUs);

    /// <summary>Audio-anchored master clock in microseconds.</summary>
    Int64  GetAudioPositionUs();

    /// <summary>
    /// Returns the most recently presented video frame *only when it differs
    /// from the one returned by the previous call*. When the displayed frame
    /// hasn't advanced, returns false and frame is null -- callers should
    /// keep using their cached bitmap. The audioPtsUs argument is accepted
    /// for API symmetry but currently ignored.
    /// </summary>
    bool TryGetFrame(Int64 audioPtsUs, [Out] FrameData^% frame);

    property int    VideoWidth  { int    get(); }
    property int    VideoHeight { int    get(); }
    property double Duration    { double get(); }

private:
    // Stored as void* to keep MagicFFmpegPlayer.h out of consumers; cast back
    // in the .cpp implementation.
    void*  _handle;
    // Cached dimensions to size the readback buffer without poking the native
    // handle on every frame.
    int    _width;
    int    _height;
    // Last frame version handed back via TryGetFrame -- comparing against the
    // current native version lets us skip the GPU readback + managed alloc
    // when nothing has changed since the previous Draw cycle.
    UInt64 _lastVersion;
    // Reused readback buffer (sized to _width * _height * 4). Avoids an
    // 8 MB+ managed allocation per frame at 1080p.
    array<Byte>^ _bgraBuffer;
};

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
