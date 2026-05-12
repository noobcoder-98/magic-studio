using MagicStudio.FFmpegPlus;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.UI;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using System.Globalization;
using System.Runtime.InteropServices;
using Windows.Foundation;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MagicStudio.UI;

/// <summary>
/// One CanvasControl renders four independent MagicFFplayPlayer instances.
/// All four players share a single D3D11 device (MagicFFplaySharedGpu) so
/// the canvas's CanvasDevice can copy frames from any of them.
/// Each slot has its own play/pause, progress slider, time label, and speed.
/// </summary>
public sealed partial class QuadFFplayControl : UserControl, IDisposable
{
    private const int SlotCount = 4;

    private static readonly Guid IID_IDXGISurface =
        new("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");

    private MagicFFplaySharedGpu? _sharedGpu;
    private CanvasDevice?         _canvasDevice;
    private DispatcherQueue?      _dispatcherQueue;
    private DispatcherQueueTimer? _progressTimer;
    private bool                  _disposed;

    private sealed class Slot : IDisposable
    {
        public int                 Index;
        public MagicFFplayPlayer?  Player;
        public CanvasRenderTarget? Bitmap;
        public IDirect3DSurface?   Surface;
        public int                 Width;
        public int                 Height;

        public bool   Playing;
        public bool   SuppressSlider;
        public bool   SliderDragging;
        public double SeekTargetSeconds;
        public double LastKnownSeconds;
        public bool   ResetPositionTracking;

        public Button    PlayPauseButton = null!;
        public Slider    ProgressSlider  = null!;
        public TextBlock TimeText        = null!;
        public NumberBox SpeedBox        = null!;

        public void Dispose()
        {
            if (Player is not null)
                Player.VideoFrameAvailable -= FrameHandler;
            FrameHandler = null!;

            (Surface as IDisposable)?.Dispose();
            Surface = null;
            Bitmap?.Dispose();
            Bitmap = null;
            Player?.Dispose();
            Player = null;
            Width = Height = 0;
            Playing = false;
        }

        public EventHandler FrameHandler = null!;
    }

    private readonly Slot[] _slots = new Slot[SlotCount];

    public QuadFFplayControl()
    {
        InitializeComponent();
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();

        _slots[0] = new Slot { Index = 0, PlayPauseButton = PlayPauseButton0, ProgressSlider = ProgressSlider0, TimeText = TimeText0, SpeedBox = SpeedBox0 };
        _slots[1] = new Slot { Index = 1, PlayPauseButton = PlayPauseButton1, ProgressSlider = ProgressSlider1, TimeText = TimeText1, SpeedBox = SpeedBox1 };
        _slots[2] = new Slot { Index = 2, PlayPauseButton = PlayPauseButton2, ProgressSlider = ProgressSlider2, TimeText = TimeText2, SpeedBox = SpeedBox2 };
        _slots[3] = new Slot { Index = 3, PlayPauseButton = PlayPauseButton3, ProgressSlider = ProgressSlider3, TimeText = TimeText3, SpeedBox = SpeedBox3 };

        // Progress polling — independent of frame events, so the slider updates
        // at a steady cadence regardless of playback speed or framerate, and a
        // brief stall in the master clock (e.g. while the atempo filter is
        // rebuilt after a speed change) doesn't jerk the slider back to zero.
        if (_dispatcherQueue is not null)
        {
            _progressTimer = _dispatcherQueue.CreateTimer();
            _progressTimer.Interval = TimeSpan.FromMilliseconds(200);
            _progressTimer.Tick += (_, _) => PollAllSlotsProgress();
            _progressTimer.Start();
        }

        Unloaded += (_, _) => Dispose();
    }

    // -------------------------------------------------------------------------
    // Public API — host wires the file picker, then calls OpenSlot.
    // -------------------------------------------------------------------------

    public bool OpenSlot(int index, string path)
    {
        if ((uint)index >= SlotCount) return false;
        return OpenSlotInternal(_slots[index], path);
    }

    // -------------------------------------------------------------------------
    // Slot lifecycle
    // -------------------------------------------------------------------------

    private bool OpenSlotInternal(Slot slot, string path)
    {
        EnsureSharedGpuAndCanvasDevice();
        if (_sharedGpu is null || _canvasDevice is null) return false;

        // Tear down the previous player in this slot — a new file may have a
        // different size.
        slot.Dispose();

        var player = new MagicFFplayPlayer();
        if (!player.Open(path, _sharedGpu)) { player.Dispose(); return false; }

        slot.Player = player;
        slot.FrameHandler = (_, _) => OnFrameAvailable(slot);
        slot.Player.VideoFrameAvailable += slot.FrameHandler;

        double duration = player.Duration;
        slot.SuppressSlider = true;
        slot.ProgressSlider.Maximum = duration > 0 ? duration : 1;
        slot.ProgressSlider.Value   = 0;
        slot.SuppressSlider = false;
        UpdateTimeLabel(slot, 0, duration);

        slot.Player.Speed     = 1.0;
        slot.SpeedBox.Value   = 1.0;

        // Native side starts un-paused immediately after open (is->paused = 0
        // by default), so mark Playing=true here.  Otherwise the progress
        // poller skips this slot until the user clicks Play, even though the
        // video is actually advancing.
        slot.Playing               = true;
        slot.LastKnownSeconds      = 0;
        slot.ResetPositionTracking = true;
        slot.PlayPauseButton.Content = ""; // Pause glyph (native already playing)
        return true;
    }

    private void OnFrameAvailable(Slot slot)
    {
        // Frame events drive only canvas invalidation; slider and time-label
        // updates are handled by the progress timer so their cadence is
        // independent of framerate and playback speed.  A brief stall in the
        // master clock during atempo rebuild (after a speed change) no longer
        // surfaces as a slider jump back to zero.
        _dispatcherQueue?.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            if (slot.Player is null || _canvasDevice is null) return;
            EnsureSlotSurface(slot);
            if (slot.Surface is null) return;
            if (slot.Player.CopyFrameToVideoSurface(slot.Surface))
                QuadCanvas.Invalidate();
        });
    }

    private void PollAllSlotsProgress()
    {
        foreach (var slot in _slots)
        {
            if (slot.Player is null || !slot.Playing || slot.SliderDragging) continue;
            UpdateProgress(slot, slot.Player.GetAudioPositionUs());
        }
    }

    private void EnsureSlotSurface(Slot slot)
    {
        if (_canvasDevice is null || slot.Player is null) return;
        int w = slot.Player.VideoWidth;
        int h = slot.Player.VideoHeight;
        if (w <= 0 || h <= 0) return;
        if (slot.Surface is not null && slot.Width == w && slot.Height == h) return;

        (slot.Surface as IDisposable)?.Dispose();
        slot.Bitmap?.Dispose();

        var rt = new CanvasRenderTarget(_canvasDevice, w, h, 96f,
            DirectXPixelFormat.B8G8R8A8UIntNormalized,
            CanvasAlphaMode.Premultiplied);

        var access = rt.As<IDirect3DDxgiInterfaceAccess>();
        Guid iid = IID_IDXGISurface;
        int hr = access.GetInterface(in iid, out IntPtr surfacePtr);
        try
        {
            Marshal.ThrowExceptionForHR(hr);
            slot.Surface = CreateDirect3DSurfaceFromDxgi(surfacePtr);
        }
        finally
        {
            Marshal.Release(surfacePtr);
        }

        slot.Bitmap = rt;
        slot.Width  = w;
        slot.Height = h;
    }

    // -------------------------------------------------------------------------
    // Shared GPU + CanvasDevice binding
    // -------------------------------------------------------------------------

    private void EnsureSharedGpuAndCanvasDevice()
    {
        if (_sharedGpu is not null && _canvasDevice is not null) return;

        _sharedGpu ??= new MagicFFplaySharedGpu();
        if (!_sharedGpu.IsValid) return;

        if (_canvasDevice is null)
        {
            _canvasDevice = _sharedGpu.CreateCanvasDevice();
            if (_canvasDevice is not null)
                QuadCanvas.CustomDevice = _canvasDevice;
        }
    }

    // -------------------------------------------------------------------------
    // Canvas callbacks — 2x2 layout
    // -------------------------------------------------------------------------

    private void QuadCanvas_CreateResources(CanvasControl sender,
                                            CanvasCreateResourcesEventArgs args) { }

    private void QuadCanvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        double halfW = sender.ActualWidth  / 2.0;
        double halfH = sender.ActualHeight / 2.0;

        DrawSlot(args, _slots[0], new Rect(0,     0,     halfW, halfH));
        DrawSlot(args, _slots[1], new Rect(halfW, 0,     halfW, halfH));
        DrawSlot(args, _slots[2], new Rect(0,     halfH, halfW, halfH));
        DrawSlot(args, _slots[3], new Rect(halfW, halfH, halfW, halfH));
    }

    private static void DrawSlot(CanvasDrawEventArgs args, Slot slot, Rect rect)
    {
        if (slot.Bitmap is not null)
            args.DrawingSession.DrawImage(slot.Bitmap, rect);
    }

    // -------------------------------------------------------------------------
    // Per-slot control handlers — Tag carries the slot index.
    // -------------------------------------------------------------------------

    private void OpenSlot_Click(object sender, RoutedEventArgs e)
    {
        int index = TagToIndex(sender);
        OpenRequested?.Invoke(this, new SlotOpenRequestedEventArgs(index));
    }

    private void PlayPause_Click(object sender, RoutedEventArgs e)
    {
        int index = TagToIndex(sender);
        var slot = _slots[index];
        if (slot.Player is null) return;

        if (slot.Playing) PauseSlot(slot);
        else              PlaySlot(slot);
    }

    private void PlaySlot(Slot slot)
    {
        if (slot.Player is null) return;
        slot.Player.Play();
        slot.Playing = true;
        slot.PlayPauseButton.Content = ""; // Pause glyph
        QuadCanvas.Invalidate();
    }

    private void PauseSlot(Slot slot)
    {
        slot.Player?.Pause();
        slot.Playing = false;
        slot.PlayPauseButton.Content = ""; // Play glyph
        QuadCanvas.Invalidate();
    }

    private void ProgressSlider_PointerPressed(object sender,
        Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        var slot = _slots[TagToIndex(sender)];
        slot.SliderDragging = true;
    }

    private void ProgressSlider_PointerCaptureLost(object sender,
        Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        var slot = _slots[TagToIndex(sender)];
        if (!slot.SliderDragging) return;
        slot.SliderDragging = false;
        PerformSeek(slot, slot.SeekTargetSeconds);
    }

    private void ProgressSlider_ValueChanged(object sender,
        Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        var slot = _slots[TagToIndex(sender)];
        if (slot.SuppressSlider || slot.Player is null) return;
        slot.SeekTargetSeconds = e.NewValue;
        if (!slot.SliderDragging)
            PerformSeek(slot, slot.SeekTargetSeconds);
    }

    private void PerformSeek(Slot slot, double seconds)
    {
        if (slot.Player is null) return;
        slot.Player.Seek(seconds);
        slot.LastKnownSeconds      = seconds;
        slot.ResetPositionTracking = true;
        slot.Playing = true;
        slot.PlayPauseButton.Content = ""; // Pause glyph
        QuadCanvas.Invalidate();
    }

    private void SetSpeed_Click(object sender, RoutedEventArgs e)
    {
        var slot = _slots[TagToIndex(sender)];
        if (slot.Player is null) return;
        double speed = Math.Clamp(slot.SpeedBox.Value, 0.1, 100.0);
        slot.Player.Speed = speed;
    }

    private static int TagToIndex(object sender)
    {
        if (sender is FrameworkElement fe && fe.Tag is not null &&
            int.TryParse(fe.Tag.ToString(), NumberStyles.Integer,
                         CultureInfo.InvariantCulture, out int idx))
            return idx;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Progress / time helpers
    // -------------------------------------------------------------------------

    private void UpdateProgress(Slot slot, long ptsUs)
    {
        double seconds  = ptsUs / 1_000_000.0;
        double duration = slot.Player?.Duration ?? 0;

        // The master clock briefly returns 0 while the atempo filter is being
        // rebuilt after a speed change.  Once a slot has advanced past the
        // start, treat a sudden snap back near zero as a spurious read.
        if (!slot.ResetPositionTracking
            && slot.LastKnownSeconds > 0.5
            && seconds < slot.LastKnownSeconds - 0.5
            && seconds < 0.2)
        {
            seconds = slot.LastKnownSeconds;
        }
        else
        {
            slot.LastKnownSeconds      = seconds;
            slot.ResetPositionTracking = false;
        }

        double clamped = Math.Clamp(seconds, 0, slot.ProgressSlider.Maximum);

        slot.SuppressSlider = true;
        slot.ProgressSlider.Value = clamped;
        slot.SuppressSlider = false;

        UpdateTimeLabel(slot, clamped, duration);
    }

    private static string FormatTime(double seconds)
    {
        if (double.IsNaN(seconds) || double.IsInfinity(seconds) || seconds < 0)
            seconds = 0;
        var ts = TimeSpan.FromSeconds(seconds);
        return ts.Hours > 0
            ? $"{ts.Hours}:{ts.Minutes:D2}:{ts.Seconds:D2}"
            : $"{ts.Minutes}:{ts.Seconds:D2}";
    }

    private static void UpdateTimeLabel(Slot slot, double current, double total)
    {
        slot.TimeText.Text = $"{FormatTime(current)} / {FormatTime(total)}";
    }

    // -------------------------------------------------------------------------
    // Open-requested event — lets host pop a file picker bound to its HWND.
    // -------------------------------------------------------------------------

    public event EventHandler<SlotOpenRequestedEventArgs>? OpenRequested;

    public sealed class SlotOpenRequestedEventArgs : EventArgs
    {
        public int Index { get; }
        public SlotOpenRequestedEventArgs(int index) => Index = index;
    }

    // -------------------------------------------------------------------------
    // Interop helpers
    // -------------------------------------------------------------------------

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
    // Cleanup
    // -------------------------------------------------------------------------

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _progressTimer?.Stop();
        _progressTimer = null;

        foreach (var slot in _slots)
            slot?.Dispose();

        _canvasDevice?.Dispose();
        _canvasDevice = null;

        _sharedGpu?.Dispose();
        _sharedGpu = null;
    }
}
