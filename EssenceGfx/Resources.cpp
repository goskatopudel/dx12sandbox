#include "Resources.h"
#include "Device.h"
#include "Hashmap.h"
#include "Freelist.h"
#include "Commands.h"
#include "Descriptors.h"

#include "d3dx12.h"

namespace Essence {

DescriptorAllocator				ShaderViewDescHeap;

enum ResourceCreationType {
	UNKNOWN_RESOURCE,
	COMMITED_RESOURCE
};

struct resource_t {
	ID3D12Resource*				resource;
	D3D12_RESOURCE_DESC			desc;
	ResourceCreationType		creation_type;
	ResourceHeapType			heap_type;
	TextId						debug_name;
};

struct resource_bind_t {
	descriptor_allocation_t		srv_locations;
	descriptor_allocation_t		rtv_location;
	descriptor_allocation_t		dsv_locations;
};

DXGI_FORMAT GetDepthStencilFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT GetDepthFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT GetStencilFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

CPU_DESC_HANDLE							G_NULL_TEXTURE2D_DESCRIPTOR;

DescriptorAllocator						RTVDescHeap;
DescriptorAllocator						DSVDescHeap;
DescriptorAllocator						ViewDescHeap;

Freelist<resource_t, resource_handle>	ResourcesTable;
Array<resource_fast_t>					ResourcesFastTable;
Array<resource_transition_t>			ResourcesTransitionTable;
Array<resource_bind_t>					ResourcesViews;

resource_fast_t* GetResourceFast(resource_handle handle) {
	Check(Contains(ResourcesTable, handle));
	return &ResourcesFastTable[handle.GetIndex()];
}

resource_transition_t*	GetResourceTransitionInfo(resource_handle handle) {
	Check(Contains(ResourcesTable, handle));
	return &ResourcesTransitionTable[handle.GetIndex()];
}

void	InitResources() {
	RTVDescHeap = std::move(DescriptorAllocator(32 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false));
	DSVDescHeap = std::move(DescriptorAllocator(32 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false));
	ViewDescHeap = std::move(DescriptorAllocator(32 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false));

	G_NULL_TEXTURE2D_DESCRIPTOR = GetCPUHandle(ViewDescHeap.Allocate(1));

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	GD12Device->CreateShaderResourceView(nullptr, &srvDesc, G_NULL_TEXTURE2D_DESCRIPTOR);
}

void	ShutdownResources() {
	for (auto& Record : ResourcesTable) {
		Record.resource->Release();
		Record = {};
	}

	FreeMemory(ResourcesTable);
	FreeMemory(ResourcesTransitionTable);
	FreeMemory(ResourcesFastTable);
	FreeMemory(ResourcesViews);

	call_destructor(ViewDescHeap);
	call_destructor(RTVDescHeap);
	call_destructor(DSVDescHeap);
}

template<typename T>
void	ResizeForIndex(Array<T>& A, u32 index) {
	if (Size(A) < index + 1) {
		Resize(A, index + 1);
	}
}

resource_handle CreateResourceEntry() {
	auto handle = Create(ResourcesTable);
	ResizeForIndex(ResourcesFastTable, handle.GetIndex());
	ResizeForIndex(ResourcesTransitionTable, handle.GetIndex());
	ResizeForIndex(ResourcesViews, handle.GetIndex());

	ResourcesTable[handle] = {};
	ResourcesFastTable[handle.index] = {};
	ResourcesTransitionTable[handle.index] = {};
	ResourcesViews[handle.index] = {};

	return handle;
}

void DeleteResourceEntry(resource_handle handle) {
	ResourcesTable[handle].resource->Release();

	DSVDescHeap.Free(ResourcesViews[handle.index].dsv_locations);
	ViewDescHeap.Free(ResourcesViews[handle.index].srv_locations);
	RTVDescHeap.Free(ResourcesViews[handle.index].rtv_location);

	ResourcesTable[handle] = {};
	ResourcesFastTable[handle.index] = {};
	ResourcesTransitionTable[handle.index] = {};
	ResourcesViews[handle.index] = {};

	Delete(ResourcesTable, handle);
	// todo: drop state tracking
}

void Delete(resource_handle resource) {

	DeleteResourceEntry(resource);
}

CPU_DESC_HANDLE ToCPUHandle(descriptor_allocation_t allocation, i32 offset = 0) {
	return allocation.allocator->GetCPUHandle(allocation, offset);
}

resource_rtv_t GetRTV(resource_slice_t resource) {
	if (resource.subresource == 0) {
		Check(Contains(ResourcesTable, resource.handle));
		auto location = ResourcesViews[resource.handle.GetIndex()].rtv_location;

		resource_rtv_t out;
		out.cpu_descriptor = ToCPUHandle(location);
		out.format = ResourcesTable[resource.handle].desc.Format;

		return out;
	}
	return {};
}

resource_rtv_t GetDSV(resource_slice_t resource, DsvAccessEnum access) {
	if (resource.subresource == 0) {
		Check(Contains(ResourcesTable, resource.handle));
		auto location = ResourcesViews[resource.handle.GetIndex()].dsv_locations;

		resource_rtv_t out;
		out.cpu_descriptor = ToCPUHandle(location, access);
		out.format = GetDepthStencilFormat(ResourcesTable[resource.handle].desc.Format);

		return out;
	}
	return{};
}

resource_handle CreateCommittedResource(D3D12_RESOURCE_DESC const* desc, ResourceHeapType heapType, const char *debugName, D3D12_CLEAR_VALUE const* clearValue, D3D12_RESOURCE_STATES initialState) {

	ID3D12Resource *resource;

	D3D12_HEAP_PROPERTIES heapProperties = {};

	switch (heapType) {
	case DEFAULT_MEMORY:
		heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		break;
	case UPLOAD_MEMORY:
		Check(initialState == D3D12_RESOURCE_STATE_COMMON || initialState == D3D12_RESOURCE_STATE_GENERIC_READ);
		heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		break;
	case READBACK_MEMORY:
		Check(initialState == D3D12_RESOURCE_STATE_COMMON || initialState == D3D12_RESOURCE_STATE_COPY_DEST);
		heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
		heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		initialState = D3D12_RESOURCE_STATE_COPY_DEST;
		break;
	}

	VerifyHr(GD12Device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		desc,
		initialState,
		clearValue,
		IID_PPV_ARGS(&resource)
		));

	if (!resource) {
		return{};
	}

	if (debugName) {
		SetDebugName(resource, debugName);
	} 

	auto handle = CreateResourceEntry();
	auto &Record = ResourcesTable[handle];
	Record.resource = resource;
	Record.debug_name = TEXT_(debugName);
	Record.desc = *desc;
	Record.heap_type = heapType;
	Record.creation_type = COMMITED_RESOURCE;

	ResourcesFastTable[handle.GetIndex()].resource = resource;

	ResourcesTransitionTable[handle.GetIndex()].default_state = D3D12_RESOURCE_STATE_COMMON;
	ResourcesTransitionTable[handle.GetIndex()].heap_type = heapType;

	RegisterResource(handle, initialState);

	return handle;
}

resource_handle	CreateTexture(u32 width, u32 height, DXGI_FORMAT format, TextureFlags textureFlags, const char* debugName, float4 clearColor, float clearDepth, u8 clearStencil) {

	Check(!((textureFlags & ALLOW_RENDER_TARGET) && (textureFlags & ALLOW_DEPTH_STENCIL)));

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = 
		(textureFlags & ALLOW_RENDER_TARGET) ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE
		| (textureFlags & ALLOW_DEPTH_STENCIL) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE
	;

	D3D12_CLEAR_VALUE clearValue;
	clearValue.Format = format;
	bool needsClearValue = false;
	if (textureFlags & ALLOW_DEPTH_STENCIL) {
		needsClearValue = true;
		clearValue.Format = GetDepthStencilFormat(format);
		clearValue.DepthStencil.Depth = clearDepth;
		clearValue.DepthStencil.Stencil = clearStencil;
	}
	else if (textureFlags & ALLOW_RENDER_TARGET) {
		needsClearValue = true;
		clearValue.Color[0] = clearColor.x;
		clearValue.Color[1] = clearColor.y;
		clearValue.Color[2] = clearColor.z;
		clearValue.Color[3] = clearColor.w;
	}

	auto handle = CreateCommittedResource(&desc, DEFAULT_MEMORY, debugName, needsClearValue ? &clearValue : nullptr, D3D12_RESOURCE_STATE_COMMON);

	if (!(textureFlags & ALLOW_DEPTH_STENCIL)) {
		ResourcesViews[handle.GetIndex()].srv_locations = ViewDescHeap.Allocate(1);

		GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations));

		ResourcesFastTable[handle.GetIndex()].srv = ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations);
	}
	else {
		enum DepthTextureReadViewsEnum {
			VIEW_DEPTH = 0,
			VIEW_STENCIL = 1
		};

		ResourcesViews[handle.GetIndex()].srv_locations = ViewDescHeap.Allocate(2);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Format = GetDepthFormat(format);
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0;
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, VIEW_DEPTH));

		srvDesc.Format = GetStencilFormat(format);

		GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, VIEW_STENCIL));

		ResourcesFastTable[handle.GetIndex()].srv = ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations);
	}

	if (textureFlags & ALLOW_RENDER_TARGET) {
		ResourcesViews[handle.GetIndex()].rtv_location = RTVDescHeap.Allocate(1);

		GD12Device->CreateRenderTargetView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].rtv_location));
	}
	if (textureFlags & ALLOW_DEPTH_STENCIL) {
		ResourcesViews[handle.GetIndex()].dsv_locations = DSVDescHeap.Allocate(DSV_ACCESS_COUNT);

		Check(format == DXGI_FORMAT_R24G8_TYPELESS);
		
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvView = {};
		dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvView.Texture2D.MipSlice = 0;

		dsvView.Flags = D3D12_DSV_FLAG_NONE;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_WRITE_ALL));

		dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY_DEPTH));

		dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY_STENCIL));

		dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY));
	}

	return handle;
}

const u32			MAX_SWAP_BUFFERS = 8;
resource_handle		GSwapChain[MAX_SWAP_BUFFERS];

void	DeregisterSwapChainBuffers() {
	for (auto i : MakeRange(MAX_SWAP_BUFFERS)) {
		if (IsValid(GSwapChain[i])) {
			Delete(GSwapChain[i]);
		}
	}
}

void	RegisterSwapChainBuffer(ID3D12Resource* resource, u32 index) {
	auto debugName = "swapchain";

	Check(index < MAX_SWAP_BUFFERS);

	SetDebugName(resource, debugName);

	auto handle = CreateResourceEntry();

	auto &Record = ResourcesTable[handle];
	Record = {};
	Record.resource = resource;
	resource->AddRef();
	Record.debug_name = TEXT_(debugName);
	Record.desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	ResourcesFastTable[handle.GetIndex()].resource = resource;

	ResourcesTransitionTable[handle.GetIndex()] = {};
	ResourcesTransitionTable[handle.GetIndex()].default_state = D3D12_RESOURCE_STATE_COMMON;

	ResourcesViews[handle.GetIndex()].rtv_location = RTVDescHeap.Allocate(1);
	GD12Device->CreateRenderTargetView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].rtv_location));

	GSwapChain[index] = handle;
}

resource_handle			GetCurrentBackbuffer() {
	extern u32 CurrentSwapBufferIndex;
	return GSwapChain[CurrentSwapBufferIndex];
}

void CopyFromCpuToSubresources(GPUCommandList* list, resource_slice_t dstResource, u32 subresourcesNum, D3D12_SUBRESOURCE_DATA const* subresourcesData);

void CopyFromCpuToSubresources(GPUQueue* queue, resource_slice_t dstResource, u32 subresourcesNum, D3D12_SUBRESOURCE_DATA const* subresourcesData) {
	auto copyCommands = GetCommandList(queue, NAME_("Copy"));
	CopyFromCpuToSubresources(copyCommands, dstResource, subresourcesNum, subresourcesData);
	Execute(copyCommands);
}

void CopyFromCpuToSubresources(GPUCommandList* list, resource_slice_t dstResource, u32 subresourcesNum, D3D12_SUBRESOURCE_DATA const* subresourcesData) {
	auto d12list = GetD12CommandList(list);

	UINT64 uploadBufferSize;
	Array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
	Array<u32> numRows;
	Array<uint64_t> rowSizes;

	Resize(layouts, subresourcesNum);
	Resize(numRows, subresourcesNum);
	Resize(rowSizes, subresourcesNum);

	auto desc = ResourcesTable[dstResource.handle].desc;
	GD12Device->GetCopyableFootprints(&desc, 0, subresourcesNum, 0, layouts.DataPtr, numRows.DataPtr, rowSizes.DataPtr, &uploadBufferSize);

	auto tmpUploadHeap = CreateBuffer(UPLOAD_MEMORY, uploadBufferSize, 0, BUFFER_NO_FLAGS, "upload heap");

	auto srcResource = GetResourceFast(tmpUploadHeap)->resource;
	byte* dataPtr;
	VerifyHr(srcResource->Map(0, nullptr, (void**)&dataPtr));
	for (auto i = 0u; i < subresourcesNum; ++i) {
		D3D12_MEMCPY_DEST dest = { dataPtr + layouts[i].Offset, layouts[i].Footprint.RowPitch, layouts[i].Footprint.RowPitch * numRows[i] };
		MemcpySubresource(&dest, subresourcesData + i, (SIZE_T)rowSizes[i], numRows[i], layouts[i].Footprint.Depth);
	}

	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		CopyBufferRegion(list, dstResource.handle, 0, tmpUploadHeap, layouts[0].Offset, layouts[0].Footprint.Width);
	}
	else {
		TransitionBarrier(list, dstResource, D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers(list);

		for (auto i : MakeRange(subresourcesNum)) {
			D3D12_TEXTURE_COPY_LOCATION srcLocation;
			srcLocation.PlacedFootprint = layouts[i];
			srcLocation.pResource = srcResource;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

			D3D12_TEXTURE_COPY_LOCATION dstLocation;
			dstLocation.pResource = GetResourceFast(dstResource.handle)->resource;
			dstLocation.SubresourceIndex = i;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

			GetD12CommandList(list)->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}

	srcResource->Unmap(0, nullptr);

	TransitionBarrier(list, dstResource, GetResourceTransitionInfo(dstResource.handle)->default_state);

	// todo:
	//DeferredDestroyResource();
}

void CopyToBuffer(GPUCommandList* list, resource_handle dstBuffer, const void* dataPtr, u64 size) {
	D3D12_SUBRESOURCE_DATA copyInfo = {};
	copyInfo.pData = dataPtr;
	copyInfo.RowPitch = size;
	copyInfo.SlicePitch = size;

	CopyFromCpuToSubresources(list, Slice(dstBuffer), 1, &copyInfo);
}

resource_handle CreateBuffer(ResourceHeapType heapType, u64 size, u64 stride, BufferFlags flags, const char* debugName) {
	
	D3D12_RESOURCE_DESC desc = (D3D12_RESOURCE_DESC)CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_NONE);

	auto initialState = D3D12_RESOURCE_STATE_COMMON;
	if (flags & ALLOW_VERTEX_BUFFER) {
		initialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	}
	if (flags & ALLOW_INDEX_BUFFER) {
		initialState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	}

	auto handle = CreateCommittedResource(&desc, heapType, debugName, nullptr, initialState);

	return handle;
}

}