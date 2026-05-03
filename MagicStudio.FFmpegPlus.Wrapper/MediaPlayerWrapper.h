#pragma once
using namespace System;
using namespace System::Runtime::InteropServices;

struct MagicPlayerHandle;
struct MagicFFplayHandle;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

/// <summary>
/// Managed wrapper around MagicFFmpegPlayer (the existing, stripped ffplay
/// variant with a process-singleton D3D11 device).
/// </summary>
public ref class MediaPlayer sealed {
public:
    MediaPlayer();
    ~MediaPlayer();
    !MediaPlayer();

    bool   Open(String^ path);
    void   Play();
    void   Pause();
    void   Stop();
    void   Seek(Int64 positionUs);

    Int64  GetAudioPositionUs();
    IntPtr AcquireDxgiDevice();
    IntPtr TryAcquireCurrentTexture([Out] UInt64% version,
                                    [Out] int%    width,
                                    [Out] int%    height);

    property int    VideoWidth  { int    get(); }
    property int    VideoHeight { int    get(); }
    property double Duration    { double get(); }

private:
    void* _handle;
};

/// <summary>
/// Managed wrapper around MagicFFplay (the C++ port of ffplay.c with its own
/// per-instance D3D11 device, for side-by-side comparison with MediaPlayer).
/// </summary>
public ref class FFplayPlayer sealed {
public:
    FFplayPlayer();
    ~FFplayPlayer();
    !FFplayPlayer();

    bool   Open(String^ path);
    void   Play();
    void   Pause();
    void   Stop();
    void   Seek(Int64 positionUs);
    void   StepFrame();

    Int64  GetAudioPositionUs();

    /// <summary>
    /// Returns the AddRef'd IDXGIDevice* for this player's own D3D11 device.
    /// The caller must Marshal.Release once the CanvasDevice has captured its
    /// own reference.
    /// </summary>
    IntPtr AcquireDxgiDevice();

    /// <summary>
    /// Returns the AddRef'd ID3D11Texture2D* for the most recently presented
    /// BGRA frame, or IntPtr.Zero if no frame is ready.  The caller must
    /// Marshal.Release after wrapping it as a CanvasBitmap.
    /// </summary>
    IntPtr TryAcquireCurrentTexture([Out] UInt64% version,
                                    [Out] int%    width,
                                    [Out] int%    height);

    /// <summary>
    /// Returns the current frame version without acquiring a texture reference.
    /// Cheap atomic load — safe to call from a UI-thread polling timer to
    /// detect when a new frame is ready.  Returns 0 if no frame yet.
    /// </summary>
    UInt64 PeekFrameVersion();

    property int    VideoWidth  { int    get(); }
    property int    VideoHeight { int    get(); }
    property double Duration    { double get(); }

    // Playback speed [0.1, 100.0].
    void   SetSpeed(double speed);
    double GetSpeed();

    // Pitch correction: true = atempo (preserve pitch); false = tape-like.
    // Forced ON at speed >= 5.0.
    void SetPitchCorrection(bool enabled);
    bool GetPitchCorrection();

private:
    void* _handle;
};

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
