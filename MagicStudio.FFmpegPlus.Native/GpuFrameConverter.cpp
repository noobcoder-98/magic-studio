#include "pch.h"
#include "GpuFrameConverter.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace MagicStudio::Native {

// HLSL Vertex Shader: Full-screen quad with texture coordinates
static const char* g_fullscreenVS = R"(
struct VS_OUTPUT
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VS_OUTPUT main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.texCoord = float2(vertexID & 1, (vertexID >> 1) & 1);
    output.position = float4(output.texCoord * 2.0f - 1.0f, 0.0f, 1.0f);
    output.texCoord.y = 1.0f - output.texCoord.y;
    return output;
}
)";

// HLSL Pixel Shader: Convert NV12 to BGRA
static const char* g_nv12ToBgraPS = R"(
Texture2D texY  : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    float y = texY.Sample(samp, input.texCoord).r;
    float u = texUV.Sample(samp, input.texCoord).r;
    float v = texUV.Sample(samp, input.texCoord).g;

    float r = y + 1.402f   * (v - 0.5f);
    float g = y - 0.344136f * (u - 0.5f) - 0.714136f * (v - 0.5f);
    float b = y + 1.772f   * (u - 0.5f);

    r = clamp(r, 0.0f, 1.0f);
    g = clamp(g, 0.0f, 1.0f);
    b = clamp(b, 0.0f, 1.0f);

    return float4(b, g, r, 1.0f);
}
)";

GpuFrameConverter::GpuFrameConverter() = default;

GpuFrameConverter::~GpuFrameConverter() {
    if (_renderTargetView) { _renderTargetView->Release(); }
    if (_renderTarget) { _renderTarget->Release(); }
    if (_nv12SrvY) { _nv12SrvY->Release(); }
    if (_nv12SrvUV) { _nv12SrvUV->Release(); }
    if (_samplerState) { _samplerState->Release(); }
    if (_pixelShader) { _pixelShader->Release(); }
    if (_context) { _context->ClearState(); _context->Release(); }
    if (_device) { _device->Release(); }
}

bool GpuFrameConverter::Initialize(ID3D11Device* device) {
    if (!device) return false;

    _device = device;
    _device->AddRef();
    _device->GetImmediateContext(&_context);
    if (!_context) return false;

    // Compile vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(
        g_fullscreenVS, strlen(g_fullscreenVS), nullptr, nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);

    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }

    ID3D11VertexShader* vertexShader = nullptr;
    hr = _device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, &vertexShader);
    if (vertexShader) vertexShader->Release();
    if (FAILED(hr)) {
        vsBlob->Release();
        return false;
    }
    vsBlob->Release();

    // Compile pixel shader
    ID3DBlob* psBlob = nullptr;
    errBlob = nullptr;
    hr = D3DCompile(
        g_nv12ToBgraPS, strlen(g_nv12ToBgraPS), nullptr, nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &psBlob, &errBlob);

    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }

    hr = _device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                     nullptr, &_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) return false;

    // Create sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = (D3D11_FILTER)4;  // D3D11_FILTER_LINEAR
    sampDesc.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)3;  // D3D11_TEXTURE_ADDRESS_CLAMP
    sampDesc.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)3;  // D3D11_TEXTURE_ADDRESS_CLAMP
    sampDesc.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)3;  // D3D11_TEXTURE_ADDRESS_CLAMP
    sampDesc.ComparisonFunc = (D3D11_COMPARISON_FUNC)1;  // D3D11_COMPARISON_NEVER
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = 3.402823466e+38F;  // D3D11_FLOAT32_MAX

    hr = _device->CreateSamplerState(&sampDesc, &_samplerState);
    return SUCCEEDED(hr);
}

bool GpuFrameConverter::EnsureResources(int width, int height) {
    if (_renderTarget && _lastWidth == width && _lastHeight == height) {
        return true;
    }

    if (_renderTarget) { _renderTarget->Release(); _renderTarget = nullptr; }
    if (_renderTargetView) { _renderTargetView->Release(); _renderTargetView = nullptr; }
    if (_nv12SrvY) { _nv12SrvY->Release(); _nv12SrvY = nullptr; }
    if (_nv12SrvUV) { _nv12SrvUV->Release(); _nv12SrvUV = nullptr; }

    _lastWidth = 0;
    _lastHeight = 0;

    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = width;
    rtDesc.Height = height;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = (D3D11_USAGE)0;
    rtDesc.BindFlags = 0x8;

    HRESULT hr = _device->CreateTexture2D(&rtDesc, nullptr, &_renderTarget);
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtvDesc.ViewDimension = (D3D11_RTV_DIMENSION)4;

    hr = _device->CreateRenderTargetView(_renderTarget, &rtvDesc, &_renderTargetView);
    if (FAILED(hr)) return false;

    _lastWidth = width;
    _lastHeight = height;
    return true;
}

ID3D11Texture2D* GpuFrameConverter::ConvertNv12ToBgra(
    ID3D11Texture2D* nv12Surface, int width, int height) {
    if (!_device || !_context || !_pixelShader || !nv12Surface) {
        return nullptr;
    }

    if (!EnsureResources(width, height)) {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = (D3D11_SRV_DIMENSION)2;
    srvDescY.Texture2D.MipLevels = 1;

    HRESULT hr = _device->CreateShaderResourceView(nv12Surface, &srvDescY, &_nv12SrvY);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDescUV.ViewDimension = (D3D11_SRV_DIMENSION)2;
    srvDescUV.Texture2D.MipLevels = 1;

    hr = _device->CreateShaderResourceView(nv12Surface, &srvDescUV, &_nv12SrvUV);
    if (FAILED(hr)) return nullptr;

    _context->OMSetRenderTargets(1, &_renderTargetView, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    _context->RSSetViewports(1, &vp);

    _context->PSSetShader(_pixelShader, nullptr, 0);
    _context->PSSetSamplers(0, 1, &_samplerState);

    ID3D11ShaderResourceView* srvs[] = { _nv12SrvY, _nv12SrvUV };
    _context->PSSetShaderResources(0, 2, srvs);

    _context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    _context->PSSetShaderResources(0, 2, nullSrvs);

    return _renderTarget;
}

ID3D11Texture2D* GpuFrameConverter::CreateStagingTexture(int width, int height) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = (D3D11_USAGE)3;
    desc.CPUAccessFlags = 0x20000;

    ID3D11Texture2D* staging = nullptr;
    _device->CreateTexture2D(&desc, nullptr, &staging);
    return staging;
}

std::vector<uint8_t> GpuFrameConverter::ReadbackTexture(ID3D11Texture2D* stagingTexture,
                                                        int width, int height) {
    std::vector<uint8_t> result;
    if (!_context || !stagingTexture) return result;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = _context->Map(stagingTexture, 0, (D3D11_MAP)1, 0, &mapped);
    if (FAILED(hr)) return result;

    const uint8_t* srcData = static_cast<const uint8_t*>(mapped.pData);
    size_t bytesPerFrame = static_cast<size_t>(width) * height * 4;

    result.reserve(bytesPerFrame);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = srcData + y * mapped.RowPitch;
        result.insert(result.end(), row, row + width * 4);
    }

    _context->Unmap(stagingTexture, 0);
    return result;
}

} // namespace MagicStudio::Native
