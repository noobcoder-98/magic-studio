#pragma once
using namespace System;
using namespace System::Runtime::InteropServices;

// Forward-declare the opaque native handle so this header stays free of
// d3d11/dxgi includes.
struct MagicPlayerHandle;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

/// <summary>
/// Managed wrapper around the native MagicFFmpegPlayer C API.
/// Open() then Play() to start; the host should bind Win2D to the player's
/// D3D11 device via AcquireDxgiDevice(), then in its Draw handler wrap the
/// pointer returned by TryAcquireCurrentTexture() as a CanvasBitmap. No
/// pixels ever cross the GPU/CPU boundary.
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
    /// Returns the AddRef'd shared IDXGIDevice* (as IntPtr) that the native
    /// decoder + video processor write to. The caller passes this through
    /// CreateDirect3D11DeviceFromDXGIDevice -> CanvasDevice.CreateFromDirect3D11Device
    /// and must call Marshal.Release on the returned pointer once the
    /// CanvasDevice has captured its own reference. Returns IntPtr.Zero if the
    /// GPU pipeline has not been initialised yet.
    /// </summary>
    IntPtr AcquireDxgiDevice();

    /// <summary>
    /// Returns the AddRef'd ID3D11Texture2D* (as IntPtr) for the most recently
    /// presented BGRA frame, or IntPtr.Zero if no frame is ready. On success
    /// <paramref name="version"/> receives a monotonic id so callers can skip
    /// rewrapping when the same frame is shown twice; <paramref name="width"/>
    /// and <paramref name="height"/> are filled from the texture description.
    /// The caller must Marshal.Release the pointer once it has been wrapped
    /// (typically via CreateDirect3D11SurfaceFromDXGISurface +
    /// CanvasBitmap.CreateFromDirect3D11Surface).
    /// </summary>
    IntPtr TryAcquireCurrentTexture([Out] UInt64% version,
                                    [Out] int%    width,
                                    [Out] int%    height);

    property int    VideoWidth  { int    get(); }
    property int    VideoHeight { int    get(); }
    property double Duration    { double get(); }

private:
    // Stored as void* to keep MagicFFmpegPlayer.h out of consumers; cast back
    // in the .cpp implementation.
    void* _handle;
};

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
