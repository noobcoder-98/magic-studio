#pragma once
#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace MagicStudio::Native {

// Converts NV12 frames (as stored in D3D11 hwframes) to BGRA using GPU shaders.
// This keeps the entire pipeline on the GPU, avoiding expensive CPU copies.
class GpuFrameConverter {
public:
    GpuFrameConverter();
    ~GpuFrameConverter();

    // Initialize with a D3D11 device. Called once during decoder startup.
    bool Initialize(ID3D11Device* device);

    // Convert an NV12 texture to BGRA.
    // Input: D3D11 surface from hwframe (AV_PIX_FMT_D3D11)
    // Output: BGRA texture suitable for Win2D interop
    // Returns: D3D11Texture2D with format DXGI_FORMAT_B8G8R8A8_UNORM
    ID3D11Texture2D* ConvertNv12ToBgra(
        ID3D11Texture2D* nv12Surface,
        int width, int height);

    // Create a staging texture for GPU->CPU readback (fallback for diagnostics)
    ID3D11Texture2D* CreateStagingTexture(int width, int height);

    // Retrieve pixel data from a staging texture
    std::vector<uint8_t> ReadbackTexture(ID3D11Texture2D* stagingTexture,
                                         int width, int height);

private:
    // Lazy initialize shader and render target resources for a given dimension.
    bool EnsureResources(int width, int height);

    ID3D11Device*           _device = nullptr;
    ID3D11DeviceContext*    _context = nullptr;
    ID3D11PixelShader*      _pixelShader = nullptr;
    ID3D11SamplerState*     _samplerState = nullptr;

    int _lastWidth = 0, _lastHeight = 0;
    ID3D11Texture2D*        _renderTarget = nullptr;
    ID3D11RenderTargetView* _renderTargetView = nullptr;
    ID3D11ShaderResourceView* _nv12SrvY = nullptr;
    ID3D11ShaderResourceView* _nv12SrvUV = nullptr;
};

} // namespace MagicStudio::Native
