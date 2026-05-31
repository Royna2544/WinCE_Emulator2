#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#endif

#include "synthetic_dll.h"

#if defined(_WIN32)
#include "../res/resource.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "../third_party/NVIDIAImageScaling/NIS/NIS_Config.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t kWindowStyleChild = 0x40000000u; // WS_CHILD

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint64_t hostTickMilliseconds() {
#if defined(_WIN32)
    return GetTickCount64();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

bool envFlagEnabled(const char* name) {
    char* rawValue = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&rawValue, &valueSize, name) != 0 || !rawValue) return false;
    std::string value(rawValue);
    std::free(rawValue);
    if (value.empty()) return false;
    std::string text = lowerAscii(value);
    return text != "0" && text != "false" && text != "no" && text != "off";
}

#if defined(_WIN32)
std::filesystem::path executableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    DWORD chars = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (!chars || chars >= std::size(buffer)) return {};
    return std::filesystem::path(buffer).parent_path();
}

std::optional<std::filesystem::path> findNisShaderPath() {
    const std::filesystem::path relative = L"third_party/NVIDIAImageScaling/NIS/NIS_Main.hlsl";
    std::vector<std::filesystem::path> candidates{
        relative,
        std::filesystem::path(L"..") / relative,
        std::filesystem::path(L"..") / L".." / relative,
    };
    const std::filesystem::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / relative);
        candidates.push_back(exeDir.parent_path() / relative);
        candidates.push_back(exeDir.parent_path().parent_path() / relative);
    }
    for (const auto& path : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) return path;
    }
    return std::nullopt;
}

std::string hresultHex(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << uint32_t(hr);
    return out.str();
}

const char* d3dBlitShaderSource() {
    return R"(
Texture2D inputTexture : register(t0);
SamplerState linearClampSampler : register(s0);

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vsMain(uint vertexId : SV_VertexID) {
    VertexOut output;
    if (vertexId == 0) {
        output.position = float4(-1.0f, -1.0f, 0.0f, 1.0f);
        output.uv = float2(0.0f, 1.0f);
    } else if (vertexId == 1) {
        output.position = float4(-1.0f, 3.0f, 0.0f, 1.0f);
        output.uv = float2(0.0f, -1.0f);
    } else {
        output.position = float4(3.0f, -1.0f, 0.0f, 1.0f);
        output.uv = float2(2.0f, 1.0f);
    }
    return output;
}

float4 psMain(VertexOut input) : SV_Target {
    return inputTexture.Sample(linearClampSampler, input.uv);
}
)";
}

class HostPresenterD3D11 {
public:
    bool render(HWND hwnd,
                const uint32_t* framebuffer,
                int sourceWidth,
                int sourceHeight,
                int clientWidth,
                int clientHeight,
                const RECT& imageRect) {
        if (!framebuffer || sourceWidth <= 0 || sourceHeight <= 0 ||
            clientWidth <= 0 || clientHeight <= 0 ||
            imageRect.right <= imageRect.left || imageRect.bottom <= imageRect.top) {
            return false;
        }
        if (!ensureDevice(hwnd, clientWidth, clientHeight)) return false;
        const int targetWidth = imageRect.right - imageRect.left;
        const int targetHeight = imageRect.bottom - imageRect.top;
        if (!ensureFrameResources(sourceWidth, sourceHeight, targetWidth, targetHeight)) return false;

        context_->UpdateSubresource(inputTexture_.Get(), 0, nullptr, framebuffer,
                                    UINT(sourceWidth * sizeof(uint32_t)), 0);

        ID3D11ShaderResourceView* sourceSrv = inputSrv_.Get();
        int currentWidth = sourceWidth;
        int currentHeight = sourceHeight;
        for (size_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            PassResource& pass = passes_[passIndex];
            const float sharpness = passIndex + 1 == passes_.size() ? 0.35f : 0.0f;
            NISConfig config{};
            if (!NVScalerUpdateConfig(config, sharpness,
                                      0, 0, uint32_t(currentWidth), uint32_t(currentHeight),
                                      uint32_t(currentWidth), uint32_t(currentHeight),
                                      0, 0, uint32_t(pass.width), uint32_t(pass.height),
                                      uint32_t(pass.width), uint32_t(pass.height),
                                      NISHDRMode::None)) {
                return false;
            }
            context_->UpdateSubresource(configBuffer_.Get(), 0, nullptr, &config, 0, 0);

            ID3D11ShaderResourceView* srvs[] = {sourceSrv, coefScalerSrv_.Get(), coefUsmSrv_.Get()};
            ID3D11UnorderedAccessView* uavs[] = {pass.uav.Get()};
            ID3D11SamplerState* samplers[] = {sampler_.Get()};
            ID3D11Buffer* constants[] = {configBuffer_.Get()};
            context_->CSSetShader(nisShader_.Get(), nullptr, 0);
            context_->CSSetShaderResources(0, 3, srvs);
            context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            context_->CSSetSamplers(0, 1, samplers);
            context_->CSSetConstantBuffers(0, 1, constants);
            context_->Dispatch(UINT((pass.width + blockWidth_ - 1) / blockWidth_),
                               UINT((pass.height + blockHeight_ - 1) / blockHeight_), 1);
            ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr, nullptr};
            ID3D11UnorderedAccessView* nullUavs[] = {nullptr};
            context_->CSSetShaderResources(0, 3, nullSrvs);
            context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

            sourceSrv = pass.srv.Get();
            currentWidth = pass.width;
            currentHeight = pass.height;
        }

        const FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = float(imageRect.left);
        viewport.TopLeftY = float(imageRect.top);
        viewport.Width = float(targetWidth);
        viewport.Height = float(targetHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* rtvs[] = {renderTarget_.Get()};
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        ID3D11ShaderResourceView* psSrvs[] = {passes_.empty() ? inputSrv_.Get() : passes_.back().srv.Get()};
        ID3D11SamplerState* pixelSamplers[] = {sampler_.Get()};
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(blitPs_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, psSrvs);
        context_->PSSetSamplers(0, 1, pixelSamplers);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullPsSrv = nullptr;
        context_->PSSetShaderResources(0, 1, &nullPsSrv);

        const HRESULT presentHr = swapChain_->Present(0, 0);
        if (FAILED(presentHr)) {
            logFailureOnce("D3D11 Present failed " + hresultHex(presentHr));
            return false;
        }
        return true;
    }

private:
    struct PassResource {
        int width{};
        int height{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    bool ensureDevice(HWND hwnd, int clientWidth, int clientHeight) {
        if (!device_ && !createDeviceAndShaders(hwnd, clientWidth, clientHeight)) return false;
        if (clientWidth != swapChainWidth_ || clientHeight != swapChainHeight_) {
            return resizeSwapChain(clientWidth, clientHeight);
        }
        return true;
    }

    bool createDeviceAndShaders(HWND hwnd, int clientWidth, int clientHeight) {
        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferDesc.Width = UINT(clientWidth);
        swapDesc.BufferDesc.Height = UINT(clientHeight);
        swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.OutputWindow = hwnd;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL chosenFeatureLevel{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, UINT(std::size(featureLevels)), D3D11_SDK_VERSION,
            &swapDesc, &swapChain_, &device_, &chosenFeatureLevel, &context_);
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels + 1, 1, D3D11_SDK_VERSION,
                &swapDesc, &swapChain_, &device_, &chosenFeatureLevel, &context_);
        }
        if (FAILED(hr)) {
            logFailureOnce("D3D11 device creation failed " + hresultHex(hr));
            return false;
        }
        if (!createRenderTarget(clientWidth, clientHeight) || !createSampler() ||
            !createNisShader() || !createBlitShaders() ||
            !createCoefficientTextures() || !createConfigBuffer()) {
            return false;
        }
        spdlog::info("host presenter D3D11/NIS backend initialized featureLevel=0x{:04x}",
                     unsigned(chosenFeatureLevel));
        return true;
    }

    bool resizeSwapChain(int clientWidth, int clientHeight) {
        renderTarget_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT hr = swapChain_->ResizeBuffers(0, UINT(clientWidth), UINT(clientHeight),
                                                     DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 ResizeBuffers failed " + hresultHex(hr));
            return false;
        }
        return createRenderTarget(clientWidth, clientHeight);
    }

    bool createRenderTarget(int clientWidth, int clientHeight) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            logFailureOnce("D3D11 GetBuffer failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 CreateRenderTargetView failed " + hresultHex(hr));
            return false;
        }
        swapChainWidth_ = clientWidth;
        swapChainHeight_ = clientHeight;
        return true;
    }

    bool createSampler() {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        const HRESULT hr = device_->CreateSamplerState(&desc, &sampler_);
        if (FAILED(hr)) logFailureOnce("D3D11 CreateSamplerState failed " + hresultHex(hr));
        return SUCCEEDED(hr);
    }

    bool createNisShader() {
        const auto shaderPath = findNisShaderPath();
        if (!shaderPath) {
            logFailureOnce("NIS shader file not found; using GDI presenter fallback");
            return false;
        }
        NISOptimizer optimizer(true, NISGPUArchitecture::NVIDIA_Generic);
        blockWidth_ = int(optimizer.GetOptimalBlockWidth());
        blockHeight_ = int(optimizer.GetOptimalBlockHeight());
        threadGroupSize_ = int(optimizer.GetOptimalThreadGroupSize());
        const std::string blockWidth = std::to_string(blockWidth_);
        const std::string blockHeight = std::to_string(blockHeight_);
        const std::string threadGroupSize = std::to_string(threadGroupSize_);
        D3D_SHADER_MACRO macros[] = {
            {"NIS_SCALER", "1"},
            {"NIS_HDR_MODE", "0"},
            {"NIS_BLOCK_WIDTH", blockWidth.c_str()},
            {"NIS_BLOCK_HEIGHT", blockHeight.c_str()},
            {"NIS_THREAD_GROUP_SIZE", threadGroupSize.c_str()},
            {"NIS_HLSL", "1"},
            {"NIS_VIEWPORT_SUPPORT", "0"},
            {"NIS_CLAMP_OUTPUT", "1"},
            {nullptr, nullptr},
        };
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(shaderPath->c_str(), macros,
                                              D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                              "main", "cs_5_0", 0, 0, &shader, &errors);
        if (FAILED(hr)) {
            std::string message = "NIS shader compile failed " + hresultHex(hr);
            if (errors && errors->GetBufferPointer()) {
                message += ": ";
                message += static_cast<const char*>(errors->GetBufferPointer());
            }
            logFailureOnce(message);
            return false;
        }
        const HRESULT createHr = device_->CreateComputeShader(shader->GetBufferPointer(),
                                                              shader->GetBufferSize(),
                                                              nullptr, &nisShader_);
        if (FAILED(createHr)) logFailureOnce("D3D11 CreateComputeShader failed " + hresultHex(createHr));
        return SUCCEEDED(createHr);
    }

    bool createBlitShaders() {
        Microsoft::WRL::ComPtr<ID3DBlob> vs;
        Microsoft::WRL::ComPtr<ID3DBlob> ps;
        const char* source = d3dBlitShaderSource();
        HRESULT hr = D3DCompile(source, std::strlen(source), "host-presenter-blit",
                                nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vs, nullptr);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 blit vertex shader compile failed " + hresultHex(hr));
            return false;
        }
        hr = D3DCompile(source, std::strlen(source), "host-presenter-blit",
                        nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &ps, nullptr);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 blit pixel shader compile failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &blitVs_);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 CreateVertexShader failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &blitPs_);
        if (FAILED(hr)) logFailureOnce("D3D11 CreatePixelShader failed " + hresultHex(hr));
        return SUCCEEDED(hr);
    }

    bool createCoefficientTextures() {
        return createCoefficientTexture(coef_scale, &coefScalerSrv_) &&
               createCoefficientTexture(coef_usm, &coefUsmSrv_);
    }

    bool createCoefficientTexture(const float (&coefficients)[kPhaseCount][kFilterSize],
                                  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srvOut) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(kFilterSize / 4);
        desc.Height = UINT(kPhaseCount);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = coefficients;
        data.SysMemPitch = UINT(kFilterSize * sizeof(float));
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device_->CreateTexture2D(&desc, &data, &texture);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 coefficient texture creation failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateShaderResourceView(texture.Get(), nullptr, srvOut->GetAddressOf());
        if (FAILED(hr)) logFailureOnce("D3D11 coefficient SRV creation failed " + hresultHex(hr));
        return SUCCEEDED(hr);
    }

    bool createConfigBuffer() {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = UINT((sizeof(NISConfig) + 15u) & ~15u);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT hr = device_->CreateBuffer(&desc, nullptr, &configBuffer_);
        if (FAILED(hr)) logFailureOnce("D3D11 NIS config buffer creation failed " + hresultHex(hr));
        return SUCCEEDED(hr);
    }

    bool ensureFrameResources(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight) {
        if (sourceWidth == sourceWidth_ && sourceHeight == sourceHeight_ &&
            targetWidth == targetWidth_ && targetHeight == targetHeight_ && inputTexture_) {
            return true;
        }
        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        targetWidth_ = targetWidth;
        targetHeight_ = targetHeight;
        passes_.clear();
        inputTexture_.Reset();
        inputSrv_.Reset();

        D3D11_TEXTURE2D_DESC inputDesc{};
        inputDesc.Width = UINT(sourceWidth);
        inputDesc.Height = UINT(sourceHeight);
        inputDesc.MipLevels = 1;
        inputDesc.ArraySize = 1;
        inputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        inputDesc.SampleDesc.Count = 1;
        inputDesc.Usage = D3D11_USAGE_DEFAULT;
        inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&inputDesc, nullptr, &inputTexture_);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 input texture creation failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateShaderResourceView(inputTexture_.Get(), nullptr, &inputSrv_);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 input SRV creation failed " + hresultHex(hr));
            return false;
        }

        int currentWidth = sourceWidth;
        int currentHeight = sourceHeight;
        while (currentWidth != targetWidth || currentHeight != targetHeight) {
            const int nextWidth = std::min(targetWidth, currentWidth * 2);
            const int nextHeight = std::min(targetHeight, currentHeight * 2);
            if (nextWidth <= currentWidth && nextHeight <= currentHeight) break;
            PassResource pass{};
            pass.width = nextWidth;
            pass.height = nextHeight;
            if (!createPassResource(pass)) return false;
            passes_.push_back(std::move(pass));
            currentWidth = nextWidth;
            currentHeight = nextHeight;
        }
        if (currentWidth != targetWidth || currentHeight != targetHeight) {
            logFailureOnce("D3D11/NIS presenter only supports host upscaling targets");
            return false;
        }
        spdlog::info("host presenter D3D11/NIS resources source={}x{} target={}x{} passes={}",
                     sourceWidth, sourceHeight, targetWidth, targetHeight, passes_.size());
        return true;
    }

    bool createPassResource(PassResource& pass) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(pass.width);
        desc.Height = UINT(pass.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &pass.texture);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 NIS pass texture creation failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateShaderResourceView(pass.texture.Get(), nullptr, &pass.srv);
        if (FAILED(hr)) {
            logFailureOnce("D3D11 NIS pass SRV creation failed " + hresultHex(hr));
            return false;
        }
        hr = device_->CreateUnorderedAccessView(pass.texture.Get(), nullptr, &pass.uav);
        if (FAILED(hr)) logFailureOnce("D3D11 NIS pass UAV creation failed " + hresultHex(hr));
        return SUCCEEDED(hr);
    }

    void logFailureOnce(const std::string& message) {
        if (failureLogged_) return;
        failureLogged_ = true;
        spdlog::warn("{}", message);
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> nisShader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> blitVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blitPs_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> coefScalerSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> coefUsmSrv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> configBuffer_;
    std::vector<PassResource> passes_;
    int swapChainWidth_{};
    int swapChainHeight_{};
    int sourceWidth_{};
    int sourceHeight_{};
    int targetWidth_{};
    int targetHeight_{};
    int blockWidth_{32};
    int blockHeight_{24};
    int threadGroupSize_{128};
    bool failureLogged_{};
};
#endif

struct HostPresenterWindow {
    SyntheticDllRuntime* runtime{};
    uint32_t guestHwnd{};
    uint32_t* framebuffer{};
    int width{};
    int height{};
    int targetWidth{};
    int targetHeight{};
#if defined(_WIN32)
    std::unique_ptr<HostPresenterD3D11> d3d11;
    bool d3d11Unavailable{};
#endif
};

int hostPresenterDisplayWidth(const HostPresenterWindow& presenter) {
    return std::max(1, presenter.targetWidth > 0 ? presenter.targetWidth : presenter.width);
}

int hostPresenterDisplayHeight(const HostPresenterWindow& presenter) {
    return std::max(1, presenter.targetHeight > 0 ? presenter.targetHeight : presenter.height);
}

bool hostPresenterUpscalingEnabled(const HostPresenterWindow& presenter) {
    return presenter.targetWidth >= 0 && presenter.targetHeight >= 0;
}

RECT hostPresenterImageRect(const HostPresenterWindow& presenter, int clientWidth, int clientHeight) {
    clientWidth = std::max(1, clientWidth);
    clientHeight = std::max(1, clientHeight);
    const int sourceWidth = std::max(1, presenter.width);
    const int sourceHeight = std::max(1, presenter.height);

    int drawWidth = clientWidth;
    int drawHeight = MulDiv(drawWidth, sourceHeight, sourceWidth);
    if (drawHeight > clientHeight) {
        drawHeight = clientHeight;
        drawWidth = MulDiv(drawHeight, sourceWidth, sourceHeight);
    }
    drawWidth = std::clamp(drawWidth, 1, clientWidth);
    drawHeight = std::clamp(drawHeight, 1, clientHeight);

    const int left = (clientWidth - drawWidth) / 2;
    const int top = (clientHeight - drawHeight) / 2;
    return RECT{left, top, left + drawWidth, top + drawHeight};
}

bool hostPresenterMapPointToGuest(const HostPresenterWindow& presenter,
                                  int hostX,
                                  int hostY,
                                  int clientWidth,
                                  int clientHeight,
                                  bool clampOutside,
                                  int& guestX,
                                  int& guestY) {
    if (presenter.width <= 0 || presenter.height <= 0) return false;
    const RECT image = hostPresenterImageRect(presenter, clientWidth, clientHeight);
    const int drawWidth = std::max(1L, image.right - image.left);
    const int drawHeight = std::max(1L, image.bottom - image.top);
    int relX = hostX - image.left;
    int relY = hostY - image.top;
    if (!clampOutside && (relX < 0 || relY < 0 || relX >= drawWidth || relY >= drawHeight)) {
        return false;
    }
    relX = std::clamp(relX, 0, drawWidth - 1);
    relY = std::clamp(relY, 0, drawHeight - 1);
    guestX = std::clamp(MulDiv(relX, presenter.width, drawWidth), 0, presenter.width - 1);
    guestY = std::clamp(MulDiv(relY, presenter.height, drawHeight), 0, presenter.height - 1);
    return true;
}

DWORD hostPresenterWindowStyle() {
    return WS_OVERLAPPEDWINDOW;
}

DWORD hostPresenterWindowExStyle() {
    return 0;
}

RECT hostPresenterOuterRectForClient(const HostPresenterWindow& presenter) {
    RECT rect{0, 0, hostPresenterDisplayWidth(presenter), hostPresenterDisplayHeight(presenter)};
    AdjustWindowRectEx(&rect, hostPresenterWindowStyle(), FALSE, hostPresenterWindowExStyle());
    return rect;
}

int hostPresenterOuterWidth(const HostPresenterWindow& presenter) {
    const RECT rect = hostPresenterOuterRectForClient(presenter);
    return std::max(1L, rect.right - rect.left);
}

int hostPresenterOuterHeight(const HostPresenterWindow& presenter) {
    const RECT rect = hostPresenterOuterRectForClient(presenter);
    return std::max(1L, rect.bottom - rect.top);
}

void presentHostWindowNow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    GdiFlush();
}

const wchar_t* hostPresenterClassName() {
    return L"FakeCEHostPresenterWindow";
}

const wchar_t* hostPresenterWindowTitle() {
    return L"iNavi Emulator @ Windows CE 6.0";
}

HICON hostPresenterIcon(bool useSmallIcon) {
    const int width = GetSystemMetrics(useSmallIcon ? SM_CXSMICON : SM_CXICON);
    const int height = GetSystemMetrics(useSmallIcon ? SM_CYSMICON : SM_CYICON);
    return reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr),
                   MAKEINTRESOURCEW(IDI_INAVI_APP),
                   IMAGE_ICON,
                   width,
                   height,
                   LR_SHARED));
}

void applyHostPresenterIcons(HWND hwnd) {
    if (HICON icon = hostPresenterIcon(false)) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
    if (HICON icon = hostPresenterIcon(true)) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
}

LRESULT CALLBACK hostPresenterWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_GETMINMAXINFO) {
        if (presenter && presenter->width > 0 && presenter->height > 0) {
            RECT minClient{0, 0, presenter->width, presenter->height};
            AdjustWindowRectEx(&minClient, hostPresenterWindowStyle(), FALSE, hostPresenterWindowExStyle());
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = std::max<LONG>(1, minClient.right - minClient.left);
            info->ptMinTrackSize.y = std::max<LONG>(1, minClient.bottom - minClient.top);
            return 0;
        }
    }
    if (message == WM_SIZE) {
        if (presenter && wParam != SIZE_MINIMIZED) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_PAINT) {
        if (presenter && presenter->framebuffer && presenter->width > 0 && presenter->height > 0) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientWidth = std::max(1L, client.right - client.left);
            const int clientHeight = std::max(1L, client.bottom - client.top);
            const RECT image = hostPresenterImageRect(*presenter, clientWidth, clientHeight);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = presenter->width;
            info.bmiHeader.biHeight = -presenter->height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            auto drawGdi = [&] (HDC dc) {
                auto fillBand = [&](const RECT& rect) {
                    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
                    HBRUSH blackBrush = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                    FillRect(dc, &rect, blackBrush);
                };
                fillBand(RECT{client.left, client.top, client.right, image.top});
                fillBand(RECT{client.left, image.bottom, client.right, client.bottom});
                fillBand(RECT{client.left, image.top, image.left, image.bottom});
                fillBand(RECT{image.right, image.top, client.right, image.bottom});
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchDIBits(dc, image.left, image.top, image.right - image.left, image.bottom - image.top,
                              0, 0, presenter->width, presenter->height,
                              presenter->framebuffer, &info, DIB_RGB_COLORS, SRCCOPY);
                GdiFlush();
            };
            static const bool disableD3dNis = envFlagEnabled("INAVI_EMU_DISABLE_D3D_NIS");
            if (hostPresenterUpscalingEnabled(*presenter) &&
                !presenter->d3d11Unavailable && !disableD3dNis) {
                PAINTSTRUCT paint{};
                HDC paintDc = BeginPaint(hwnd, &paint);
                (void)paintDc;
                EndPaint(hwnd, &paint);
                if (!presenter->d3d11) presenter->d3d11 = std::make_unique<HostPresenterD3D11>();
                if (presenter->d3d11->render(hwnd, presenter->framebuffer, presenter->width, presenter->height,
                                             clientWidth, clientHeight, image)) {
                    return 0;
                }
                presenter->d3d11Unavailable = true;
                HDC fallbackDc = GetDC(hwnd);
                if (fallbackDc) {
                    drawGdi(fallbackDc);
                    ReleaseDC(hwnd, fallbackDc);
                }
                return 0;
            }
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            drawGdi(dc);
            EndPaint(hwnd, &paint);
        } else {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
        }
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if ((message == WM_MOUSEMOVE && (wParam & MK_LBUTTON)) ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        if (presenter && presenter->runtime) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientWidth = std::max(1L, client.right - client.left);
            const int clientHeight = std::max(1L, client.bottom - client.top);
            const int hostX = int16_t(LOWORD(lParam));
            const int hostY = int16_t(HIWORD(lParam));
            int x = 0;
            int y = 0;
            const bool pointerCaptured = GetCapture() == hwnd;
            if (!hostPresenterMapPointToGuest(*presenter, hostX, hostY, clientWidth, clientHeight,
                                              pointerCaptured, x, y)) {
                return 0;
            }
            uint32_t guestMessage = 0;
            if (message == WM_LBUTTONDOWN) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                guestMessage = 0x0201; // WM_LBUTTONDOWN
            } else if (message == WM_LBUTTONUP) {
                ReleaseCapture();
                guestMessage = 0x0202; // WM_LBUTTONUP
            } else {
                guestMessage = 0x0200; // WM_MOUSEMOVE
            }
            presenter->runtime->queueHostMouseMessage(presenter->guestHwnd, guestMessage, x, y);
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        delete presenter;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

ATOM registerHostPresenterClass() {
    static ATOM atom = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = hostPresenterWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = hostPresenterIcon(false);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = hostPresenterClassName();
        return RegisterClassW(&wc);
    }();
    return atom;
}
}

uint32_t SyntheticDllRuntime::makeGuestWindow(const std::string& className, const std::string& title,
                                              uint32_t style, uint32_t exStyle, uint32_t parent,
                                              uint32_t menu, uint32_t instance, uint32_t param,
                                              int32_t x, int32_t y, int32_t width, int32_t height,
                                              bool visible, uint32_t wndProc) {
    const uint32_t hwnd = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
    GuestWindow window{};
    window.hwnd = hwnd;
    window.className = lowerAscii(className);
    window.title = title;
    window.style = style;
    window.exStyle = exStyle;
    window.parent = parent;
    window.menu = menu;
    window.instance = instance;
    window.param = param;
    window.wndProc = wndProc;
    window.ownerThread = ceKernel_.activeGuestThread() ? ceKernel_.activeGuestThread() : ceKernel_.mainThreadPseudoHandle();
    window.zOrder = nextWindowZOrder();
    window.x = x;
    window.y = y;
    window.width = std::max<int32_t>(1, width);
    window.height = std::max<int32_t>(1, height);
    window.visible = visible;
    ceGwe_.windows()[hwnd] = window;
    ceGwe_.registerWindowOwner(hwnd, window.ownerThread);
    ensureHostWindow(hwnd, ceGwe_.windows()[hwnd]);
    publishGuestWindowState(hwnd);
    return hwnd;
}

uint64_t SyntheticDllRuntime::nextWindowZOrder() {
    return ++windowZOrder_;
}

uint64_t SyntheticDllRuntime::windowZOrder(uint32_t hwnd) const {
    const auto it = ceGwe_.windows().find(hwnd);
    return it == ceGwe_.windows().end() ? 0 : it->second.zOrder;
}

std::vector<uint32_t> SyntheticDllRuntime::orderedWindowsTopToBottom() const {
    std::vector<uint32_t> ordered;
    ordered.reserve(ceGwe_.windows().size());
    for (const auto& [hwnd, _] : ceGwe_.windows()) ordered.push_back(hwnd);
    std::sort(ordered.begin(), ordered.end(), [&](uint32_t left, uint32_t right) {
        const auto leftIt = ceGwe_.windows().find(left);
        const auto rightIt = ceGwe_.windows().find(right);
        const uint64_t leftZ = leftIt == ceGwe_.windows().end() ? 0 : leftIt->second.zOrder;
        const uint64_t rightZ = rightIt == ceGwe_.windows().end() ? 0 : rightIt->second.zOrder;
        if (leftZ != rightZ) return leftZ > rightZ;
        return left > right;
    });
    return ordered;
}

std::vector<uint32_t> SyntheticDllRuntime::orderedSiblingWindows(uint32_t parent,
                                                                 bool childWindow) const {
    std::vector<uint32_t> siblings;
    for (const auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.destroyed && window.parent == parent &&
            ((window.style & kWindowStyleChild) != 0) == childWindow) {
            siblings.push_back(hwnd);
        }
    }
    std::sort(siblings.begin(), siblings.end(), [&](uint32_t left, uint32_t right) {
        const auto leftIt = ceGwe_.windows().find(left);
        const auto rightIt = ceGwe_.windows().find(right);
        const uint64_t leftZ = leftIt == ceGwe_.windows().end() ? 0 : leftIt->second.zOrder;
        const uint64_t rightZ = rightIt == ceGwe_.windows().end() ? 0 : rightIt->second.zOrder;
        if (leftZ != rightZ) return leftZ > rightZ;
        return left > right;
    });
    return siblings;
}

void SyntheticDllRuntime::ensureHostWindow(uint32_t guestHwnd, GuestWindow& window) {
#if defined(_WIN32)
    if (window.destroyed) return;
    if (window.parent || !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    if (hostPresenterGuestHwnd_ && hostPresenterGuestHwnd_ != guestHwnd) return;
    if (!window.hostHwnd) {
        if (!registerHostPresenterClass()) {
            spdlog::warn("host presenter RegisterClassW failed error={}", GetLastError());
            return;
        }
        auto* presenter = new HostPresenterWindow{this, guestHwnd, framebuffer_, framebufferWidth_, framebufferHeight_,
                                                  hostPresenterTargetWidth_, hostPresenterTargetHeight_};
        HWND hwnd = CreateWindowExW(hostPresenterWindowExStyle(), hostPresenterClassName(), hostPresenterWindowTitle(),
                                    hostPresenterWindowStyle(),
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    hostPresenterOuterWidth(*presenter), hostPresenterOuterHeight(*presenter),
                                    nullptr, nullptr, GetModuleHandleW(nullptr), presenter);
        if (!hwnd) {
            spdlog::warn("host presenter CreateWindowExW failed guest=0x{:08x} error={}", guestHwnd, GetLastError());
            delete presenter;
            return;
        }
        SetWindowTextW(hwnd, hostPresenterWindowTitle());
        applyHostPresenterIcons(hwnd);
        hostPresenterGuestHwnd_ = guestHwnd;
        window.hostHwnd = reinterpret_cast<uintptr_t>(hwnd);
        spdlog::info("created host presenter HWND={} for guest HWND=0x{:08x} guest={}x{} framebuffer={}x{}",
                     static_cast<void*>(hwnd), guestHwnd, window.width, window.height,
                     presenter->width, presenter->height);
    }
    HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
    if (window.visible) ShowWindow(hwnd, SW_SHOWNORMAL);
    presentHostWindows(true);
#else
    (void)guestHwnd;
    (void)window;
#endif
}

void SyntheticDllRuntime::destroyHostWindow(GuestWindow& window) {
#if defined(_WIN32)
    if (window.hostHwnd) {
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (IsWindow(hwnd)) {
            uint32_t replacementHwnd = 0;
            GuestWindow* replacementWindow = nullptr;
            for (auto it = ceGwe_.windows().rbegin(); it != ceGwe_.windows().rend(); ++it) {
                GuestWindow& candidate = it->second;
                if (candidate.hwnd == window.hwnd || candidate.destroyed || !candidate.visible || candidate.hostHwnd) {
                    continue;
                }
                const bool coversFramebuffer =
                    candidate.width >= framebufferWidth_ && candidate.height >= framebufferHeight_;
                const bool ownedByDestroyedWindow = candidate.parent == window.hwnd;
                if (!replacementWindow || coversFramebuffer || ownedByDestroyedWindow) {
                    replacementHwnd = candidate.hwnd;
                    replacementWindow = &candidate;
                    if (coversFramebuffer || ownedByDestroyedWindow) break;
                }
            }
            if (replacementWindow) {
                auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (presenter) presenter->guestHwnd = replacementHwnd;
                replacementWindow->hostHwnd = window.hostHwnd;
                window.hostHwnd = 0;
                hostPresenterGuestHwnd_ = replacementHwnd;
                SetWindowTextW(hwnd, hostPresenterWindowTitle());
                spdlog::info("transferred host presenter HWND={} from destroyed guest HWND=0x{:08x} to live guest HWND=0x{:08x}",
                             static_cast<void*>(hwnd), window.hwnd, replacementHwnd);
                presentHostWindows(true);
                return;
            }
            SetWindowTextW(hwnd, hostPresenterWindowTitle());
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindows(true);
            retainedHostWindows_.push_back(window.hostHwnd);
            if (hostPresenterGuestHwnd_ == window.hwnd) hostPresenterGuestHwnd_ = 0;
        }
        window.hostHwnd = 0;
    }
#else
    (void)window;
#endif
}

void SyntheticDllRuntime::syncHostWindowPlacement(GuestWindow& window, bool present) {
#if defined(_WIN32)
    if (!window.hostHwnd) return;
    HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
    if (!IsWindow(hwnd)) return;
    auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const int hostWidth = presenter ? hostPresenterOuterWidth(*presenter) : std::max(1, window.width);
    const int hostHeight = presenter ? hostPresenterOuterHeight(*presenter) : std::max(1, window.height);
    SetWindowPos(hwnd, nullptr, window.x, window.y, hostWidth, hostHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, window.visible ? SW_SHOWNORMAL : SW_HIDE);
    if (present) presentHostWindows(true);
#else
    (void)window;
    (void)present;
#endif
}

void SyntheticDllRuntime::presentHostWindows(bool force) {
#if defined(_WIN32)
    if (hostPresentDeferDepth_ > 0) {
        hostPresentDirty_ = true;
        return;
    }
    if (!force && !hostPresentDirty_) return;
    const uint64_t now = hostTickMilliseconds();
    constexpr uint64_t kMinPresentIntervalMs = 16;
    if (!force && lastHostPresentMs_ && now - lastHostPresentMs_ < kMinPresentIntervalMs) return;
    hostPresentDirty_ = false;
    lastHostPresentMs_ = now;
    for (auto& [guestHwnd, window] : ceGwe_.windows()) {
        (void)guestHwnd;
        if (!window.hostHwnd) continue;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (!IsWindow(hwnd)) continue;
        presentHostWindowNow(hwnd);
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (!hwnd || !IsWindow(hwnd)) continue;
        presentHostWindowNow(hwnd);
    }
#else
    (void)force;
#endif
}

void SyntheticDllRuntime::invalidateHostWindows() {
    hostPresentDirty_ = true;
}

void SyntheticDllRuntime::beginHostUiBatchPresentDeferral() {
    if (hostPresentUiBatchActive_) return;
    hostPresentUiBatchActive_ = true;
    hostPresentUiBatchStartMs_ = hostTickMilliseconds();
    ++hostPresentDeferDepth_;
    hostPresentDirty_ = true;
}

void SyntheticDllRuntime::releaseHostUiBatchPresentDeferral() {
    if (!hostPresentUiBatchActive_) return;
    hostPresentUiBatchActive_ = false;
    hostPresentUiBatchStartMs_ = 0;
    if (hostPresentDeferDepth_ > 0) --hostPresentDeferDepth_;
    presentHostWindows(true);
}

void SyntheticDllRuntime::flushHostUiBatchPresentDeferral(uint64_t maxDeferredMs) {
    if (!hostPresentUiBatchActive_) return;
    const uint64_t now = hostTickMilliseconds();
    if (hostPresentUiBatchStartMs_ && now - hostPresentUiBatchStartMs_ < maxDeferredMs) return;
    releaseHostUiBatchPresentDeferral();
    if (ceGwe_.hasMessages() || !ceGwe_.pendingMessageTransfers().empty()) {
        beginHostUiBatchPresentDeferral();
    }
}

bool SyntheticDllRuntime::beginHostErasePresentDeferral(uint32_t hwnd) {
    if (!hwnd) return false;
    const auto [_, inserted] = hostPresentDeferredEraseHwnds_.insert(hwnd);
    if (inserted) {
        ++hostPresentDeferDepth_;
        hostPresentDirty_ = true;
    }
    return inserted;
}

bool SyntheticDllRuntime::hasHostErasePresentDeferral(uint32_t hwnd) const {
    return hwnd && hostPresentDeferredEraseHwnds_.contains(hwnd);
}

void SyntheticDllRuntime::releaseHostErasePresentDeferral(uint32_t hwnd) {
    if (!hwnd) return;
    if (hostPresentDeferredEraseHwnds_.erase(hwnd) && hostPresentDeferDepth_ > 0) {
        --hostPresentDeferDepth_;
    }
}

void SyntheticDllRuntime::queueGuestPaint(uint32_t hwnd, bool erase) {
    auto it = ceGwe_.windows().find(hwnd);
    if (!hwnd || it == ceGwe_.windows().end() || it->second.destroyed || !it->second.visible) return;
    const auto hasQueued = [&](uint32_t message) {
        return ceGwe_.anyMessage([&](const GuestMessage& queued) {
            return queued.hwnd == hwnd && queued.message == message;
        });
    };
    if (erase) {
        if (!hasQueued(kGuestWmEraseBkgnd)) {
            GuestMessage eraseMessage{};
            eraseMessage.hwnd = hwnd;
            eraseMessage.message = kGuestWmEraseBkgnd;
            eraseMessage.wParam = makeGuestDc(hwnd);
            applyPaintUpdateClip(hwnd, eraseMessage.wParam);
            eraseMessage.time = uint32_t(++tick_ * 16);
            ceGwe_.postPostedMessage(eraseMessage);
        }
    }
    if (!hasQueued(kGuestWmPaint)) {
        GuestMessage paint{};
        paint.hwnd = hwnd;
        paint.message = kGuestWmPaint;
        paint.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(paint);
    }
    invalidateHostWindows();
}

size_t SyntheticDllRuntime::discardQueuedWindowUpdateMessages(uint32_t hwnd) {
    if (!hwnd) return 0;
    auto isSameOrDescendant = [&](uint32_t candidate) {
        for (uint32_t current = candidate; current;) {
            if (current == hwnd) return true;
            auto window = ceGwe_.windows().find(current);
            if (window == ceGwe_.windows().end()) break;
            current = window->second.parent;
        }
        return false;
    };
    return ceGwe_.eraseIf([&](const GuestMessage& message) {
        if (!isSameOrDescendant(message.hwnd)) return false;
        return message.message == kGuestWmPaint ||
               message.message == kGuestWmEraseBkgnd ||
               message.message == kGuestWmShowWindow;
    });
}

void SyntheticDllRuntime::prioritizeQueuedWindowMessages(uint32_t hwnd) {
    std::deque<GuestMessage> selected =
        ceGwe_.takeIf([&](const GuestMessage& message) { return message.hwnd == hwnd; });
    while (!selected.empty()) {
        ceGwe_.postFront(selected.back());
        selected.pop_back();
    }
}

void SyntheticDllRuntime::queueVisibleFullScreenPopupPaint(uint32_t hwnd) {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end() || it->second.destroyed || !it->second.visible ||
        !isOwnedPopupWindow(hwnd) || !guestWindowCoversFramebuffer(hwnd)) {
        return;
    }
    const bool replacesOlderFullScreenPopup = std::any_of(ceGwe_.windows().begin(), ceGwe_.windows().end(),
                                                          [&](const auto& entry) {
                                                              const uint32_t otherHwnd = entry.first;
                                                              const GuestWindow& other = entry.second;
                                                              return windowZOrder(otherHwnd) < windowZOrder(hwnd) &&
                                                                     !other.destroyed && other.visible &&
                                                                     isOwnedPopupWindow(otherHwnd) &&
                                                                     guestWindowCoversFramebuffer(otherHwnd);
                                                          });
    if (!replacesOlderFullScreenPopup) return;

    retireOlderFullScreenOwnedPopupsForPopup(hwnd);
    const bool hasShow = ceGwe_.anyMessage([&](const GuestMessage& message) {
        return message.hwnd == hwnd && message.message == kGuestWmShowWindow;
    });
    if (!hasShow) {
        GuestMessage show{};
        show.hwnd = hwnd;
        show.message = kGuestWmShowWindow;
        show.wParam = 1;
        show.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(show);
    }
    queueGuestPaint(hwnd, true);
    prioritizeQueuedWindowMessages(hwnd);
    spdlog::info("prioritized visible full-screen owned popup paint hwnd=0x{:08x}", hwnd);
}

void SyntheticDllRuntime::queueVisiblePopupPaint(uint32_t hwnd) {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end() || it->second.destroyed || !it->second.visible ||
        (it->second.style & kWindowStyleChild) || it->second.width <= 0 || it->second.height <= 0) {
        return;
    }
    constexpr uint32_t kWindowStylePopup = 0x80000000u;
    const bool ownedPopup = isOwnedPopupWindow(hwnd);
    const bool topLevelPopup = !it->second.parent && (it->second.style & kWindowStylePopup);
    if (!ownedPopup && !topLevelPopup) return;
    if (ownedPopup && guestWindowCoversFramebuffer(hwnd)) return;

    const bool hasShow = ceGwe_.anyMessage([&](const GuestMessage& message) {
        return message.hwnd == hwnd && message.message == kGuestWmShowWindow;
    });
    if (!hasShow) {
        GuestMessage show{};
        show.hwnd = hwnd;
        show.message = kGuestWmShowWindow;
        show.wParam = 1;
        show.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(show);
    }
    queueGuestPaint(hwnd, true);
    size_t discardedMouseMoves = 0;
    ceGwe_.eraseIf([&](const GuestMessage& message) {
        if (message.message != 0x0200) return false;
        if (message.hwnd == hwnd || isWindowInOwnedPopupStack(message.hwnd, hwnd)) {
            return false;
        }
        ++discardedMouseMoves;
        return true;
    });
    prioritizeQueuedWindowMessages(hwnd);
    spdlog::info("queued visible popup paint hwnd=0x{:08x} discardedMouseMoves={}",
                 hwnd, discardedMouseMoves);
}

void SyntheticDllRuntime::queueVisiblePopupPaintsAbove(uint32_t hwnd) {
    std::vector<uint32_t> popups;
    for (const auto& [popupHwnd, window] : ceGwe_.windows()) {
        if (windowZOrder(popupHwnd) <= windowZOrder(hwnd) || window.destroyed || !window.visible ||
            (window.style & kWindowStyleChild) || window.width <= 0 || window.height <= 0) {
            continue;
        }
        constexpr uint32_t kWindowStylePopup = 0x80000000u;
        const bool ownedPopup = isOwnedPopupWindow(popupHwnd);
        const bool topLevelPopup = !window.parent && (window.style & kWindowStylePopup);
        if (!ownedPopup && !topLevelPopup) continue;
        popups.push_back(popupHwnd);
    }

    if (popups.empty()) return;
    for (uint32_t popupHwnd : popups) {
        queueGuestPaint(popupHwnd, true);
    }
    for (auto it = popups.rbegin(); it != popups.rend(); ++it) {
        prioritizeQueuedWindowMessages(*it);
    }
    spdlog::info("queued {} visible popup repaint(s) above hwnd=0x{:08x}",
                 popups.size(), hwnd);
}

std::pair<int32_t, int32_t> SyntheticDllRuntime::guestWindowOrigin(uint32_t hwnd) const {
    int32_t x = 0;
    int32_t y = 0;
    for (uint32_t current = hwnd; current;) {
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        x += it->second.x;
        y += it->second.y;
        current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
    }
    return {x, y};
}

void SyntheticDllRuntime::noteGuestWindowPaint(uint32_t hwnd,
                                               int32_t left,
                                               int32_t top,
                                               int32_t right,
                                               int32_t bottom) {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end() || it->second.destroyed || left >= right || top >= bottom) return;
    GuestWindow& window = it->second;
    if (!window.paintBoundsValid) {
        window.paintLeft = left;
        window.paintTop = top;
        window.paintRight = right;
        window.paintBottom = bottom;
        window.paintBoundsValid = true;
        return;
    }
    window.paintLeft = std::min(window.paintLeft, left);
    window.paintTop = std::min(window.paintTop, top);
    window.paintRight = std::max(window.paintRight, right);
    window.paintBottom = std::max(window.paintBottom, bottom);
}

void SyntheticDllRuntime::captureGuestWindowBacking(uint32_t hwnd) {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end()) return;
    if (it->second.destroyed || !it->second.visible) return;
    const bool childWindow = (it->second.style & kWindowStyleChild) != 0;
    const bool ownedPopup = isOwnedPopupWindow(hwnd);
    const bool smallTopLevelPopup = isTopLevelPopupWindow(hwnd) && !guestWindowCoversFramebuffer(hwnd);
    const bool fullScreenPopup = ownedPopup && guestWindowCoversFramebuffer(hwnd);
    if (!fullScreenPopup && coveringFullScreenOwnedPopup(hwnd)) return;
    if (fullScreenPopup) {
        retireOlderFullScreenOwnedPopupsForPopup(hwnd);
    }
    const uint32_t visualParent = (childWindow || ownedPopup) ? it->second.parent : 0;
    if ((!visualParent && !smallTopLevelPopup) || it->second.backingValid ||
        !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        it->second.width <= 0 || it->second.height <= 0) {
        return;
    }
    if (visualParent) {
        auto parent = ceGwe_.windows().find(visualParent);
        if (parent == ceGwe_.windows().end()) return;
        if (childWindow && !parent->second.parent) return;
    }

    const auto [originX, originY] = guestWindowOrigin(hwnd);
    const int32_t left = std::clamp<int32_t>(originX, 0, framebufferWidth_);
    const int32_t top = std::clamp<int32_t>(originY, 0, framebufferHeight_);
    const int32_t right = std::clamp<int32_t>(originX + it->second.width, 0, framebufferWidth_);
    const int32_t bottom = std::clamp<int32_t>(originY + it->second.height, 0, framebufferHeight_);
    if (left >= right || top >= bottom) return;

    const int32_t width = right - left;
    const int32_t height = bottom - top;
    std::vector<uint32_t> pixels(size_t(width) * size_t(height));
    for (int32_t y = 0; y < height; ++y) {
        const uint32_t* src = framebuffer_ + size_t(top + y) * size_t(framebufferWidth_) + size_t(left);
        std::copy(src, src + width, pixels.begin() + size_t(y) * size_t(width));
    }
    it->second.backingX = left;
    it->second.backingY = top;
    it->second.backingWidth = width;
    it->second.backingHeight = height;
    it->second.backingPixels = std::move(pixels);
    it->second.backingValid = true;
    spdlog::info("captured guest window backing hwnd=0x{:08x} rect={},{} {}x{}",
                 hwnd, left, top, width, height);
}

bool SyntheticDllRuntime::guestWindowCoversFramebuffer(uint32_t hwnd) const {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end() || it->second.destroyed || !framebuffer_ ||
        framebufferWidth_ <= 0 || framebufferHeight_ <= 0) {
        return false;
    }
    const auto [x, y] = guestWindowOrigin(hwnd);
    return x <= 0 && y <= 0 &&
           x + it->second.width >= framebufferWidth_ &&
           y + it->second.height >= framebufferHeight_;
}

bool SyntheticDllRuntime::isWindowInOwnedPopupStack(uint32_t hwnd, uint32_t ancestor) const {
    for (uint32_t current = hwnd; current;) {
        if (current == ancestor) return true;
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        current = it->second.parent;
    }
    return false;
}

uint32_t SyntheticDllRuntime::inferredWindowOwner(uint32_t hwnd) const {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end() || it->second.destroyed) return 0;
    const GuestWindow& window = it->second;
    if (window.style & kWindowStyleChild) return 0;
    if (window.parent) return window.parent;
    constexpr uint32_t kWindowStylePopup = 0x80000000u;
    if ((window.style & kWindowStylePopup) == 0) return 0;

    uint32_t best = 0;
    uint64_t bestZ = 0;
    for (const auto& [candidateHwnd, candidate] : ceGwe_.windows()) {
        if (candidateHwnd == hwnd || candidate.destroyed || !candidate.visible) continue;
        if (window.ownerThread && candidate.ownerThread && candidate.ownerThread != window.ownerThread) continue;
        if (candidate.parent || (candidate.style & kWindowStyleChild)) continue;
        if (candidate.style & kWindowStylePopup) continue;
        if (window.zOrder && candidate.zOrder > window.zOrder) continue;
        if (!best || candidate.zOrder >= bestZ) {
            best = candidateHwnd;
            bestZ = candidate.zOrder;
        }
    }
    if (best) return best;
    for (const auto& [candidateHwnd, candidate] : ceGwe_.windows()) {
        if (candidateHwnd == hwnd || candidate.destroyed || !candidate.visible) continue;
        if (window.ownerThread && candidate.ownerThread && candidate.ownerThread != window.ownerThread) continue;
        if (candidate.style & kWindowStyleChild) continue;
        if (candidate.style & kWindowStylePopup) continue;
        if (window.zOrder && candidate.zOrder > window.zOrder) continue;
        if (!best || candidate.zOrder >= bestZ) {
            best = candidateHwnd;
            bestZ = candidate.zOrder;
        }
    }
    return best;
}

uint32_t SyntheticDllRuntime::rootWindowForStack(uint32_t hwnd) const {
    uint32_t root = hwnd;
    uint32_t current = hwnd;
    for (size_t guard = 0; current && guard < ceGwe_.windows().size() + 1; ++guard) {
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        root = current;
        if (it->second.style & kWindowStyleChild) {
            current = it->second.parent;
        } else {
            current = inferredWindowOwner(current);
        }
    }
    return root;
}

bool SyntheticDllRuntime::isWindowInGweStack(uint32_t hwnd, uint32_t ancestor) const {
    if (!hwnd || !ancestor) return false;
    if (ceGwe_.isWindowInStack(hwnd, ancestor)) return true;
    for (uint32_t current = hwnd; current;) {
        if (current == ancestor) return true;
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        current = (it->second.style & kWindowStyleChild) ? it->second.parent : inferredWindowOwner(current);
    }
    return false;
}

size_t SyntheticDllRuntime::discardQueuedPointerMessagesForWindowStack(uint32_t hwnd) {
    const size_t discarded = ceGwe_.eraseIf([&](const GuestMessage& message) {
        return message.message >= 0x0200 && message.message <= 0x0202 &&
               isWindowInGweStack(message.hwnd, hwnd);
    });
    if (isWindowInGweStack(capturedWindow_, hwnd)) capturedWindow_ = 0;
    if (isWindowInGweStack(hostPointerCaptureWindow_, hwnd)) hostPointerCaptureWindow_ = 0;
    if (isWindowInGweStack(pendingSyntheticChildButtonUpWindow_, hwnd)) {
        pendingSyntheticChildButtonUpWindow_ = 0;
    }
    return discarded;
}

uint32_t SyntheticDllRuntime::repaintOwnerAfterStackChange(uint32_t hwnd, bool eraseHiddenWindow) {
    auto it = ceGwe_.windows().find(hwnd);
    if (it == ceGwe_.windows().end()) return 0;
    const uint32_t owner = inferredWindowOwner(hwnd);
    const uint32_t parent = it->second.parent;
    const uint32_t repaintTarget = parent ? parent : owner;
    if (eraseHiddenWindow) eraseGuestWindowArea(hwnd, it->second);
    if (repaintTarget) {
        spdlog::debug("queued owner-stack repaint target=0x{:08x} after window=0x{:08x}",
                      repaintTarget, hwnd);
        queueGuestPaint(repaintTarget, true);
        if ((it->second.style & kWindowStyleChild) == 0 || owner) {
            queueVisiblePopupPaintsAbove(repaintTarget);
        }
    }
    return repaintTarget;
}

uint32_t SyntheticDllRuntime::coveringFullScreenOwnedPopup(uint32_t hwnd) const {
    auto isOwnerChildDescendant = [&](uint32_t targetHwnd, uint32_t ownerHwnd) {
        bool traversedChild = false;
        for (uint32_t current = targetHwnd; current;) {
            if (current == ownerHwnd) return traversedChild;
            auto it = ceGwe_.windows().find(current);
            if (it == ceGwe_.windows().end() || !(it->second.style & kWindowStyleChild)) break;
            traversedChild = true;
            current = it->second.parent;
        }
        return false;
    };
    for (uint32_t popupHwnd : orderedWindowsTopToBottom()) {
        const auto it = ceGwe_.windows().find(popupHwnd);
        if (it == ceGwe_.windows().end()) continue;
        const GuestWindow& popup = it->second;
        if (popup.destroyed || !popup.visible ||
            !isOwnedPopupWindow(popupHwnd) || !guestWindowCoversFramebuffer(popupHwnd)) {
            continue;
        }
        if (isWindowInOwnedPopupStack(hwnd, popupHwnd)) continue;
        const bool coversByZOrder = windowZOrder(popupHwnd) > windowZOrder(hwnd);
        const bool coversOwnedChild = isOwnerChildDescendant(hwnd, popup.parent);
        if (!coversByZOrder && !coversOwnedChild) continue;
        return popupHwnd;
    }
    return 0;
}

void SyntheticDllRuntime::retireOlderFullScreenOwnedPopupsForPopup(uint32_t popupHwnd) {
    auto target = ceGwe_.windows().find(popupHwnd);
    if (target == ceGwe_.windows().end() || target->second.destroyed || !target->second.visible ||
        !isOwnedPopupWindow(popupHwnd) || !guestWindowCoversFramebuffer(popupHwnd)) {
        return;
    }

    std::vector<uint32_t> retired;
    for (auto& [hwnd, window] : ceGwe_.windows()) {
        if (window.zOrder >= target->second.zOrder || window.destroyed || !window.visible ||
            !isOwnedPopupWindow(hwnd) || !guestWindowCoversFramebuffer(hwnd)) {
            continue;
        }

        window.visible = false;
        window.backingValid = false;
        window.backingPixels.clear();
        retired.push_back(hwnd);
    }

    if (retired.empty()) return;
    ceGwe_.eraseIf([&](const GuestMessage& message) {
        return std::find(retired.begin(), retired.end(), message.hwnd) != retired.end();
    });
    for (uint32_t hwnd : retired) {
        spdlog::info("retired older full-screen owned popup hwnd=0x{:08x} for popup hwnd=0x{:08x}",
                     hwnd, popupHwnd);
    }
}

bool SyntheticDllRuntime::restoreGuestWindowBacking(uint32_t hwnd,
                                                   GuestWindow& window,
                                                   bool allowCoveredByNewer,
                                                   bool presentRestoredFrame) {
    (void)hwnd;
    if (!window.backingValid || window.backingPixels.empty() ||
        !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        window.backingWidth <= 0 || window.backingHeight <= 0) {
        return false;
    }
    if (!allowCoveredByNewer && isOwnedPopupWindow(hwnd) && guestWindowCoversFramebuffer(hwnd)) {
        const auto coversFramebuffer = [&](const GuestWindow& candidate) {
            const auto [x, y] = guestWindowOrigin(candidate.hwnd);
            return x <= 0 && y <= 0 &&
                   x + candidate.width >= framebufferWidth_ &&
                   y + candidate.height >= framebufferHeight_;
        };
        for (const auto& [candidateHwnd, candidate] : ceGwe_.windows()) {
            if (candidate.zOrder > windowZOrder(hwnd) && !candidate.destroyed && candidate.visible &&
                candidate.parent && !(candidate.style & kWindowStyleChild) && coversFramebuffer(candidate)) {
                spdlog::info("skipped stale full-screen owned popup backing restore hwnd=0x{:08x} newer=0x{:08x}",
                             hwnd, candidateHwnd);
                window.backingValid = false;
                window.backingPixels.clear();
                return true;
            }
        }
    }
    if (isOwnedPopupWindow(hwnd) && !guestWindowCoversFramebuffer(hwnd) &&
        coveringFullScreenOwnedPopup(hwnd)) {
        spdlog::info("skipped covered owned popup backing restore hwnd=0x{:08x}", hwnd);
        window.backingValid = false;
        window.backingPixels.clear();
        return true;
    }
    const int32_t left = std::clamp<int32_t>(window.backingX, 0, framebufferWidth_);
    const int32_t top = std::clamp<int32_t>(window.backingY, 0, framebufferHeight_);
    const int32_t right = std::clamp<int32_t>(window.backingX + window.backingWidth, 0, framebufferWidth_);
    const int32_t bottom = std::clamp<int32_t>(window.backingY + window.backingHeight, 0, framebufferHeight_);
    if (left >= right || top >= bottom) {
        window.backingValid = false;
        window.backingPixels.clear();
        return false;
    }
    const int32_t copyWidth = right - left;
    const int32_t copyHeight = bottom - top;
    if (size_t(window.backingWidth) * size_t(window.backingHeight) > window.backingPixels.size()) {
        window.backingValid = false;
        window.backingPixels.clear();
        return false;
    }
    for (int32_t y = 0; y < copyHeight; ++y) {
        const uint32_t* src = window.backingPixels.data() + size_t(y) * size_t(window.backingWidth);
        uint32_t* dst = framebuffer_ + size_t(top + y) * size_t(framebufferWidth_) + size_t(left);
        std::copy(src, src + copyWidth, dst);
    }
    spdlog::info("restored guest window backing hwnd=0x{:08x} rect={},{} {}x{}",
                 hwnd, left, top, copyWidth, copyHeight);
    window.backingValid = false;
    window.backingPixels.clear();
    if (presentRestoredFrame) presentHostWindows(true);
    return true;
}

void SyntheticDllRuntime::eraseGuestWindowArea(uint32_t hwnd, const GuestWindow& window) {
    if (!framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        window.width <= 0 || window.height <= 0) {
        return;
    }
    auto mutableWindow = ceGwe_.windows().find(hwnd);
    const bool childWindow = (window.style & kWindowStyleChild) != 0;
    constexpr uint32_t kWindowStylePopup = 0x80000000u;
    const bool ownerStackPopup =
        !childWindow && (window.style & kWindowStylePopup) &&
        (window.parent || inferredWindowOwner(hwnd));
    if (mutableWindow != ceGwe_.windows().end() && childWindow) {
        mutableWindow->second.backingValid = false;
        mutableWindow->second.backingPixels.clear();
    }
    if (mutableWindow != ceGwe_.windows().end() && ownerStackPopup) {
        if (guestWindowCoversFramebuffer(hwnd) &&
            restoreGuestWindowBacking(hwnd, mutableWindow->second)) {
            return;
        }
        mutableWindow->second.backingValid = false;
        mutableWindow->second.backingPixels.clear();
        spdlog::debug("discarded owner-stack popup backing hwnd=0x{:08x}; owner repaint is authoritative",
                      hwnd);
        return;
    }
    if (mutableWindow != ceGwe_.windows().end() && !childWindow &&
        restoreGuestWindowBacking(hwnd, mutableWindow->second)) {
        return;
    }
    if (isOwnedPopupWindow(hwnd)) {
        spdlog::info("skipped black erase for owned popup hwnd=0x{:08x} without saved backing", hwnd);
        return;
    }
    if (window.parent) {
        auto parent = ceGwe_.windows().find(window.parent);
        if (parent != ceGwe_.windows().end() && !parent->second.parent) return;
    }
    uint32_t pixel = 0xff000000u;
    if (window.parent) {
        auto parent = ceGwe_.windows().find(window.parent);
        if (parent != ceGwe_.windows().end()) {
            auto cls = ceGwe_.windowClassesByName().find(parent->second.className);
            uint32_t brushHandle = 0;
            if (cls != ceGwe_.windowClassesByName().end()) {
                std::memcpy(&brushHandle, cls->second.bytes.data() + 28, sizeof(brushHandle));
            }
            auto brush = ceMgdi_.brushes().find(brushHandle);
            if (brush != ceMgdi_.brushes().end()) pixel = colorRefToPixel(brush->second.colorRef);
        }
    }
    if (pixel == 0) pixel = 0xff000000u;
    const auto [x, y] = guestWindowOrigin(hwnd);
    GuestDc screenDc{};
    screenDc.hwnd = 0;
    fillFramebufferRect(screenDc, x, y, x + window.width, y + window.height, pixel);
    presentHostWindows(true);
}

bool SyntheticDllRuntime::isWindowOrDescendant(uint32_t hwnd, uint32_t ancestor) const {
    for (uint32_t current = hwnd; current;) {
        if (current == ancestor) return true;
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
    }
    return false;
}

bool SyntheticDllRuntime::isOwnedPopupWindow(uint32_t hwnd) const {
    auto it = ceGwe_.windows().find(hwnd);
    return it != ceGwe_.windows().end() && it->second.parent &&
           !(it->second.style & kWindowStyleChild);
}

bool SyntheticDllRuntime::isTopLevelPopupWindow(uint32_t hwnd) const {
    auto it = ceGwe_.windows().find(hwnd);
    constexpr uint32_t kWindowStylePopup = 0x80000000u;
    return it != ceGwe_.windows().end() && !it->second.parent &&
           (it->second.style & kWindowStylePopup);
}

bool SyntheticDllRuntime::hasCoveringRootPopup(uint32_t hwnd) const {
    auto target = ceGwe_.windows().find(hwnd);
    if (target == ceGwe_.windows().end() || target->second.destroyed || isOwnedPopupWindow(hwnd)) return false;

    uint32_t current = hwnd;
    uint32_t root = hwnd;
    bool nestedChild = false;
    while (current) {
        auto it = ceGwe_.windows().find(current);
        if (it == ceGwe_.windows().end()) break;
        if (it->second.style & kWindowStyleChild) {
            nestedChild = true;
            root = it->second.parent;
            current = it->second.parent;
        } else {
            root = current;
            break;
        }
    }
    auto rootWindow = ceGwe_.windows().find(root);
    if (!nestedChild || rootWindow == ceGwe_.windows().end() || rootWindow->second.destroyed ||
        rootWindow->second.parent) {
        return false;
    }

    if ((target->second.style & kWindowStyleChild) && target->second.parent == root) {
        return false;
    }

    const auto [rootX, rootY] = guestWindowOrigin(root);
    const int32_t rootRight = rootX + rootWindow->second.width;
    const int32_t rootBottom = rootY + rootWindow->second.height;
    for (const auto& [popupHwnd, popup] : ceGwe_.windows()) {
        if (popup.zOrder <= target->second.zOrder) continue;
        if (!popup.visible || popup.destroyed || popup.parent != root ||
            (popup.style & kWindowStyleChild)) {
            continue;
        }
        const auto [popupX, popupY] = guestWindowOrigin(popupHwnd);
        if (popupX <= rootX && popupY <= rootY &&
            popupX + popup.width >= rootRight &&
            popupY + popup.height >= rootBottom) {
            return true;
        }
    }
    return false;
}

void SyntheticDllRuntime::pumpHostMessages() {
#if defined(_WIN32)
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}

void SyntheticDllRuntime::enqueueDueTimers() {
    const uint64_t now = hostTickMilliseconds();
    for (auto& [key, timer] : ceGwe_.timers()) {
        (void)key;
        if (timer.intervalMs == 0 || now < timer.nextDueMs) continue;
        const bool alreadyQueued = ceGwe_.anyMessage([&](const GuestMessage& queued) {
            return queued.hwnd == timer.hwnd &&
                   queued.message == 0x0113 &&
                   queued.wParam == timer.id;
        });
        if (alreadyQueued) continue;
        GuestMessage message{};
        message.hwnd = timer.hwnd;
        message.message = 0x0113; // WM_TIMER
        message.wParam = timer.id;
        message.lParam = timer.callback;
        message.time = uint32_t(++tick_ * 16);
        ceGwe_.postTimerMessage(message);
        const uint32_t interval = std::max<uint32_t>(1, timer.intervalMs);
        do {
            timer.nextDueMs += interval;
        } while (timer.nextDueMs <= now);
    }
}

uint32_t SyntheticDllRuntime::timerWaitMilliseconds() const {
    if (ceGwe_.timers().empty()) return 50;
    const uint64_t now = hostTickMilliseconds();
    uint64_t best = 50;
    for (const auto& [key, timer] : ceGwe_.timers()) {
        (void)key;
        if (timer.nextDueMs <= now) return 1;
        best = std::min<uint64_t>(best, timer.nextDueMs - now);
    }
    return uint32_t(std::max<uint64_t>(1, best));
}

uint32_t SyntheticDllRuntime::windowAtPoint(uint32_t rootGuestHwnd, int32_t x, int32_t y,
                                            int32_t& clientX, int32_t& clientY) const {
    auto root = ceGwe_.windows().find(rootGuestHwnd);
    if (root == ceGwe_.windows().end() || root->second.destroyed) rootGuestHwnd = 0;
    const CeGwe::HitTestResult hit = ceGwe_.hitTest(rootGuestHwnd, x, y);
    clientX = hit.clientX;
    clientY = hit.clientY;
    if (hit.blockedByModal) {
        spdlog::info("gwe hit-test blocked by modal/top popup root=0x{:08x} blocker=0x{:08x} point={},{}",
                     rootGuestHwnd, hit.blocker, x, y);
        return 0;
    }
    return hit.hwnd;
}

void SyntheticDllRuntime::compactQueuedPointerMotion(size_t maxMotionPerWindow) {
    std::map<uint32_t, size_t> keptMotion;
    ceGwe_.eraseReverseIf([&](const GuestMessage& message) {
        if (message.message != 0x0200) return false;
        size_t& kept = keptMotion[message.hwnd];
        if (kept >= maxMotionPerWindow) return true;
        ++kept;
        return false;
    });
}

void SyntheticDllRuntime::queueHostMouseMessage(uint32_t rootGuestHwnd, uint32_t message,
                                                int32_t hostX, int32_t hostY) {
    constexpr uint32_t kGuestWmMouseMove = 0x0200;
    constexpr uint32_t kGuestWmLButtonDown = 0x0201;
    constexpr uint32_t kGuestWmLButtonUp = 0x0202;
    if (hostPointerDropUntilRelease_) {
        if (message == kGuestWmLButtonUp) {
            hostPointerDropUntilRelease_ = false;
            hostPointerCaptureWindow_ = 0;
            hostPointerDragActive_ = false;
            spdlog::info("discarded host mouse up for rejected touch sequence point={},{} queued={}",
                         hostX, hostY, ceGwe_.messageCount());
        } else if (message == kGuestWmLButtonDown || message == kGuestWmMouseMove) {
            spdlog::debug("discarded host mouse msg=0x{:04x} while rejected touch sequence awaits release point={},{} queued={}",
                          message, hostX, hostY, ceGwe_.messageCount());
        }
        return;
    }
    auto root = ceGwe_.windows().find(rootGuestHwnd);
    if (root == ceGwe_.windows().end() || root->second.destroyed) {
        auto presenterRoot = ceGwe_.windows().find(hostPresenterGuestHwnd_);
        if (presenterRoot != ceGwe_.windows().end() && !presenterRoot->second.destroyed) {
            spdlog::info("rebased host mouse root from destroyed guest HWND=0x{:08x} to presenter guest HWND=0x{:08x}",
                         rootGuestHwnd, hostPresenterGuestHwnd_);
            rootGuestHwnd = hostPresenterGuestHwnd_;
        } else {
            rootGuestHwnd = 0;
        }
    }
    int32_t clientX = hostX;
    int32_t clientY = hostY;
    uint32_t hwnd = 0;
    auto ownedPopupRootForInput = [&](uint32_t target) {
        for (uint32_t current = target; current;) {
            if (isOwnedPopupWindow(current)) return current;
            auto window = ceGwe_.windows().find(current);
            if (window == ceGwe_.windows().end()) break;
            current = window->second.parent;
        }
        return 0u;
    };
    auto canUseCapturedWindow = [&](uint32_t capturedHwnd) {
        const uint32_t popupRoot = ownedPopupRootForInput(capturedHwnd);
        bool abovePopupRoot = false;
        for (uint32_t current = capturedHwnd; current;) {
            auto window = ceGwe_.windows().find(current);
            if (window == ceGwe_.windows().end() || window->second.destroyed ||
                !ceGwe_.visibleRectForWindow(current) ||
                (!window->second.enabled && !abovePopupRoot)) {
                return false;
            }
            if (current == popupRoot) abovePopupRoot = true;
            current = window->second.parent;
        }
        return true;
    };
    if (message == kGuestWmLButtonUp && hostPointerCaptureWindow_) {
        auto captured = ceGwe_.windows().find(hostPointerCaptureWindow_);
        if (captured != ceGwe_.windows().end() && canUseCapturedWindow(hostPointerCaptureWindow_)) {
            hwnd = hostPointerCaptureWindow_;
        } else {
            hostPointerCaptureWindow_ = 0;
        }
    } else if (message != kGuestWmLButtonDown && capturedWindow_) {
        auto captured = ceGwe_.windows().find(capturedWindow_);
        if (captured != ceGwe_.windows().end() && canUseCapturedWindow(capturedWindow_)) {
            hwnd = capturedWindow_;
        } else {
            capturedWindow_ = 0;
        }
    }
    if (!hwnd) {
        hwnd = windowAtPoint(rootGuestHwnd, hostX, hostY, clientX, clientY);
    }
    if (!hwnd) return;

    const auto isPointerMessage = [&](uint32_t msg) {
        return msg == kGuestWmMouseMove || msg == kGuestWmLButtonDown || msg == kGuestWmLButtonUp;
    };
    const bool hasQueuedPointer = ceGwe_.anyMessage([&](const GuestMessage& queued) {
        return isPointerMessage(queued.message);
    });
    if (message == kGuestWmLButtonDown && hasQueuedPointer) {
        hostPointerDropUntilRelease_ = true;
        spdlog::info("discarded host mouse down while previous touch sequence is still queued hwnd=0x{:08x} point={},{} queued={}",
                     hwnd, hostX, hostY, ceGwe_.messageCount());
        return;
    }
    if (message == kGuestWmMouseMove) {
        if (hasQueuedPointer || !ceGwe_.pendingMessageTransfers().empty()) {
            spdlog::debug("discarded host mouse move while pointer dispatch is in flight hwnd=0x{:08x} point={},{} queued={} transfers={}",
                          hwnd, hostX, hostY, ceGwe_.messageCount(), ceGwe_.pendingMessageTransfers().size());
            return;
        }
        if (hostPointerCaptureWindow_) {
            constexpr int32_t kTapJitterSlopPixels = 10;
            constexpr int32_t kDragMoveStepPixels = 4;
            const int32_t downDx = std::abs(hostX - hostPointerDownX_);
            const int32_t downDy = std::abs(hostY - hostPointerDownY_);
            if (!hostPointerDragActive_) {
                if (std::max(downDx, downDy) < kTapJitterSlopPixels) {
                    spdlog::debug("discarded host mouse tap-jitter move hwnd=0x{:08x} point={},{} down={},{}",
                                  hwnd, hostX, hostY, hostPointerDownX_, hostPointerDownY_);
                    return;
                }
                hostPointerDragActive_ = true;
            }
            const int32_t stepDx = std::abs(hostX - hostPointerLastMoveX_);
            const int32_t stepDy = std::abs(hostY - hostPointerLastMoveY_);
            if (std::max(stepDx, stepDy) < kDragMoveStepPixels) {
                spdlog::debug("discarded host mouse sub-step drag move hwnd=0x{:08x} point={},{} last={},{}",
                              hwnd, hostX, hostY, hostPointerLastMoveX_, hostPointerLastMoveY_);
                return;
            }
            hostPointerLastMoveX_ = hostX;
            hostPointerLastMoveY_ = hostY;
        }
    }
    if (message == kGuestWmMouseMove && !hostPointerCaptureWindow_ && !hasQueuedPointer) {
        return;
    }
    if (message == kGuestWmLButtonUp && !hostPointerCaptureWindow_ && !hasQueuedPointer) {
        spdlog::info("discarded stray host mouse up hwnd=0x{:08x} point={},{}", hwnd, hostX, hostY);
        return;
    }

    auto targetWindow = ceGwe_.windows().find(hwnd);
    if (targetWindow != ceGwe_.windows().end() && !targetWindow->second.parent &&
        hwnd != rootGuestHwnd && (targetWindow->second.style & 0x80000000u) &&
        !ceGwe_.visibleRegionContainsPoint(hwnd, hostX, hostY)) {
        if (message == kGuestWmLButtonUp && hostPointerCaptureWindow_ == hwnd) {
            hostPointerCaptureWindow_ = 0;
        }
        spdlog::info("discarded host mouse msg=0x{:04x} outside modal popup hwnd=0x{:08x} point={},{} client={},{}",
                     message, hwnd, hostX, hostY, clientX, clientY);
        return;
    }

    const uint32_t popupRoot = ownedPopupRootForInput(hwnd);
    bool abovePopupRoot = false;
    for (uint32_t current = hwnd; current;) {
        auto window = ceGwe_.windows().find(current);
        if (window == ceGwe_.windows().end()) break;
        if (!window->second.enabled && !abovePopupRoot) {
            if (message == kGuestWmLButtonUp && hostPointerCaptureWindow_ == hwnd) {
                hostPointerCaptureWindow_ = 0;
            }
            spdlog::info("discarded host mouse msg=0x{:04x} hwnd=0x{:08x} disabledAt=0x{:08x} point={},{}",
                         message, hwnd, current, hostX, hostY);
            return;
        }
        if (current == popupRoot) abovePopupRoot = true;
        current = window->second.parent;
    }

    auto originOf = [&](uint32_t target) {
        int32_t x = 0;
        int32_t y = 0;
        for (uint32_t current = target; current;) {
            auto it = ceGwe_.windows().find(current);
            if (it == ceGwe_.windows().end()) break;
            x += it->second.x;
            y += it->second.y;
            current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
        }
        return std::pair<int32_t, int32_t>{x, y};
    };
    const auto [originX, originY] = originOf(hwnd);
    clientX = hostX - originX;
    clientY = hostY - originY;
    auto queueInputMessage = [&](const GuestMessage& input) {
        const auto isInputPriority = [&](const GuestMessage& queued) {
            return queued.message == 0x0007 || queued.message == 0x0008 ||
                   queued.message == kGuestWmMouseMove || queued.message == kGuestWmLButtonDown ||
                   queued.message == kGuestWmLButtonUp;
        };
        const auto mustRunBeforeInputForSameWindow = [](uint32_t message) {
            return message == 0x0001 || // WM_CREATE
                   message == 0x0005 || // WM_SIZE
                   message == 0x0018;   // WM_SHOWWINDOW
        };
        const bool hasPendingLifecycleForTarget =
            ceGwe_.anyMessage([&](const GuestMessage& queued) {
                return queued.hwnd == input.hwnd && mustRunBeforeInputForSameWindow(queued.message);
            });
        if (hasPendingLifecycleForTarget) {
            ceGwe_.postInputMessage(input);
            compactQueuedPointerMotion();
            return;
        }
        GuestMessage queuedInput = input;
        queuedInput.queueKind = CeGwe::MessageQueueKind::Input;
        ceGwe_.postAfterLeadingMatches(queuedInput, isInputPriority);
        compactQueuedPointerMotion();
    };
    if (message == kGuestWmLButtonDown) {
        hostPointerCaptureWindow_ = hwnd;
        hostPointerDownX_ = hostX;
        hostPointerDownY_ = hostY;
        hostPointerLastMoveX_ = hostX;
        hostPointerLastMoveY_ = hostY;
        hostPointerDragActive_ = false;
        if (focusedWindow_ != hwnd) {
            auto focused = ceGwe_.windows().find(focusedWindow_);
            if (focused != ceGwe_.windows().end() && !focused->second.destroyed) {
                queueInputMessage({focusedWindow_, 0x0008, hwnd, 0, uint32_t(++tick_ * 16), 0, 0});
            }
            queueInputMessage({hwnd, 0x0007, focusedWindow_, 0, uint32_t(++tick_ * 16), 0, 0});
            focusedWindow_ = hwnd;
        }
    } else if (message == kGuestWmLButtonUp) {
        if (pendingSyntheticChildButtonUpWindow_) {
            const uint32_t childHwnd = pendingSyntheticChildButtonUpWindow_;
            pendingSyntheticChildButtonUpWindow_ = 0;
            auto child = ceGwe_.windows().find(childHwnd);
            if (child != ceGwe_.windows().end() && !child->second.destroyed) {
                GuestMessage childUp{};
                childUp.hwnd = childHwnd;
                childUp.message = kGuestWmLButtonUp;
                childUp.wParam = 0;
                childUp.lParam = 0;
                childUp.time = uint32_t(++tick_ * 16);
                childUp.x = uint32_t(hostX);
                childUp.y = uint32_t(hostY);
                queueInputMessage(childUp);
                spdlog::info("queued mirrored child button-up hwnd=0x{:08x} for host release at {},{}",
                             childHwnd, hostX, hostY);
            }
        }
        hostPointerCaptureWindow_ = 0;
        hostPointerDragActive_ = false;
    }
    GuestMessage guest{};
    guest.hwnd = hwnd;
    guest.message = message;
    guest.wParam = message == kGuestWmLButtonDown ? 1 : 0;
    guest.lParam = uint32_t(uint16_t(clientX) | (uint32_t(uint16_t(clientY)) << 16));
    guest.time = uint32_t(++tick_ * 16);
    // MSG.pt/GetMessagePos are screen/root coordinates; lParam remains client.
    guest.x = uint32_t(hostX);
    guest.y = uint32_t(hostY);
    lastHostInputQueuedAt_ = std::chrono::steady_clock::now();
    queueInputMessage(guest);
    spdlog::info("queued host mouse msg=0x{:04x} root=0x{:08x} hwnd=0x{:08x} point={},{} client={},{} queued={}",
                 message, rootGuestHwnd, hwnd, hostX, hostY, clientX, clientY, ceGwe_.messageCount());
    uc_emu_stop(uc_);
}

bool SyntheticDllRuntime::hasHostWindows() const {
#if defined(_WIN32)
    for (const auto& [guestHwnd, window] : ceGwe_.windows()) {
        (void)guestHwnd;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (hwnd && IsWindow(hwnd)) return true;
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (hwnd && IsWindow(hwnd)) return true;
    }
#endif
    return false;
}

void SyntheticDllRuntime::runHostMessageLoopUntilClosed(bool showHostWindows) {
#if defined(_WIN32)
    if (!hasHostWindows()) return;
    struct HookGuard {
        uc_engine* uc{};
        uc_hook hook{};
        ~HookGuard() {
            if (uc && hook) uc_hook_del(uc, hook);
        }
    } interactiveSliceHook{uc_};
    const uc_err hookErr = uc_hook_add(uc_, &interactiveSliceHook.hook, UC_HOOK_BLOCK,
                                       (void*)SyntheticDllRuntime::hookBasicBlock, this, 1, 0);
    if (hookErr != UC_ERR_OK) {
        spdlog::warn("interactive basic-block watchdog hook failed: {} ({})",
                     int(hookErr), uc_strerror(hookErr));
    }
    if (showHostWindows) {
        for (auto& [guestHwnd, window] : ceGwe_.windows()) {
            (void)guestHwnd;
            HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
            if (!hwnd || !IsWindow(hwnd)) continue;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindowNow(hwnd);
        }
        for (uintptr_t hostHwnd : retainedHostWindows_) {
            HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
            if (!hwnd || !IsWindow(hwnd)) continue;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindowNow(hwnd);
        }
    }
    spdlog::info("entering host GUI message loop mode={}; close the presenter window to exit",
                 showHostWindows ? "visible" : "headless");
    MSG message{};
    auto hasPendingUserInput = [&]() {
        return ceGwe_.anyMessage([](const GuestMessage& message) {
            return message.message >= 0x0200 && message.message <= 0x0202;
        });
    };
    auto recentlyQueuedUserInput = [&]() {
        return lastHostInputQueuedAt_ != std::chrono::steady_clock::time_point{} &&
               (std::chrono::steady_clock::now() - lastHostInputQueuedAt_) < std::chrono::milliseconds(750);
    };
    auto hasPendingSynchronousMessage = [&]() {
        return ceGwe_.anyMessage([](const GuestMessage& message) {
            return message.synchronousSender != 0;
        });
    };
    auto resumeGuestSlice = [&](uint64_t instructionBudget, const char* reason) -> bool {
        uint32_t pc = 0;
        uint32_t ra = 0;
        uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
        uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
        if ((!pc || !isGuestRangeReadable(pc, 4)) && !ceKernel_.activeGuestThread()) {
            if (!pendingBlockingApis_.empty()) {
                if (completeReadyPendingBlockingMainContinuation("invalid-pc-ready")) {
                    uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
                    uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
                } else {
                    pumpHostMessages();
                    return true;
                }
            }
            if (restoreMainThreadContextIfRunnable(reason)) {
                uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
                uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
            }
        }
        if (!pc || !isGuestRangeReadable(pc, 4)) {
            spdlog::error("refusing to start guest slice at invalid pc reason={} pc=0x{:08x} ra=0x{:08x} "
                          "activeThread=0x{:08x} queued={}",
                          reason, pc, ra, ceKernel_.activeGuestThread(), ceGwe_.messageCount());
            return false;
        }
        const uint32_t startPc = pc;
        const auto sliceStart = std::chrono::steady_clock::now();
        // Guest worker threads run cooperatively on the host UI thread.  Keep
        // their slices short so touch input and presenter paints stay live even
        // when a worker performs a long pure-guest loop between API calls.
        const bool servicingQueuedMessages = std::strcmp(reason, "queued-message") == 0;
        const bool servicingMessageTransfer = std::strcmp(reason, "message-transfer") == 0;
        const bool servicingBlockedMainWork = std::strncmp(reason, "blocked-main-", 13) == 0;
        const bool synchronousQueuedMessage = servicingQueuedMessages && hasPendingSynchronousMessage();
        const bool pendingUserInput = hasPendingUserInput();
        const bool recentUserInput = recentlyQueuedUserInput();
        const bool hostInputPressure = pendingUserInput || recentUserInput;
        const bool hostUiPressure = pendingUserInput || recentUserInput || hostPresentDirty_;
        const bool backloggedQueuedWork = servicingQueuedMessages && ceGwe_.messageCount() >= 3;
        if (servicingBlockedMainWork && !hostInputPressure) {
            instructionBudget = std::max<uint64_t>(instructionBudget, 25000000u);
        } else if (servicingMessageTransfer) {
            if (hostUiPressure) {
                instructionBudget = std::min<uint64_t>(instructionBudget, pendingUserInput ? 25000u : 100000u);
            } else if (!ceGwe_.hasMessages()) {
                instructionBudget = std::max<uint64_t>(instructionBudget, 5000000u);
            } else {
                instructionBudget = std::max<uint64_t>(instructionBudget, 2000000u);
            }
        } else if (synchronousQueuedMessage && ceKernel_.activeGuestThread() && !pendingUserInput && !recentUserInput) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 500000u : 250000u);
        } else if (synchronousQueuedMessage) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 50000u : 25000u);
        } else if (pendingUserInput) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 50000u : 12000u);
        } else if (recentUserInput && !backloggedQueuedWork) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 12000u);
        } else if (servicingQueuedMessages && !ceKernel_.activeGuestThread()) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 50000u : 25000u);
        } else if (servicingQueuedMessages && ceKernel_.activeGuestThread()) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 500000u : 250000u);
        } else if (servicingQueuedMessages) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 250000u);
        }
        const auto wallBudget = servicingBlockedMainWork
            ? (hostInputPressure ? std::chrono::milliseconds(20) : std::chrono::milliseconds(250))
            : (servicingMessageTransfer
            ? (hostUiPressure ? std::chrono::milliseconds(12) : std::chrono::milliseconds(180))
            : (synchronousQueuedMessage
                   ? (backloggedQueuedWork ? std::chrono::milliseconds(60) : std::chrono::milliseconds(12))
                   : (pendingUserInput
                          ? (backloggedQueuedWork ? std::chrono::milliseconds(60) : std::chrono::milliseconds(12))
                          : (recentUserInput && !backloggedQueuedWork
                                 ? std::chrono::milliseconds(12)
                          : (servicingQueuedMessages ? std::chrono::milliseconds(60)
                                                     : (ceKernel_.activeGuestThread() ? std::chrono::milliseconds(60)
                                                                          : std::chrono::milliseconds(120)))))));
        beginInteractiveSlice(wallBudget, reason, instructionBudget);
        const uc_err err = uc_emu_start(uc_, pc, 0, 0, instructionBudget);
        const bool stoppedByWatchdog = interactiveSliceStopRequested_;
        endInteractiveSlice();
        constexpr uint64_t kSchedulerMappingSyncIntervalMs = 50;
        const uint64_t afterSliceMs = hostTickMilliseconds();
        if (!mappedViews_.empty() &&
            (!lastSchedulerMappingSyncMs_ ||
             afterSliceMs - lastSchedulerMappingSyncMs_ >= kSchedulerMappingSyncIntervalMs ||
             hostUiPressure)) {
            syncNamedMappedViews();
            lastSchedulerMappingSyncMs_ = afterSliceMs;
        }
        uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
        uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
        if (err != UC_ERR_OK) {
            const uint32_t activeThread = ceKernel_.activeGuestThread();
            const uint32_t v0 = reg(UC_MIPS_REG_V0);
            const uint32_t t9 = reg(UC_MIPS_REG_T9);
            if (pc == 0 && activeThread) {
                auto active = ceKernel_.threads().find(activeThread);
                if (active != ceKernel_.threads().end() &&
                    active->second.state == GuestThreadRunState::Running &&
                    active->second.startAddress == t9) {
                    spdlog::info("guest thread returned through null pc; completing as CE thread exit "
                                 "handle=0x{:08x} start=0x{:08x} exitCode=0x{:08x} reason={}",
                                 activeThread, active->second.startAddress, v0,
                                 reason ? reason : "guest-slice");
                    finishActiveGuestThread(v0);
                    pumpHostMessages();
                    presentHostWindows(false);
                    return true;
                }
            }
            if (pc == 0) {
                spdlog::error("interactive emulation stopped hard error reason={} err={} ({}) pc=0x{:08x} ra=0x{:08x} activeThread=0x{:08x}",
                              reason, int(err), uc_strerror(err), pc, ra, ceKernel_.activeGuestThread());
            } else {
                spdlog::warn("interactive emulation stopped reason={} err={} ({}) pc=0x{:08x} ra=0x{:08x}",
                             reason, int(err), uc_strerror(err), pc, ra);
            }
            auto describeAddress = [&](uint32_t address) {
                for (const auto& [base, module] : loadedModulesByBase_) {
                    const uint64_t begin = base;
                    const uint64_t end = begin + (module.imageSize ? module.imageSize : 0x1000u);
                    if (address >= begin && uint64_t(address) < end) {
                        std::ostringstream oss;
                        oss << module.name << "+0x" << std::hex << std::setw(8)
                            << std::setfill('0') << (address - base);
                        return oss.str();
                    }
                }
                return std::string{"<unmapped>"};
            };
            const uint32_t sp = reg(UC_MIPS_REG_SP);
            const uint32_t gp = reg(UC_MIPS_REG_GP);
            const uint32_t a0 = reg(UC_MIPS_REG_A0);
            const uint32_t a1 = reg(UC_MIPS_REG_A1);
            const uint32_t a2 = reg(UC_MIPS_REG_A2);
            const uint32_t a3 = reg(UC_MIPS_REG_A3);
            spdlog::warn("interactive crash context activeThread=0x{:08x} pc={} ra={} sp=0x{:08x} gp=0x{:08x} "
                         "v0=0x{:08x} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} t9=0x{:08x} queued={}",
                         ceKernel_.activeGuestThread(), describeAddress(pc), describeAddress(ra), sp, gp,
                         v0, a0, a1, a2, a3, t9, ceGwe_.messageCount());
            std::array<uint32_t, 8> stackWords{};
            if (sp && isGuestRangeReadable(sp, uint32_t(stackWords.size() * sizeof(uint32_t))) &&
                uc_mem_read(uc_, sp, stackWords.data(), stackWords.size() * sizeof(uint32_t)) == UC_ERR_OK) {
                spdlog::warn("interactive crash stack sp=0x{:08x}: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                             sp,
                             stackWords[0], stackWords[1], stackWords[2], stackWords[3],
                             stackWords[4], stackWords[5], stackWords[6], stackWords[7]);
            }
            ceGwe_.forEachMessage([&](const GuestMessage& queued, size_t queuedIndex) {
                if (queuedIndex >= 8) return false;
                spdlog::warn("interactive crash queued[{}] hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} "
                             "lparam=0x{:08x} sync=0x{:08x} crossProcess={}",
                             queuedIndex, queued.hwnd, queued.message, queued.wParam,
                             queued.lParam, queued.synchronousSender, queued.crossProcess);
                return true;
            });
            return false;
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sliceStart).count();
        if (elapsedMs >= 250) {
            const uint32_t owner = servicingMessageTransfer && !ceGwe_.pendingMessageTransfers().empty()
                ? ceGwe_.pendingMessageTransfers().back().ownerThread
                : ceGwe_.oldestPendingOwner().value_or(0);
            const auto ownerQueue = ceGwe_.ownerQueueSnapshot(owner);
            spdlog::info("long guest slice reason={} activeThread=0x{:08x} budget={} startPc=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={} owner=0x{:08x} ownerPosted={} ownerSent={} ownerInput={} ownerTimer={}",
                         reason, ceKernel_.activeGuestThread(), instructionBudget, startPc, pc, ra,
                         ceGwe_.messageCount(), owner, ownerQueue.posted, ownerQueue.sent,
                         ownerQueue.input, ownerQueue.timers);
        }
        const bool mainOwnedMessageTransfer =
            servicingMessageTransfer &&
            !ceGwe_.pendingMessageTransfers().empty() &&
            ceGwe_.pendingMessageTransfers().back().ownerThread == ceKernel_.mainThreadPseudoHandle();
        if (stoppedByWatchdog && mainOwnedMessageTransfer && ceKernel_.activeGuestThread()) {
            const uint32_t displacedThread = ceKernel_.activeGuestThread();
            ceKernel_.mainThreadContext() = captureGuestCpuContext();
            updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
            spdlog::debug("saved main-owned message transfer context activeThread=0x{:08x} pc=0x{:08x} queued={}",
                          displacedThread, pc, ceGwe_.messageCount());
            auto displaced = ceKernel_.threads().find(displacedThread);
            if (displaced != ceKernel_.threads().end() &&
                displaced->second.state == GuestThreadRunState::Running) {
                displaced->second.state = GuestThreadRunState::Runnable;
            }
            ceKernel_.activeGuestThread() = 0;
        } else if (stoppedByWatchdog && ceKernel_.activeGuestThread()) {
            spdlog::info("guest thread timeslice yield handle=0x{:08x} reason={} pc=0x{:08x} queued={}",
                         ceKernel_.activeGuestThread(), reason, pc, ceGwe_.messageCount());
            yieldActiveGuestThread("timeslice");
        } else if (stoppedByWatchdog && !ceKernel_.activeGuestThread() &&
                   pc && isGuestRangeReadable(pc, 4)) {
            ceKernel_.mainThreadContext() = captureGuestCpuContext();
            updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
            spdlog::debug("saved main thread timeslice context reason={} pc=0x{:08x} ra=0x{:08x} queued={}",
                          reason, pc, ra, ceGwe_.messageCount());
        }
        pumpHostMessages();
        presentHostWindows(false);
        compactQueuedPointerMotion();
        enqueueDueTimers();
        return true;
    };
    auto guestThreadRunStateName = [](GuestThreadRunState state) -> const char* {
        switch (state) {
        case GuestThreadRunState::Suspended: return "suspended";
        case GuestThreadRunState::Runnable: return "runnable";
        case GuestThreadRunState::Running: return "running";
        case GuestThreadRunState::Waiting: return "waiting";
        case GuestThreadRunState::WaitingForMessage: return "waiting-message";
        case GuestThreadRunState::WaitingForSendMessage: return "waiting-send";
        case GuestThreadRunState::WaitingForSerialRead: return "waiting-serial";
        case GuestThreadRunState::Terminated: return "terminated";
        }
        return "unknown";
    };
    auto describeGuestAddress = [&](uint32_t address) {
        for (const auto& [base, module] : loadedModulesByBase_) {
            const uint64_t begin = base;
            const uint64_t end = begin + (module.imageSize ? module.imageSize : 0x1000u);
            if (address >= begin && uint64_t(address) < end) {
                std::ostringstream out;
                out << module.name << "+0x" << std::hex << std::setw(8)
                    << std::setfill('0') << (address - base);
                return out.str();
            }
        }
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << address;
        return out.str();
    };
    auto lastSchedulerDiagAt = std::chrono::steady_clock::time_point{};
    auto logGuestSchedulerDiag = [&](const char* where) {
        const auto now = std::chrono::steady_clock::now();
        if (lastSchedulerDiagAt != std::chrono::steady_clock::time_point{} &&
            now - lastSchedulerDiagAt < std::chrono::milliseconds(1000)) {
            return;
        }
        lastSchedulerDiagAt = now;
        std::ostringstream threads;
        bool first = true;
        for (const auto& [handle, thread] : ceKernel_.threads()) {
            if (!first) threads << "; ";
            first = false;
            const auto pcIt = thread.context.registers.find(UC_MIPS_REG_PC);
            const auto raIt = thread.context.registers.find(UC_MIPS_REG_RA);
            const uint32_t pc = pcIt == thread.context.registers.end() ? 0 : pcIt->second;
            const uint32_t ra = raIt == thread.context.registers.end() ? 0 : raIt->second;
            threads << "0x" << std::hex << std::setw(8) << std::setfill('0') << handle
                    << "/" << guestThreadRunStateName(thread.state)
                    << " valid=" << (thread.context.valid ? 1 : 0)
                    << " pc=" << describeGuestAddress(pc)
                    << " ra=" << describeGuestAddress(ra)
                    << " wait=0x" << std::setw(8) << thread.waitHandle;
            if (!thread.waitHandles.empty()) {
                threads << " waits=[";
                for (size_t index = 0; index < thread.waitHandles.size(); ++index) {
                    if (index) threads << ",";
                    threads << "0x" << std::hex << std::setw(8) << thread.waitHandles[index];
                }
                threads << "]";
            }
            threads << " sleep=" << std::dec << thread.sleepUntilMs;
        }
        spdlog::info("guest scheduler diag where={} active=0x{:08x} queued={} timers={} threads=[{}]",
                     where, ceKernel_.activeGuestThread(), ceGwe_.messageCount(), ceGwe_.timers().size(), threads.str());
    };
    auto resumeReadyBlockingMain = [&]() -> bool {
        if (ceKernel_.activeGuestThread() ||
            pendingBlockingApis_.empty()) {
            return false;
        }
        if (!completeReadyPendingBlockingMainContinuation("blocking-main-ready")) {
            return false;
        }
        if (!resumeGuestSlice(250000, "blocking-main-ready")) {
            return false;
        }
        return true;
    };
    auto resumeQueuedWorkerBurst = [&]() -> bool {
        if (!ceGwe_.hasMessages() || !hasHostWindows() ||
            hasPendingUserInput() || hasPendingSynchronousMessage() ||
            !ceGwe_.pendingMessageTransfers().empty()) {
            return true;
        }
        if (ceGwe_.oldestPendingOwner()) {
            return true;
        }
        if (ceGwe_.messageCount() < 3) {
            return true;
        }
        constexpr uint32_t kMaxWorkerSlicesBeforeMessage = 4;
        for (uint32_t slice = 0;
             slice < kMaxWorkerSlicesBeforeMessage && ceGwe_.hasMessages() && hasHostWindows() &&
             !hasPendingUserInput() && !hasPendingSynchronousMessage();
             ++slice) {
            if (!ceKernel_.activeGuestThread()) {
                if (!hasRunnableGuestThread()) {
                    logGuestSchedulerDiag(slice == 0 ? "pre-queued-no-runnable"
                                                     : "pre-queued-burst-no-runnable");
                    break;
                }
                switchToRunnableGuestThread(slice == 0 ? "pre-queued-worker" : "pre-queued-worker-burst");
            }
            if (!ceKernel_.activeGuestThread()) break;
            if (!resumeGuestSlice(5000000, slice == 0 ? "pre-queued-worker" : "pre-queued-worker-burst")) {
                return false;
            }
        }
        return true;
    };
    auto runWorkerWhileMainBlocked = [&](const char* reason, uint64_t instructionBudget) -> bool {
        if (!ceKernel_.activeGuestThread()) {
            if (!hasRunnableGuestThread()) {
                logGuestSchedulerDiag(reason ? reason : "blocked-main-no-runnable");
                pumpHostMessages();
                presentHostWindows(false);
                return true;
            }
            switchToRunnableGuestThread(reason ? reason : "blocked-main-worker");
        }
        if (!ceKernel_.activeGuestThread()) {
            pumpHostMessages();
            presentHostWindows(false);
            return true;
        }
        return resumeGuestSlice(instructionBudget, reason ? reason : "blocked-main-worker");
    };
    while (hasHostWindows()) {
        drainRemoteInputEvents();
        if (resumeReadyBlockingMain()) {
            continue;
        }
        bool remotePaused = false;
        {
            std::lock_guard<std::mutex> lock(ceRemote_.mutex());
            remotePaused = ceRemote_.paused();
        }
        if (remotePaused) {
            pumpHostMessages();
            presentHostWindows(false);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }
        pollCrossProcessGuestMessages();
        enqueueDueTimers();
        if (resumeReadyBlockingMain()) {
            continue;
        }
        if (ceGwe_.hasMessages()) {
            beginHostUiBatchPresentDeferral();
        }
        if (!ceGwe_.pendingMessageTransfers().empty()) {
            const uint32_t transferOwner = ceGwe_.pendingMessageTransfers().back().ownerThread;
            if (transferOwner == ceKernel_.mainThreadPseudoHandle() &&
                !pendingBlockingApis_.empty()) {
                spdlog::debug("deferring main-owned message transfer while main wait is parked queued={} transfers={}",
                              ceGwe_.messageCount(), ceGwe_.pendingMessageTransfers().size());
                if (!runWorkerWhileMainBlocked("blocked-main-message-transfer", 5000000)) {
                    return;
                }
                flushHostUiBatchPresentDeferral(50);
                continue;
            }
            if (transferOwner == ceKernel_.mainThreadPseudoHandle() &&
                ceKernel_.activeGuestThread()) {
                const uint32_t mainPc = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_PC);
                if (mainPc && isGuestRangeReadable(mainPc, 4)) {
                    const uint32_t activeThread = ceKernel_.activeGuestThread();
                    auto active = ceKernel_.threads().find(activeThread);
                    if (active != ceKernel_.threads().end()) {
                        active->second.context = captureGuestCpuContext();
                        if (active->second.state == GuestThreadRunState::Running) {
                            active->second.state = GuestThreadRunState::Runnable;
                        }
                    }
                    ceKernel_.activeGuestThread() = 0;
                    restoreMainThreadContextIfRunnable("message-transfer-owner");
                    spdlog::debug("restored main owner for pending message transfer owner=0x{:08x} queued={}",
                                  transferOwner, ceGwe_.messageCount());
                }
            } else if (transferOwner != CeGwe::kNoOwnerThread &&
                       transferOwner != ceKernel_.mainThreadPseudoHandle() &&
                       transferOwner != ceKernel_.activeGuestThread()) {
                switchToRunnableGuestThread("message-transfer-owner", 0, transferOwner);
            }
            if (!resumeGuestSlice(1000000, "message-transfer")) {
                return;
            }
            flushHostUiBatchPresentDeferral(50);
            continue;
        }
        if (!resumeQueuedWorkerBurst()) return;
        if (ceGwe_.hasMessages() && hasHostWindows()) {
            const std::optional<uint32_t> oldestOwner = ceGwe_.oldestPendingOwner();
            if (!pendingBlockingApis_.empty() &&
                oldestOwner &&
                *oldestOwner == ceKernel_.mainThreadPseudoHandle()) {
                if (!runWorkerWhileMainBlocked("blocked-main-queued-message", 5000000)) {
                    return;
                }
                flushHostUiBatchPresentDeferral(100);
                continue;
            }
            if (ceKernel_.activeGuestThread()) {
                if ((!oldestOwner || *oldestOwner != ceKernel_.activeGuestThread()) &&
                    hasSchedulableGweMessageOwner()) {
                    yieldActiveGuestThread("queued-message-preempt");
                }
            }
            if (!ceKernel_.activeGuestThread()) {
                uint32_t currentPc = 0;
                uc_reg_read(uc_, UC_MIPS_REG_PC, &currentPc);
                if (!currentPc || !isGuestRangeReadable(currentPc, 4)) {
                    if (restoreMainThreadContextIfRunnable("queued-message")) {
                        spdlog::debug("restored parked main thread for queued messages queued={}",
                                      ceGwe_.messageCount());
                    }
                } else {
                    updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
                }
            }
            compactQueuedPointerMotion();
            const uint64_t budget = 250000u;
            spdlog::debug("resuming guest for queued message queued={} budget={}", ceGwe_.messageCount(), budget);
            if (!resumeGuestSlice(budget, "queued-message")) {
                return;
            }
            if (!ceGwe_.hasMessages() && hasHostWindows() && hasRunnableGuestThread()) {
                if (!ceKernel_.activeGuestThread()) {
                    switchToRunnableGuestThread("queued-worker");
                }
                if (ceKernel_.activeGuestThread() && !resumeGuestSlice(25000, "queued-worker")) {
                    return;
                }
            }
            if (!ceGwe_.hasMessages()) {
                releaseHostUiBatchPresentDeferral();
            } else if (hostPresentUiBatchActive_ &&
                       hostTickMilliseconds() - hostPresentUiBatchStartMs_ >= 100) {
                flushHostUiBatchPresentDeferral(100);
            }
        }
        const DWORD waitMs = std::max<DWORD>(1, std::min<DWORD>(50, timerWaitMilliseconds()));
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        presentHostWindows(false);
        if (!ceGwe_.hasMessages() && hasHostWindows() && !ceKernel_.activeGuestThread()) {
            releaseHostUiBatchPresentDeferral();
            if (hasRunnableGuestThread()) {
                switchToRunnableGuestThread("idle-worker");
            } else {
                logGuestSchedulerDiag("idle-no-runnable");
            }
        }
        if (!ceGwe_.hasMessages() && hasHostWindows() && !resumeGuestSlice(5000000, "idle")) {
            return;
        }
        if (!ceGwe_.hasMessages() && !ceKernel_.activeGuestThread() && !hasRunnableGuestThread()) {
            logGuestSchedulerDiag("wait-no-runnable");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        }
    }
#endif
}
