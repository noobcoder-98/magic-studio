#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <mutex>

namespace MagicStudio::Native {

// Owns a shared ID3D11Device used by:
//   1. FFmpeg's d3d11va hardware device context (for HW decode into NV12 textures)
//   2. ID3D11VideoProcessor for NV12 -> BGRA conversion on the GPU
//   3. Win2D (the same device is wrapped as a CanvasDevice on the UI side)
// Keeping all three on one device avoids cross-device texture sharing and
// lets us pass the BGRA output texture straight to Win2D as a CanvasBitmap.
class GpuVideoConverter {
public:
    GpuVideoConverter();
    ~GpuVideoConverter();

    GpuVideoConverter(const GpuVideoConverter&)            = delete;
    GpuVideoConverter& operator=(const GpuVideoConverter&) = delete;

    // Creates the device on first call. Recreates the video processor when the
    // source dimensions change. Cheap to call repeatedly with the same size.
    bool Initialize(int width, int height);

    // Convert a single subresource of a NV12 array texture (FFmpeg's frame pool)
    // into a fresh BGRA ID3D11Texture2D.  Returns null on failure.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Convert(
        ID3D11Texture2D* nv12Source, UINT subresourceIndex);

    ID3D11Device* Device()     const { return _device.Get(); }
    IDXGIDevice*  DxgiDevice() const { return _dxgiDevice.Get(); }

    // Recursive lock used as the FFmpeg AVD3D11VADeviceContext lock callback.
    void Lock()   { _ffmpegLock.lock(); }
    void Unlock() { _ffmpegLock.unlock(); }

private:
    bool CreateDevice();
    bool CreateVideoProcessor();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> AllocateOutputTexture();

    Microsoft::WRL::ComPtr<ID3D11Device>                   _device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>            _context;
    Microsoft::WRL::ComPtr<IDXGIDevice>                    _dxgiDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>              _videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>             _videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> _enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor>           _processor;

    int _width  = 0;
    int _height = 0;

    // Serialises VideoProcessorBlt + view creation; D3D11 itself is
    // multithreaded but the video pipeline benefits from one writer at a time.
    std::mutex _bltMutex;
    // Recursive lock handed to FFmpeg (its docs require a recursive mutex).
    std::recursive_mutex _ffmpegLock;
};

} // namespace MagicStudio::Native
