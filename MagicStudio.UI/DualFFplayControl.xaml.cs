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
/// Demo: a single CanvasControl rendering frames from two MagicFFplayPlayer
/// instances at the same time.  Both players share one D3D11 device through
/// a MagicFFplaySharedGpu, which is what makes this work — without sharing
/// the device, the second player's frames could not be copied into a render
/// target on the canvas's CanvasDevice.
/// </summary>
public sealed partial class DualFFplayControl : UserControl, IDisposable
{
    private static readonly Guid IID_IDXGISurface =
        new("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");

    private MagicFFplaySharedGpu? _sharedGpu;
    private CanvasDevice?         _canvasDevice;
    private DispatcherQueue?      _dispatcherQueue;
    private bool                  _disposed;

    private sealed class Slot : IDisposable
    {
        public MagicFFplayPlayer?  Player;
        public CanvasRenderTarget? Bitmap;
        public IDirect3DSurface?   Surface;
        public int                 Width;
        public int                 Height;

        public void Dispose()
        {
            (Surface as IDisposable)?.Dispose();
            Surface = null;
            Bitmap?.Dispose();
            Bitmap = null;
            Player?.Dispose();
            Player = null;
            Width = Height = 0;
        }
    }

    private readonly Slot _a = new();
    private readonly Slot _b = new();

    public DualFFplayControl()
    {
        InitializeComponent();
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
        Unloaded += (_, _) => Dispose();
    }

    // -------------------------------------------------------------------------
    // Public API — host wires file pickers, calls these.
    // -------------------------------------------------------------------------

    public bool OpenA(string path) => OpenSlot(_a, path);
    public bool OpenB(string path) => OpenSlot(_b, path);

    private void Play_Click(object sender, RoutedEventArgs e)
    {
        _a.Player?.Play();
        _b.Player?.Play();
    }

    private void Pause_Click(object sender, RoutedEventArgs e)
    {
        _a.Player?.Pause();
        _b.Player?.Pause();
    }

    private void SetSpeedA_Click(object sender, RoutedEventArgs e)
        => ApplySpeed(_a, SpeedANumberBox.Value);

    private void SetSpeedB_Click(object sender, RoutedEventArgs e)
        => ApplySpeed(_b, SpeedBNumberBox.Value);

    private static void ApplySpeed(Slot slot, double value)
    {
        if (slot.Player is null) return;
        slot.Player.Speed = Math.Clamp(value, 0.1, 100.0);
    }

    // -------------------------------------------------------------------------
    // Slot lifecycle
    // -------------------------------------------------------------------------

    private bool OpenSlot(Slot slot, string path)
    {
        EnsureSharedGpuAndCanvasDevice();
        if (_sharedGpu is null || _canvasDevice is null) return false;

        // Tear down the previous player in this slot, including its bitmap —
        // a new file may have a different size.
        slot.Dispose();

        var player = new MagicFFplayPlayer();
        if (!player.Open(path, _sharedGpu)) { player.Dispose(); return false; }
        slot.Player = player;
        slot.Player.VideoFrameAvailable += (_, _) => OnFrameAvailable(slot);
        return true;
    }

    private void OnFrameAvailable(Slot slot)
    {
        // Fires on the native refresh thread.  Marshal to UI to do GPU copy
        // (CanvasRenderTarget creation must happen on the UI thread anyway).
        _dispatcherQueue?.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            if (slot.Player is null || _canvasDevice is null) return;
            EnsureSlotSurface(slot);
            if (slot.Surface is null) return;
            if (slot.Player.CopyFrameToVideoSurface(slot.Surface))
                DualCanvas.Invalidate();
        });
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
            IntPtr dxgi = _sharedGpu.AcquireDxgiDevice();
            if (dxgi == IntPtr.Zero) return;
            try
            {
                IDirect3DDevice d3d = CreateDirect3DDeviceFromDxgi(dxgi);
                _canvasDevice = CanvasDevice.CreateFromDirect3D11Device(d3d);
                DualCanvas.CustomDevice = _canvasDevice;
            }
            finally
            {
                Marshal.Release(dxgi);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Canvas callbacks
    // -------------------------------------------------------------------------

    private void DualCanvas_CreateResources(CanvasControl sender,
                                            CanvasCreateResourcesEventArgs args) { }

    private void DualCanvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        // Side-by-side layout: A on the left half, B on the right half.
        double half = sender.ActualWidth / 2.0;
        double full = sender.ActualHeight;

        if (_a.Bitmap is not null)
            args.DrawingSession.DrawImage(_a.Bitmap, new Rect(0,    0, half, full));
        if (_b.Bitmap is not null)
            args.DrawingSession.DrawImage(_b.Bitmap, new Rect(half, 0, half, full));
    }

    // -------------------------------------------------------------------------
    // Interop helpers (same pattern as MagicFFplayControl)
    // -------------------------------------------------------------------------

    [ComImport]
    [Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDirect3DDxgiInterfaceAccess
    {
        [PreserveSig]
        int GetInterface([In] in Guid iid, out IntPtr ppv);
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
    // Cleanup
    // -------------------------------------------------------------------------

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _a.Dispose();
        _b.Dispose();

        _canvasDevice?.Dispose();
        _canvasDevice = null;

        _sharedGpu?.Dispose();
        _sharedGpu = null;
    }
}
