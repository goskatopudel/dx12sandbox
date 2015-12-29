#pragma once

#include "Essence.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcommon.h>

namespace Essence {

template<typename T>
struct OwningComPtr {
	T*		Ptr;

	T**		GetInitPtr() {
		return &Ptr;
	}

	T*		operator*() const {
		return Ptr;
	}

	T*		operator->() const {
		return Ptr;
	}

	OwningComPtr() : Ptr(nullptr) {}
	OwningComPtr(OwningComPtr<T> const& other) = delete;

	OwningComPtr(OwningComPtr<T> && other) {
		*this = other;
	}

	OwningComPtr<T>& operator = (OwningComPtr<T> && other) {
		Ptr = other.Ptr;
		other.Ptr = nullptr;

		return *this;
	}

	~OwningComPtr() {
		if (Ptr) {
			Ptr->Release();
			Ptr = nullptr;
		}
	}
};

const u32						MaxGpuBufferedFrames = 4;
const D3D_FEATURE_LEVEL			MinFeatureLevel = D3D_FEATURE_LEVEL_11_0;

typedef D3D12_CPU_DESCRIPTOR_HANDLE	CPU_DESC_HANDLE;
typedef D3D12_GPU_DESCRIPTOR_HANDLE GPU_DESC_HANDLE;
typedef D3D12_GPU_VIRTUAL_ADDRESS	GPU_VIRTUAL_ADDRESS;

extern ID3D12Device*			GD12Device;
extern i32						GD12RtvDescIncrement;
extern i32						GD12CbvSrvUavDescIncrement;
extern i32						GD12SamplerDescIncrement;
extern i32						GD12DsvDescIncrement;

void							InitDevice(HWND	hwnd, bool useWarpAdapter, bool enableDebugLayer);
void							ShutdownDevice();

void							CreateSwapChain(ID3D12CommandQueue* queue, u32 buffersNum);
void							Present(u32 vsync);
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