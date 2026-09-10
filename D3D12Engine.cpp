#include "D3D12Engine.hpp"

#include <d3dcompiler.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

    ShowWindow(windowHandle, SW_SHOW);
    UpdateWindow(windowHandle);
}

void D3D12Engine::initializeD3D12()
{
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

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;
    throwIfFailed(
        device->CreateCommandQueue(
            &queueDescription,
            IID_PPV_ARGS(&commandQueue)),
        "ID3D12Device::CreateCommandQueue");

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
    throwIfFailed(
        factory->CreateSwapChainForHwnd(
            commandQueue.Get(),
            windowHandle,
            &description,
            nullptr,
            nullptr,
            &swapChain1),
        "CreateSwapChainForHwnd");
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
        DXGI_FORMAT_R8G8B8A8_UNORM,
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

    D3D12_RANGE readRange{0, 0};
    throwIfFailed(
        objectBuffer->Map(0, &readRange, &objectBufferMapped),
        "Map object buffer");

    D3D12_UNORDERED_ACCESS_VIEW_DESC textureUav{};
    textureUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    textureUav.Texture2D.MipSlice = 0;
    textureUav.Texture2D.PlaneSlice = 0;
    device->CreateUnorderedAccessView(
        outputTexture.Get(),
        nullptr,
        &textureUav,
        cpuDescriptor(COLOR_UAV_INDEX));
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
    textureSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureSrv.Texture2D.MostDetailedMip = 0;
    textureSrv.Texture2D.MipLevels = 1;
    textureSrv.Texture2D.PlaneSlice = 0;
    textureSrv.Texture2D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        outputTexture.Get(),
        &textureSrv,
        cpuDescriptor(COLOR_SRV_INDEX));
    device->CreateShaderResourceView(
        materialTexture.Get(),
        &textureSrv,
        cpuDescriptor(MATERIAL_SRV_INDEX));
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
    if(rayTracing && !sampleLimitReached)
        recordRaytrace(schwarzschildRadius, aspect);
    else
        transitionTexture(
            outputTexture.Get(),
            outputTextureState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTexture(
        materialTexture.Get(),
        materialTextureState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    recordGraphics(schwarzschildRadius, aspect, !rayTracing);
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

void D3D12Engine::recordGraphics(
    double schwarzschildRadius,
    float aspect,
    bool drawFallback)
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
        gpuDescriptor(COLOR_SRV_INDEX));
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
        if(wParam == VK_ESCAPE)
        {
            closing = true;
            DestroyWindow(windowHandle);
            return 0;
        }
        break;

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
