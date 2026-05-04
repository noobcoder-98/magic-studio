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

    private FFplayPlayer?      _player;
    private CanvasDevice?      _canvasDevice;
    private CanvasBitmap?      _frameBitmap;
    private ulong              _lastVersion;
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

        _frameBitmap?.Dispose();
        _frameBitmap = null;
        _lastVersion = 0;

        _canvasDevice?.Dispose();
        _canvasDevice = null;

        _player?.Dispose();
        _player = new FFplayPlayer();
        bool ok = _player.Open(path);
        if (ok)
        {
            _player.VideoFrameAvailable += OnPlayerFrameAvailable;
            BindCanvasDeviceToPlayer();

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
        // Marshal to the UI thread; skip if not playing (paused/stopped).
        if (!_playing) return;
        _dispatcherQueue?.TryEnqueue(DispatcherQueuePriority.Normal,
            () => { if (_playing) FFplayCanvas.Invalidate(); });
    }

    // -------------------------------------------------------------------------
    // Win2D canvas callbacks
    // -------------------------------------------------------------------------

    private void FFplayCanvas_CreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
    {
        // Resources are wrapped lazily from the native texture on first draw.
    }

    private void FFplayCanvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        if (_player is null)
            return;

        IntPtr texturePtr = _player.TryAcquireCurrentTexture(out ulong version, out _, out _);
        if (texturePtr != IntPtr.Zero)
        {
            try
            {
                if (version != _lastVersion)
                {
                    var newBitmap = WrapTextureAsBitmap(sender, texturePtr);
                    _frameBitmap?.Dispose();
                    _frameBitmap = newBitmap;
                    _lastVersion = version;
                    UpdateProgress(_player.GetAudioPositionUs());
                }
            }
            finally
            {
                Marshal.Release(texturePtr);
            }
        }

        if (_frameBitmap is not null)
        {
            args.DrawingSession.DrawImage(
                _frameBitmap,
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

        IntPtr dxgiPtr = _player.AcquireDxgiDevice();
        if (dxgiPtr == IntPtr.Zero) return;

        try
        {
            IDirect3DDevice d3dDevice = CreateDirect3DDeviceFromDxgi(dxgiPtr);
            _canvasDevice = CanvasDevice.CreateFromDirect3D11Device(d3dDevice);
            FFplayCanvas.CustomDevice = _canvasDevice;
        }
        finally
        {
            Marshal.Release(dxgiPtr);
        }
    }

    private static CanvasBitmap WrapTextureAsBitmap(ICanvasResourceCreator rc, IntPtr texturePtr)
    {
        Guid iid = IID_IDXGISurface;
        int hr = Marshal.QueryInterface(texturePtr, ref iid, out IntPtr surfacePtr);
        IDirect3DSurface? d3dSurface = null;
        try
        {
            Marshal.ThrowExceptionForHR(hr);
            d3dSurface = CreateDirect3DSurfaceFromDxgi(surfacePtr);
            return CanvasBitmap.CreateFromDirect3D11Surface(rc, d3dSurface);
        }
        finally
        {
            (d3dSurface as IDisposable)?.Dispose();
            Marshal.Release(surfacePtr);
        }
    }

    private static IDirect3DDevice CreateDirect3DDeviceFromDxgi(IntPtr dxgiDevice)
    {
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, out IntPtr abi);
        try { return MarshalInspectable<IDirect3DDevice>.FromAbi(abi); }
        finally { Marshal.Release(abi); }
    }

    private static IDirect3DSurface CreateDirect3DSurfaceFromDxgi(IntPtr dxgiSurface)
    {
        CreateDirect3D11SurfaceFromDXGISurface(dxgiSurface, out IntPtr abi);
        try { return MarshalInspectable<IDirect3DSurface>.FromAbi(abi); }
        finally { Marshal.Release(abi); }
    }

    [DllImport("d3d11.dll", ExactSpelling = true, PreserveSig = false)]
    private static extern void CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice,
                                                                    out IntPtr graphicsDevice);

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

        _frameBitmap?.Dispose();
        _frameBitmap = null;

        _canvasDevice?.Dispose();
        _canvasDevice = null;
    }
}
