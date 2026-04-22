#include "pch.h"
#include "GpuVideoConverter.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace MagicStudio::Native {

GpuVideoConverter::GpuVideoConverter() = default;

GpuVideoConverter::~GpuVideoConverter() {
    _processor.Reset();
    _enumerator.Reset();
    _videoContext.Reset();
    _videoDevice.Reset();
    _dxgiDevice.Reset();
    _context.Reset();
    _device.Reset();
}

bool GpuVideoConverter::Initialize(int width, int height) {
    if (!_device && !CreateDevice()) return false;

    if (_width != width || _height != height) {
        _processor.Reset();
        _enumerator.Reset();
        _width  = width;
        _height = height;
    }
    if (!_processor && !CreateVideoProcessor()) return false;
    return true;
}

bool GpuVideoConverter::CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT      // required by Direct2D / Win2D
               | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;    // required by VideoProcessor

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &_device, nullptr, &_context);
    if (FAILED(hr)) return false;

    // FFmpeg's d3d11va decoder and Win2D may both touch the immediate context
    // from different threads — opt into D3D11's internal serialisation.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(_device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(_device.As(&_dxgiDevice)))      return false;
    if (FAILED(_device.As(&_videoDevice)))     return false;
    if (FAILED(_context.As(&_videoContext)))   return false;
    return true;
}

bool GpuVideoConverter::CreateVideoProcessor() {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat            = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator    = 60;
    desc.InputFrameRate.Denominator  = 1;
    desc.InputWidth                  = static_cast<UINT>(_width);
    desc.InputHeight                 = static_cast<UINT>(_height);
    desc.OutputFrameRate.Numerator   = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth                 = static_cast<UINT>(_width);
    desc.OutputHeight                = static_cast<UINT>(_height);
    desc.Usage                       = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(_videoDevice->CreateVideoProcessorEnumerator(&desc, &_enumerator)))
        return false;
    if (FAILED(_videoDevice->CreateVideoProcessor(_enumerator.Get(), 0, &_processor)))
        return false;

    // BT.709 limited-range YCbCr -> full-range RGB. Good default for HD content;
    // SD (BT.601) sources will be slightly off but still display correctly.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs = {};
    inCs.YCbCr_Matrix  = 1;                                                      // BT.709
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    _videoContext->VideoProcessorSetStreamColorSpace(_processor.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
    outCs.RGB_Range = 0;                                                         // full range
    _videoContext->VideoProcessorSetOutputColorSpace(_processor.Get(), &outCs);
    return true;
}

ComPtr<ID3D11Texture2D> GpuVideoConverter::AllocateOutputTexture() {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = static_cast<UINT>(_width);
    desc.Height           = static_cast<UINT>(_height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(_device->CreateTexture2D(&desc, nullptr, &tex))) return nullptr;
    return tex;
}

ComPtr<ID3D11Texture2D> GpuVideoConverter::Convert(
    ID3D11Texture2D* nv12Source, UINT subresource) {

    if (!_processor || !nv12Source) return nullptr;

    auto out = AllocateOutputTexture();
    if (!out) return nullptr;

    std::lock_guard<std::mutex> blt(_bltMutex);
    // Hold the FFmpeg lock too: the source texture is shared with the d3d11va
    // decoder pool, which uses the same video context.
    std::lock_guard<std::recursive_mutex> ff(_ffmpegLock);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.FourCC               = 0;
    ivDesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice   = 0;
    ivDesc.Texture2D.ArraySlice = subresource;

    ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(_videoDevice->CreateVideoProcessorInputView(
            nv12Source, _enumerator.Get(), &ivDesc, &inView)))
        return nullptr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
    ovDesc.ViewDimension      = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovDesc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(_videoDevice->CreateVideoProcessorOutputView(
            out.Get(), _enumerator.Get(), &ovDesc, &outView)))
        return nullptr;

    RECT rect = { 0, 0, _width, _height };
    _videoContext->VideoProcessorSetStreamSourceRect(_processor.Get(), 0, TRUE, &rect);
    _videoContext->VideoProcessorSetStreamDestRect  (_processor.Get(), 0, TRUE, &rect);
    _videoContext->VideoProcessorSetOutputTargetRect(_processor.Get(),    TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable           = TRUE;
    stream.OutputIndex      = 0;
    stream.InputFrameOrField= 0;
    stream.PastFrames       = 0;
    stream.FutureFrames     = 0;
    stream.pInputSurface    = inView.Get();

    if (FAILED(_videoContext->VideoProcessorBlt(
            _processor.Get(), outView.Get(), 0, 1, &stream)))
        return nullptr;

    return out;
}

} // namespace MagicStudio::Native
