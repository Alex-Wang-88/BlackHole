#include "D3D12Engine.hpp"

#include <d3dcompiler.h>
#include <commctrl.h>

#include <glm/gtc/matrix_transform.hpp>

#ifdef BLACKHOLE_HAS_STREAMLINE
#include <sl_dlss.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

struct RaytraceConstants
{
    glm::vec4 cameraPos;
    glm::vec4 target;
    float fovYRadians;
    float aspect;
    std::uint32_t renderWidth;
    std::uint32_t renderHeight;
    std::uint32_t maxSteps;
    float dLambda;
    float escapeR;
    float horizonR;
    float diskR1;
    float diskR2;
    std::uint32_t objectCount;
    std::uint32_t sampleIndex;
    float jitterX;
    float jitterY;
    float padding2;
    float padding3;
};

static_assert(sizeof(RaytraceConstants) == 96);

struct GridConstants
{
    glm::mat4 mvp;
    glm::vec4 color;
};

static_assert(sizeof(GridConstants) == 80);

struct CompositeConstants
{
    glm::vec4 fallback;
    std::uint32_t fallbackEnabled;
    float padding[3];
};

static_assert(sizeof(CompositeConstants) == 32);

constexpr std::array<std::array<float, 2>, 8> JITTER_OFFSETS =
{{
    {{ 0.000f,  0.000f}},
    {{-0.250f, -0.250f}},
    {{ 0.250f,  0.250f}},
    {{ 0.250f, -0.250f}},
    {{-0.250f,  0.250f}},
    {{-0.375f,  0.000f}},
    {{ 0.375f,  0.000f}},
    {{ 0.000f,  0.000f}}
}};

constexpr DXGI_FORMAT COLOR_TEXTURE_FORMAT =
    DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT MATERIAL_TEXTURE_FORMAT =
    DXGI_FORMAT_R8G8B8A8_UNORM;

#ifdef BLACKHOLE_HAS_STREAMLINE
template<typename T>
T loadStreamlineFunction(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

void streamlineLogMessage(sl::LogType type, const char* message)
{
    // Keep the console usable while preserving the complete Streamline log
    // under the runtime directory for diagnostics.
    if(type == sl::LogType::eInfo)
        return;

    const char* prefix = "info";
    if(type == sl::LogType::eWarn) prefix = "warning";
    if(type == sl::LogType::eError) prefix = "error";
    std::cerr << "Streamline [" << prefix << "] "
              << (message != nullptr ? message : "") << "\n";
}

std::filesystem::path executableDirectory()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if(length == 0 || length >= buffer.size())
        return std::filesystem::current_path();
    return std::filesystem::path(buffer.data(), buffer.data() + length)
        .parent_path();
}

std::filesystem::path findStreamlineRuntimeDirectory(
    const std::filesystem::path& shaderDirectory)
{
    const char* environmentValue = std::getenv(
        "BLACKHOLE_STREAMLINE_RUNTIME_DIR");
    const std::array<std::filesystem::path, 5> candidates =
    {{
        environmentValue != nullptr && *environmentValue != '\0'
            ? std::filesystem::path(environmentValue)
            : std::filesystem::path(),
        shaderDirectory,
        executableDirectory(),
        shaderDirectory.parent_path() / "third_party" / "streamline" / "bin",
        std::filesystem::current_path() / "third_party" / "streamline" / "bin"
    }};

    for(const auto& candidate : candidates)
    {
        if(!candidate.empty() &&
           std::filesystem::exists(candidate / "sl.interposer.dll"))
            return candidate;
    }
    return {};
}
#endif

const wchar_t* dlssModeText(DlssQualityMode mode)
{
    switch(mode)
    {
    case DlssQualityMode::Quality:
        return L"Quality (highest)";
    case DlssQualityMode::Balanced:
        return L"Balanced";
    case DlssQualityMode::Performance:
        return L"Performance";
    case DlssQualityMode::UltraPerformance:
        return L"Ultra Performance";
    }
    return L"Quality (highest)";
}

float defaultRenderScaleForDlssMode(DlssQualityMode mode)
{
    switch(mode)
    {
    case DlssQualityMode::Quality:
        return 0.6667f;
    case DlssQualityMode::Balanced:
        return 0.5833f;
    case DlssQualityMode::Performance:
        return 0.5000f;
    case DlssQualityMode::UltraPerformance:
        return 0.3333f;
    }
    return 0.6667f;
}

#ifdef BLACKHOLE_HAS_STREAMLINE
sl::DLSSMode toStreamlineDlssMode(DlssQualityMode mode)
{
    switch(mode)
    {
    case DlssQualityMode::Quality:
        return sl::DLSSMode::eMaxQuality;
    case DlssQualityMode::Balanced:
        return sl::DLSSMode::eBalanced;
    case DlssQualityMode::Performance:
        return sl::DLSSMode::eMaxPerformance;
    case DlssQualityMode::UltraPerformance:
        return sl::DLSSMode::eUltraPerformance;
    }
    return sl::DLSSMode::eMaxQuality;
}
#endif

void throwIfFailed(HRESULT result, const char* operation)
{
    if(SUCCEEDED(result)) return;

    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << std::uppercase << static_cast<unsigned long>(result);
    throw std::runtime_error(message.str());
}

std::runtime_error shaderError(
    const std::filesystem::path& path,
    const char* entryPoint,
    const char* target,
    ID3DBlob* errors)
{
    std::string message = "Shader compilation failed for " +
        path.string() + " (" + entryPoint + ", " + target + ")";
    if(errors != nullptr && errors->GetBufferPointer() != nullptr)
    {
        message += ":\n";
        message.append(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize());
    }
    return std::runtime_error(message);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RASTERIZER_DESC defaultRasterizer()
{
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return rasterizer;
}

D3D12_BLEND_DESC alphaBlend()
{
    D3D12_BLEND_DESC blend{};
    D3D12_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return blend;
}

D3D12_BLEND_DESC premultipliedBlend()
{
    D3D12_BLEND_DESC blend{};
    D3D12_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return blend;
}
}

D3D12Engine::D3D12Engine(
    const RenderSettings& settingsValue,
    const std::filesystem::path& shaderDirectoryValue)
    : settings(settingsValue),
      shaderDirectory(shaderDirectoryValue),
      instance(GetModuleHandleW(nullptr)),
      WIDTH(settingsValue.windowWidth),
      HEIGHT(settingsValue.windowHeight),
      RENDER_WIDTH(settingsValue.renderWidth),
      RENDER_HEIGHT(settingsValue.renderHeight)
{
    if(WIDTH <= 0 || HEIGHT <= 0 || RENDER_WIDTH <= 0 || RENDER_HEIGHT <= 0)
        throw std::runtime_error("Window and render dimensions must be positive");

    createWindow();
    initializeD3D12();

    std::cout << "GPU raytrace resolution: " << RENDER_WIDTH << " x "
              << RENDER_HEIGHT << "\n";
    std::cout << "GPU MAX_STEPS: " << settings.maxSteps << "\n";
    std::cout << "Accretion disk: " << DISK_R1_RS << " r_s -> "
              << DISK_R2_RS << " r_s\n";
    if(settings.temporalSampleLimit == 0)
        std::cout << "Temporal anti-aliasing: unlimited accumulation\n";
    else
        std::cout << "Temporal anti-aliasing: " << settings.temporalSampleLimit
                  << " samples\n";
}

D3D12Engine::~D3D12Engine()
{
    if(commandQueue != nullptr && fence != nullptr && fenceEvent != nullptr)
    {
        try
        {
            waitForGpu();
        }
        catch(...)
        {
        }
    }

    shutdownStreamline();

    if(objectBuffer != nullptr && objectBufferMapped != nullptr)
        objectBuffer->Unmap(0, nullptr);
    objectBufferMapped = nullptr;

    if(fenceEvent != nullptr)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }

    if(windowHandle != nullptr && IsWindow(windowHandle))
        DestroyWindow(windowHandle);
    windowHandle = nullptr;

    if(instance != nullptr)
        UnregisterClassW(L"BlackHoleD3D12Window", instance);

    if(uiFont != nullptr)
        DeleteObject(uiFont);
    uiFont = nullptr;
    if(uiHeadingFont != nullptr)
        DeleteObject(uiHeadingFont);
    uiHeadingFont = nullptr;
    if(qualityPanelBrush != nullptr)
        DeleteObject(qualityPanelBrush);
    qualityPanelBrush = nullptr;
}

void D3D12Engine::createQualityPanel()
{
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&commonControls);

    uiFont = CreateFontW(
        -14,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    uiHeadingFont = CreateFontW(
        -16,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    qualityPanelBrush = CreateSolidBrush(RGB(15, 19, 27));

    qualityPanel = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        0,
        0,
        windowHandle,
        nullptr,
        instance,
        nullptr);

    if(qualityPanel == nullptr)
        throw std::runtime_error("Create quality panel failed");

    auto makeControl = [this](
                           LPCWSTR className,
                           LPCWSTR text,
                           DWORD style,
                           int controlId)
    {
        HWND control = CreateWindowExW(
            0,
            className,
            text,
            style,
            0,
            0,
            0,
            0,
            windowHandle,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance,
            nullptr);
        if(control == nullptr)
            throw std::runtime_error("Create quality control failed");
        if(uiFont != nullptr)
            SendMessageW(
                control,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(uiFont),
                TRUE);
        qualityControls.push_back(control);
        return control;
    };

    fpsLabel = makeControl(
        L"STATIC",
        L"FPS  --",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    if(uiHeadingFont != nullptr)
        SendMessageW(
            fpsLabel,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(uiHeadingFont),
            TRUE);

    qualityHeader = makeControl(
        L"STATIC",
        L"IMAGE QUALITY",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    if(uiHeadingFont != nullptr)
        SendMessageW(
            qualityHeader,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(uiHeadingFont),
            TRUE);
    qualityHint = makeControl(
        L"STATIC",
        L"Live controls - changes apply immediately",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);

    renderGroup = makeControl(
        L"BUTTON",
        L"Ray tracing / internal resolution",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0);
    rayTracingCheck = makeControl(
        L"BUTTON",
        L"Ray tracing (full black-hole effect)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        IDC_QUALITY_RAYTRACE);
    renderScaleLabel = makeControl(
        L"STATIC",
        L"Render scale",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    renderScaleTrack = makeControl(
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ,
        IDC_QUALITY_SCALE);
    renderScaleValue = makeControl(
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    rayStepsLabel = makeControl(
        L"STATIC",
        L"Ray integration steps",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    rayStepsTrack = makeControl(
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ,
        IDC_QUALITY_STEPS);
    rayStepsValue = makeControl(
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);

    upscaleGroup = makeControl(
        L"BUTTON",
        L"Super resolution",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0);
    dlssCheck = makeControl(
        L"BUTTON",
        L"NVIDIA DLSS Super Resolution",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        IDC_QUALITY_DLSS);
    dlssModeLabel = makeControl(
        L"STATIC",
        L"DLSS mode",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    dlssModeCombo = makeControl(
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        IDC_QUALITY_DLSS_MODE);
    SendMessageW(
        dlssModeCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(L"Quality (highest)"));
    SendMessageW(
        dlssModeCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(L"Balanced"));
    SendMessageW(
        dlssModeCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(L"Performance"));
    SendMessageW(
        dlssModeCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(L"Ultra Performance"));
    dlssStatus = makeControl(
        L"STATIC",
        L"DLSS status: checking runtime...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);

    accumulationGroup = makeControl(
        L"BUTTON",
        L"Temporal reconstruction",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0);
    taaLabel = makeControl(
        L"STATIC",
        L"Ray accumulation samples",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    taaCombo = makeControl(
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        IDC_QUALITY_TAA);
    const wchar_t* taaOptions[] =
    {
        L"1 sample (fast)",
        L"2 samples",
        L"4 samples",
        L"8 samples",
        L"16 samples",
        L"Unlimited"
    };
    for(const wchar_t* option : taaOptions)
        SendMessageW(
            taaCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(option));
    vsyncCheck = makeControl(
        L"BUTTON",
        L"VSync (display-paced)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        IDC_QUALITY_VSYNC);

    statusLabel = makeControl(
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    outputLabel = makeControl(
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0);
    resetButton = makeControl(
        L"BUTTON",
        L"Reset quality defaults",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        IDC_QUALITY_RESET);

    SendMessageW(
        renderScaleTrack,
        TBM_SETRANGE,
        TRUE,
        MAKELONG(25, 100));
    SendMessageW(renderScaleTrack, TBM_SETTICFREQ, 5, 0);
    SendMessageW(
        rayStepsTrack,
        TBM_SETRANGE,
        TRUE,
        MAKELONG(512, 16384));
    SendMessageW(rayStepsTrack, TBM_SETTICFREQ, 2048, 0);

    layoutQualityPanel();
    updateQualityPanel();
}

void D3D12Engine::layoutQualityPanel()
{
    if(windowHandle == nullptr)
        return;

    RECT clientRect{};
    GetClientRect(windowHandle, &clientRect);
    const int clientWidth = std::max<int>(clientRect.right, 1);
    const int clientHeight = std::max<int>(clientRect.bottom, 1);
    const int panelWidth = std::min(QUALITY_PANEL_WIDTH, clientWidth);
    const int panelX = clientWidth - panelWidth;
    const int left = panelX + 16;
    const int contentWidth = std::max(panelWidth - 32, 120);
    const int valueWidth = 72;
    const int trackWidth = std::max(contentWidth - valueWidth - 8, 80);

    MoveWindow(qualityPanel, panelX, 0, panelWidth, clientHeight, TRUE);
    MoveWindow(fpsLabel, 12, 10, 180, 28, TRUE);
    MoveWindow(qualityHeader, left, 12, contentWidth, 24, TRUE);
    MoveWindow(qualityHint, left, 36, contentWidth, 20, TRUE);

    MoveWindow(renderGroup, left, 62, contentWidth, 178, TRUE);
    MoveWindow(rayTracingCheck, left + 12, 84, contentWidth - 24, 22, TRUE);
    MoveWindow(renderScaleLabel, left + 12, 113, contentWidth - 24, 18, TRUE);
    MoveWindow(renderScaleTrack, left + 12, 132, trackWidth, 28, TRUE);
    MoveWindow(renderScaleValue, left + 12 + trackWidth + 8, 136, valueWidth, 22, TRUE);
    MoveWindow(rayStepsLabel, left + 12, 166, contentWidth - 24, 18, TRUE);
    MoveWindow(rayStepsTrack, left + 12, 185, trackWidth, 28, TRUE);
    MoveWindow(rayStepsValue, left + 12 + trackWidth + 8, 189, valueWidth, 22, TRUE);

    MoveWindow(upscaleGroup, left, 248, contentWidth, 118, TRUE);
    MoveWindow(dlssCheck, left + 12, 270, contentWidth - 24, 22, TRUE);
    MoveWindow(dlssModeLabel, left + 12, 298, 80, 18, TRUE);
    MoveWindow(dlssModeCombo, left + 94, 294, contentWidth - 106, 24, TRUE);
    MoveWindow(dlssStatus, left + 12, 329, contentWidth - 24, 28, TRUE);

    MoveWindow(accumulationGroup, left, 374, contentWidth, 103, TRUE);
    MoveWindow(taaLabel, left + 12, 396, 145, 18, TRUE);
    MoveWindow(taaCombo, left + 160, 392, contentWidth - 172, 24, TRUE);
    MoveWindow(vsyncCheck, left + 12, 431, contentWidth - 24, 22, TRUE);

    MoveWindow(statusLabel, left, 486, contentWidth, 20, TRUE);
    MoveWindow(outputLabel, left, 508, contentWidth, 20, TRUE);
    MoveWindow(resetButton, left, clientHeight - 42, contentWidth, 28, TRUE);
}

void D3D12Engine::updateQualityFps(double fps)
{
    if(fpsLabel == nullptr)
        return;

    std::wostringstream text;
    text << L"FPS  " << std::fixed << std::setprecision(1) << fps;
    SetWindowTextW(fpsLabel, text.str().c_str());
}

void D3D12Engine::updateFps(double fps)
{
    updateQualityFps(fps);
}

void D3D12Engine::updateQualityPanel()
{
    if(qualityPanel == nullptr)
        return;

    SendMessageW(
        rayTracingCheck,
        BM_SETCHECK,
        settings.rayTracing ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        dlssCheck,
        BM_SETCHECK,
        settings.dlss ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        vsyncCheck,
        BM_SETCHECK,
        settings.vsync ? BST_CHECKED : BST_UNCHECKED,
        0);

    const int scalePercent = std::clamp(
        static_cast<int>(std::lround(
            100.0 * static_cast<double>(RENDER_WIDTH) /
            static_cast<double>(std::max(WIDTH, 1)))),
        25,
        100);
    SendMessageW(renderScaleTrack, TBM_SETPOS, TRUE, scalePercent);
    SendMessageW(
        rayStepsTrack,
        TBM_SETPOS,
        TRUE,
        std::clamp<int>(settings.maxSteps, 512, 16384));
    SendMessageW(
        dlssModeCombo,
        CB_SETCURSEL,
        static_cast<int>(settings.dlssMode),
        0);

    int taaSelection = 0;
    switch(settings.temporalSampleLimit)
    {
    case 2:
        taaSelection = 1;
        break;
    case 4:
        taaSelection = 2;
        break;
    case 8:
        taaSelection = 3;
        break;
    case 16:
        taaSelection = 4;
        break;
    case 0:
        taaSelection = 5;
        break;
    default:
        taaSelection = 0;
        break;
    }
    SendMessageW(taaCombo, CB_SETCURSEL, taaSelection, 0);

    EnableWindow(dlssCheck, dlssActive ? TRUE : FALSE);
    EnableWindow(dlssModeCombo, dlssActive ? TRUE : FALSE);

    std::wostringstream scaleText;
    scaleText << std::fixed << std::setprecision(1)
              << (100.0 * static_cast<double>(RENDER_WIDTH) /
                  static_cast<double>(std::max(WIDTH, 1)))
              << L"%  " << RENDER_WIDTH << L"x" << RENDER_HEIGHT;
    SetWindowTextW(renderScaleValue, scaleText.str().c_str());

    std::wostringstream stepsText;
    stepsText << settings.maxSteps << L" steps";
    SetWindowTextW(rayStepsValue, stepsText.str().c_str());

    if(dlssActive)
    {
        std::wstring text = L"DLSS active | ";
        text += dlssModeText(settings.dlssMode);
        text += L" | preset K";
        SetWindowTextW(dlssStatus, text.c_str());
    }
    else
    {
        SetWindowTextW(
            dlssStatus,
            settings.dlss
                ? L"DLSS unavailable | native compositor fallback"
                : L"DLSS disabled | native compositor fallback");
    }

    SetWindowTextW(
        statusLabel,
        settings.rayTracing
            ? L"Ray tracing: ON | frame pacing: unlimited"
            : L"Ray tracing: OFF | frame pacing: unlimited");

    std::wostringstream outputText;
    outputText << L"Output: " << WIDTH << L"x" << HEIGHT
               << L" | API: Direct3D 12";
    SetWindowTextW(outputLabel, outputText.str().c_str());
}

void D3D12Engine::toggleQualityPanel()
{
    qualityPanelVisible = !qualityPanelVisible;
    const int visibility = qualityPanelVisible ? SW_SHOW : SW_HIDE;
    ShowWindow(qualityPanel, visibility);
    for(HWND control : qualityControls)
    {
        if(control != fpsLabel)
            ShowWindow(control, visibility);
    }
    ShowWindow(fpsLabel, SW_SHOW);
}

void D3D12Engine::resetAccumulation()
{
    accumulatedSampleCount = 0;
    hasPreviousCamera = false;
    currentJitterX = 0.0f;
    currentJitterY = 0.0f;
}

void D3D12Engine::recreateRayResources(int renderWidth, int renderHeight)
{
    renderWidth = std::clamp(renderWidth, 64, 8192);
    renderHeight = std::clamp(renderHeight, 64, 8192);
    if(renderWidth == RENDER_WIDTH && renderHeight == RENDER_HEIGHT)
    {
        resetAccumulation();
        updateQualityPanel();
        return;
    }

    waitForGpu();
    if(objectBuffer != nullptr && objectBufferMapped != nullptr)
        objectBuffer->Unmap(0, nullptr);
    objectBufferMapped = nullptr;

    outputTexture.Reset();
    materialTexture.Reset();
    accumulationBuffer.Reset();
    materialAccumulationBuffer.Reset();
    objectBuffer.Reset();
#ifdef BLACKHOLE_HAS_STREAMLINE
    dlssOutputTexture.Reset();
    dlssDepthTexture.Reset();
    dlssMotionVectorTexture.Reset();
#endif

    RENDER_WIDTH = renderWidth;
    RENDER_HEIGHT = renderHeight;
    settings.renderWidth = renderWidth;
    settings.renderHeight = renderHeight;
    settings.renderScale = static_cast<float>(renderWidth) /
                           static_cast<float>(std::max(WIDTH, 1));
    outputTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    materialTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dlssOutputTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dlssDepthTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dlssMotionVectorTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    createRayResources();
    clearInitialResources();
    resetAccumulation();
    updateQualityPanel();
}

void D3D12Engine::applyRenderScale(int percent)
{
    percent = std::clamp(percent, 25, 100);
    const float scale = static_cast<float>(percent) / 100.0f;
    const int renderWidth = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(WIDTH) * scale)),
        64,
        8192);
    const int renderHeight = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(HEIGHT) * scale)),
        64,
        8192);
    settings.renderScale = scale;
    recreateRayResources(renderWidth, renderHeight);
}

void D3D12Engine::applyRaySteps(int steps)
{
    settings.maxSteps = static_cast<std::uint32_t>(
        std::clamp(steps, 512, 16384));
    resetAccumulation();
    updateQualityPanel();
}

void D3D12Engine::applyTemporalSamples(int selection)
{
    static constexpr std::uint32_t samples[] = {1, 2, 4, 8, 16, 0};
    selection = std::clamp(selection, 0, 5);
    settings.temporalSampleLimit = samples[selection];
    resetAccumulation();
    updateQualityPanel();
}

void D3D12Engine::applyDlssMode(int selection)
{
    selection = std::clamp(selection, 0, 3);
    const DlssQualityMode previousMode = settings.dlssMode;
    settings.dlssMode = static_cast<DlssQualityMode>(selection);

#ifdef BLACKHOLE_HAS_STREAMLINE
    if(dlssActive && dlssSetOptions != nullptr)
    {
        const sl::DLSSOptions previousOptions = dlssOptions;
        dlssOptions.mode = toStreamlineDlssMode(settings.dlssMode);
        waitForGpu();
        const sl::Result result = dlssSetOptions(dlssViewport, dlssOptions);
        if(result != sl::Result::eOk)
        {
            dlssOptions = previousOptions;
            settings.dlssMode = previousMode;
            std::cerr << "DLSS mode change was rejected (code "
                      << static_cast<int>(result) << ")\n";
            updateQualityPanel();
            return;
        }

        int renderWidth = static_cast<int>(std::lround(
            static_cast<float>(WIDTH) *
            defaultRenderScaleForDlssMode(settings.dlssMode)));
        int renderHeight = static_cast<int>(std::lround(
            static_cast<float>(HEIGHT) *
            defaultRenderScaleForDlssMode(settings.dlssMode)));
        sl::DLSSOptimalSettings optimal{};
        if(dlssGetOptimalSettings != nullptr &&
           dlssGetOptimalSettings(dlssOptions, optimal) == sl::Result::eOk &&
           optimal.optimalRenderWidth > 0 && optimal.optimalRenderHeight > 0)
        {
            renderWidth = static_cast<int>(optimal.optimalRenderWidth);
            renderHeight = static_cast<int>(optimal.optimalRenderHeight);
        }
        recreateRayResources(renderWidth, renderHeight);
        return;
    }
#endif

    resetAccumulation();
    updateQualityPanel();
}

void D3D12Engine::resetQualityDefaults()
{
    settings.rayTracing = DEFAULT_RAYTRACING;
    settings.dlss = DEFAULT_DLSS;
    settings.dlssMode = DlssQualityMode::Quality;
    settings.maxSteps = DEFAULT_MAX_STEPS;
    settings.temporalSampleLimit = DEFAULT_TEMPORAL_SAMPLE_LIMIT;
    settings.vsync = false;

    applyDlssMode(static_cast<int>(DlssQualityMode::Quality));
    if(!dlssActive)
        recreateRayResources(DEFAULT_RENDER_WIDTH, DEFAULT_RENDER_HEIGHT);
    resetAccumulation();
    updateQualityPanel();
}

void D3D12Engine::handleQualityCommand(WPARAM wParam, LPARAM lParam)
{
    const int controlId = static_cast<int>(LOWORD(wParam));
    const int notification = static_cast<int>(HIWORD(wParam));
    if(notification != BN_CLICKED &&
       !(controlId == IDC_QUALITY_TAA ||
         controlId == IDC_QUALITY_DLSS_MODE))
        return;

    switch(controlId)
    {
    case IDC_QUALITY_RAYTRACE:
        settings.rayTracing = SendMessageW(
            rayTracingCheck,
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;
        resetAccumulation();
        updateQualityPanel();
        break;

    case IDC_QUALITY_DLSS:
        settings.dlss = SendMessageW(
            dlssCheck,
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;
        resetAccumulation();
        updateQualityPanel();
        break;

    case IDC_QUALITY_VSYNC:
        settings.vsync = SendMessageW(
            vsyncCheck,
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;
        break;

    case IDC_QUALITY_DLSS_MODE:
        if(notification == CBN_SELCHANGE)
            applyDlssMode(static_cast<int>(SendMessageW(
                dlssModeCombo,
                CB_GETCURSEL,
                0,
                0)));
        break;

    case IDC_QUALITY_TAA:
        if(notification == CBN_SELCHANGE)
            applyTemporalSamples(static_cast<int>(SendMessageW(
                taaCombo,
                CB_GETCURSEL,
                0,
                0)));
        break;

    case IDC_QUALITY_RESET:
        resetQualityDefaults();
        break;

    default:
        break;
    }

    (void)lParam;
}

void D3D12Engine::handleQualityScroll(WPARAM wParam, LPARAM lParam)
{
    HWND control = reinterpret_cast<HWND>(lParam);
    const int notification = static_cast<int>(LOWORD(wParam));
    if(control == renderScaleTrack)
    {
        const int position = static_cast<int>(SendMessageW(
            renderScaleTrack,
            TBM_GETPOS,
            0,
            0));
        const int previewWidth = static_cast<int>(std::lround(
            static_cast<float>(WIDTH) * position / 100.0f));
        const int previewHeight = static_cast<int>(std::lround(
            static_cast<float>(HEIGHT) * position / 100.0f));
        std::wostringstream text;
        text << position << L"%  " << previewWidth << L"x" << previewHeight;
        SetWindowTextW(renderScaleValue, text.str().c_str());
        if(notification != TB_THUMBTRACK)
            applyRenderScale(position);
    }
    else if(control == rayStepsTrack)
    {
        const int position = static_cast<int>(SendMessageW(
            rayStepsTrack,
            TBM_GETPOS,
            0,
            0));
        std::wstring text = std::to_wstring(position) + L" steps";
        SetWindowTextW(rayStepsValue, text.c_str());
        if(notification != TB_THUMBTRACK)
            applyRaySteps(position);
    }
}

void D3D12Engine::createWindow()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &D3D12Engine::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"BlackHoleD3D12Window";

    if(RegisterClassExW(&windowClass) == 0 &&
       GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        throw std::runtime_error("RegisterClassExW failed");
    }

    constexpr DWORD windowStyle =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT clientRect{0, 0, WIDTH, HEIGHT};
    if(!AdjustWindowRect(&clientRect, windowStyle, FALSE))
        throw std::runtime_error("AdjustWindowRect failed");

    windowHandle = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"Black Hole - Direct3D 12",
        windowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        nullptr,
        nullptr,
        instance,
        this);
    if(windowHandle == nullptr)
        throw std::runtime_error("CreateWindowExW failed");

    createQualityPanel();
    ShowWindow(windowHandle, SW_SHOW);
    UpdateWindow(windowHandle);
}

void D3D12Engine::initializeD3D12()
{
    // Streamline must be initialized before the first DXGI/D3D call. It is
    // optional at runtime; the renderer falls back cleanly when its DLLs are
    // absent or the GPU/driver does not support DLSS.
    initializeStreamline();
    createDeviceAndQueue();
    createCommandObjects();
    createSwapChain();
    createRenderTargets();
    createDepthBuffer();
    createDescriptorHeap();
    createRayResources();
    createGridGeometry();
    createPipelines();
    clearInitialResources();
    updateQualityPanel();
}

void D3D12Engine::initializeStreamline()
{
#ifdef BLACKHOLE_HAS_STREAMLINE
    if(!settings.dlss)
        return;

    streamlineRuntimeDirectory = findStreamlineRuntimeDirectory(
        shaderDirectory);
    if(streamlineRuntimeDirectory.empty())
    {
        std::cerr << "DLSS runtime not found; using native compositor fallback.\n";
        return;
    }

    const std::filesystem::path interposerPath =
        streamlineRuntimeDirectory / "sl.interposer.dll";
    streamline.module = LoadLibraryW(interposerPath.c_str());
    if(streamline.module == nullptr)
    {
        std::cerr << "Unable to load " << interposerPath.string()
                  << "; using native compositor fallback.\n";
        return;
    }

    streamline.init = loadStreamlineFunction<StreamlineApi::Init>(
        streamline.module,
        "slInit");
    streamline.shutdown = loadStreamlineFunction<StreamlineApi::Shutdown>(
        streamline.module,
        "slShutdown");
    streamline.setD3DDevice =
        loadStreamlineFunction<StreamlineApi::SetD3DDevice>(
            streamline.module,
            "slSetD3DDevice");
    streamline.getFeatureFunction =
        loadStreamlineFunction<StreamlineApi::GetFeatureFunction>(
            streamline.module,
            "slGetFeatureFunction");
    streamline.setConstants =
        loadStreamlineFunction<StreamlineApi::SetConstants>(
            streamline.module,
            "slSetConstants");
    streamline.evaluateFeature =
        loadStreamlineFunction<StreamlineApi::EvaluateFeature>(
            streamline.module,
            "slEvaluateFeature");
    streamline.getNewFrameToken =
        loadStreamlineFunction<StreamlineApi::GetNewFrameToken>(
            streamline.module,
            "slGetNewFrameToken");
    streamline.upgradeInterface =
        loadStreamlineFunction<StreamlineApi::UpgradeInterface>(
            streamline.module,
            "slUpgradeInterface");

    if(streamline.init == nullptr || streamline.shutdown == nullptr ||
       streamline.setD3DDevice == nullptr ||
       streamline.getFeatureFunction == nullptr ||
       streamline.setConstants == nullptr ||
       streamline.evaluateFeature == nullptr ||
       streamline.getNewFrameToken == nullptr ||
       streamline.upgradeInterface == nullptr)
    {
        std::cerr << "Streamline core exports are incomplete; using native "
                     "compositor fallback.\n";
        shutdownStreamline();
        return;
    }

    std::error_code directoryError;
    streamlineLogDirectory = streamlineRuntimeDirectory / "logs";
    std::filesystem::create_directories(
        streamlineLogDirectory,
        directoryError);

    const sl::Feature featuresToLoad[] = {sl::kFeatureDLSS};
    const wchar_t* pluginPath = streamlineRuntimeDirectory.c_str();
    static constexpr char PROJECT_ID[] =
        "3e7853c7-6a2b-4b3c-97b0-3e2bb261b3b1";
    static constexpr char ENGINE_VERSION[] = "BlackHole-D3D12-1.0";

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = &pluginPath;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = streamlineLogDirectory.c_str();
    preferences.logMessageCallback = &streamlineLogMessage;
    preferences.flags =
        sl::PreferenceFlags::eDisableCLStateTracking |
        sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = featuresToLoad;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = ENGINE_VERSION;
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eD3D12;

    const sl::Result result = streamline.init(preferences, sl::kSDKVersion);
    if(result != sl::Result::eOk)
    {
        std::cerr << "Streamline initialization failed (code "
                  << static_cast<int>(result)
                  << "); using native compositor fallback.\n";
        shutdownStreamline();
        return;
    }

    streamlineInitialized = true;
    std::cout << "Streamline: initialized for DLSS Quality\n";
#else
    // This build was configured without the optional Streamline headers.
#endif
}

void D3D12Engine::connectStreamlineDevice()
{
#ifdef BLACKHOLE_HAS_STREAMLINE
    if(!streamlineInitialized)
        return;

    sl::Result result = streamline.setD3DDevice(device.Get());
    if(result != sl::Result::eOk)
    {
        std::cerr << "Streamline could not attach to the D3D12 device (code "
                  << static_cast<int>(result)
                  << "); using native compositor fallback.\n";
        shutdownStreamline();
        return;
    }

    auto getFeatureFunction = [this](const char* name) -> void*
    {
        void* function = nullptr;
        if(streamline.getFeatureFunction(
               sl::kFeatureDLSS,
               name,
               function) != sl::Result::eOk)
            return nullptr;
        return function;
    };

    dlssSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(
        getFeatureFunction("slDLSSSetOptions"));
    dlssGetOptimalSettings =
        reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(
            getFeatureFunction("slDLSSGetOptimalSettings"));
    if(dlssSetOptions == nullptr || dlssGetOptimalSettings == nullptr)
    {
        std::cerr << "Streamline DLSS plugin exports are unavailable; using "
                     "native compositor fallback.\n";
        shutdownStreamline();
        return;
    }

    dlssOptions = sl::DLSSOptions{};
    dlssOptions.mode = toStreamlineDlssMode(settings.dlssMode);
    dlssOptions.outputWidth = static_cast<std::uint32_t>(WIDTH);
    dlssOptions.outputHeight = static_cast<std::uint32_t>(HEIGHT);
    dlssOptions.colorBuffersHDR = sl::eFalse;
    dlssOptions.useAutoExposure = sl::eTrue;
    dlssOptions.alphaUpscalingEnabled = sl::eFalse;
    dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
    dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
    dlssOptions.performancePreset = sl::DLSSPreset::ePresetK;
    dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetK;
    dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetK;

    result = dlssSetOptions(dlssViewport, dlssOptions);
    if(result != sl::Result::eOk)
    {
        std::cerr << "DLSS options were rejected (code "
                  << static_cast<int>(result)
                  << "); using native compositor fallback.\n";
        shutdownStreamline();
        return;
    }

    sl::DLSSOptimalSettings optimalSettings{};
    result = dlssGetOptimalSettings(dlssOptions, optimalSettings);
    if(result == sl::Result::eOk)
    {
        std::cout << "DLSS Quality source recommendation: "
                  << optimalSettings.optimalRenderWidth << " x "
                  << optimalSettings.optimalRenderHeight << "\n";
    }
    else
    {
        std::cerr << "DLSS optimal-resolution query failed (code "
                  << static_cast<int>(result)
                  << "); continuing with configured render resolution.\n";
    }

    dlssActive = true;
    std::wcout << L"DLSS: enabled (" << dlssModeText(settings.dlssMode)
               << L", preset K)\n";
#else
    (void)device;
#endif
}

void D3D12Engine::shutdownStreamline()
{
#ifdef BLACKHOLE_HAS_STREAMLINE
    if(streamlineDeviceProxy != nullptr && streamlineDeviceProxy != device.Get())
        streamlineDeviceProxy->Release();
    streamlineDeviceProxy = nullptr;
    if(streamlineFactoryProxy != nullptr && streamlineFactoryProxy != factory.Get())
        streamlineFactoryProxy->Release();
    streamlineFactoryProxy = nullptr;

    dlssActive = false;
    dlssFrameToken = nullptr;
    dlssSetOptions = nullptr;
    dlssGetOptimalSettings = nullptr;

    if(streamlineInitialized && streamline.shutdown != nullptr)
        streamline.shutdown();
    streamlineInitialized = false;

    if(streamline.module != nullptr)
    {
        FreeLibrary(streamline.module);
        streamline.module = nullptr;
    }

    streamline.init = nullptr;
    streamline.shutdown = nullptr;
    streamline.setD3DDevice = nullptr;
    streamline.getFeatureFunction = nullptr;
    streamline.setConstants = nullptr;
    streamline.evaluateFeature = nullptr;
    streamline.getNewFrameToken = nullptr;
    streamline.upgradeInterface = nullptr;

#endif
}

void D3D12Engine::createDeviceAndQueue()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    throwIfFailed(
        CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)),
        "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> selectedAdapter;
    DXGI_ADAPTER_DESC1 selectedDescription{};

    for(UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            adapterIndex,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter));
        if(result == DXGI_ERROR_NOT_FOUND)
            break;
        throwIfFailed(result, "EnumAdapterByGpuPreference");

        DXGI_ADAPTER_DESC1 description{};
        throwIfFailed(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;

        ComPtr<ID3D12Device> testDevice;
        if(SUCCEEDED(D3D12CreateDevice(
               adapter.Get(),
               D3D_FEATURE_LEVEL_11_0,
               IID_PPV_ARGS(&testDevice))))
        {
            selectedAdapter = adapter;
            selectedDescription = description;
            break;
        }
    }

    if(selectedAdapter == nullptr)
        throw std::runtime_error("No Direct3D 12-capable GPU was found");

    throwIfFailed(
        D3D12CreateDevice(
            selectedAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device)),
        "D3D12CreateDevice");
    std::wcout << L"D3D12 GPU: " << selectedDescription.Description << L"\n";

#ifdef BLACKHOLE_HAS_STREAMLINE
    // The device must be registered before any manual hook is activated. In
    // particular, this must precede upgrading the device/factory proxies.
    if(streamlineInitialized)
        connectStreamlineDevice();

    if(streamlineInitialized && streamline.upgradeInterface != nullptr)
    {
        streamlineFactoryProxy = factory.Get();
        const sl::Result result = streamline.upgradeInterface(
            reinterpret_cast<void**>(&streamlineFactoryProxy));
        if(result != sl::Result::eOk || streamlineFactoryProxy == nullptr)
        {
            std::cerr << "Streamline could not upgrade the DXGI factory (code "
                      << static_cast<int>(result)
                      << "); using native compositor fallback.\n";
            streamlineFactoryProxy = nullptr;
            shutdownStreamline();
        }
    }

    ID3D12Device* queueDevice = device.Get();
    if(streamlineInitialized && streamline.upgradeInterface != nullptr)
    {
        streamlineDeviceProxy = queueDevice;
        const sl::Result result = streamline.upgradeInterface(
            reinterpret_cast<void**>(&streamlineDeviceProxy));
        if(result != sl::Result::eOk || streamlineDeviceProxy == nullptr)
        {
            std::cerr << "Streamline could not upgrade the D3D12 device (code "
                      << static_cast<int>(result)
                      << "); using native compositor fallback.\n";
            streamlineDeviceProxy = nullptr;
            shutdownStreamline();
        }
        else
        {
            queueDevice = streamlineDeviceProxy;
        }
    }
#else
    ID3D12Device* queueDevice = device.Get();
#endif

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;
    const HRESULT queueResult = queueDevice->CreateCommandQueue(
            &queueDescription,
            IID_PPV_ARGS(&commandQueue));
    if(streamlineDeviceProxy != nullptr && streamlineDeviceProxy != device.Get())
        streamlineDeviceProxy->Release();
    streamlineDeviceProxy = nullptr;
    throwIfFailed(queueResult, "ID3D12Device::CreateCommandQueue");

    ComPtr<IDXGIFactory5> factory5;
    if(SUCCEEDED(factory.As(&factory5)))
    {
        BOOL tearing = FALSE;
        if(SUCCEEDED(factory5->CheckFeatureSupport(
               DXGI_FEATURE_PRESENT_ALLOW_TEARING,
               &tearing,
               sizeof(tearing))))
        {
            allowTearing = tearing == TRUE;
        }
    }
}

void D3D12Engine::createCommandObjects()
{
    throwIfFailed(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)),
        "CreateCommandAllocator");
    throwIfFailed(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)),
        "CreateCommandList");
    throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close");

    throwIfFailed(
        device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fence)),
        "CreateFence");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(fenceEvent == nullptr)
        throw std::runtime_error("CreateEventW failed");
}

void D3D12Engine::createSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(WIDTH);
    description.Height = static_cast<UINT>(HEIGHT);
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = FRAME_COUNT;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    description.Flags = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    IDXGIFactory6* swapChainFactory = factory.Get();
#ifdef BLACKHOLE_HAS_STREAMLINE
    if(streamlineFactoryProxy != nullptr)
        swapChainFactory = streamlineFactoryProxy;
#endif

    const HRESULT swapChainResult = swapChainFactory->CreateSwapChainForHwnd(
            commandQueue.Get(),
            windowHandle,
            &description,
            nullptr,
            nullptr,
            &swapChain1);
    if(streamlineFactoryProxy != nullptr && streamlineFactoryProxy != factory.Get())
        streamlineFactoryProxy->Release();
    streamlineFactoryProxy = nullptr;
    throwIfFailed(swapChainResult, "CreateSwapChainForHwnd");
    throwIfFailed(
        swapChain1.As(&swapChain),
        "Query IDXGISwapChain3");
    throwIfFailed(
        factory->MakeWindowAssociation(windowHandle, DXGI_MWA_NO_ALT_ENTER),
        "MakeWindowAssociation");
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
}

void D3D12Engine::createRenderTargets()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.NumDescriptors = FRAME_COUNT;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;
    throwIfFailed(
        device->CreateDescriptorHeap(
            &heapDescription,
            IID_PPV_ARGS(&rtvHeap)),
        "Create RTV descriptor heap");
    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(UINT bufferIndex = 0; bufferIndex < FRAME_COUNT; ++bufferIndex)
    {
        throwIfFailed(
            swapChain->GetBuffer(
                bufferIndex,
                IID_PPV_ARGS(&renderTargets[bufferIndex])),
            "IDXGISwapChain3::GetBuffer");
        device->CreateRenderTargetView(
            renderTargets[bufferIndex].Get(),
            nullptr,
            handle);
        handle.ptr += rtvDescriptorSize;
    }
}

void D3D12Engine::createDepthBuffer()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDescription.NumDescriptors = 1;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;
    throwIfFailed(
        device->CreateDescriptorHeap(
            &heapDescription,
            IID_PPV_ARGS(&dsvHeap)),
        "Create DSV descriptor heap");

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(WIDTH);
    description.Height = static_cast<UINT>(HEIGHT);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    throwIfFailed(
        device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&depthBuffer)),
        "Create depth buffer");

    D3D12_DEPTH_STENCIL_VIEW_DESC viewDescription{};
    viewDescription.Format = DXGI_FORMAT_D32_FLOAT;
    viewDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    viewDescription.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(
        depthBuffer.Get(),
        &viewDescription,
        dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Engine::createDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = DESCRIPTOR_COUNT;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    description.NodeMask = 0;
    throwIfFailed(
        device->CreateDescriptorHeap(
            &description,
            IID_PPV_ARGS(&srvUavHeap)),
        "Create SRV/UAV descriptor heap");
    srvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12Engine::createRayResources()
{
    const UINT64 pixelCount = static_cast<UINT64>(RENDER_WIDTH) *
                              static_cast<UINT64>(RENDER_HEIGHT);
    const UINT objectCapacity = static_cast<UINT>(
        std::max<std::size_t>(objects.size(), 1u));

    outputTexture = createTexture(
        RENDER_WIDTH,
        RENDER_HEIGHT,
        COLOR_TEXTURE_FORMAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    materialTexture = createTexture(
        RENDER_WIDTH,
        RENDER_HEIGHT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    accumulationBuffer = createBuffer(
        pixelCount * sizeof(glm::vec4),
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    materialAccumulationBuffer = createBuffer(
        pixelCount * sizeof(glm::vec4),
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    objectBuffer = createBuffer(
        static_cast<UINT64>(objectCapacity) * sizeof(Object),
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ);

#ifdef BLACKHOLE_HAS_STREAMLINE
    if(dlssActive)
    {
        dlssOutputTexture = createTexture(
            WIDTH,
            HEIGHT,
            COLOR_TEXTURE_FORMAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dlssDepthTexture = createTexture(
            RENDER_WIDTH,
            RENDER_HEIGHT,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dlssMotionVectorTexture = createTexture(
            RENDER_WIDTH,
            RENDER_HEIGHT,
            DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
#endif

    D3D12_RANGE readRange{0, 0};
    throwIfFailed(
        objectBuffer->Map(0, &readRange, &objectBufferMapped),
        "Map object buffer");

    D3D12_UNORDERED_ACCESS_VIEW_DESC textureUav{};
    textureUav.Format = COLOR_TEXTURE_FORMAT;
    textureUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    textureUav.Texture2D.MipSlice = 0;
    textureUav.Texture2D.PlaneSlice = 0;
    device->CreateUnorderedAccessView(
        outputTexture.Get(),
        nullptr,
        &textureUav,
        cpuDescriptor(COLOR_UAV_INDEX));

    textureUav.Format = MATERIAL_TEXTURE_FORMAT;
    device->CreateUnorderedAccessView(
        materialTexture.Get(),
        nullptr,
        &textureUav,
        cpuDescriptor(MATERIAL_UAV_INDEX));

    D3D12_UNORDERED_ACCESS_VIEW_DESC structuredUav{};
    structuredUav.Format = DXGI_FORMAT_UNKNOWN;
    structuredUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    structuredUav.Buffer.FirstElement = 0;
    structuredUav.Buffer.NumElements = static_cast<UINT>(pixelCount);
    structuredUav.Buffer.StructureByteStride = sizeof(glm::vec4);
    structuredUav.Buffer.CounterOffsetInBytes = 0;
    structuredUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(
        accumulationBuffer.Get(),
        nullptr,
        &structuredUav,
        cpuDescriptor(ACCUMULATION_UAV_INDEX));
    device->CreateUnorderedAccessView(
        materialAccumulationBuffer.Get(),
        nullptr,
        &structuredUav,
        cpuDescriptor(MATERIAL_ACCUMULATION_UAV_INDEX));

    D3D12_SHADER_RESOURCE_VIEW_DESC objectSrv{};
    objectSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    objectSrv.Format = DXGI_FORMAT_UNKNOWN;
    objectSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    objectSrv.Buffer.FirstElement = 0;
    objectSrv.Buffer.NumElements = objectCapacity;
    objectSrv.Buffer.StructureByteStride = sizeof(Object);
    objectSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(
        objectBuffer.Get(),
        &objectSrv,
        cpuDescriptor(OBJECT_SRV_INDEX));

    D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
    textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureSrv.Format = COLOR_TEXTURE_FORMAT;
    textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureSrv.Texture2D.MostDetailedMip = 0;
    textureSrv.Texture2D.MipLevels = 1;
    textureSrv.Texture2D.PlaneSlice = 0;
    textureSrv.Texture2D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        outputTexture.Get(),
        &textureSrv,
        cpuDescriptor(COLOR_SRV_INDEX));

    textureSrv.Format = MATERIAL_TEXTURE_FORMAT;
    device->CreateShaderResourceView(
        materialTexture.Get(),
        &textureSrv,
        cpuDescriptor(MATERIAL_SRV_INDEX));

#ifdef BLACKHOLE_HAS_STREAMLINE
    if(dlssOutputTexture != nullptr)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC dlssOutputUav{};
        dlssOutputUav.Format = COLOR_TEXTURE_FORMAT;
        dlssOutputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(
            dlssOutputTexture.Get(),
            nullptr,
            &dlssOutputUav,
            cpuDescriptor(DLSS_OUTPUT_UAV_INDEX));

        D3D12_SHADER_RESOURCE_VIEW_DESC dlssOutputSrv{};
        dlssOutputSrv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dlssOutputSrv.Format = COLOR_TEXTURE_FORMAT;
        dlssOutputSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dlssOutputSrv.Texture2D.MostDetailedMip = 0;
        dlssOutputSrv.Texture2D.MipLevels = 1;
        dlssOutputSrv.Texture2D.PlaneSlice = 0;
        dlssOutputSrv.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(
            dlssOutputTexture.Get(),
            &dlssOutputSrv,
            cpuDescriptor(DLSS_OUTPUT_SRV_INDEX));

        // The compositor consumes a contiguous color/material descriptor
        // range, so keep a second material SRV next to the DLSS output SRV.
        D3D12_SHADER_RESOURCE_VIEW_DESC dlssMaterialSrv = dlssOutputSrv;
        dlssMaterialSrv.Format = MATERIAL_TEXTURE_FORMAT;
        device->CreateShaderResourceView(
            materialTexture.Get(),
            &dlssMaterialSrv,
            cpuDescriptor(DLSS_MATERIAL_SRV_INDEX));

        D3D12_UNORDERED_ACCESS_VIEW_DESC dlssDepthUav{};
        dlssDepthUav.Format = DXGI_FORMAT_R32_FLOAT;
        dlssDepthUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(
            dlssDepthTexture.Get(),
            nullptr,
            &dlssDepthUav,
            cpuDescriptor(DLSS_DEPTH_UAV_INDEX));

        D3D12_UNORDERED_ACCESS_VIEW_DESC dlssMotionVectorUav{};
        dlssMotionVectorUav.Format = DXGI_FORMAT_R16G16_FLOAT;
        dlssMotionVectorUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(
            dlssMotionVectorTexture.Get(),
            nullptr,
            &dlssMotionVectorUav,
            cpuDescriptor(DLSS_MOTION_VECTOR_UAV_INDEX));
    }
#endif
}

void D3D12Engine::createGridGeometry()
{
    const int sideVertexCount = GRID_HALF_CELLS * 2 + 1;
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>(sideVertexCount * sideVertexCount));

    for(int zIndex = 0; zIndex < sideVertexCount; ++zIndex)
    {
        const float z = static_cast<float>(zIndex - GRID_HALF_CELLS) * GRID_STEP_RS;
        for(int xIndex = 0; xIndex < sideVertexCount; ++xIndex)
        {
            const float x = static_cast<float>(xIndex - GRID_HALF_CELLS) * GRID_STEP_RS;
            const float radius = std::sqrt(x * x + z * z);
            const float normalizedRadius = radius / GRID_WELL_RADIUS_RS;
            const float y = -GRID_WELL_DEPTH_RS /
                (1.0f + normalizedRadius * normalizedRadius);
            vertices.emplace_back(x, y, z);
        }
    }

    const auto vertexIndex = [sideVertexCount](int zIndex, int xIndex)
    {
        return static_cast<std::uint32_t>(zIndex * sideVertexCount + xIndex);
    };

    indices.reserve(static_cast<std::size_t>(sideVertexCount) *
                    static_cast<std::size_t>(sideVertexCount - 1) * 4u);
    for(int zIndex = 0; zIndex < sideVertexCount; ++zIndex)
    {
        for(int xIndex = 0; xIndex < sideVertexCount - 1; ++xIndex)
        {
            indices.push_back(vertexIndex(zIndex, xIndex));
            indices.push_back(vertexIndex(zIndex, xIndex + 1));
        }
    }
    for(int xIndex = 0; xIndex < sideVertexCount; ++xIndex)
    {
        for(int zIndex = 0; zIndex < sideVertexCount - 1; ++zIndex)
        {
            indices.push_back(vertexIndex(zIndex, xIndex));
            indices.push_back(vertexIndex(zIndex + 1, xIndex));
        }
    }

    gridVertexBuffer = createBuffer(
        static_cast<UINT64>(vertices.size() * sizeof(glm::vec3)),
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    gridIndexBuffer = createBuffer(
        static_cast<UINT64>(indices.size() * sizeof(std::uint32_t)),
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ);

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    throwIfFailed(
        gridVertexBuffer->Map(0, &readRange, &mapped),
        "Map grid vertex buffer");
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(glm::vec3));
    gridVertexBuffer->Unmap(0, nullptr);

    mapped = nullptr;
    throwIfFailed(
        gridIndexBuffer->Map(0, &readRange, &mapped),
        "Map grid index buffer");
    std::memcpy(mapped, indices.data(), indices.size() * sizeof(std::uint32_t));
    gridIndexBuffer->Unmap(0, nullptr);

    gridVertexView.BufferLocation = gridVertexBuffer->GetGPUVirtualAddress();
    gridVertexView.SizeInBytes = static_cast<UINT>(
        vertices.size() * sizeof(glm::vec3));
    gridVertexView.StrideInBytes = sizeof(glm::vec3);
    gridIndexView.BufferLocation = gridIndexBuffer->GetGPUVirtualAddress();
    gridIndexView.SizeInBytes = static_cast<UINT>(
        indices.size() * sizeof(std::uint32_t));
    gridIndexView.Format = DXGI_FORMAT_R32_UINT;
    gridIndexCount = static_cast<UINT>(indices.size());
}

void D3D12Engine::createPipelines()
{
    createRootSignatures();
    createPipelineStates(shaderDirectory);
}

void D3D12Engine::createRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE computeUavRange{};
    computeUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeUavRange.NumDescriptors = 4;
    computeUavRange.BaseShaderRegister = 0;
    computeUavRange.RegisterSpace = 0;
    computeUavRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE computeSrvRange{};
    computeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    computeSrvRange.NumDescriptors = 1;
    computeSrvRange.BaseShaderRegister = 0;
    computeSrvRange.RegisterSpace = 0;
    computeSrvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[0].Constants.Num32BitValues = 24;
    computeParameters[0].Constants.ShaderRegister = 0;
    computeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    computeParameters[1].DescriptorTable.pDescriptorRanges = &computeUavRange;
    computeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    computeParameters[2].DescriptorTable.pDescriptorRanges = &computeSrvRange;
    computeParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = 3;
    computeDescription.pParameters = computeParameters;
    computeDescription.NumStaticSamplers = 0;
    computeDescription.pStaticSamplers = nullptr;
    computeDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(
        &computeDescription,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &errors);
    if(FAILED(result))
    {
        if(errors != nullptr)
            throw std::runtime_error(static_cast<const char*>(
                errors->GetBufferPointer()));
        throwIfFailed(result, "Serialize compute root signature");
    }
    throwIfFailed(
        device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&computeRootSignature)),
        "Create compute root signature");

    D3D12_DESCRIPTOR_RANGE graphicsSrvRange{};
    graphicsSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    graphicsSrvRange.NumDescriptors = 2;
    graphicsSrvRange.BaseShaderRegister = 0;
    graphicsSrvRange.RegisterSpace = 0;
    graphicsSrvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsParameters[3]{};
    graphicsParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[0].Constants.Num32BitValues = 20;
    graphicsParameters[0].Constants.ShaderRegister = 0;
    // The grid matrix is consumed by the vertex shader and its color by the
    // grid pixel shader, so the shared b0 root constants must be visible to
    // both stages.
    graphicsParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    graphicsParameters[1].DescriptorTable.pDescriptorRanges = &graphicsSrvRange;
    graphicsParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[2].Constants.Num32BitValues = 8;
    graphicsParameters[2].Constants.ShaderRegister = 1;
    graphicsParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = 3;
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.NumStaticSamplers = 1;
    graphicsDescription.pStaticSamplers = &sampler;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized.Reset();
    errors.Reset();
    result = D3D12SerializeRootSignature(
        &graphicsDescription,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &errors);
    if(FAILED(result))
    {
        if(errors != nullptr)
            throw std::runtime_error(static_cast<const char*>(
                errors->GetBufferPointer()));
        throwIfFailed(result, "Serialize graphics root signature");
    }
    throwIfFailed(
        device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&graphicsRootSignature)),
        "Create graphics root signature");
}

void D3D12Engine::createPipelineStates(
    const std::filesystem::path& shaderDirectoryValue)
{
    const ComPtr<ID3DBlob> raytraceShader = compileShader(
        shaderDirectoryValue / "raytrace.hlsl",
        "CSMain",
        "cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDescription{};
    computeDescription.pRootSignature = computeRootSignature.Get();
    computeDescription.CS.pShaderBytecode = raytraceShader->GetBufferPointer();
    computeDescription.CS.BytecodeLength = raytraceShader->GetBufferSize();
    computeDescription.NodeMask = 0;
    computeDescription.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    throwIfFailed(
        device->CreateComputePipelineState(
            &computeDescription,
            IID_PPV_ARGS(&computePipeline)),
        "Create ray-tracing pipeline");

    const ComPtr<ID3DBlob> gridVertexShader = compileShader(
        shaderDirectoryValue / "scene.hlsl",
        "GridVS",
        "vs_5_1");
    const ComPtr<ID3DBlob> gridPixelShader = compileShader(
        shaderDirectoryValue / "scene.hlsl",
        "GridPS",
        "ps_5_1");
    const ComPtr<ID3DBlob> fullscreenVertexShader = compileShader(
        shaderDirectoryValue / "composite.hlsl",
        "FullscreenVS",
        "vs_5_1");
    const ComPtr<ID3DBlob> compositePixelShader = compileShader(
        shaderDirectoryValue / "composite.hlsl",
        "CompositePS",
        "ps_5_1");

    const D3D12_INPUT_ELEMENT_DESC inputElement{
        "POSITION",
        0,
        DXGI_FORMAT_R32G32B32_FLOAT,
        0,
        0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        0};
    const D3D12_RASTERIZER_DESC rasterizer = defaultRasterizer();
    const D3D12_BLEND_DESC gridBlend = alphaBlend();
    const D3D12_BLEND_DESC compositeBlend = premultipliedBlend();

    D3D12_DEPTH_STENCIL_DESC gridDepth{};
    gridDepth.DepthEnable = TRUE;
    gridDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gridDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    gridDepth.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gridDescription{};
    gridDescription.pRootSignature = graphicsRootSignature.Get();
    gridDescription.VS.pShaderBytecode = gridVertexShader->GetBufferPointer();
    gridDescription.VS.BytecodeLength = gridVertexShader->GetBufferSize();
    gridDescription.PS.pShaderBytecode = gridPixelShader->GetBufferPointer();
    gridDescription.PS.BytecodeLength = gridPixelShader->GetBufferSize();
    gridDescription.BlendState = gridBlend;
    gridDescription.SampleMask = UINT_MAX;
    gridDescription.RasterizerState = rasterizer;
    gridDescription.DepthStencilState = gridDepth;
    gridDescription.InputLayout = {&inputElement, 1};
    gridDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    gridDescription.NumRenderTargets = 1;
    gridDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gridDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gridDescription.SampleDesc.Count = 1;
    throwIfFailed(
        device->CreateGraphicsPipelineState(
            &gridDescription,
            IID_PPV_ARGS(&gridPipeline)),
        "Create grid pipeline");

    D3D12_DEPTH_STENCIL_DESC compositeDepth{};
    compositeDepth.DepthEnable = FALSE;
    compositeDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    compositeDepth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    compositeDepth.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC compositeDescription{};
    compositeDescription.pRootSignature = graphicsRootSignature.Get();
    compositeDescription.VS.pShaderBytecode = fullscreenVertexShader->GetBufferPointer();
    compositeDescription.VS.BytecodeLength = fullscreenVertexShader->GetBufferSize();
    compositeDescription.PS.pShaderBytecode = compositePixelShader->GetBufferPointer();
    compositeDescription.PS.BytecodeLength = compositePixelShader->GetBufferSize();
    compositeDescription.BlendState = compositeBlend;
    compositeDescription.SampleMask = UINT_MAX;
    compositeDescription.RasterizerState = rasterizer;
    compositeDescription.DepthStencilState = compositeDepth;
    compositeDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    compositeDescription.NumRenderTargets = 1;
    compositeDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    compositeDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    compositeDescription.SampleDesc.Count = 1;
    throwIfFailed(
        device->CreateGraphicsPipelineState(
            &compositeDescription,
            IID_PPV_ARGS(&compositePipeline)),
        "Create composite pipeline");
}

void D3D12Engine::clearInitialResources()
{
    throwIfFailed(commandAllocator->Reset(), "Reset initialization allocator");
    throwIfFailed(
        commandList->Reset(commandAllocator.Get(), nullptr),
        "Reset initialization command list");
    ID3D12DescriptorHeap* descriptorHeaps[] = {srvUavHeap.Get()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->ClearUnorderedAccessViewFloat(
        gpuDescriptor(COLOR_UAV_INDEX),
        cpuDescriptor(COLOR_UAV_INDEX),
        outputTexture.Get(),
        clearColor,
        0,
        nullptr);
    commandList->ClearUnorderedAccessViewFloat(
        gpuDescriptor(MATERIAL_UAV_INDEX),
        cpuDescriptor(MATERIAL_UAV_INDEX),
        materialTexture.Get(),
        clearColor,
        0,
        nullptr);

#ifdef BLACKHOLE_HAS_STREAMLINE
    if(dlssOutputTexture != nullptr)
    {
        const float depthClear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float motionClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        commandList->ClearUnorderedAccessViewFloat(
            gpuDescriptor(DLSS_OUTPUT_UAV_INDEX),
            cpuDescriptor(DLSS_OUTPUT_UAV_INDEX),
            dlssOutputTexture.Get(),
            clearColor,
            0,
            nullptr);
        commandList->ClearUnorderedAccessViewFloat(
            gpuDescriptor(DLSS_DEPTH_UAV_INDEX),
            cpuDescriptor(DLSS_DEPTH_UAV_INDEX),
            dlssDepthTexture.Get(),
            depthClear,
            0,
            nullptr);
        commandList->ClearUnorderedAccessViewFloat(
            gpuDescriptor(DLSS_MOTION_VECTOR_UAV_INDEX),
            cpuDescriptor(DLSS_MOTION_VECTOR_UAV_INDEX),
            dlssMotionVectorTexture.Get(),
            motionClear,
            0,
            nullptr);
    }
#endif

    throwIfFailed(commandList->Close(), "Close initialization command list");
    ID3D12CommandList* commandLists[] = {commandList.Get()};
    commandQueue->ExecuteCommandLists(1, commandLists);
    waitForGpu();
}

void D3D12Engine::processMessages()
{
    MSG message{};
    while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
    {
        if(message.message == WM_QUIT)
        {
            closing = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void D3D12Engine::render(double schwarzschildRadius, bool rayTracing)
{
    if(closing) return;

    throwIfFailed(commandAllocator->Reset(), "Reset frame allocator");
    throwIfFailed(
        commandList->Reset(commandAllocator.Get(), nullptr),
        "Reset frame command list");

    const float aspect = static_cast<float>(WIDTH) /
                         static_cast<float>(HEIGHT);
    const bool cameraChanged =
        !hasPreviousCamera ||
        glm::length(camera.pos - previousCameraPos) > 1.0e4f ||
        glm::length(camera.target - previousCameraTarget) > 1.0e4f ||
        std::fabs(camera.fovY - previousCameraFov) > 1.0e-5f;
    if(cameraChanged)
    {
        accumulatedSampleCount = 0;
        previousCameraPos = camera.pos;
        previousCameraTarget = camera.target;
        previousCameraFov = camera.fovY;
        hasPreviousCamera = true;
    }

    const bool sampleLimitReached =
        settings.temporalSampleLimit != 0 &&
        accumulatedSampleCount >= settings.temporalSampleLimit;
    currentJitterX = 0.0f;
    currentJitterY = 0.0f;
    if(rayTracing && !sampleLimitReached)
        recordRaytrace(schwarzschildRadius, aspect);
    else
        transitionTexture(
            outputTexture.Get(),
        outputTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    const bool useDlss = rayTracing && recordDlss(cameraChanged);
    transitionTexture(
        materialTexture.Get(),
        materialTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    recordGraphics(schwarzschildRadius, aspect, !rayTracing, useDlss);
    executeFrame();
}

void D3D12Engine::recordRaytrace(double schwarzschildRadius, float aspect)
{
    transitionTexture(
        outputTexture.Get(),
        outputTextureState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transitionTexture(
        materialTexture.Get(),
        materialTextureState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if(accumulatedSampleCount > 0)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = accumulationBuffer.Get();
        commandList->ResourceBarrier(1, &barrier);
        barrier.UAV.pResource = materialAccumulationBuffer.Get();
        commandList->ResourceBarrier(1, &barrier);
    }

    updateObjectBuffer();

    const float inverseSchwarzschildRadius = static_cast<float>(
        1.0 / schwarzschildRadius);
    const std::size_t jitterIndex = accumulatedSampleCount % JITTER_OFFSETS.size();
    RaytraceConstants constants{};
    constants.cameraPos = glm::vec4(camera.pos * inverseSchwarzschildRadius, 0.0f);
    constants.target = glm::vec4(camera.target * inverseSchwarzschildRadius, 0.0f);
    constants.fovYRadians = glm::radians(camera.fovY);
    constants.aspect = aspect;
    constants.renderWidth = static_cast<std::uint32_t>(RENDER_WIDTH);
    constants.renderHeight = static_cast<std::uint32_t>(RENDER_HEIGHT);
    constants.maxSteps = settings.maxSteps;
    constants.dLambda = static_cast<float>(
        D_LAMBDA_METERS / schwarzschildRadius);
    constants.escapeR = static_cast<float>(
        ESCAPE_R_METERS / schwarzschildRadius);
    constants.horizonR = 1.0f;
    constants.diskR1 = DISK_R1_RS;
    constants.diskR2 = DISK_R2_RS;
    constants.objectCount = static_cast<std::uint32_t>(objects.size());
    constants.sampleIndex = accumulatedSampleCount;
    constants.jitterX = JITTER_OFFSETS[jitterIndex][0];
    constants.jitterY = JITTER_OFFSETS[jitterIndex][1];
    currentJitterX = constants.jitterX;
    currentJitterY = constants.jitterY;

    ID3D12DescriptorHeap* descriptorHeaps[] = {srvUavHeap.Get()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetPipelineState(computePipeline.Get());
    commandList->SetComputeRootSignature(computeRootSignature.Get());
    commandList->SetComputeRoot32BitConstants(
        0,
        24,
        &constants,
        0);
    commandList->SetComputeRootDescriptorTable(
        1,
        gpuDescriptor(COLOR_UAV_INDEX));
    commandList->SetComputeRootDescriptorTable(
        2,
        gpuDescriptor(OBJECT_SRV_INDEX));
    commandList->Dispatch(
        (static_cast<UINT>(RENDER_WIDTH) + 7u) / 8u,
        (static_cast<UINT>(RENDER_HEIGHT) + 7u) / 8u,
        1);

    transitionTexture(
        outputTexture.Get(),
        outputTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTexture(
        materialTexture.Get(),
        materialTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if(accumulatedSampleCount < std::numeric_limits<std::uint32_t>::max() - 1u)
        ++accumulatedSampleCount;
}

bool D3D12Engine::recordDlss(bool cameraChanged)
{
#ifdef BLACKHOLE_HAS_STREAMLINE
    if(!settings.dlss || !dlssActive ||
       dlssOutputTexture == nullptr || dlssDepthTexture == nullptr ||
       dlssMotionVectorTexture == nullptr)
        return false;

    ++streamlineFrameIndex;
    sl::Result result = streamline.getNewFrameToken(
        dlssFrameToken,
        &streamlineFrameIndex);
    if(result != sl::Result::eOk || dlssFrameToken == nullptr)
    {
        std::cerr << "Streamline could not create a frame token (code "
                  << static_cast<int>(result)
                  << "); disabling DLSS for this run.\n";
        dlssActive = false;
        return false;
    }

    // The ray pass produces the input image in render resolution. DLSS reads
    // it as a non-pixel shader resource and writes the full-resolution output
    // UAV. The depth/motion inputs are explicit zero-motion/far-depth guides:
    // this visualization has no rasterized geometry depth, and a camera change
    // resets temporal history so stale reprojection is never reused.
    transitionTexture(
        outputTexture.Get(),
        outputTextureState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTexture(
        dlssOutputTexture.Get(),
        dlssOutputTextureState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transitionTexture(
        dlssDepthTexture.Get(),
        dlssDepthTextureState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transitionTexture(
        dlssMotionVectorTexture.Get(),
        dlssMotionVectorTextureState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const float depthClear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float motionClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->ClearUnorderedAccessViewFloat(
        gpuDescriptor(DLSS_DEPTH_UAV_INDEX),
        cpuDescriptor(DLSS_DEPTH_UAV_INDEX),
        dlssDepthTexture.Get(),
        depthClear,
        0,
        nullptr);
    commandList->ClearUnorderedAccessViewFloat(
        gpuDescriptor(DLSS_MOTION_VECTOR_UAV_INDEX),
        cpuDescriptor(DLSS_MOTION_VECTOR_UAV_INDEX),
        dlssMotionVectorTexture.Get(),
        motionClear,
        0,
        nullptr);

    const float inverseSchwarzschildRadius = static_cast<float>(
        1.0 / SagA.r_s);
    const glm::vec3 eye = camera.pos * inverseSchwarzschildRadius;
    const glm::vec3 target = camera.target * inverseSchwarzschildRadius;
    const glm::mat4 projection = glm::perspective(
        glm::radians(camera.fovY),
        static_cast<float>(WIDTH) / static_cast<float>(HEIGHT),
        0.05f,
        200.0f);

    const auto toStreamlineMatrix = [](const glm::mat4& matrix)
    {
        sl::float4x4 result{};
        for(std::uint32_t row = 0; row < 4; ++row)
        {
            result.row[row] = sl::float4(
                matrix[0][row],
                matrix[1][row],
                matrix[2][row],
                matrix[3][row]);
        }
        return result;
    };

    sl::Constants constants{};
    constants.cameraViewToClip = toStreamlineMatrix(projection);
    constants.clipToCameraView = toStreamlineMatrix(glm::inverse(projection));
    constants.clipToPrevClip = toStreamlineMatrix(glm::mat4(1.0f));
    constants.prevClipToClip = toStreamlineMatrix(glm::mat4(1.0f));
    constants.jitterOffset = sl::float2(currentJitterX, currentJitterY);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(eye.x, eye.y, eye.z);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward = glm::normalize(target - eye);
    glm::vec3 right = glm::cross(
        forward,
        glm::vec3(0.0f, 1.0f, 0.0f));
    if(glm::dot(right, right) < 1.0e-8f)
        right = glm::vec3(0.0f, 0.0f, 1.0f);
    else
        right = glm::normalize(right);
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    constants.cameraRight = sl::float3(right.x, right.y, right.z);
    constants.cameraFwd = sl::float3(forward.x, forward.y, forward.z);
    constants.cameraNear = 0.05f;
    constants.cameraFar = 200.0f;
    constants.cameraFOV = glm::radians(camera.fovY);
    constants.cameraAspectRatio =
        static_cast<float>(WIDTH) / static_cast<float>(HEIGHT);
    constants.depthInverted = sl::eFalse;
    constants.cameraMotionIncluded = sl::eTrue;
    constants.motionVectors3D = sl::eFalse;
    constants.reset = cameraChanged ? sl::eTrue : sl::eFalse;
    constants.orthographicProjection = sl::eFalse;
    constants.motionVectorsDilated = sl::eFalse;
    constants.motionVectorsJittered = sl::eFalse;

    result = streamline.setConstants(
        constants,
        *dlssFrameToken,
        dlssViewport);
    if(result != sl::Result::eOk)
    {
        std::cerr << "Streamline rejected the frame constants (code "
                  << static_cast<int>(result)
                  << "); disabling DLSS for this run.\n";
        dlssActive = false;
        transitionTexture(
            outputTexture.Get(),
            outputTextureState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return false;
    }

    const sl::Extent renderExtent{
        0,
        0,
        static_cast<std::uint32_t>(RENDER_WIDTH),
        static_cast<std::uint32_t>(RENDER_HEIGHT)};
    const sl::Extent outputExtent{
        0,
        0,
        static_cast<std::uint32_t>(WIDTH),
        static_cast<std::uint32_t>(HEIGHT)};

    sl::Resource colorIn{
        sl::ResourceType::eTex2d,
        outputTexture.Get(),
        static_cast<std::uint32_t>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    colorIn.width = static_cast<std::uint32_t>(RENDER_WIDTH);
    colorIn.height = static_cast<std::uint32_t>(RENDER_HEIGHT);
    colorIn.nativeFormat = static_cast<std::uint32_t>(COLOR_TEXTURE_FORMAT);
    colorIn.mipLevels = 1;
    colorIn.arrayLayers = 1;
    colorIn.flags = static_cast<std::uint32_t>(
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    sl::Resource colorOut{
        sl::ResourceType::eTex2d,
        dlssOutputTexture.Get(),
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    colorOut.width = static_cast<std::uint32_t>(WIDTH);
    colorOut.height = static_cast<std::uint32_t>(HEIGHT);
    colorOut.nativeFormat = static_cast<std::uint32_t>(COLOR_TEXTURE_FORMAT);
    colorOut.mipLevels = 1;
    colorOut.arrayLayers = 1;
    colorOut.flags = static_cast<std::uint32_t>(
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    sl::Resource depth{
        sl::ResourceType::eTex2d,
        dlssDepthTexture.Get(),
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    depth.width = static_cast<std::uint32_t>(RENDER_WIDTH);
    depth.height = static_cast<std::uint32_t>(RENDER_HEIGHT);
    depth.nativeFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT);
    depth.mipLevels = 1;
    depth.arrayLayers = 1;
    depth.flags = static_cast<std::uint32_t>(
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    sl::Resource motionVectors{
        sl::ResourceType::eTex2d,
        dlssMotionVectorTexture.Get(),
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    motionVectors.width = static_cast<std::uint32_t>(RENDER_WIDTH);
    motionVectors.height = static_cast<std::uint32_t>(RENDER_HEIGHT);
    motionVectors.nativeFormat = static_cast<std::uint32_t>(
        DXGI_FORMAT_R16G16_FLOAT);
    motionVectors.mipLevels = 1;
    motionVectors.arrayLayers = 1;
    motionVectors.flags = static_cast<std::uint32_t>(
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    sl::ResourceTag colorInTag{
        &colorIn,
        sl::kBufferTypeScalingInputColor,
        sl::eOnlyValidNow,
        &renderExtent};
    sl::ResourceTag colorOutTag{
        &colorOut,
        sl::kBufferTypeScalingOutputColor,
        sl::eValidUntilPresent,
        &outputExtent};
    sl::ResourceTag depthTag{
        &depth,
        sl::kBufferTypeDepth,
        sl::eOnlyValidNow,
        &renderExtent};
    sl::ResourceTag motionVectorTag{
        &motionVectors,
        sl::kBufferTypeMotionVectors,
        sl::eOnlyValidNow,
        &renderExtent};

    const sl::BaseStructure* inputs[] = {
        &dlssViewport,
        &colorInTag,
        &colorOutTag,
        &depthTag,
        &motionVectorTag};
    result = streamline.evaluateFeature(
        sl::kFeatureDLSS,
        *dlssFrameToken,
        inputs,
        static_cast<std::uint32_t>(std::size(inputs)),
        reinterpret_cast<sl::CommandBuffer*>(commandList.Get()));
    if(result != sl::Result::eOk)
    {
        std::cerr << "DLSS evaluation failed (code "
                  << static_cast<int>(result)
                  << "); disabling DLSS for this run.\n";
        dlssActive = false;
        transitionTexture(
            outputTexture.Get(),
            outputTextureState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return false;
    }

    // Streamline may replace the command-list bindings. recordGraphics binds
    // its own root signature and pipeline, but restoring the shader-visible
    // heap here also keeps the state explicit for the next pass.
    ID3D12DescriptorHeap* descriptorHeaps[] = {srvUavHeap.Get()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    transitionTexture(
        dlssOutputTexture.Get(),
        dlssOutputTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
#else
    (void)cameraChanged;
    return false;
#endif
}

void D3D12Engine::recordGraphics(
    double schwarzschildRadius,
    float aspect,
    bool drawFallback,
    bool useDlss)
{
    D3D12_RESOURCE_BARRIER backBufferBarrier{};
    backBufferBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBufferBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    backBufferBarrier.Transition.pResource = renderTargets[backBufferIndex].Get();
    backBufferBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    backBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    backBufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &backBufferBarrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = [&]
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvDescriptorSize;
        return handle;
    }();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        dsvHeap->GetCPUDescriptorHandleForHeapStart();

    const D3D12_VIEWPORT viewport{
        0.0f,
        0.0f,
        static_cast<float>(WIDTH),
        static_cast<float>(HEIGHT),
        0.0f,
        1.0f};
    const D3D12_RECT scissor{0, 0, WIDTH, HEIGHT};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(
        dsv,
        D3D12_CLEAR_FLAG_DEPTH,
        1.0f,
        0,
        0,
        nullptr);

    ID3D12DescriptorHeap* descriptorHeaps[] = {srvUavHeap.Get()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetGraphicsRootSignature(graphicsRootSignature.Get());

    if(schwarzschildRadius > 0.0 && gridIndexCount != 0)
    {
        const float inverseSchwarzschildRadius = static_cast<float>(
            1.0 / schwarzschildRadius);
        const glm::vec3 eye = camera.pos * inverseSchwarzschildRadius;
        const glm::vec3 target = camera.target * inverseSchwarzschildRadius;
        const glm::mat4 projection = glm::perspective(
            glm::radians(camera.fovY),
            aspect,
            0.05f,
            200.0f);
        const glm::mat4 view = glm::lookAt(
            eye,
            target,
            glm::vec3(0.0f, 1.0f, 0.0f));

        const GridConstants constants{
            projection * view,
            glm::vec4(0.86f, 0.53f, 0.38f, 0.78f)};
        commandList->SetPipelineState(gridPipeline.Get());
        commandList->SetGraphicsRoot32BitConstants(0, 20, &constants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commandList->IASetVertexBuffers(0, 1, &gridVertexView);
        commandList->IASetIndexBuffer(&gridIndexView);
        commandList->DrawIndexedInstanced(gridIndexCount, 1, 0, 0, 0);
    }

    const float inverseSchwarzschildRadius = static_cast<float>(
        1.0 / schwarzschildRadius);
    const glm::vec3 eye = camera.pos * inverseSchwarzschildRadius;
    const glm::vec3 target = camera.target * inverseSchwarzschildRadius;
    const glm::vec3 forward = glm::normalize(target - eye);
    glm::vec3 right = glm::cross(
        forward,
        glm::vec3(0.0f, 1.0f, 0.0f));
    if(glm::dot(right, right) < 1.0e-8f)
        right = glm::vec3(0.0f, 0.0f, 1.0f);
    else
        right = glm::normalize(right);
    const glm::vec3 screenUp = glm::normalize(glm::cross(right, forward));
    const glm::mat4 projection = glm::perspective(
        glm::radians(camera.fovY),
        aspect,
        0.05f,
        200.0f);
    const glm::mat4 view = glm::lookAt(
        eye,
        target,
        glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec4 centerClip = projection * view *
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 edgeClip = projection * view *
        glm::vec4(screenUp * 2.6f, 1.0f);

    CompositeConstants constants{};
    if(centerClip.w > 0.0f && edgeClip.w > 0.0f)
    {
        const float centerNdcX = centerClip.x / centerClip.w;
        const float centerNdcY = centerClip.y / centerClip.w;
        const float edgeNdcY = edgeClip.y / edgeClip.w;
        const float radius = std::fabs(edgeNdcY - centerNdcY) * 0.5f;
        constants.fallback = glm::vec4(
            centerNdcX * 0.5f + 0.5f,
            0.5f - centerNdcY * 0.5f,
            radius,
            aspect);
        constants.fallbackEnabled = drawFallback && radius > 1.0e-5f ? 1u : 0u;
    }

    commandList->SetPipelineState(compositePipeline.Get());
    commandList->SetGraphicsRootDescriptorTable(
        1,
        gpuDescriptor(useDlss
                          ? DLSS_OUTPUT_SRV_INDEX
                          : COLOR_SRV_INDEX));
    commandList->SetGraphicsRoot32BitConstants(2, 8, &constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(6, 1, 0, 0);

    backBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &backBufferBarrier);
}

void D3D12Engine::updateObjectBuffer()
{
    std::vector<Object> normalizedObjects = objects;
    const double inverseSchwarzschildRadius = 1.0 / SagA.r_s;
    const float scale = static_cast<float>(inverseSchwarzschildRadius);
    for(Object& object : normalizedObjects)
        object.posRadius *= scale;

    const std::size_t objectBytes = std::max<std::size_t>(
        normalizedObjects.size(), 1u) * sizeof(Object);
    if(!normalizedObjects.empty())
        std::memcpy(objectBufferMapped, normalizedObjects.data(), objectBytes);
    else
        std::memset(objectBufferMapped, 0, sizeof(Object));
}

void D3D12Engine::waitForGpu()
{
    const UINT64 fenceValue = nextFenceValue++;
    throwIfFailed(
        commandQueue->Signal(fence.Get(), fenceValue),
        "Signal fence");
    if(fence->GetCompletedValue() < fenceValue)
    {
        throwIfFailed(
            fence->SetEventOnCompletion(fenceValue, fenceEvent),
            "Set fence event");
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void D3D12Engine::executeFrame()
{
    throwIfFailed(commandList->Close(), "Close frame command list");
    ID3D12CommandList* commandLists[] = {commandList.Get()};
    commandQueue->ExecuteCommandLists(1, commandLists);

    const UINT syncInterval = settings.vsync ? 1u : 0u;
    const UINT presentFlags =
        (!settings.vsync && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    throwIfFailed(
        swapChain->Present(syncInterval, presentFlags),
        "IDXGISwapChain3::Present");
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    waitForGpu();
}

void D3D12Engine::transitionTexture(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES& currentState,
    D3D12_RESOURCE_STATES desiredState)
{
    if(currentState == desiredState) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter = desiredState;
    commandList->ResourceBarrier(1, &barrier);
    currentState = desiredState;
}

ComPtr<ID3DBlob> D3D12Engine::compileShader(
    const std::filesystem::path& path,
    const char* entryPoint,
    const char* target) const
{
    const std::string source = readTextFile(path);
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source.data(),
        source.size(),
        path.string().c_str(),
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &shader,
        &errors);
    if(FAILED(result))
        throw shaderError(path, entryPoint, target, errors.Get());
    return shader;
}

std::string D3D12Engine::readTextFile(const std::filesystem::path& path) const
{
    std::ifstream file(path, std::ios::binary);
    if(!file)
        throw std::runtime_error("Unable to open shader: " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

ComPtr<ID3D12Resource> D3D12Engine::createBuffer(
    UINT64 size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState) const
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = std::max<UINT64>(size, 1u);
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    const D3D12_HEAP_PROPERTIES properties = heapProperties(heapType);
    ComPtr<ID3D12Resource> resource;
    throwIfFailed(
        device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource)),
        "Create buffer");
    return resource;
}

ComPtr<ID3D12Resource> D3D12Engine::createTexture(
    int width,
    int height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState) const
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(width);
    description.Height = static_cast<UINT>(height);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;

    const D3D12_HEAP_PROPERTIES properties = heapProperties(
        D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;
    throwIfFailed(
        device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource)),
        "Create texture");
    return resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Engine::cpuDescriptor(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * srvUavDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Engine::gpuDescriptor(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle =
        srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * srvUavDescriptorSize;
    return handle;
}

LRESULT CALLBACK D3D12Engine::windowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if(message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* engine = static_cast<D3D12Engine*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(engine));
        engine->windowHandle = window;
    }

    auto* engine = reinterpret_cast<D3D12Engine*>(GetWindowLongPtrW(
        window,
        GWLP_USERDATA));
    if(engine != nullptr)
        return engine->handleMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT D3D12Engine::handleMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if(handleCameraMessage(windowHandle, message, wParam, lParam))
        return 0;

    switch(message)
    {
    case WM_KEYDOWN:
        if(wParam == VK_F1)
        {
            toggleQualityPanel();
            return 0;
        }
        if(wParam == VK_ESCAPE)
        {
            closing = true;
            DestroyWindow(windowHandle);
            return 0;
        }
        break;

    case WM_COMMAND:
        handleQualityCommand(wParam, lParam);
        return 0;

    case WM_HSCROLL:
        handleQualityScroll(wParam, lParam);
        return 0;

    case WM_SIZE:
        if(qualityPanel != nullptr)
            layoutQualityPanel();
        return 0;

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        if(control == fpsLabel)
        {
            SetTextColor(dc, RGB(245, 224, 150));
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        SetTextColor(dc, RGB(218, 224, 235));
        return reinterpret_cast<LRESULT>(qualityPanelBrush != nullptr
                                             ? qualityPanelBrush
                                             : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(218, 224, 235));
        SetBkColor(dc, RGB(15, 19, 27));
        return reinterpret_cast<LRESULT>(qualityPanelBrush != nullptr
                                             ? qualityPanelBrush
                                             : GetStockObject(BLACK_BRUSH));
    }

    case WM_CLOSE:
        closing = true;
        DestroyWindow(windowHandle);
        return 0;

    case WM_DESTROY:
        closing = true;
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }

    return DefWindowProcW(windowHandle, message, wParam, lParam);
}
