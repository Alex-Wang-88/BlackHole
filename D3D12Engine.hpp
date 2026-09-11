#ifndef BLACK_HOLE_D3D12_ENGINE_HPP
#define BLACK_HOLE_D3D12_ENGINE_HPP

#include "Scene.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#ifdef BLACKHOLE_HAS_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class D3D12Engine
{
public:
    D3D12Engine(
        const RenderSettings& settings,
        const std::filesystem::path& shaderDirectory);
    ~D3D12Engine();

    D3D12Engine(const D3D12Engine&) = delete;
    D3D12Engine& operator=(const D3D12Engine&) = delete;

    void processMessages();
    void render(double schwarzschildRadius, bool rayTracing);
    void updateFps(double fps);

    bool shouldClose() const { return closing; }
    HWND window() const { return windowHandle; }
    bool rayTracingEnabled() const { return settings.rayTracing; }
    bool dlssEnabled() const { return settings.dlss && dlssActive; }

    int WIDTH = DEFAULT_WINDOW_WIDTH;
    int HEIGHT = DEFAULT_WINDOW_HEIGHT;
    int RENDER_WIDTH = DEFAULT_RENDER_WIDTH;
    int RENDER_HEIGHT = DEFAULT_RENDER_HEIGHT;

private:
    static constexpr UINT FRAME_COUNT = 2;
    static constexpr UINT DESCRIPTOR_COUNT = 12;
    static constexpr UINT COLOR_UAV_INDEX = 0;
    static constexpr UINT MATERIAL_UAV_INDEX = 1;
    static constexpr UINT ACCUMULATION_UAV_INDEX = 2;
    static constexpr UINT MATERIAL_ACCUMULATION_UAV_INDEX = 3;
    static constexpr UINT OBJECT_SRV_INDEX = 4;
    static constexpr UINT COLOR_SRV_INDEX = 5;
    static constexpr UINT MATERIAL_SRV_INDEX = 6;
    static constexpr UINT DLSS_OUTPUT_UAV_INDEX = 7;
    static constexpr UINT DLSS_OUTPUT_SRV_INDEX = 8;
    static constexpr UINT DLSS_MATERIAL_SRV_INDEX = 9;
    static constexpr UINT DLSS_DEPTH_UAV_INDEX = 10;
    static constexpr UINT DLSS_MOTION_VECTOR_UAV_INDEX = 11;
    static constexpr int QUALITY_PANEL_WIDTH = 320;
    static constexpr int MIN_RENDER_WIDTH = 320;
    static constexpr int MIN_RENDER_HEIGHT = 240;
    static constexpr DWORD MAIN_WINDOW_STYLE =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
        WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    enum QualityControlId : int
    {
        IDC_QUALITY_RAYTRACE = 4101,
        IDC_QUALITY_DLSS = 4102,
        IDC_QUALITY_VSYNC = 4103,
        IDC_QUALITY_SCALE = 4104,
        IDC_QUALITY_STEPS = 4105,
        IDC_QUALITY_TAA = 4106,
        IDC_QUALITY_DLSS_MODE = 4107,
        IDC_QUALITY_RESET = 4108
    };

    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    RenderSettings settings;
    std::filesystem::path shaderDirectory;
    HINSTANCE instance = nullptr;
    HWND windowHandle = nullptr;
    HWND renderWindowHandle = nullptr;
    bool closing = false;
    bool allowTearing = false;
    bool windowSizing = false;
    bool resizePending = false;
    int pendingClientWidth = 0;
    int pendingClientHeight = 0;

    HWND qualityPanel = nullptr;
    HWND fpsLabel = nullptr;
    HWND qualityHeader = nullptr;
    HWND qualityHint = nullptr;
    HWND renderGroup = nullptr;
    HWND rayTracingCheck = nullptr;
    HWND renderScaleLabel = nullptr;
    HWND renderScaleTrack = nullptr;
    HWND renderScaleValue = nullptr;
    HWND rayStepsLabel = nullptr;
    HWND rayStepsTrack = nullptr;
    HWND rayStepsValue = nullptr;
    HWND upscaleGroup = nullptr;
    HWND dlssCheck = nullptr;
    HWND dlssModeLabel = nullptr;
    HWND dlssModeCombo = nullptr;
    HWND dlssStatus = nullptr;
    HWND accumulationGroup = nullptr;
    HWND taaLabel = nullptr;
    HWND taaCombo = nullptr;
    HWND vsyncCheck = nullptr;
    HWND statusLabel = nullptr;
    HWND outputLabel = nullptr;
    HWND resetButton = nullptr;
    HFONT uiFont = nullptr;
    HFONT uiHeadingFont = nullptr;
    HBRUSH qualityPanelBrush = nullptr;
    bool qualityPanelVisible = true;
    std::vector<HWND> qualityControls;
    int qualityPanelDragTarget = 0;
    int qualityPanelPreviewScale = -1;
    int qualityPanelPreviewSteps = -1;
    bool qualityTaaMenuOpen = false;
    double currentFps = 0.0;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    UINT64 nextFenceValue = 1;
    HANDLE fenceEvent = nullptr;

    ComPtr<IDXGISwapChain3> swapChain;
    UINT backBufferIndex = 0;
    ComPtr<ID3D12Resource> renderTargets[FRAME_COUNT];
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvDescriptorSize = 0;

    ComPtr<ID3D12Resource> depthBuffer;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;

    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    UINT srvUavDescriptorSize = 0;

    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12Resource> materialTexture;
    ComPtr<ID3D12Resource> accumulationBuffer;
    ComPtr<ID3D12Resource> materialAccumulationBuffer;
    ComPtr<ID3D12Resource> objectBuffer;
    ComPtr<ID3D12Resource> dlssOutputTexture;
    ComPtr<ID3D12Resource> dlssDepthTexture;
    ComPtr<ID3D12Resource> dlssMotionVectorTexture;
    void* objectBufferMapped = nullptr;
    D3D12_RESOURCE_STATES outputTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES materialTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES dlssOutputTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES dlssDepthTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES dlssMotionVectorTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> gridVertexBuffer;
    ComPtr<ID3D12Resource> gridIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW gridVertexView{};
    D3D12_INDEX_BUFFER_VIEW gridIndexView{};
    UINT gridIndexCount = 0;

    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12RootSignature> graphicsRootSignature;
    ComPtr<ID3D12PipelineState> computePipeline;
    ComPtr<ID3D12PipelineState> gridPipeline;
    ComPtr<ID3D12PipelineState> compositePipeline;

    std::uint32_t accumulatedSampleCount = 0;
    bool hasPreviousCamera = false;
    glm::vec3 previousCameraPos = glm::vec3(0.0f);
    glm::vec3 previousCameraTarget = glm::vec3(0.0f);
    float previousCameraFov = 0.0f;
    float currentJitterX = 0.0f;
    float currentJitterY = 0.0f;
    bool dlssActive = false;
    bool lastFrameRayTracing = false;

#ifdef BLACKHOLE_HAS_STREAMLINE
    struct StreamlineApi
    {
        HMODULE module = nullptr;

        using Init = PFun_slInit*;
        using Shutdown = PFun_slShutdown*;
        using SetD3DDevice = PFun_slSetD3DDevice*;
        using GetFeatureFunction = PFun_slGetFeatureFunction*;
        using SetConstants = PFun_slSetConstants*;
        using EvaluateFeature = PFun_slEvaluateFeature*;
        using GetNewFrameToken = PFun_slGetNewFrameToken*;
        using UpgradeInterface = PFun_slUpgradeInterface*;

        Init init = nullptr;
        Shutdown shutdown = nullptr;
        SetD3DDevice setD3DDevice = nullptr;
        GetFeatureFunction getFeatureFunction = nullptr;
        SetConstants setConstants = nullptr;
        EvaluateFeature evaluateFeature = nullptr;
        GetNewFrameToken getNewFrameToken = nullptr;
        UpgradeInterface upgradeInterface = nullptr;
    } streamline;

    // Manual Streamline hooking keeps the renderer on native D3D12 interfaces
    // and uses proxies only for the documented hook points (device queue
    // creation and swap-chain creation/presentation).
    ID3D12Device* streamlineDeviceProxy = nullptr;
    IDXGIFactory6* streamlineFactoryProxy = nullptr;

    sl::ViewportHandle dlssViewport{0};
    sl::FrameToken* dlssFrameToken = nullptr;
    PFun_slDLSSSetOptions* dlssSetOptions = nullptr;
    PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;
    sl::DLSSOptions dlssOptions{};
    std::filesystem::path streamlineRuntimeDirectory;
    std::filesystem::path streamlineLogDirectory;
    bool streamlineInitialized = false;
    std::uint32_t streamlineFrameIndex = 0;
#endif

    void createWindow();
    void createQualityPanel();
    void layoutQualityPanel(
        int displayRenderWidth = -1,
        int displayRenderHeight = -1);
    void resizePresentation(int clientWidth, int clientHeight);
    void updateQualityPanel();
    void toggleQualityPanel();
    void updateQualityFps(double fps);
    void paintQualityPanel(HDC dc);
    LRESULT handleQualityPanelMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    void handleQualityCommand(WPARAM wParam, LPARAM lParam);
    void handleQualityScroll(WPARAM wParam, LPARAM lParam);
    void resetQualityDefaults();
    void applyRenderScale(int percent);
    void applyRaySteps(int steps);
    void applyTemporalSamples(int selection);
    void applyDlssMode(int selection);
    void recreateRayResources(
        int renderWidth,
        int renderHeight,
        bool force = false);
    void resetAccumulation();
    void initializeD3D12();
    void createDeviceAndQueue();
    void createCommandObjects();
    void createSwapChain();
    void createRenderTargets();
    void createDepthBuffer();
    void createDescriptorHeap();
    void createRayResources();
    void createGridGeometry();
    void createPipelines();
    void clearInitialResources();
    void initializeStreamline();
    void connectStreamlineDevice();
    void shutdownStreamline();

    void recordRaytrace(double schwarzschildRadius, float aspect);
    bool recordDlss(bool cameraChanged);
    void recordGraphics(
        double schwarzschildRadius,
        float aspect,
        bool drawFallback,
        bool useDlss);
    void updateObjectBuffer();
    void waitForGpu();
    void executeFrame();
    void transitionTexture(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES& currentState,
        D3D12_RESOURCE_STATES desiredState);

    ComPtr<ID3DBlob> compileShader(
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target) const;
    std::string readTextFile(const std::filesystem::path& path) const;
    ComPtr<ID3D12Resource> createBuffer(
        UINT64 size,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState) const;
    ComPtr<ID3D12Resource> createTexture(
        int width,
        int height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState) const;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor(UINT index) const;
    void createRootSignatures();
    void createPipelineStates(
        const std::filesystem::path& shaderDirectory);

    static LRESULT CALLBACK windowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK renderWindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK qualityPanelProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    LRESULT handleMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
};

#endif
