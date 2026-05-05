using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;
using WrapperFFplay     = MagicStudio.FFmpegPlus.Wrapper.FFplayPlayer;
using WrapperSharedGpu  = MagicStudio.FFmpegPlus.Wrapper.FFplaySharedGpu;

namespace MagicStudio.FFmpegPlus;

/// <summary>
/// Refcounted handle to a D3D11 device that can be shared across multiple
/// <see cref="MagicFFplayPlayer"/> instances.  Pass one to the
/// <see cref="MagicFFplayPlayer.Open(string, MagicFFplaySharedGpu)"/> overload
/// so all players land on the same GPU pipeline — required when a single
/// CanvasDevice needs to render frames from more than one player.
/// </summary>
public sealed class MagicFFplaySharedGpu : IDisposable
{
    internal readonly WrapperSharedGpu _impl = new();
    private bool _disposed;

    public bool IsValid => _impl.IsValid;

    /// <summary>
    /// AddRef'd IDXGIDevice* of the shared D3D11 device.  Caller releases via
    /// <see cref="Marshal.Release(IntPtr)"/> once a CanvasDevice has captured
    /// its own reference.
    /// </summary>
    public IntPtr AcquireDxgiDevice() => _impl.AcquireDxgiDevice();

    public void Dispose()
    {
        if (_disposed) return;
        _impl.Dispose();
        _disposed = true;
    }
}


public enum MagicFFplayState
{
    Opening,
    Playing,
    Paused,
    Closed
}

/// <summary>
/// C# façade over the MagicFFplay C++ port of ffplay.c.
/// Each instance owns its own D3D11 device so it can run alongside a Player
/// instance for side-by-side comparison on independent GPU pipelines.
/// </summary>
public sealed class MagicFFplayPlayer : IDisposable
{
    private readonly WrapperFFplay _impl = new();
    private MagicFFplayState _state = MagicFFplayState.Opening;
    private bool _disposed;

    public bool Open(string path)
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(MagicFFplayPlayer));
        }

        var ret = _impl.Open(path);
        _state = ret ? MagicFFplayState.Paused : MagicFFplayState.Closed;
        return ret;
    }

    /// <summary>
    /// Open <paramref name="path"/> bound to a caller-provided shared GPU.
    /// All players sharing the same <see cref="MagicFFplaySharedGpu"/> use
    /// the same underlying D3D11 device, which lets a single CanvasDevice
    /// render frames from any of them.
    /// </summary>
    public bool Open(string path, MagicFFplaySharedGpu shared)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(shared);

        var ret = _impl.Open(path, shared._impl);
        _state = ret ? MagicFFplayState.Paused : MagicFFplayState.Closed;
        return ret;
    }

    public void Play()
    {
        _impl.Play();
        _state = MagicFFplayState.Playing;
    }

    public void Pause()
    {
        _impl.Pause();
        _state = MagicFFplayState.Paused;
    }

    public void Stop()
    {
        _impl.Stop();
        _state = MagicFFplayState.Closed;
    }   

    public void SetPosition(TimeSpan position, bool retrieveFrame)
    {
        _impl.SetPosition(position, retrieveFrame);
    }

    public TimeSpan GetPosition() => _impl.GetPosition();
    public MagicFFplayState GetCurrentState() => _state;
    public void ResetPlayerPosition(TimeSpan position) => _impl.ResetPlayerPosition(position);
    public void SetTimelineControllerPositionOffset(TimeSpan offset) => _impl.SetTimelineOffset(offset);
    public TimeSpan GetTimelineControllerPositionOffset() => _impl.GetTimelineOffset();
    public void SetVolume(double volume) => _impl.SetVolume(volume);
    public double GetVolume() => _impl.GetVolume();
    public void SetMute(bool mute) => _impl.SetMute(mute);
    public bool GetMute() => _impl.IsMuted();
    public void StepFrame() => _impl.StepFrame();
    public void Seek(double seconds) => _impl.Seek((long)(seconds * 1_000_000));
    public void SetSpeed(double speed) => _impl.SetSpeed(speed);
    public void GetSpeed() => _impl.GetSpeed();
    public void SetPitch(bool pitch) => _impl.SetPitchCorrection(pitch);
    public bool GetPitch() => _impl.GetPitchCorrection();
    public double Speed
    {
        get => _impl.GetSpeed();
        set => _impl.SetSpeed(value);
    }

    /// <summary>
    /// Pitch correction: true = atempo (pitch preserved); false = tape-like.
    /// Forced true when Speed >= 5.0 regardless of this value.
    /// </summary>
    public bool PitchCorrection
    {
        get => _impl.GetPitchCorrection();
        set => _impl.SetPitchCorrection(value);
    }
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
    /// Use this to detect new frames without the cost of a full acquire.
    /// </summary>
    public ulong PeekFrameVersion() => _impl.PeekFrameVersion();

    /// <summary>
    /// Copies the latest frame into <paramref name="destination"/> on the GPU.
    /// Mirrors <c>Windows.Media.Playback.MediaPlayer.CopyFrameToVideoSurface</c>.
    /// The surface must be backed by a BGRA texture on the same D3D11 device
    /// returned by <see cref="AcquireDxgiDevice"/>, with width/height matching
    /// the current frame.  Returns false if no frame is ready or the contract
    /// is violated.
    /// </summary>
    public bool CopyFrameToVideoSurface(IDirect3DSurface destination)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (destination is null) return false;

        // CsWinRT projection: a plain `(IDirect3DDxgiInterfaceAccess)` cast skips
        // QI and throws InvalidCastException.  `.As<T>()` does the real COM QI.
        var access = destination.As<IDirect3DDxgiInterfaceAccess>();
        Guid iid = IID_ID3D11Texture2D;
        int hr = access.GetInterface(in iid, out IntPtr texPtr);
        if (hr < 0 || texPtr == IntPtr.Zero) return false;
        try
        {
            return _impl.CopyFrameToTexture(texPtr);
        }
        finally
        {
            Marshal.Release(texPtr);
        }
    }

    private static readonly Guid IID_ID3D11Texture2D =
        new("6f15aaf2-d208-4e89-9ab4-489535d34f9c");

    [ComImport]
    [Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDirect3DDxgiInterfaceAccess
    {
        [PreserveSig]
        int GetInterface([In] in Guid iid, out IntPtr ppv);
    }

    /// <summary>
    /// Fires on the native refresh thread each time a new BGRA frame is ready.
    /// Mirrors Windows.Media.Playback.MediaPlayer.VideoFrameAvailable —
    /// callers must marshal to the UI thread themselves if needed.
    /// After this fires, TryAcquireCurrentTexture returns the new frame.
    /// </summary>
    public event EventHandler? VideoFrameAvailable
    {
        add    => _impl.VideoFrameAvailable += value;
        remove => _impl.VideoFrameAvailable -= value;
    }

    public int    VideoWidth  => _impl.VideoWidth;
    public int    VideoHeight => _impl.VideoHeight;
    public double Duration    => _impl.Duration;

    /// <summary>Playback speed [0.1, 100.0]. Default 1.0.</summary>

    public void Dispose()
    {
        if (_disposed) return;
        _impl.Dispose();
        _disposed = true;
    }
}
