#pragma once

#include "Essence.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcommon.h>

namespace Essence {

const D3D_FEATURE_LEVEL			MinFeatureLevel = D3D_FEATURE_LEVEL_11_0;

typedef D3D12_CPU_DESCRIPTOR_HANDLE	CPU_DESC_HANDLE;
typedef D3D12_GPU_DESCRIPTOR_HANDLE GPU_DESC_HANDLE;
typedef D3D12_GPU_VIRTUAL_ADDRESS	GPU_VIRTUAL_ADDRESS;

extern ID3D12Device*			GD12Device;
extern i32						GD12RtvDescIncrement;
extern i32						GD12CbvSrvUavDescIncrement;
extern i32						GD12SamplerDescIncrement;
extern i32						GD12DsvDescIncrement;

void							InitDevice(HWND	hwnd, bool useWarpAdapter, bool enableDebugLayer, i32 adapterIndex);
void							ShutdownDevice();

void							CreateSwapChain(ID3D12CommandQueue* queue);
void							ResizeSwapChain(u32 width, u32 height);
void							Present();

DXGI_QUERY_VIDEO_MEMORY_INFO	GetLocalMemoryInfo();
DXGI_QUERY_VIDEO_MEMORY_INFO	GetNonLocalMemoryInfo();
void							SetStablePower(bool enable);
HANDLE							CreateEvent();
void							DestroyEvent(HANDLE h);
void							SetDebugName(ID3D12DeviceChild* child, const char* name);


inline D3D12_CPU_DESCRIPTOR_HANDLE offseted_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle, INT offsetInDescriptors, UINT descriptorIncrementSize) {
	handle.ptr += offsetInDescriptors * descriptorIncrementSize;
	return handle;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE offseted_handle(D3D12_GPU_DESCRIPTOR_HANDLE handle, INT offsetInDescriptors, UINT descriptorIncrementSize) {
	handle.ptr += offsetInDescriptors * descriptorIncrementSize;
	return handle;
}

}