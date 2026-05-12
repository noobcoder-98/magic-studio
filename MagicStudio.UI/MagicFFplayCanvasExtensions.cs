using MagicStudio.FFmpegPlus;
using Microsoft.Graphics.Canvas;
using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MagicStudio.UI;

/// <summary>
/// Bridges MagicFFplay's native D3D11 device into a Win2D <see cref="CanvasDevice"/>.
/// Use these when you need to render player frames into a Win2D surface — the
/// CanvasDevice must be created from the player's DXGI device, otherwise
/// CopyFrameToVideoSurface will reject the destination as cross-device.
/// </summary>
public static class MagicFFplayCanvasExtensions
{
    /// <summary>
    /// Creates a <see cref="CanvasDevice"/> bound to <paramref name="player"/>'s
    /// D3D11 device. Returns null if the player has no device yet (e.g. Open
    /// hasn't succeeded). The returned CanvasDevice is owned by the caller.
    /// </summary>
    public static CanvasDevice? CreateCanvasDevice(this MagicFFplayPlayer player)
    {
        ArgumentNullException.ThrowIfNull(player);
        return CreateFromAcquiredDxgi(player.AcquireDxgiDevice());
    }

    /// <summary>
    /// Creates a <see cref="CanvasDevice"/> bound to the shared D3D11 device
    /// in <paramref name="shared"/>. Returns null if the shared GPU is invalid.
    /// </summary>
    public static CanvasDevice? CreateCanvasDevice(this MagicFFplaySharedGpu shared)
    {
        ArgumentNullException.ThrowIfNull(shared);
        return CreateFromAcquiredDxgi(shared.AcquireDxgiDevice());
    }

    private static CanvasDevice? CreateFromAcquiredDxgi(IntPtr dxgiDevice)
    {
        if (dxgiDevice == IntPtr.Zero) return null;
        try
        {
            CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, out IntPtr abi);
            try
            {
                IDirect3DDevice d3d = MarshalInspectable<IDirect3DDevice>.FromAbi(abi);
                return CanvasDevice.CreateFromDirect3D11Device(d3d);
            }
            finally { Marshal.Release(abi); }
        }
        finally { Marshal.Release(dxgiDevice); }
    }

    [DllImport("d3d11.dll", ExactSpelling = true, PreserveSig = false)]
    private static extern void CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice,
                                                                    out IntPtr graphicsDevice);
}
