#pragma once
using namespace System;
using namespace System::Runtime::InteropServices;

// Forward-declare the native type so the managed header stays include-free.
namespace MagicStudio { namespace Native { class MediaDecoder; } }

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
/// Managed wrapper around the native MediaDecoder.
/// Open() → Play() to start; call TryGetFrame() from your render loop.
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

    /// <summary>Current audio clock position (master clock) in microseconds.</summary>
    Int64  GetAudioPositionUs();

    /// <summary>
    /// Returns the video frame whose presentation time best matches
    /// audioPtsUs. Returns false if no frame is available yet.
    /// </summary>
    bool TryGetFrame(Int64 audioPtsUs, [Out] FrameData^% frame);

    property int    VideoWidth  { int    get(); }
    property int    VideoHeight { int    get(); }
    property double Duration    { double get(); }

private:
    // Stored as void* to avoid exposing native headers to C# consumers;
    // cast back in the .cpp implementation.
    MagicStudio::Native::MediaDecoder* _decoder;
};

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
