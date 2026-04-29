using System;
using WrapperFFplay = MagicStudio.FFmpegPlus.Wrapper.FFplayPlayer;

namespace MagicStudio.FFmpegPlus;

/// <summary>
/// C# façade over the MagicFFplay C++ port of ffplay.c.
/// Each instance owns its own D3D11 device so it can run alongside a Player
/// instance for side-by-side comparison on independent GPU pipelines.
/// </summary>
public sealed class FFplayPlayer : IDisposable
{
    private readonly WrapperFFplay _impl = new();
    private bool _disposed;

    public bool Open(string path)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _impl.Open(path);
    }

    public void Play()      => _impl.Play();
    public void Pause()     => _impl.Pause();
    public void Stop()      => _impl.Stop();
    public void StepFrame() => _impl.StepFrame();
    public void Seek(double seconds) => _impl.Seek((long)(seconds * 1_000_000));

    public long GetAudioPositionUs() => _impl.GetAudioPositionUs();

    /// <summary>
    /// AddRef'd IDXGIDevice* for this player's own D3D11 device.
    /// Caller releases via Marshal.Release once CanvasDevice has captured its ref.
    /// </summary>
    public IntPtr AcquireDxgiDevice() => _impl.AcquireDxgiDevice();

    /// <summary>
    /// AddRef'd ID3D11Texture2D* for the most recently presented BGRA frame,
    /// or IntPtr.Zero if no frame is ready.  Caller releases after wrapping.
    /// </summary>
    public IntPtr TryAcquireCurrentTexture(out ulong version, out int width, out int height)
        => _impl.TryAcquireCurrentTexture(out version, out width, out height);

    /// <summary>
    /// Cheap atomic load of the current frame version, with no texture AddRef.
    /// Use this to detect new frames from a UI-thread polling timer without
    /// the cost of wrapping a CanvasBitmap on every tick.
    /// </summary>
    public ulong PeekFrameVersion() => _impl.PeekFrameVersion();

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
