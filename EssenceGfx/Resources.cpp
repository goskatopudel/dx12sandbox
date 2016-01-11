#include "Resources.h"
#include "Device.h"
#include "Hashmap.h"
#include "Freelist.h"
#include "Commands.h"
#include "Descriptors.h"

#include "d3dx12.h"
#include <SDL.h>
#include <SDL_syswm.h>

namespace Essence {

DescriptorAllocator				ShaderViewDescHeap;

struct resource_bind_t {
	descriptor_allocation_t		srv_locations;
	descriptor_allocation_t		rtv_locations;
	descriptor_allocation_t		dsv_locations;
	descriptor_allocation_t		uav_locations;
	u32							has_stencil_views : 1;
};

DXGI_FORMAT GetDepthStencilFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_D32_FLOAT;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT GetDepthReadFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_FLOAT;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT GetStencilReadFormat(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_UNKNOWN;
	default:
		Check(0);
		return DXGI_FORMAT_UNKNOWN;
	}
}

CPU_DESC_HANDLE							G_NULL_TEXTURE2D_SRV_DESCRIPTOR;
CPU_DESC_HANDLE							G_NULL_TEXTURE2D_UAV_DESCRIPTOR;

DescriptorAllocator						RTVDescHeap;
DescriptorAllocator						DSVDescHeap;
DescriptorAllocator						ViewDescHeap;

Freelist<resource_t, resource_handle>	ResourcesTable;
Array<resource_fast_t>					ResourcesFastTable;
Array<resource_transition_t>			ResourcesTransitionTable;
Array<resource_bind_t>					ResourcesViews;

resource_t*	GetResourceInfo(resource_handle handle) {
	return &ResourcesTable[handle];
}

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

	G_NULL_TEXTURE2D_SRV_DESCRIPTOR = GetCPUHandle(ViewDescHeap.Allocate(1));

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	GD12Device->CreateShaderResourceView(nullptr, &srvDesc, G_NULL_TEXTURE2D_SRV_DESCRIPTOR);

	G_NULL_TEXTURE2D_UAV_DESCRIPTOR = GetCPUHandle(ViewDescHeap.Allocate(1));

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
	ZeroMemory(&uavDesc, sizeof(uavDesc));
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;
	GD12Device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, G_NULL_TEXTURE2D_UAV_DESCRIPTOR);
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
	RTVDescHeap.Free(ResourcesViews[handle.index].rtv_locations);

	ResourcesTable[handle] = {};
	ResourcesFastTable[handle.index] = {};
	ResourcesTransitionTable[handle.index] = {};
	//ResourcesViews[handle.index].subresource_views.elements
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

u32 CalcSubresource(resource_handle resource, u32 mipmap) {
	auto subresource = D3D12CalcSubresource(mipmap, 0, 0, GetResourceInfo(resource)->miplevels, GetResourceInfo(resource)->depth_or_array_size);
	Check(subresource < GetResourceInfo(resource)->subresources_num);
	return subresource;
}

resource_rtv_t			GetRTV(resource_handle resource) {
	resource_rtv_t rtv = {};

	if (IsValid(resource)) {
		rtv.slice = Slice(resource);
		rtv.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].rtv_locations);
		rtv.format = ResourcesTable[resource].desc.Format;
	}

	return rtv;
}

resource_rtv_t			GetRTV(resource_handle resource, u32 mipmap);

resource_dsv_t			GetDSV(resource_handle resource) {
	resource_dsv_t dsv = {};

	if (IsValid(resource)) {
		dsv.slice = Slice(resource);
		dsv.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].dsv_locations);
		dsv.format = GetDepthStencilFormat(ResourcesTable[resource].desc.Format);
		dsv.has_stencil = ResourcesViews[resource.GetIndex()].dsv_locations.size == DSV_ACCESS_COUNT;
	}

	return dsv;
}

resource_dsv_t			GetDSV(resource_handle resource, u32 mipmap) {
	resource_dsv_t dsv = {};

	if (IsValid(resource)) {
		dsv.slice = Slice(resource);
		dsv.has_stencil = ResourcesViews[resource.GetIndex()].dsv_locations.size == DSV_ACCESS_COUNT;
		dsv.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].dsv_locations, CalcSubresource(resource, mipmap) * (dsv.has_stencil ? DSV_ACCESS_COUNT : DSV_NO_STENCIL_ACCESS_COUNT));
		dsv.format = GetDepthStencilFormat(ResourcesTable[resource].desc.Format);
	}

	return dsv;
}

resource_uav_t			GetUAV(resource_handle resource) {
	resource_uav_t uav = {};

	if (IsValid(resource)) {
		uav.slice = Slice(resource);
		uav.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].uav_locations);
	}

	return uav;
}

resource_uav_t			GetUAV(resource_handle resource, u32 mipmap) {
	resource_uav_t uav = {};

	if (IsValid(resource)) {
		uav.slice = Slice(resource, CalcSubresource(resource, mipmap) + 1);
		uav.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].uav_locations, CalcSubresource(resource, mipmap));
	}

	return uav;
}

resource_srv_t			GetSRV(resource_handle resource) {
	resource_srv_t srv = {};

	if (IsValid(resource)) {
		srv.slice = Slice(resource);
		srv.cpu_descriptor = GetResourceFast(resource)->srv;
		srv.fixed_state = GetResourceFast(resource)->is_read_only;

		if (!srv.fixed_state) {
			srv.is_depth = ResourcesViews[resource.GetIndex()].dsv_locations.size;
		}
	}

	return srv;
}

resource_srv_t			GetSRV(resource_handle resource, u32 mipmap) {
	resource_srv_t srv = {};

	if (IsValid(resource)) {
		srv.slice = Slice(resource, CalcSubresource(resource, mipmap) + 1);
		srv.fixed_state = GetResourceFast(resource)->is_read_only;

		bool has_depth_views = ResourcesViews[resource.GetIndex()].dsv_locations.size > 0;
		bool has_stencil_views = ResourcesViews[resource.GetIndex()].has_stencil_views;
		if (!srv.fixed_state) {
			srv.is_depth = ResourcesViews[resource.GetIndex()].dsv_locations.size;
		}

		srv.cpu_descriptor = ToCPUHandle(ResourcesViews[resource.GetIndex()].srv_locations, 
			(1 + CalcSubresource(resource, mipmap)) * (has_stencil_views ? 2 : 1));
	}

	return srv;
}

resource_handle CreateReservedResource(D3D12_RESOURCE_DESC const* desc, const char *debugName, D3D12_CLEAR_VALUE const* clearValue, D3D12_RESOURCE_STATES initialState) {
	ID3D12Resource *resource;

	VerifyHr(GD12Device->CreateReservedResource(desc, initialState, clearValue, IID_PPV_ARGS(&resource)));

	if (!resource) {
		return{};
	}

	if (debugName) {
		SetDebugName(resource, debugName);
	}

	auto fDesc = resource->GetDesc();

	auto handle = CreateResourceEntry();
	auto &Record = ResourcesTable[handle];
	Record.resource = resource;
	Record.subresources_num = fDesc.MipLevels * fDesc.DepthOrArraySize;
	Record.depth_or_array_size = fDesc.DepthOrArraySize;
	Record.width = fDesc.Width;
	Record.height = fDesc.Height;
	Record.miplevels = fDesc.MipLevels;
	Record.debug_name = TEXT_(debugName);
	Record.desc = *desc;
	Record.heap_type = UNKNOWN_MEMORY;
	Record.creation_type = RESERVED_RESOURCE;

	ResourcesFastTable[handle.GetIndex()].resource = resource;

	ResourcesTransitionTable[handle.GetIndex()].default_state = D3D12_RESOURCE_STATE_COMMON;
	ResourcesTransitionTable[handle.GetIndex()].heap_type = UNKNOWN_MEMORY;

	RegisterResource(handle, initialState);

	return handle;
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
	
	auto fDesc = resource->GetDesc();

	auto handle = CreateResourceEntry();
	auto &Record = ResourcesTable[handle];
	Record.resource = resource;
	Record.subresources_num = fDesc.MipLevels * fDesc.DepthOrArraySize;
	Record.depth_or_array_size = fDesc.DepthOrArraySize;
	Record.width = fDesc.Width;
	Record.height = fDesc.Height;
	Record.miplevels = fDesc.MipLevels;
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
	desc.MipLevels = (textureFlags & TEX_MIPMAPPED) ? 0 : 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = (textureFlags & TEX_VIRTUAL) ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags =
		(textureFlags & ALLOW_RENDER_TARGET) ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE
		| (textureFlags & ALLOW_DEPTH_STENCIL) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE
		| (textureFlags & ALLOW_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE
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

	resource_handle handle;

	if (textureFlags & TEX_VIRTUAL) {
		handle = CreateReservedResource(&desc, debugName, needsClearValue ? &clearValue : nullptr, D3D12_RESOURCE_STATE_COMMON);
	}
	else {
		handle = CreateCommittedResource(&desc, DEFAULT_MEMORY, debugName, needsClearValue ? &clearValue : nullptr, D3D12_RESOURCE_STATE_COMMON);
	}

	bool readOnly = (textureFlags & (ALLOW_DEPTH_STENCIL | ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS)) == 0;
	u32 subresourcesNum = GetResourceInfo(handle)->subresources_num;

	ResourcesFastTable[handle.GetIndex()].is_read_only = readOnly;

	if (!(textureFlags & ALLOW_DEPTH_STENCIL)) {
		if (!(textureFlags & TEX_MIPMAPPED)) {
			ResourcesViews[handle.GetIndex()].srv_locations = ViewDescHeap.Allocate(1);
		}
		else {
			ResourcesViews[handle.GetIndex()].srv_locations = ViewDescHeap.Allocate(1 + subresourcesNum);

			for (auto subIndex : MakeRange(subresourcesNum)) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Format = format;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.MostDetailedMip = subIndex;
				srvDesc.Texture2D.ResourceMinLODClamp = 0;
				srvDesc.Texture2D.PlaneSlice = 0;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

				GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, subIndex + 1));
			}
		}

		GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations));

		ResourcesFastTable[handle.GetIndex()].srv = ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations);
	}
	else {
		enum DepthTextureReadViewsEnum {
			VIEW_DEPTH = 0,
			VIEW_STENCIL = 1
		};

		bool hasStencil = GetStencilReadFormat(format) != DXGI_FORMAT_UNKNOWN;
		bool mipmapped = (textureFlags & TEX_MIPMAPPED) > 0;

		u32 viewsPerSubresource = hasStencil ? 2 : 1;

		ResourcesViews[handle.GetIndex()].srv_locations = ViewDescHeap.Allocate(viewsPerSubresource * (subresourcesNum + 1));
		ResourcesViews[handle.GetIndex()].has_stencil_views = hasStencil;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Format = GetDepthReadFormat(format);
		srvDesc.Texture2D.MipLevels = subresourcesNum;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0;
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, VIEW_DEPTH));

		if (hasStencil) {
			srvDesc.Format = GetStencilReadFormat(format);
			GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, VIEW_STENCIL));
		}

		for (auto subIndex : MakeRange(subresourcesNum)) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Format = GetDepthReadFormat(format);
			srvDesc.Texture2D.MipLevels = 1;
			srvDesc.Texture2D.MostDetailedMip = subIndex;
			srvDesc.Texture2D.ResourceMinLODClamp = 0;
			srvDesc.Texture2D.PlaneSlice = 0;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, viewsPerSubresource * (subIndex + 1) + VIEW_DEPTH));

			if (hasStencil) {
				srvDesc.Format = GetStencilReadFormat(format);

				GD12Device->CreateShaderResourceView(ResourcesTable[handle].resource, &srvDesc, ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations, viewsPerSubresource * (subIndex + 1) + VIEW_STENCIL));
			}
		}

		ResourcesFastTable[handle.GetIndex()].srv = ToCPUHandle(ResourcesViews[handle.GetIndex()].srv_locations);
	}

	if (textureFlags & ALLOW_RENDER_TARGET) {
		if (!(textureFlags & TEX_MIPMAPPED)) {
			ResourcesViews[handle.GetIndex()].rtv_locations = RTVDescHeap.Allocate(1);
		}
		else {
			ResourcesViews[handle.GetIndex()].rtv_locations = RTVDescHeap.Allocate(subresourcesNum);

			for (auto subIndex : MakeRange(subresourcesNum)) {
				D3D12_RENDER_TARGET_VIEW_DESC rtvView = {};
				rtvView.Format = format;
				rtvView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				rtvView.Texture2D.MipSlice = subIndex;
				rtvView.Texture2D.PlaneSlice = 0;

				GD12Device->CreateRenderTargetView(ResourcesTable[handle].resource, &rtvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].rtv_locations, subIndex));
			}
		}
		GD12Device->CreateRenderTargetView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].rtv_locations));
	}
	if (textureFlags & ALLOW_UNORDERED_ACCESS) {
		if (!(textureFlags & TEX_MIPMAPPED)) {
			ResourcesViews[handle.GetIndex()].uav_locations = ViewDescHeap.Allocate(1);
		}
		else {
			ResourcesViews[handle.GetIndex()].uav_locations = ViewDescHeap.Allocate(subresourcesNum);

			for (auto subIndex : MakeRange(subresourcesNum)) {
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavView = {};
				uavView.Format = format;
				uavView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uavView.Texture2D.MipSlice = subIndex;
				uavView.Texture2D.PlaneSlice = 0;

				GD12Device->CreateUnorderedAccessView(ResourcesTable[handle].resource, nullptr, &uavView, ToCPUHandle(ResourcesViews[handle.GetIndex()].uav_locations, subIndex));
			}
		}

		GD12Device->CreateUnorderedAccessView(ResourcesTable[handle].resource, nullptr, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].uav_locations));
	}
	if (textureFlags & ALLOW_DEPTH_STENCIL) {
		Check(GetDepthStencilFormat(format) != DXGI_FORMAT_UNKNOWN);

		bool hasStencil = GetStencilReadFormat(format) != DXGI_FORMAT_UNKNOWN;

		if (!(textureFlags & TEX_MIPMAPPED)) {
			ResourcesViews[handle.GetIndex()].dsv_locations = DSVDescHeap.Allocate(hasStencil ? DSV_ACCESS_COUNT : DSV_NO_STENCIL_ACCESS_COUNT);
		}
		else {
			ResourcesViews[handle.GetIndex()].dsv_locations = DSVDescHeap.Allocate((hasStencil ? DSV_ACCESS_COUNT : DSV_NO_STENCIL_ACCESS_COUNT) * (subresourcesNum));

			for (auto subIndex : MakeRange(1u, subresourcesNum)) {
				auto offset = (hasStencil ? DSV_ACCESS_COUNT : DSV_NO_STENCIL_ACCESS_COUNT) * subIndex;

				D3D12_DEPTH_STENCIL_VIEW_DESC dsvView = {};
				dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				dsvView.Texture2D.MipSlice = subIndex;

				dsvView.Flags = D3D12_DSV_FLAG_NONE;
				dsvView.Format = GetDepthStencilFormat(format);
				GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, offset + DSV_WRITE_ALL));

				dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
				dsvView.Format = GetDepthStencilFormat(format);
				GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, offset + DSV_READ_ONLY_DEPTH));

				if (hasStencil) {
					dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
					dsvView.Format = GetDepthStencilFormat(format);
					GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, offset + DSV_READ_ONLY_STENCIL));

					dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
					dsvView.Format = GetDepthStencilFormat(format);
					GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, offset + DSV_READ_ONLY));
				}
			}
		}

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvView = {};
		dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvView.Texture2D.MipSlice = 0;

		dsvView.Flags = D3D12_DSV_FLAG_NONE;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_WRITE_ALL));

		dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
		dsvView.Format = GetDepthStencilFormat(format);
		GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY_DEPTH));

		if (hasStencil) {
			dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
			dsvView.Format = GetDepthStencilFormat(format);
			GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY_STENCIL));

			dsvView.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
			dsvView.Format = GetDepthStencilFormat(format);
			GD12Device->CreateDepthStencilView(ResourcesTable[handle].resource, &dsvView, ToCPUHandle(ResourcesViews[handle.GetIndex()].dsv_locations, DSV_READ_ONLY));
		}
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

	ResourcesViews[handle.GetIndex()].rtv_locations = RTVDescHeap.Allocate(1);
	GD12Device->CreateRenderTargetView(ResourcesTable[handle].resource, nullptr, ToCPUHandle(ResourcesViews[handle.GetIndex()].rtv_locations));

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

resource_handle CreateReadbackBufferForResource(resource_handle targetedResource) {
	UINT64 readbackBufferSize;

	auto desc = ResourcesTable[targetedResource].desc;
	u32 subresourcesNum = GetResourceInfo(targetedResource)->subresources_num;
	GD12Device->GetCopyableFootprints(&desc, 0, subresourcesNum, 0, nullptr, nullptr, nullptr, &readbackBufferSize);

	return CreateBuffer(READBACK_MEMORY, readbackBufferSize, 0, BUFFER_NO_FLAGS, "readback heap");
}

void MapReadbackBuffer(resource_handle buffer, resource_handle readAs, Array<subresource_read_info_t> *outReadInfo) {
	UINT64 readbackBufferSize;
	Array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(GetThreadScratchAllocator());
	Array<u32> numRows(GetThreadScratchAllocator());
	Array<uint64_t> rowSizes(GetThreadScratchAllocator());

	u32 subresourcesNum = GetResourceInfo(readAs)->subresources_num;
	Resize(layouts, subresourcesNum);
	Resize(numRows, subresourcesNum);
	Resize(rowSizes, subresourcesNum);

	auto desc = ResourcesTable[readAs].desc;
	GD12Device->GetCopyableFootprints(&desc, 0, subresourcesNum, 0, layouts.DataPtr, numRows.DataPtr, rowSizes.DataPtr, &readbackBufferSize);

	auto readbackRes = GetResourceFast(buffer)->resource;

	void* readData;
	readbackRes->Map(0, nullptr, &readData);

	if (outReadInfo) {
		for (auto i : MakeRange(subresourcesNum)) {
			subresource_read_info_t readInfo = {};
			readInfo.data = pointer_add(readData, layouts[i].Offset);
			readInfo.row_pitch = layouts[i].Footprint.RowPitch;
			readInfo.width = readInfo.width = layouts[i].Footprint.Width;
			readInfo.height = layouts[i].Footprint.Height;
			readInfo.format = layouts[i].Footprint.Format;
			PushBack(*outReadInfo, readInfo);
		}
	}
}

void UnmapReadbackBuffer(resource_handle buffer) {
	auto readbackRes = GetResourceFast(buffer)->resource;
	readbackRes->Unmap(0, nullptr);
}

void CopyToReadbackBuffer(GPUCommandList* list, resource_handle dst, resource_handle src) {
	auto d12list = GetD12CommandList(list);

	UINT64 readbackBufferSize;
	Array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(GetThreadScratchAllocator());
	Array<u32> numRows(GetThreadScratchAllocator());
	Array<uint64_t> rowSizes(GetThreadScratchAllocator());

	u32 subresourcesNum = GetResourceInfo(src)->subresources_num;
	Resize(layouts, subresourcesNum);
	Resize(numRows, subresourcesNum);
	Resize(rowSizes, subresourcesNum);

	auto desc = ResourcesTable[src].desc;
	GD12Device->GetCopyableFootprints(&desc, 0, subresourcesNum, 0, layouts.DataPtr, numRows.DataPtr, rowSizes.DataPtr, &readbackBufferSize);

	auto srcResource = GetResourceFast(src)->resource;
	auto dstResource = GetResourceFast(dst)->resource;

	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		Check(0);
	}
	else {
		TransitionBarrier(list, Slice(src), D3D12_RESOURCE_STATE_COPY_SOURCE);
		FlushBarriers(list);

		for (auto i : MakeRange(subresourcesNum)) {
			D3D12_TEXTURE_COPY_LOCATION srcLocation;
			srcLocation.pResource = srcResource;
			srcLocation.SubresourceIndex = i;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

			D3D12_TEXTURE_COPY_LOCATION dstLocation;
			dstLocation.pResource = dstResource;
			dstLocation.PlacedFootprint = layouts[i];
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

			GetD12CommandList(list)->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}
}

struct DeferredDeletion {
	resource_handle	handle;
	GPUFenceHandle	trigger_fence;
};

Ringbuffer<DeferredDeletion> Deletions;

void DeferredDestroyResource(GPUCommandList* list, resource_handle resource) {

}

void CopyFromCpuToSubresources(GPUCommandList* list, resource_slice_t dst, u32 subresourcesNum, D3D12_SUBRESOURCE_DATA const* subresourcesData) {
	auto d12list = GetD12CommandList(list);

	UINT64 uploadBufferSize;
	Array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(GetThreadScratchAllocator());
	Array<u32> numRows(GetThreadScratchAllocator());
	Array<uint64_t> rowSizes(GetThreadScratchAllocator());

	Resize(layouts, subresourcesNum);
	Resize(numRows, subresourcesNum);
	Resize(rowSizes, subresourcesNum);

	auto desc = ResourcesTable[dst.handle].desc;
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
		CopyBufferRegion(list, dst.handle, 0, tmpUploadHeap, layouts[0].Offset, layouts[0].Footprint.Width);
	}
	else {
		TransitionBarrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers(list);

		for (auto i : MakeRange(subresourcesNum)) {
			D3D12_TEXTURE_COPY_LOCATION srcLocation;
			srcLocation.PlacedFootprint = layouts[i];
			srcLocation.pResource = srcResource;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

			D3D12_TEXTURE_COPY_LOCATION dstLocation;
			dstLocation.pResource = GetResourceFast(dst.handle)->resource;
			dstLocation.SubresourceIndex = i;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

			GetD12CommandList(list)->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}

	srcResource->Unmap(0, nullptr);

	TransitionBarrier(list, dst, GetResourceTransitionInfo(dst.handle)->default_state);

	DeferredDestroyResource(list, tmpUploadHeap);
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