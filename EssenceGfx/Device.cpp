#include "Device.h"
#include "Resources.h"
#include "Application.h"

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3D12.lib")

namespace Essence {

IDXGIFactory4*		GDXGIFactory;
IDXGIAdapter3*		GDXGIAdapter;
ID3D12Debug*		GDebugLayer;
ID3D12Device*		GD12Device;
IDXGISwapChain3*	GSwapChain;

const u32			MaxSwapBuffersNum = 8;
u32					SwapBuffersNum;
u32					CurrentSwapBufferIndex;
ID3D12Resource*		SwapChainBuffer[MaxSwapBuffersNum];
HWND				GHWND;
HANDLE				VBlankWaitable;

const DXGI_FORMAT	BackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;


D3D12_FEATURE_DATA_D3D12_OPTIONS				GD12Options;
D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT	GD12VirtualAddressSupport;

i32					GD12RtvDescIncrement;
i32					GD12CbvSrvUavDescIncrement;
i32					GD12SamplerDescIncrement;
i32					GD12DsvDescIncrement;

void SetStablePower(bool enable) {
	VerifyHr(GD12Device->SetStablePowerState(enable));
}

void SetDebugName(ID3D12DeviceChild * child, const char * name) {
	if (child != nullptr && name != nullptr) {
		VerifyHr(child->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name));
	}
}

void PrintAdaptersList();
void PrintDeviceInfo(ID3D12Device* device);
void PrintAdapterInfo(IDXGIAdapter3* adapter);

HANDLE CreateEvent() {
	return CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
}

void DestroyEvent(HANDLE h) {
	CloseHandle(h);
}

void InitDevice(
	HWND	hwnd,
	bool	useWarpAdapter,
	bool	enableDebugLayer,
	i32		adapterIndex)
{
	IDXGIFactory4* DXGIFactory;
	VerifyHr(CreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory)));

	if (enableDebugLayer) {
		VerifyHr(D3D12GetDebugInterface(IID_PPV_ARGS(&GDebugLayer)));
		GDebugLayer->EnableDebugLayer();
	}

	if (useWarpAdapter) {
		IDXGIAdapter *warpAdapter = nullptr;
		VerifyHr(DXGIFactory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		VerifyHr(D3D12CreateDevice(
			warpAdapter,
			MinFeatureLevel,
			IID_PPV_ARGS(&GD12Device)
			));
	}
	else {
		IDXGIAdapter1* adapter = nullptr;
		if (adapterIndex != -1) {
			DXGIFactory->EnumAdapters1(adapterIndex, &adapter);
		}

		VerifyHr(D3D12CreateDevice(
			adapter,
			MinFeatureLevel,
			IID_PPV_ARGS(&GD12Device)
			));
	}

	GD12RtvDescIncrement = GD12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	GD12CbvSrvUavDescIncrement = GD12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	GD12SamplerDescIncrement = GD12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	GD12DsvDescIncrement = GD12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	GHWND = hwnd;

	VerifyHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&GDXGIFactory)));
	VerifyHr(DXGIFactory->EnumAdapterByLuid(GD12Device->GetAdapterLuid(), IID_PPV_ARGS(&GDXGIAdapter)));

	VerifyHr(GD12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &GD12Options, sizeof(GD12Options)));
	VerifyHr(GD12Device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &GD12VirtualAddressSupport, sizeof(GD12VirtualAddressSupport)));

	debugf("Adapters\n--------\n");
	PrintAdaptersList();

	debugf("Current device\n--------\n");
	PrintAdapterInfo(GDXGIAdapter);
	PrintDeviceInfo(GD12Device);
	debugf("\n");
}

void PrintAdaptersList() {
	auto adapterIndex = 0u;
	IDXGIAdapter1* adapterPtr = nullptr;
	VerifyHr(GDXGIFactory->EnumAdapters1(adapterIndex, &adapterPtr));

	while (adapterPtr) {
		DXGI_ADAPTER_DESC1 desc;
		adapterPtr->GetDesc1(&desc);

		debugf(Format(
			"LUID: %d%d, Description: %s\n",
			desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
			(const char*)ScratchString(desc.Description)
			)
			);

		adapterIndex++;
		GDXGIFactory->EnumAdapters1(adapterIndex, &adapterPtr);
	}
	debugf("\n");
}

void PrintDeviceInfo(ID3D12Device* device) {

	D3D12_FEATURE_DATA_D3D12_OPTIONS options;
	VerifyHr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
	D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT addressSupport;
	VerifyHr(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &addressSupport, sizeof(addressSupport)));

	debugf(Format(
		"Node count: %u\n"
		"Node sharing tier: %u\n"
		"Resource binding tier: %d\n"
		"Resource heap tier: %d\n"
		"Tiled resources tier: %d\n"
		"Conservative rasterization tier: %d\n"
		"Virtual address bits: %d\n"
		"ROVs support: %d\n"
		"Standard swizzle 64kb support: %d\n"
		"Typed UAV load additional formats: %d\n"
		"VP and RT array index with no GS: %d\n",
		device->GetNodeCount(),
		options.CrossNodeSharingTier,
		options.ResourceBindingTier,
		options.ResourceHeapTier,
		options.TiledResourcesTier,
		options.ConservativeRasterizationTier,
		options.MaxGPUVirtualAddressBitsPerResource,
		options.ROVsSupported,
		options.StandardSwizzle64KBSupported,
		options.TypedUAVLoadAdditionalFormats,
		options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation
		));
}

void PrintAdapterInfo(IDXGIAdapter3* adapter) {
	DXGI_ADAPTER_DESC2 desc;
	adapter->GetDesc2(&desc);

	debugf(Format(
		"LUID: %d%d\n"
		"Description: %s\n"
		"Device id: %u\n"
		"System memory: %llu Mb\n"
		"Video memory: %llu Mb\n"
		"Shared memory: %llu Mb\n",
		desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
		(const char*)ScratchString(desc.Description),
		desc.DeviceId,
		Megabytes(desc.DedicatedSystemMemory),
		Megabytes(desc.DedicatedVideoMemory),
		Megabytes(desc.SharedSystemMemory)));
}

void CreateSwapChain(ID3D12CommandQueue* queue) {
	for (auto i : i32Range(MaxSwapBuffersNum)) {
		if (SwapChainBuffer[i]) {
			SwapChainBuffer[i]->Release();
			SwapChainBuffer[i] = nullptr;
		}
	}

	ComRelease(GSwapChain);

	SwapBuffersNum = GDisplaySettings.backbuffers_num;
	Check(SwapBuffersNum <= MaxSwapBuffersNum);
	CurrentSwapBufferIndex = 0;

	DXGI_SWAP_CHAIN_DESC descSwapChain;
	ZeroMemory(&descSwapChain, sizeof(descSwapChain));
	descSwapChain.BufferCount = SwapBuffersNum;
	descSwapChain.BufferDesc.Format = BackbufferFormat;
	descSwapChain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	descSwapChain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	descSwapChain.OutputWindow = GHWND;
	descSwapChain.SampleDesc.Count = 1;
	descSwapChain.Windowed = TRUE;
	descSwapChain.Flags = 
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH 
		// using waitable flag and ingoring it causes trouble
		| (GDisplaySettings.wait_to_vblank ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0) ;

	IDXGISwapChain* swapChain;
	VerifyHr(GDXGIFactory->CreateSwapChain(
		queue,
		&descSwapChain,
		&swapChain));

	VerifyHr(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&GSwapChain));
	swapChain->Release();

	for (auto i : i32Range(SwapBuffersNum)) {
		ID3D12Resource* resource;
		VerifyHr(GSwapChain->GetBuffer(i, IID_PPV_ARGS(&resource)));
		SwapChainBuffer[i] = resource;

		RegisterSwapChainBuffer(resource, i);
	}

	if (GDisplaySettings.wait_to_vblank) {
		VBlankWaitable = GSwapChain->GetFrameLatencyWaitableObject();
		VerifyHr(GSwapChain->SetMaximumFrameLatency(GDisplaySettings.max_gpu_buffered_frames));
	}
}

void Present() {
	{
		PROFILE_SCOPE(wait_for_present);
		VerifyHr(GSwapChain->Present(GDisplaySettings.vsync, 0));
	}

	if (GDisplaySettings.wait_to_vblank) {
		PROFILE_SCOPE(wait_for_vblank);
		auto result = WaitForSingleObject(VBlankWaitable, INFINITE);
		Check(result == WAIT_OBJECT_0);
	}

	CurrentSwapBufferIndex = (CurrentSwapBufferIndex + 1) % SwapBuffersNum;
}

void ResizeSwapChain(u32 width, u32 height) {
	DeregisterSwapChainBuffers();

	for (auto i : i32Range(MaxSwapBuffersNum)) {
		if (SwapChainBuffer[i]) {
			SwapChainBuffer[i]->Release();
			SwapChainBuffer[i] = nullptr;
		}
	}

	Check(GSwapChain);

	VerifyHr(GSwapChain->ResizeBuffers(SwapBuffersNum, width, height, BackbufferFormat, 
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH 
		| (GDisplaySettings.wait_to_vblank ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0)));

	for (auto i : i32Range(SwapBuffersNum)) {
		ID3D12Resource* resource;
		VerifyHr(GSwapChain->GetBuffer(i, IID_PPV_ARGS(&resource)));
		SwapChainBuffer[i] = resource;

		RegisterSwapChainBuffer(resource, i);
	}

	CurrentSwapBufferIndex = 0;
}

DXGI_QUERY_VIDEO_MEMORY_INFO GetLocalMemoryInfo() {
	DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
	VerifyHr(GDXGIAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo));
	return memInfo;
}

DXGI_QUERY_VIDEO_MEMORY_INFO GetNonLocalMemoryInfo() {
	DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
	VerifyHr(GDXGIAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memInfo));
	return memInfo;
}

void ShutdownDevice() {
	if (VBlankWaitable) {
		Verify(CloseHandle(VBlankWaitable));
	}

	for (auto i : i32Range(MaxSwapBuffersNum)) {
		if (SwapChainBuffer[i]) {
			SwapChainBuffer[i]->Release();
			SwapChainBuffer[i] = nullptr;
		}
	}
	ComRelease(GSwapChain);
	ComRelease(GD12Device);
	ComRelease(GDebugLayer);
	ComRelease(GDXGIFactory);
	ComRelease(GDXGIAdapter);
}

}