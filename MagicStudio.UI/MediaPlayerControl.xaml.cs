using MagicStudio.FFmpegPlus;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.UI;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using System.Runtime.InteropServices;
using Windows.Foundation;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MagicStudio.UI;

public sealed partial class MediaPlayerControl : UserControl, IDisposable
{
    // IID_IDXGISurface — used to QI the AddRef'd ID3D11Texture2D into a
    // surface that CreateDirect3D11SurfaceFromDXGISurface can wrap.
    private static readonly Guid IID_IDXGISurface =
        new("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");

    private Player?       _player;
    private CanvasDevice? _canvasDevice;
    private CanvasBitmap? _frameBitmap;
    private ulong         _lastVersion;
    private bool          _playing;
    private bool          _disposed;

    // Prevents slider ValueChanged from re-triggering during programmatic updates.
    private bool   _suppressSlider;
    private bool   _sliderDragging;
    private double _seekTargetSeconds;

    public MediaPlayerControl()
    {
        InitializeComponent();
        Unloaded += OnUnloaded;
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    public bool Open(string path)
    {
        // Drop the previous file's cached frame; the new stream will publish
        // its own textures with fresh version numbers.
        _frameBitmap?.Dispose();
        _frameBitmap = null;
        _lastVersion = 0;

        _player?.Dispose();
        _player = new Player();
        bool ok = _player.Open(path);
        if (ok)
        {
            EnsureCanvasDeviceBoundToPlayer();

            double duration = _player.Duration;
            _suppressSlider = true;
            ProgressSlider.Maximum = duration > 0 ? duration : 1;
            ProgressSlider.Value   = 0;
            _suppressSlider = false;
            UpdateTimeLabel(0, duration);
        }
        return ok;
    }

    public void Play()
    {
        if (_player is null) return;
        _player.Play();
        _playing = true;
        PlayPauseButton.Content = ""; // Pause glyph
        VideoCanvas.Invalidate();
    }

    public void Pause()
    {
        _player?.Pause();
        _playing = false;
        PlayPauseButton.Content = ""; // Play glyph
        VideoCanvas.Invalidate();
    }

    // -------------------------------------------------------------------------
    // Win2D canvas callbacks
    // -------------------------------------------------------------------------

    private void VideoCanvas_CreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
    {
        // Nothing to pre-allocate; the frame bitmap is wrapped lazily around
        // whatever ID3D11Texture2D the native player publishes next.
    }

    private void VideoCanvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
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
                    // New frame -- wrap its texture as a CanvasBitmap. This is
                    // pure ref-counting; no pixel data leaves the GPU.
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

        if (_playing)
            sender.Invalidate();
    }

    // -------------------------------------------------------------------------
    // Controls
    // -------------------------------------------------------------------------

    private void PlayPauseButton_Click(object sender, RoutedEventArgs e)
    {
        if (_playing) Pause();
        else          Play();
    }

    private void ProgressSlider_PointerPressed(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        _sliderDragging = true;
    }

    private void ProgressSlider_PointerCaptureLost(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (!_sliderDragging) return;
        _sliderDragging = false;
        PerformSeek(_seekTargetSeconds);
    }

    private void ProgressSlider_ValueChanged(object sender,
        Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_suppressSlider || _player is null) return;
        _seekTargetSeconds = e.NewValue;
        if (!_sliderDragging)
            PerformSeek(_seekTargetSeconds); // keyboard navigation: seek immediately
    }

    private void PerformSeek(double seconds)
    {
        if (_player is null) return;
        _player.Seek(seconds);
        _playing = true;
        PlayPauseButton.Content = ""; // Pause glyph
        VideoCanvas.Invalidate();
    }

    // -------------------------------------------------------------------------
    // Win2D <-> native D3D11 device + texture binding
    // -------------------------------------------------------------------------

    // Build a CanvasDevice that wraps the *same* D3D11 device the FFmpeg
    // decoder + video processor write into, so CanvasBitmaps wrapping the
    // player's BGRA textures can be drawn without a CPU copy. The native
    // pipeline holds a process-singleton device, so we only ever do this
    // once; subsequent Open() calls reuse the existing CanvasDevice.
    private void EnsureCanvasDeviceBoundToPlayer()
    {
        if (_canvasDevice is not null || _player is null) return;

        IntPtr dxgiPtr = _player.AcquireDxgiDevice();
        if (dxgiPtr == IntPtr.Zero) return;

        try
        {
            IDirect3DDevice d3dDevice = CreateDirect3DDeviceFromDxgi(dxgiPtr);
            _canvasDevice = CanvasDevice.CreateFromDirect3D11Device(d3dDevice);
            VideoCanvas.CustomDevice = _canvasDevice;
        }
        finally
        {
            Marshal.Release(dxgiPtr);
        }
    }

    // QI(ID3D11Texture2D -> IDXGISurface), wrap the surface as IDirect3DSurface,
    // then hand it to Win2D.
    //
    // Lifetime note: CanvasBitmap.CreateFromDirect3D11Surface QIs its own
    // independent ref on the underlying IDXGISurface (and through that the
    // ID3D11Texture2D). The IDirect3DSurface RCW we built via MarshalInspectable
    // *also* owns a ref through its IInspectable wrapper. If we leave the RCW
    // for the GC to finalize, that ref keeps an 8MB BGRA texture alive per
    // frame -- at 30+ fps the finalizer queue can't keep up and managed memory
    // balloons until a Gen2 collection runs (visible as a render-loop stutter).
    // Casting to IDisposable invokes IClosable::Close on the WinRT wrapper,
    // which releases its internal IDXGISurface ref synchronously and lets the
    // texture die the moment the next CanvasBitmap replaces this one.
    private static CanvasBitmap WrapTextureAsBitmap(ICanvasResourceCreator resourceCreator, IntPtr texturePtr)
    {
        Guid iid = IID_IDXGISurface;
        int hr = Marshal.QueryInterface(texturePtr, ref iid, out IntPtr surfacePtr);
        Marshal.ThrowExceptionForHR(hr);
        IDirect3DSurface? d3dSurface = null;
        try
        {
            d3dSurface = CreateDirect3DSurfaceFromDxgi(surfacePtr);
            return CanvasBitmap.CreateFromDirect3D11Surface(resourceCreator, d3dSurface);
        }
        finally
        {
            (d3dSurface as IDisposable)?.Dispose();
            Marshal.Release(surfacePtr);
        }
    }

    // The two d3d11.dll exports return AddRef'd IInspectable* via PreserveSig=false,
    // so the HRESULT is converted into an exception automatically. We then build a
    // managed RCW with MarshalInspectable<T>.FromAbi (which adds its own ref via
    // ComWrappers) and Release the original native ref to keep the count balanced.
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
        ProgressSlider.Value = Math.Clamp(seconds, 0, ProgressSlider.Maximum);
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
        TimeText.Text = $"{FormatTime(current)} / {FormatTime(total)}";
    }

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    private void OnUnloaded(object sender, RoutedEventArgs e) => Dispose();

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _frameBitmap?.Dispose();
        _frameBitmap = null;

        // Order matters: drop the player first so its singleton D3D11 device
        // stops being referenced before we tear down the CanvasDevice that
        // also wraps it.
        _player?.Dispose();
        _player = null;

        _canvasDevice?.Dispose();
        _canvasDevice = null;
    }
}
