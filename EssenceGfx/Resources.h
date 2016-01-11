#pragma once

#include "Essence.h"
#include <d3d12.h>
#include "Device.h"

namespace Essence {

typedef GenericHandle32<20, TYPE_ID(Resource)> resource_handle;
typedef GenericHandle32<20, TYPE_ID(VertexFactory)> vertex_factory_handle;	

class GPUCommandList;

struct input_layout_element_t {
	DXGI_FORMAT		format;
	const char*		semantic_name;
};

namespace VertexInput {
static const input_layout_element_t POSITION_4_32F	= { DXGI_FORMAT_R32G32B32A32_FLOAT	, "POSITION" };
static const input_layout_element_t POSITION_3_32F	= { DXGI_FORMAT_R32G32B32_FLOAT		, "POSITION" };
static const input_layout_element_t POSITION_2_32F	= { DXGI_FORMAT_R32G32_FLOAT		, "POSITION" };
static const input_layout_element_t NORMAL_32F		= { DXGI_FORMAT_R32G32B32_FLOAT		, "NORMAL" };
static const input_layout_element_t TEXCOORD_32F	= { DXGI_FORMAT_R32G32_FLOAT		, "TEXCOORD" };
static const input_layout_element_t BONE_INDICES_8U	= { DXGI_FORMAT_R8G8B8A8_UINT		, "BONE_INDICES" };
static const input_layout_element_t BONE_WEIGHTS_32F= { DXGI_FORMAT_R32G32B32A32_FLOAT	, "BONE_WEIGHTS" };
static const input_layout_element_t COLOR_RGBA_8U =	  { DXGI_FORMAT_R8G8B8A8_UNORM		, "COLOR" };
};

struct buffer_location_t {
	GPU_VIRTUAL_ADDRESS	address;
	u32 stride;
	u32 size;
};

const u32 PER_SUBRESOURCE_STATE_TRACKING = 0xFFFFFFFF;

struct resource_slice_t {
	resource_handle		handle;
	u32					subresource; // -> 0 = all, 1 = 0, etc (shift by 1 from api)
};

enum TextureFlags {
	NO_TEXTURE_FLAGS = 0,
	ALLOW_RENDER_TARGET = 1,
	ALLOW_DEPTH_STENCIL = 2,
	ALLOW_UNORDERED_ACCESS = 4,
	TEX_MIPMAPPED = 8,
	TEX_VIRTUAL = 0x10
};
DEFINE_ENUM_FLAG_OPERATORS(TextureFlags);

enum DsvAccessEnum {
	DSV_WRITE_ALL = 0,
	DSV_READ_ONLY_DEPTH,
	DSV_READ_ONLY_STENCIL,
	DSV_READ_ONLY,
	DSV_ACCESS_COUNT,
	DSV_NO_STENCIL_ACCESS_COUNT = DSV_READ_ONLY_STENCIL
};

struct subresource_read_info_t {
	void*		data;
	u32			row_pitch;
	u32			width;
	u32			height;
	DXGI_FORMAT	format;
};

resource_handle	CreateTexture(u32 width, u32 height, DXGI_FORMAT format, TextureFlags textureFlags, const char* debugName, float4 clearColor = float4(0, 0, 0, 0), float clearDepth = 1.f, u8 clearStencil = 0);
void			CopyFromCpuToSubresources(class GPUQueue* queue, resource_slice_t dstResource, u32 subresourcesNum, D3D12_SUBRESOURCE_DATA const* subresourcesData);
resource_handle CreateReadbackBufferForResource(resource_handle targetedResource);
void			CopyToReadbackBuffer(GPUCommandList* list, resource_handle dst, resource_handle src);
void			UnmapReadbackBuffer(resource_handle buffer);
void			MapReadbackBuffer(resource_handle buffer, resource_handle readAs, Array<subresource_read_info_t> *outReadInfo);

void	InitResources();
void	ShutdownResources();

enum ResourceCreationType {
	UNKNOWN_RESOURCE,
	COMMITED_RESOURCE,
	RESERVED_RESOURCE
};

enum ResourceHeapType {
	UNKNOWN_MEMORY,
	DEFAULT_MEMORY,
	UPLOAD_MEMORY,
	READBACK_MEMORY
};

struct resource_t {
	ID3D12Resource*				resource;
	u32							subresources_num;
	u64							width;
	u64							height;
	u16							miplevels;
	u16							depth_or_array_size;
	D3D12_RESOURCE_DESC			desc;
	ResourceCreationType		creation_type;
	ResourceHeapType			heap_type;
	TextId						debug_name;
};

struct resource_fast_t {
	ID3D12Resource*		resource;
	CPU_DESC_HANDLE		srv;
	u32					is_read_only : 1;
};

struct resource_transition_t {
	D3D12_RESOURCE_STATES	default_state;
	ResourceHeapType		heap_type;
};

struct resource_rtv_t {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor;
	DXGI_FORMAT					format;
	resource_slice_t			slice;
};

struct resource_dsv_t {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor;
	DXGI_FORMAT					format;
	resource_slice_t			slice;
	u32							has_stencil : 1;
};

inline bool IsValid(resource_dsv_t dsv) {
	return IsValid(dsv.slice.handle);
}

struct resource_srv_t {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor;
	resource_slice_t			slice;
	u8							fixed_state : 1;
	u8							is_depth : 1;
	u8							is_stencil : 1;
};

struct resource_uav_t {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor;
	resource_slice_t			slice;
};

extern CPU_DESC_HANDLE	G_NULL_TEXTURE2D_SRV_DESCRIPTOR;
extern CPU_DESC_HANDLE	G_NULL_TEXTURE2D_UAV_DESCRIPTOR;

resource_t*				GetResourceInfo(resource_handle);
resource_fast_t*		GetResourceFast(resource_handle);
resource_transition_t*	GetResourceTransitionInfo(resource_handle);

resource_rtv_t			GetRTV(resource_handle resource);
resource_rtv_t			GetRTV(resource_handle resource, u32 mipmap);

resource_dsv_t			GetDSV(resource_handle resource);
resource_dsv_t			GetDSV(resource_handle resource, u32 mipmap);

resource_uav_t			GetUAV(resource_handle resource);
resource_uav_t			GetUAV(resource_handle resource, u32 mipmap);

resource_srv_t			GetSRV(resource_handle resource);
resource_srv_t			GetSRV(resource_handle resource, u32 mipmap);

void					Delete(resource_handle);
void					DeregisterSwapChainBuffers();
void					RegisterSwapChainBuffer(ID3D12Resource* resource, u32 index);
resource_handle			GetCurrentBackbuffer();

inline resource_slice_t Slice(resource_handle resource, u32 subresource = 0) {
	resource_slice_t slice;
	slice.handle = resource;
	slice.subresource = subresource;
	return slice;
}

enum BufferFlags {
	BUFFER_NO_FLAGS = 0,
	ALLOW_VERTEX_BUFFER = 1,
	ALLOW_INDEX_BUFFER = 2
};

resource_handle			CreateBuffer(ResourceHeapType heapType, u64 size, u64 stride, BufferFlags flags, const char* debugName);
void					CopyToBuffer(GPUCommandList* list, resource_handle dstBuffer, const void* dataPtr, u64 size);

inline void* HandleToImGuiTexID(resource_handle handle) {
	u64 val = 0;
	*reinterpret_cast<resource_handle*>(&val) = handle;
	return (void*)val;
}

}