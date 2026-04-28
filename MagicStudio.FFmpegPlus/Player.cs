using System;
using MagicStudio.FFmpegPlus.Wrapper;

namespace MagicStudio.FFmpegPlus;

/// <summary>
/// Thin C# façade over the C++/CLI MediaPlayer. The host instantiates one,
/// calls Open(), wires Win2D to AcquireDxgiDevice() once, then on each Draw
/// pulls the current frame's ID3D11Texture2D via TryAcquireCurrentTexture()
/// and wraps it as a CanvasBitmap.
/// </summary>
public sealed partial class Player : IDisposable
{
    private readonly MediaPlayer _impl = new();
    private bool _disposed;

    public bool Open(string path)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _impl.Open(path);
    }

    public void Play()  => _impl.Play();
    public void Pause() => _impl.Pause();
    public void Stop()  => _impl.Stop();
    public void Seek(double seconds) => _impl.Seek((long)(seconds * 1_000_000));

    /// <summary>Audio clock position in microseconds (master clock for A/V sync).</summary>
    public long GetAudioPositionUs() => _impl.GetAudioPositionUs();

    /// <summary>
    /// AddRef'd IDXGIDevice* for the player's shared D3D11 device, or
    /// IntPtr.Zero if unavailable. Caller releases via Marshal.Release once
    /// CanvasDevice has captured its own reference.
    /// </summary>
    public IntPtr AcquireDxgiDevice() => _impl.AcquireDxgiDevice();

    /// <summary>
    /// AddRef'd ID3D11Texture2D* for the most recently presented frame, or
    /// IntPtr.Zero if no frame is ready. Caller releases the pointer once it
    /// has been wrapped (e.g. into a CanvasBitmap). The version output lets
    /// the host skip rewrapping when the same frame is shown twice.
    /// </summary>
    public IntPtr TryAcquireCurrentTexture(out ulong version, out int width, out int height)
        => _impl.TryAcquireCurrentTexture(out version, out width, out height);

    public int    VideoWidth  => _impl.VideoWidth;
    public int    VideoHeight => _impl.VideoHeight;
    public double Duration    => _impl.Duration;

    public void Dispose()
    {
        if (_disposed) return;
        _impl.Dispose();
        _disposed = true;
    }
}
