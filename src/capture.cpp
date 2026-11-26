#include "capture.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
ComPtr<IDXGIOutput> FindOutput(HMONITOR monitor, ComPtr<IDXGIAdapter1>& match) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(index, &adapter))) break;
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(outputIndex, &output))) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor && desc.AttachedToDesktop) {
                match = std::move(adapter);
                return output;
            }
        }
    }
    return {};
}

Bitmap ReadDesktop(const D3D11_MAPPED_SUBRESOURCE& map, UINT width, UINT height,
                   DXGI_MODE_ROTATION rotation) {
    const bool sideways = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
    Bitmap result{sideways ? height : width, sideways ? width : height, {}};
    result.pixels.resize(static_cast<size_t>(result.width) * result.height);
    const auto row = [&](UINT y) {
        return reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(map.pData) +
                                                      static_cast<size_t>(y) * map.RowPitch);
    };
    for (UINT y = 0; y < result.height; ++y) {
        auto* target = result.pixels.data() + static_cast<size_t>(y) * result.width;
        switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            for (UINT x = 0; x < result.width; ++x) target[x] = row(height - 1 - x)[y];
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            std::reverse_copy(row(height - 1 - y), row(height - 1 - y) + width, target);
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            for (UINT x = 0; x < result.width; ++x) target[x] = row(x)[width - 1 - y];
            break;
        default:
            std::memcpy(target, row(y), static_cast<size_t>(width) * sizeof(*target));
            break;
        }
    }
    return result;
}
}

DesktopCapture::DesktopCapture(HWND receiver)
    : receiver_(receiver), worker_([this](std::stop_token stop) { Run(stop); }) {}

DesktopCapture::~DesktopCapture() {
    Cancel();
    worker_.request_stop();
}

unsigned DesktopCapture::Start(HMONITOR monitor, ULONGLONG deadline) {
    std::lock_guard lock(mutex_);
    const unsigned id = ++generation_;
    pending_ = Job{monitor, deadline, id};
    complete_.reset();
    changed_.notify_one();
    return id;
}

void DesktopCapture::Cancel() {
    std::lock_guard lock(mutex_);
    ++generation_;
    pending_.reset();
    complete_.reset();
}

std::optional<CaptureResult> DesktopCapture::TakeResult() {
    std::lock_guard lock(mutex_);
    return std::exchange(complete_, std::nullopt);
}

void DesktopCapture::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [this] { return pending_.has_value(); })) return;
            job = *std::exchange(pending_, std::nullopt);
        }
        CaptureResult result;
        try {
            result = Capture(job);
        } catch (const std::bad_alloc&) {
            result = {job.id, CaptureIssue::Unsupported, {}};
        }
        {
            std::lock_guard lock(mutex_);
            if (generation_ != job.id) continue;
            complete_ = std::move(result);
        }
        PostMessageW(receiver_, CaptureReadyMessage, 0, 0);
    }
}

CaptureResult DesktopCapture::Capture(const Job& job) {
    const auto fail = [&](CaptureIssue issue) { return CaptureResult{job.id, issue, {}}; };
    ComPtr<IDXGIAdapter1> adapter;
    const auto output = FindOutput(job.monitor, adapter);
    if (!output || generation_ != job.id) return fail(CaptureIssue::Unsupported);
    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 desc1{};
    if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1)) &&
        desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        return fail(CaptureIssue::Hdr);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        return fail(CaptureIssue::Unsupported);
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> duplication;
    if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device.Get(), &duplication)))
        return fail(CaptureIssue::Unsupported);

    DXGI_OUTDUPL_DESC duplicationDesc{};
    duplication->GetDesc(&duplicationDesc);
    while (generation_ == job.id && GetTickCount64() < job.deadline) {
        DXGI_OUTDUPL_FRAME_INFO frame{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquired = duplication->AcquireNextFrame(16, &frame, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(acquired)) return fail(CaptureIssue::Unsupported);
        if (frame.ProtectedContentMaskedOut) return fail(CaptureIssue::Protected);
        if (!frame.LastPresentTime.QuadPart || !resource) {
            duplication->ReleaseFrame();
            continue;
        }
        ComPtr<ID3D11Texture2D> desktop;
        if (FAILED(resource.As(&desktop))) return fail(CaptureIssue::Unsupported);
        D3D11_TEXTURE2D_DESC desc{};
        desktop->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return fail(CaptureIssue::Unsupported);
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return fail(CaptureIssue::Unsupported);
        context->CopyResource(staging.Get(), desktop.Get());
        duplication->ReleaseFrame();
        if (generation_ != job.id) return fail(CaptureIssue::Timeout);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
            return fail(CaptureIssue::Unsupported);
        auto pixels = ReadDesktop(map, desc.Width, desc.Height, duplicationDesc.Rotation);
        context->Unmap(staging.Get(), 0);
        return {job.id, CaptureIssue::None, std::move(pixels)};
    }
    return fail(CaptureIssue::Timeout);
}
