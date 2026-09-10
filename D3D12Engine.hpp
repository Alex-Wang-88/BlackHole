#ifndef BLACK_HOLE_D3D12_ENGINE_HPP
#define BLACK_HOLE_D3D12_ENGINE_HPP

#include "Scene.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <cstdint>
#include <filesystem>
#include <string>

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

    bool shouldClose() const { return closing; }
    HWND window() const { return windowHandle; }

    int WIDTH = DEFAULT_WINDOW_WIDTH;
    int HEIGHT = DEFAULT_WINDOW_HEIGHT;
    int RENDER_WIDTH = DEFAULT_RENDER_WIDTH;
    int RENDER_HEIGHT = DEFAULT_RENDER_HEIGHT;

private:
    static constexpr UINT FRAME_COUNT = 2;
    static constexpr UINT DESCRIPTOR_COUNT = 7;
    static constexpr UINT COLOR_UAV_INDEX = 0;
    static constexpr UINT MATERIAL_UAV_INDEX = 1;
    static constexpr UINT ACCUMULATION_UAV_INDEX = 2;
    static constexpr UINT MATERIAL_ACCUMULATION_UAV_INDEX = 3;
    static constexpr UINT OBJECT_SRV_INDEX = 4;
    static constexpr UINT COLOR_SRV_INDEX = 5;
    static constexpr UINT MATERIAL_SRV_INDEX = 6;

    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    RenderSettings settings;
    std::filesystem::path shaderDirectory;
    HINSTANCE instance = nullptr;
    HWND windowHandle = nullptr;
    bool closing = false;
    bool allowTearing = false;

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
    void* objectBufferMapped = nullptr;
    D3D12_RESOURCE_STATES outputTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES materialTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

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

    void createWindow();
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

    void recordRaytrace(double schwarzschildRadius, float aspect);
    void recordGraphics(
        double schwarzschildRadius,
        float aspect,
        bool drawFallback);
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
    LRESULT handleMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
};

#endif
