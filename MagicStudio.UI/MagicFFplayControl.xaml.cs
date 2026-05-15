using MagicStudio.FFmpegPlus;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.UI;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using System.Runtime.InteropServices;
using Windows.Foundation;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MagicStudio.UI;

/// <summary>
/// Video player control backed by MagicFFplay — the C++ port of ffplay.c.
/// Uses its own Win2D CanvasControl (FFplayCanvas) and its own D3D11 device,
/// so it runs independently from MediaPlayerControl for side-by-side comparison.
/// Canvas invalidation is driven by FFplayPlayer.VideoFrameAvailable (push),
/// eliminating the polling timer used previously.
/// </summary>
public sealed partial class MagicFFplayControl : UserControl, IDisposable
{
    private static readonly Guid IID_IDXGISurface =
        new("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");

    private MagicFFplayPlayer?      _player;
    private CanvasDevice?      _canvasDevice;
    private CanvasDevice?      _playerCanvasDevice;
    private CanvasRenderTarget? _destBitmap;
    private IDirect3DSurface?  _destSurface;
    private int                _destWidth;
    private int                _destHeight;
    private bool               _playing;
    private bool               _disposed;
    private DispatcherQueue?   _dispatcherQueue;

    private bool   _suppressSlider;
    private bool   _sliderDragging;
    private double _seekTargetSeconds;

    public MagicFFplayControl()
    {
        InitializeComponent();
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
        Unloaded += OnUnloaded;
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    public bool Open(string path)
    {
        // Unsubscribe from the old player before disposing it.
        if (_player is not null)
            _player.VideoFrameAvailable -= OnPlayerFrameAvailable;

        DisposeDestSurface();

        _canvasDevice?.Dispose();
        _canvasDevice = null;

        _player?.Dispose();
        _player = new MagicFFplayPlayer();
        bool ok = _player.Open(path);
        if (ok)
        {
            _player.VideoFrameAvailable += OnPlayerFrameAvailable;
            BindCanvasDeviceToPlayer();
            _playerCanvasDevice = _canvasDevice;

            double duration = _player.Duration;
            _suppressSlider = true;
            FfProgressSlider.Maximum = duration > 0 ? duration : 1;
            FfProgressSlider.Value   = 0;
            _suppressSlider = false;
            UpdateTimeLabel(0, duration);

            // Reset speed to 1.0x and sync UI
            _player.Speed = 10;
            FfSpeedNumberBox.Value = 10;
            UpdateSpeedUI(10);
        }
        return ok;
    }

    public void Play()
    {
        if (_player is null) return;
        _player.Play();
        _playing = true;
        FfPlayPauseButton.Content = ""; // Pause glyph
        FFplayCanvas.Invalidate();
    }

    public void Pause()
    {
        _player?.Pause();
        _playing = false;
        FfPlayPauseButton.Content = ""; // Play glyph
        FFplayCanvas.Invalidate();
    }

    // -------------------------------------------------------------------------
    // Frame-available handler (fires on the native refresh thread)
    // -------------------------------------------------------------------------

    private void OnPlayerFrameAvailable(object? sender, EventArgs e)
    {
        // Fires on the native refresh thread.  Marshal to the UI thread, copy
        // into our destination surface (GPU-side), then invalidate to draw it.
        if (!_playing) return;
        _dispatcherQueue?.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            if (!_playing || _player is null) return;
            EnsureDestSurface();
            if (_destSurface is null) return;
            if (_player.CopyFrameToVideoSurface(_destSurface))
            {
                UpdateProgress(_player.GetAudioPositionUs());
                FFplayCanvas.Invalidate();
            }
        });
    }

    // -------------------------------------------------------------------------
    // Win2D canvas callbacks
    // -------------------------------------------------------------------------

    private void FFplayCanvas_CreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
    {
        // Destination surface is created lazily once the first frame arrives,
        // since we need the player's video size which isn't known before Open.
    }

    private void FFplayCanvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        if (_destBitmap is not null)
        {
            args.DrawingSession.DrawImage(
                _destBitmap,
                new Rect(0, 0, sender.ActualWidth, sender.ActualHeight));
        }
    }

    // -------------------------------------------------------------------------
    // Controls
    // -------------------------------------------------------------------------

    private void FfPlayPauseButton_Click(object sender, RoutedEventArgs e)
    {
        if (_playing) Pause();
        else          Play();
    }

    private void FfProgressSlider_PointerPressed(object sender,
        Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        _sliderDragging = true;
    }

    private void FfProgressSlider_PointerCaptureLost(object sender,
        Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (!_sliderDragging) return;
        _sliderDragging = false;
        PerformSeek(_seekTargetSeconds);
    }

    private void FfProgressSlider_ValueChanged(object sender,
        Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_suppressSlider || _player is null) return;
        _seekTargetSeconds = e.NewValue;
        if (!_sliderDragging)
            PerformSeek(_seekTargetSeconds);
    }

    private void PerformSeek(double seconds)
    {
        if (_player is null) return;
        _player.Seek(seconds);
        _playing = true;
        FfPlayPauseButton.Content = ""; // Pause glyph
        FFplayCanvas.Invalidate();
    }

    private void FfSetSpeedButton_Click(object sender, RoutedEventArgs e)
    {
        if (_player is null) return;
        double speed = Math.Clamp(FfSpeedNumberBox.Value, 0.1, 100.0);
        _player.Speed = speed;
        UpdateSpeedUI(speed);
    }

    private void FfPitchButton_Click(object sender, RoutedEventArgs e)
    {
        if (_player is null) return;
        _player.PitchCorrection = FfPitchButton.IsChecked == true;
    }

    private void UpdateSpeedUI(double speed)
    {
        bool forced = speed >= 5.0;
        FfPitchForcedText.Visibility = forced
            ? Microsoft.UI.Xaml.Visibility.Visible
            : Microsoft.UI.Xaml.Visibility.Collapsed;
        FfPitchButton.IsEnabled = !forced;
        if (forced)
            FfPitchButton.IsChecked = true;
    }

    // -------------------------------------------------------------------------
    // Win2D <-> native D3D11 device binding
    // -------------------------------------------------------------------------

    private void BindCanvasDeviceToPlayer()
    {
        if (_player is null) return;

        _canvasDevice = new() { LowPriority = true };
        if (_canvasDevice is not null)
            FFplayCanvas.CustomDevice = _canvasDevice;
    }

    // Lazily (re)create the destination CanvasRenderTarget + cached IDirect3DSurface
    // wrapper.  Both views point at the same underlying ID3D11Texture2D — Win2D
    // owns it and draws from it; the IDirect3DSurface wrapper is what we hand
    // to FFplayPlayer.CopyFrameToVideoSurface every frame.
    private void EnsureDestSurface()
    {
        if (_canvasDevice is null || _player is null) return;
        int w = _player.VideoWidth;
        int h = _player.VideoHeight;
        if (w <= 0 || h <= 0) return;
        if (_destSurface is not null && _destWidth == w && _destHeight == h) return;

        DisposeDestSurface();

        var rt = new CanvasRenderTarget(_playerCanvasDevice, w, h, 96f,
            DirectXPixelFormat.B8G8R8A8UIntNormalized,
            CanvasAlphaMode.Premultiplied);

        // CanvasRenderTarget doesn't implement IDirect3DSurface directly; QI
        // its IDXGISurface and wrap that as IDirect3DSurface.
        var access = rt.As<IDirect3DDxgiInterfaceAccess>();
        Guid iid = IID_IDXGISurface;
        int hr = access.GetInterface(in iid, out IntPtr surfacePtr);
        try
        {
            Marshal.ThrowExceptionForHR(hr);
            _destSurface = CreateDirect3DSurfaceFromDxgi(surfacePtr);
        }
        finally
        {
            Marshal.Release(surfacePtr);
        }

        _destBitmap = rt;
        _destWidth  = w;
        _destHeight = h;
    }

    private void DisposeDestSurface()
    {
        (_destSurface as IDisposable)?.Dispose();
        _destSurface = null;
        _destBitmap?.Dispose();
        _destBitmap = null;
        _destWidth  = 0;
        _destHeight = 0;
    }

    [ComImport]
    [Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDirect3DDxgiInterfaceAccess
    {
        [PreserveSig]
        int GetInterface([In] in Guid iid, out IntPtr ppv);
    }

    private static IDirect3DSurface CreateDirect3DSurfaceFromDxgi(IntPtr dxgiSurface)
    {
        CreateDirect3D11SurfaceFromDXGISurface(dxgiSurface, out IntPtr abi);
        try { return MarshalInspectable<IDirect3DSurface>.FromAbi(abi); }
        finally { Marshal.Release(abi); }
    }

    [DllImport("d3d11.dll", ExactSpelling = true, PreserveSig = false)]
    private static extern void CreateDirect3D11SurfaceFromDXGISurface(IntPtr dxgiSurface,
                                                                      out IntPtr graphicsSurface);

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private void UpdateProgress(long ptsUs)
    {
        double seconds  = ptsUs / 1_000_000.0;
        double duration = _player?.Duration ?? 0;

        _suppressSlider = true;
        FfProgressSlider.Value = Math.Clamp(seconds, 0, FfProgressSlider.Maximum);
        _suppressSlider = false;

        UpdateTimeLabel(seconds, duration);
    }

    private static string FormatTime(double seconds)
    {
        var ts = TimeSpan.FromSeconds(seconds);
        return ts.Hours > 0
            ? $"{ts.Hours}:{ts.Minutes:D2}:{ts.Seconds:D2}"
            : $"{ts.Minutes}:{ts.Seconds:D2}";
    }

    private void UpdateTimeLabel(double current, double total)
    {
        FfTimeText.Text = $"{FormatTime(current)} / {FormatTime(total)}";
    }

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    private void OnUnloaded(object sender, RoutedEventArgs e) => Dispose();

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        if (_player is not null)
        {
            _player.VideoFrameAvailable -= OnPlayerFrameAvailable;
            _player.Dispose();
            _player = null;
        }

        DisposeDestSurface();

        _canvasDevice?.Dispose();
        _canvasDevice = null;
    }
}
