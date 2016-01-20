#pragma once

#include "Essence.h"
#include <d3d12.h>
#include "Shader.h"
#include "Resources.h"

namespace Essence {
class GPUQueue;
class GPUCommandList;

enum GPUQueueTypeEnum {
	DIRECT_QUEUE,
	COMPUTE_QUEUE,
	COPY_QUEUE
};

struct GPUFenceHandle {
	u32	handle;
	u32	generation;
};

inline bool operator !=(GPUFenceHandle A, GPUFenceHandle B) {
	return A.handle != B.handle || A.generation == B.generation;
}

struct upload_allocation_t {
	GPU_VIRTUAL_ADDRESS		virtual_address;
	void*			write_ptr;
};

void						WaitForCompletion();
void						InitRenderingEngines();
void						ShutdownRenderingEngines();

GPUQueue*					CreateQueue(GPUQueueTypeEnum type = DIRECT_QUEUE, u32 adapterIndex = 0);
ID3D12CommandQueue*			GetD12Queue(GPUQueue* queue);
void						WaitForCompletion(GPUQueue* queue);

GPUCommandList*				GetCommandList(GPUQueue* queue, ResourceNameId usage);
ID3D12GraphicsCommandList*	GetD12CommandList(GPUCommandList*);
GPUFenceHandle				GetFence(GPUQueue*);
GPUFenceHandle				GetFence(GPUCommandList*);
void						Close(GPUCommandList*);
void						Execute(GPUCommandList*);

void						WaitForCompletion(GPUFenceHandle);
bool						IsFenceCompleted(GPUFenceHandle);
void						EndCommandsFrame(GPUQueue* mainQueue, u32 limitGpuBufferedFrames = 0xFFFF);
void						QueueWait(GPUQueue* queue, GPUFenceHandle);

void						RegisterResource(resource_handle src, D3D12_RESOURCE_STATES initialState);
vertex_factory_handle		GetVertexFactory(std::initializer_list<input_layout_element_t> elements);

void						CopyBufferRegion(GPUCommandList*, resource_handle dst, u64 srcOffset, resource_handle src, u64 dstOffset, u64 size);
void						CopyResource(GPUCommandList*, resource_handle dst, resource_handle src);
void						TransitionBarrier(GPUCommandList*, resource_slice_t slice, D3D12_RESOURCE_STATES after);
void						FlushBarriers(GPUCommandList*);
void						ClearRenderTarget(GPUCommandList*, resource_rtv_t, float4 = float4(0, 0, 0, 0));

enum ClearDepthFlagEnum {
	CLEAR_DEPTH = 1,
	CLEAR_STENCIL = 2,
	CLEAR_ALL = 3 
};
void						ClearDepthStencil(GPUCommandList*, resource_dsv_t, ClearDepthFlagEnum flags = CLEAR_ALL, float depth = 1.f, u8 stencil = 0, u32 numRects = 0, D3D12_RECT* rects = nullptr);
void						ClearUnorderedAccess(GPUCommandList*, resource_uav_t);
void						SetShaderState	(GPUCommandList*, shader_handle vs, shader_handle ps, vertex_factory_handle vertexFactory);
void						SetComputeShaderState(GPUCommandList*, shader_handle cs);
void						SetTopology(GPUCommandList*, D3D_PRIMITIVE_TOPOLOGY topology);
void						SetRenderTarget(GPUCommandList*, u32 index, resource_rtv_t);
void						SetRasterizer(GPUCommandList*, D3D12_RASTERIZER_DESC const& desc);
void						SetBlendState(GPUCommandList*, u32 index, D3D12_RENDER_TARGET_BLEND_DESC const& desc);
void						SetDepthStencil(GPUCommandList*, resource_dsv_t);
void						SetViewport(GPUCommandList*, float width, float height, float x = 0, float y = 0, float minDepth = 0, float maxDepth = 1);
void						SetScissorRect(GPUCommandList* list, D3D12_RECT rect);
void						Draw(GPUCommandList*, u32 vertexCount, u32 startVertex = 0, u32 instances = 1, u32 startInstance = 0);
void						DrawIndexed(GPUCommandList*, u32 indexCount, u32 startIndex = 0, i32 baseVertex = 0, u32 instances = 1, u32 startInstance = 0);
void						Dispatch(GPUCommandList*, u32 threadGroupX, u32 threadGroupY, u32 threadGroupZ);
void						SetConstant(GPUCommandList*, TextId var, const void* srcPtr, size_t srcSize);
void						SetTexture2D(GPUCommandList*, TextId slot, resource_srv_t srv);
void						SetRWTexture2D(GPUCommandList*, TextId slot, resource_uav_t uav);
void						SetVertexStream(GPUCommandList*, u32 index, buffer_location_t stream);
void						SetIndexBuffer(GPUCommandList*, buffer_location_t stream);
upload_allocation_t			AllocateSmallUploadMemory(GPUCommandList*, u64 size, u64 alignment);

template<typename T> void	SetConstant(GPUCommandList* list, TextId var, T const&srcRef) {
	SetConstant(list, var, &srcRef, sizeof(T));
}

void						GetD3D12StateDefaults(D3D12_RASTERIZER_DESC *pDest);

void						UpdatePipelineStates();

}