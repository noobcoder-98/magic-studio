using System;
using MagicStudio.FFmpegPlus.Wrapper;

namespace MagicStudio.FFmpegPlus;

/// <summary>
/// Thin C# façade over the C++/CLI MediaPlayer.
/// Instantiate, call Open() then Play(), then call GetAudioPositionUs() +
/// TryGetFrame() from your Win2D Draw handler.
/// </summary>
public sealed class Player : IDisposable
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

    /// <summary>Audio clock position in microseconds (master clock for A/V sync).</summary>
    public long GetAudioPositionUs() => _impl.GetAudioPositionUs();

    /// <summary>
    /// Retrieves the video frame that should be displayed at <paramref name="audioPtsUs"/>.
    /// Returns false when no frame has been decoded yet.
    /// </summary>
    public bool TryGetFrame(long audioPtsUs,
                            out byte[]? bgraData,
                            out int width,
                            out int height)
    {
        FrameData frame = default!;
        bool got = _impl.TryGetFrame(audioPtsUs, out frame);
        bgraData = got ? frame?.BgraData : null;
        width    = got ? frame?.Width  ?? 0 : 0;
        height   = got ? frame?.Height ?? 0 : 0;
        return got;
    }

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
