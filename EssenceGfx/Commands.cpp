#include "Device.h"
#include "Commands.h"
#include "Hashmap.h"
#include "Array.h"
#include "Descriptors.h"
#include "Ringbuffer.h"
#include "Freelist.h"

#include <d3d12shader.h>
#include <d3dcompiler.h>
#include "d3dx12.h"

#include "remotery\Remotery.h"

#define GPU_PROFILING 1

namespace Essence {

const bool GVerbosePipelineStates = false;
const bool GVerboseRootSingatures = false;

enum PipelineTypeEnum {
	PIPELINE_UNKNOWN,
	PIPELINE_GRAPHICS,
	PIPELINE_COMPUTE
};

bool IsExclusiveState(D3D12_RESOURCE_STATES state) {
	switch (state) {
	case D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
	case D3D12_RESOURCE_STATE_INDEX_BUFFER:
	case D3D12_RESOURCE_STATE_RENDER_TARGET:
	case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
	case D3D12_RESOURCE_STATE_DEPTH_WRITE:
	case D3D12_RESOURCE_STATE_STREAM_OUT:
	case D3D12_RESOURCE_STATE_COPY_DEST:
	case D3D12_RESOURCE_STATE_COMMON:
		return true;
	default:
		return false;
	}
}

bool NeedStateChange(GPUQueueTypeEnum queueType, ResourceHeapType heapType, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool exclusive = false) {
	if (heapType != DEFAULT_MEMORY) {
		return false;
	}

	if (queueType == DIRECT_QUEUE) {
		return (after != before) && ((after & before) == 0 || exclusive);
	}
	else if (queueType == COPY_QUEUE) {
		return false;
	}

	Check(0);
	return false;
}

D3D12_RESOURCE_STATES GetNextState(GPUQueueTypeEnum queueType, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	if (IsExclusiveState(after) || IsExclusiveState(before)) {
		return after;
	}

	Check(before != after);

	return before | after;
}

void GetD3D12StateDefaults(D3D12_RASTERIZER_DESC *pDest) {
	*pDest = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
}

bool IsDepthReadOnly(D3D12_GRAPHICS_PIPELINE_STATE_DESC const* desc) {
	return 
		desc->DepthStencilState.DepthEnable == false ||
		desc->DepthStencilState.DepthFunc == D3D12_COMPARISON_FUNC_NEVER ||
		desc->DepthStencilState.DepthWriteMask == 0;
}

bool IsStencilReadOnly(D3D12_GRAPHICS_PIPELINE_STATE_DESC const* desc) {
	return 
		desc->DepthStencilState.StencilEnable == false ||
		desc->DepthStencilState.StencilWriteMask == 0 ||
		(desc->DepthStencilState.FrontFace.StencilFunc == D3D12_COMPARISON_FUNC_NEVER ||
			(desc->DepthStencilState.FrontFace.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP
			&& desc->DepthStencilState.FrontFace.StencilFailOp == D3D12_STENCIL_OP_KEEP
			&& desc->DepthStencilState.FrontFace.StencilPassOp == D3D12_STENCIL_OP_KEEP)) ||
		(desc->DepthStencilState.BackFace.StencilFunc == D3D12_COMPARISON_FUNC_NEVER ||
			(desc->DepthStencilState.BackFace.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP
			&& desc->DepthStencilState.BackFace.StencilFailOp == D3D12_STENCIL_OP_KEEP
			&& desc->DepthStencilState.BackFace.StencilPassOp == D3D12_STENCIL_OP_KEEP));
}

// threadsafe ringbuffer, not suited for bigger allocations
class UploadHeapAllocator {
public:
	struct block_fence_t {
		u64				read_offset;
		GPUFenceHandle	fence;
	};

	struct MemoryBlock {
		ID3D12Resource*				D12Resource;
		void*						MappedPtr;
		Ringbuffer<block_fence_t>	Fences;
		u64							Size;

		au64						ReadOffset;
		au64						WriteOffset;

		MemoryBlock(u32 size) : Size(size), MappedPtr(nullptr), ReadOffset(0), WriteOffset(0) {
			Reserve(Fences, 8);

			VerifyHr(GD12Device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(Size),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&D12Resource)));

			VerifyHr(D12Resource->Map(0, nullptr, &MappedPtr));
		}

		~MemoryBlock() {
			if (D12Resource) {
				D12Resource->Unmap(0, nullptr);
			}
			ComRelease(D12Resource);
			FreeMemory(Fences);
		}
	};

	u32						MinBlockSize = 1024 * 1024; // 1 Mb chunks, larger if needed
	Array<MemoryBlock*>		AvailableBlocks;
	Array<MemoryBlock*>		PendingBlocks;
	MemoryBlock*			CurrentBlock;

	CriticalSection			CS;

	UploadHeapAllocator() : CurrentBlock(nullptr) {
	}

	UploadHeapAllocator& operator =(UploadHeapAllocator&& other) {
		AvailableBlocks = std::move(other.AvailableBlocks);
		PendingBlocks = std::move(other.PendingBlocks);
		CurrentBlock = other.CurrentBlock;
		other.CurrentBlock = nullptr;

		return *this;
	}

	UploadHeapAllocator(UploadHeapAllocator&& other) {
		*this = std::move(other);
	}

	~UploadHeapAllocator() {
		RemoveAll(AvailableBlocks, [](auto block) { return block == nullptr; });
		FreeTemporaryAllocations();

		for (auto b : AvailableBlocks) {
			_delete(b);
		}
		for (auto b : PendingBlocks) {
			_delete(b);
		}
		_delete(CurrentBlock);
		CurrentBlock = nullptr;

		FreeMemory(AvailableBlocks);
		FreeMemory(PendingBlocks);
	}

	void				AllocateNewBlock() {
		u32 blockSize = MinBlockSize;

		MemoryBlock* block;
		_new(block, blockSize);

		CurrentBlock = block;
	}

	upload_allocation_t AllocateTemporary(u32 size, u32 alignment = 256) {
		Check(size < MinBlockSize);

		if (CurrentBlock == nullptr) {
			ScopeLock lock(&CS);
			if (CurrentBlock == nullptr) {
				AllocateNewBlock();
			}
		}

		auto block = CurrentBlock;
		auto paddedSize = (size + alignment - 1) & ~(alignment - 1);

		u64 writeOffset = block->WriteOffset.load();
		while (true) {
			auto nextWriteOffset = (u64)align_forward((void*)writeOffset, alignment) + paddedSize;
			auto readOffset = block->ReadOffset.load();
			auto diff = nextWriteOffset - readOffset;
			auto blockSize = block->Size;

			auto notContinous = (writeOffset / blockSize) != (nextWriteOffset / blockSize);

			if (notContinous) {
				// rewind
				auto blockEnd = ((writeOffset / blockSize) + 1) * blockSize;
				if (block->WriteOffset.compare_exchange_strong(writeOffset, blockEnd)) {
					writeOffset = blockEnd;
				}
				continue;
			}
			if (diff > blockSize) {
				ScopeLock lock(&CS);
				if (block == CurrentBlock) {
					PushBack(PendingBlocks, block);
					bool recycled = false;
					for (u32 i = 0; i < Size(AvailableBlocks); ++i) {
						if (AvailableBlocks[i] != nullptr) {
							CurrentBlock = AvailableBlocks[i];
							AvailableBlocks[i] = nullptr;
							recycled = true;
							break;
						}
					}
					if (!recycled) {
						AllocateNewBlock();
					}
				}
				block = CurrentBlock;
				writeOffset = block->WriteOffset.load();
				continue;
			}

			if (block->WriteOffset.compare_exchange_strong(writeOffset, nextWriteOffset)) {
				break;
			}
		}

		u64 alignedWriteOffset = (u64)align_forward((void*)writeOffset, alignment);
		
		auto block_byte_offset = alignedWriteOffset % block->Size;

		Check(block_byte_offset + size <= block->Size );

		upload_allocation_t out;
		out.virtual_address = block->D12Resource->GetGPUVirtualAddress() + block_byte_offset;
		out.write_ptr = pointer_add(block->MappedPtr, block_byte_offset);

		return out;
	}

	void FreeTemporaryAllocations() {
		for (auto& block : PendingBlocks) {

			while (Size(block->Fences)) {
				if (IsFenceCompleted(Front(block->Fences).fence)) {
					block->ReadOffset = Front(block->Fences).read_offset;
					PopFront(block->Fences);
				}
				else {
					break;
				}
			}

			if (!Size(block->Fences)) {
				PushBack(AvailableBlocks, block);
				block = nullptr;
			}
		}

		RemoveAll(PendingBlocks, [](auto block) { return block == nullptr; });
		if (CurrentBlock == nullptr) {
			CurrentBlock = Size(AvailableBlocks) ? AvailableBlocks[0] : nullptr;
		}
	}

	void FenceTemporaryAllocations(GPUFenceHandle fence) {
		RemoveAll(AvailableBlocks, [](auto block) { return block == nullptr; });

		for (auto& block : AvailableBlocks) {
			if ((!Size(block->Fences) && block->ReadOffset != block->WriteOffset) ||
				(Size(block->Fences) && Back(block->Fences).read_offset != block->WriteOffset)) {
				block_fence_t blockFence;
				blockFence.fence = fence;
				blockFence.read_offset = block->WriteOffset;
				PushBack(block->Fences, blockFence);
			}
		}
		for (auto& block : PendingBlocks) {
			if (!Size(block->Fences) || 
				(Size(block->Fences) && Back(block->Fences).read_offset != block->WriteOffset)) {
				block_fence_t blockFence;
				blockFence.fence = fence;
				blockFence.read_offset = block->WriteOffset;
				PushBack(block->Fences, blockFence);
			}
		}
	}
};

struct gpu_sample {
	cstr	label;
	u32*	rmt_name_hash;
	u32		timestamp_index_begin;
	u32		timestamp_index_end;
	GPUCommandList*	cl;
};

struct sample_internal {
	cstr	label;
	u32		name_hash;
	u32		timestamp_index_begin;
	u32		timestamp_index_end;
};

class GPUProfiler {
public:
	struct query_readback_frame_fence {
		u64 index_from;
		u64 num;
	};

	// own-ptr
	OwningComPtr<ID3D12QueryHeap>			QueryHeap;
	// data
	GPUQueue*								Queue;
	u32										MaxTimestamps;
	resource_handle							ReadbackBuffers;
	std::atomic_uint64_t					QueryIssueIndex;
	u64										QueryResolveIndex;

	u64										QueueFrequency;
	u64										QueueClockGpuCtr;
	u64										QueueClockCpuCalibratedCtr;

	CriticalSection							CS;

	Ringbuffer<query_readback_frame_fence>	Fences;
	Ringbuffer<sample_internal>				Samples;

	static const u32						MaxPendingQueries = 64 * 1024;
	static const u32						MaxQueuedFrames = 3;
	u32										ReadIndex;
	u32										WriteIndex;
	GPUFenceHandle							ReadFences[MaxQueuedFrames];
	resource_handle							Readback[MaxQueuedFrames];

	GPUProfiler() :
		Fences(),
		Samples(),
		QueryIssueIndex(0),
		QueryResolveIndex(0)
	{
		MaxTimestamps = MaxPendingQueries;

		D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
		queryHeapDesc.Count = MaxPendingQueries;
		queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		VerifyHr(GD12Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(QueryHeap.GetInitPtr())));

		for (u32 i = 0; i < MaxQueuedFrames; ++i) {
			Readback[i] = CreateBuffer(READBACK_MEMORY, sizeof(u64) * (MaxPendingQueries + MaxQueuedFrames - 1) / MaxQueuedFrames, BUFFER_NO_FLAGS, "readback buffer");
			ReadFences[i] = {};
		}

		ReadIndex = WriteIndex = 0;
	}

	void FreeMemory() {
		Essence::FreeMemory(Fences);
		Essence::FreeMemory(Samples);
	}

	~GPUProfiler() {
		FreeMemory();
	}

	void AttachToQueue(GPUQueue* queue);
	void GatherListSamples(Ringbuffer<sample_internal> *rb);

	void ResolveFrameProfilingQueries(GPUCommandList* list);
	void ReadbackAndFeedProfiler();
};

class GPUProfilerContext {
public:
	GPUProfiler*					Profiler;
	Ringbuffer<sample_internal>		Samples;

	void Begin(gpu_sample *sample);
	void End(gpu_sample *sample);

	~GPUProfilerContext() {
		FreeMemory(Samples);
	}
};

struct VertexFactory {
	D3D12_INPUT_ELEMENT_DESC*	Elements;
	u32							ElementsNum;
};

Hashmap<u64, vertex_factory_handle>					VertexFactoryByHash;
Freelist<VertexFactory, vertex_factory_handle>		VertexFactories;

vertex_factory_handle	GetVertexFactory(std::initializer_list<input_layout_element_t> elements) {
	D3D12_INPUT_ELEMENT_DESC d12elements[16];
	Check(elements.size() <= _countof(d12elements));

	u32 index = 0;
	for (auto in : elements) {
		d12elements[index] = {};

		d12elements[index].SemanticName = in.semantic_name;
		d12elements[index].SemanticIndex = 0;
		d12elements[index].Format = in.format;
		d12elements[index].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
		d12elements[index].InputSlot = 0;
		d12elements[index].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		d12elements[index].InstanceDataStepRate = 0;

		++index;
	}

	auto bytesize = sizeof(d12elements[0]) * elements.size();

	u64 hash = Hash::MurmurHash2_64(d12elements, bytesize, 0);
	auto ptr = Get(VertexFactoryByHash, hash);
	if (ptr) {
		return *ptr;
	}

	auto storePtr = GetMallocAllocator()->Allocate(bytesize, alignof(D3D12_INPUT_ELEMENT_DESC));
	memcpy(storePtr, d12elements, bytesize);

	VertexFactory factory;
	factory.Elements = (D3D12_INPUT_ELEMENT_DESC*)storePtr;
	factory.ElementsNum = (u32)elements.size();

	auto handle = Create(VertexFactories);
	VertexFactories[handle] = factory;

	Set(VertexFactoryByHash, hash, handle);

	return handle;
}

D3D12_INPUT_LAYOUT_DESC	GetInputLayoutDesc(vertex_factory_handle handle) {
	if (IsValid(handle)) {
		D3D12_INPUT_LAYOUT_DESC out = {};
		out.NumElements = VertexFactories[handle].ElementsNum;
		out.pInputElementDescs = VertexFactories[handle].Elements;
		return out;
	}
	return{};
}

D3D12_COMMAND_LIST_TYPE GetD12QueueType(GPUQueueTypeEnum type) {
	switch (type) {
	case COMPUTE_QUEUE:
		return D3D12_COMMAND_LIST_TYPE_COMPUTE;
	case COPY_QUEUE:
		return D3D12_COMMAND_LIST_TYPE_COPY;
	case DIRECT_QUEUE:
	default:
		return D3D12_COMMAND_LIST_TYPE_DIRECT;
	}
}

struct GPUFence {
	GPUQueue*	queue;
	u64			value;
};

const u32					MAX_PENDING_FENCES = 4096;
GPUFence					Fences[MAX_PENDING_FENCES];
u32							FenceGenerations[MAX_PENDING_FENCES];
u64							FenceCounter;

Array<GPUQueue*>			GPUQueues;
UploadHeapAllocator			ConstantsAllocator;
DescriptorAllocator			GpuDescriptorsAllocator;
GPUFenceHandle				CreateFence(GPUQueue* queue);
Ringbuffer<GPUFenceHandle>	FrameFences;

void				InitRenderingEngines() {
	GpuDescriptorsAllocator = std::move(DescriptorAllocator(512 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true));
	ConstantsAllocator = std::move(UploadHeapAllocator());
}

class GPUCommandListPool;
class GPUCommandAllocatorPool;

class GPUQueue {
public:
	// own-ptrs
	OwningComPtr<ID3D12CommandQueue>	D12CommandQueue;
	OwningComPtr<ID3D12Fence>			D12Fence;
	OwningComPtr<ID3D12Fence>			D12SharedFence;
	OwningPtr<GPUProfiler>				Profiler;

	Hashmap<ResourceNameId, GPUCommandListPool*>		CommandListPools;
	Hashmap<ResourceNameId, GPUCommandAllocatorPool*>	CommandAllocatorPools;

	// data
	GPUQueueTypeEnum	Type;
	u32					AdapterIndex;
	HANDLE				SyncEvent;
	u64					FenceValue;
	u64					LastSignaledValue;
	GPUFenceHandle		LastSignaledFence;
	char				DebugName[64];

	GPUQueue(TextId name, GPUQueueTypeEnum type, u32 adapterIndex);
	~GPUQueue();

	u64					GetCompletedValue();
	void				AdvanceFence();
	void				EndFrame();
};

GPUFenceHandle GetLastSignaledFence(GPUQueue* queue) {
	return queue->LastSignaledFence;
}

u64 GPUQueue::GetCompletedValue() {
	return D12Fence->GetCompletedValue();
}

void GPUQueue::AdvanceFence() {
	VerifyHr(D12CommandQueue->Signal(*D12Fence, FenceValue));
	VerifyHr(D12CommandQueue->Signal(*D12SharedFence, FenceValue));

	LastSignaledValue = FenceValue++;
}

GPUFenceHandle			CreateFence(GPUQueue* queue) {
	auto index = FenceCounter++;
	index = index % MAX_PENDING_FENCES;

	if (Fences[index].queue) {
		Check(Fences[index].value <= Fences[index].queue->GetCompletedValue());
	}

	Fences[index].queue = queue;
	Fences[index].value = 0;
	auto generation = ++FenceGenerations[index];

	GPUFenceHandle handle;
	handle.handle = (u32)index;
	handle.generation = generation;

	return handle;
}

bool IsFenceCompleted(GPUFenceHandle fence) {
	if (FenceGenerations[fence.handle] != fence.generation) {
		return true;
	}

	if (Fences[fence.handle].value == 0) {
		return false;
	}

	return Fences[fence.handle].value <= Fences[fence.handle].queue->GetCompletedValue();
}

void WaitForCompletion(GPUQueue* queue, u64 fenceValue);

void WaitForCompletion(GPUFenceHandle fence) {
	if (!IsFenceCompleted(fence)) {
		WaitForCompletion(Fences[fence.handle].queue, Fences[fence.handle].value);
	}
}

enum CommandAllocatorStateEnum {
	CA_READY,
	CA_RECORDING,
	CA_PENDING
};

class GPUCommandAllocator {
public:
	// owned data
	OwningComPtr<ID3D12CommandAllocator>	D12CommandAllocator;
	//
	ResourceNameId				Usage;
	GPUQueueTypeEnum			Type;
	Array<GPUFenceHandle>		Fences;
	GPUCommandAllocatorPool*	Pool;
	u32							ListsRecorded;
	CommandAllocatorStateEnum	State;

	GPUCommandAllocator(GPUQueueTypeEnum type, ResourceNameId usage, GPUCommandAllocatorPool* pool) :
		Type(type),
		Usage(usage),
		Pool(pool),
		ListsRecorded(0)
	{
		VerifyHr(GD12Device->CreateCommandAllocator(GetD12QueueType(type), IID_PPV_ARGS(D12CommandAllocator.GetInitPtr())));
		D12CommandAllocator->Reset();

		State = CA_READY;
	}

	~GPUCommandAllocator() {
		FreeMemory(Fences);
	}

	void FenceExecution(GPUFenceHandle handle) {
		if (Size(Fences) && Back(Fences) != handle) {
			PushBack(Fences, handle);
		}
	}

	bool IsCompleted() {
		for (auto fence : Fences) {
			if (!IsFenceCompleted(fence)) {
				return false;
			}
		}
		Clear(Fences);
		return true;
	}
};

class GPUCommandAllocatorPool {
public:
	// own-ptrs
	Array<GPUCommandAllocator*>	Allocators;
	// weak-ptrs
	Array<GPUCommandAllocator*>	ReadyAllocators;
	Array<GPUCommandAllocator*>	PendingAllocators;
	GPUQueue*					Queue;
	ResourceNameId				Usage;

	GPUCommandAllocatorPool(GPUQueue* queue, ResourceNameId usage) : Queue(queue), Usage(usage) {
	}

	~GPUCommandAllocatorPool() {
		RecycleProcessed();

		Check(Size(PendingAllocators) == 0);
		Check(Size(Allocators) == Size(ReadyAllocators));

		for (auto ptr : Allocators) {
			_delete(ptr);
		}

		FreeMemory(Allocators);
		FreeMemory(ReadyAllocators);
		FreeMemory(PendingAllocators);
	}

	// returns command allocator, ready to start recording
	GPUCommandAllocator* Get() {
		if (Size(ReadyAllocators)) {
			auto last = Back(ReadyAllocators);
			PopBack(ReadyAllocators);

			Check(last->State == CA_READY);
			last->State = CA_RECORDING;
			return last;
		}

		GPUCommandAllocator* allocator;
		_new(allocator, Queue->Type, Usage, this);

		PushBack(Allocators, allocator);
		allocator->State = CA_RECORDING;
		return allocator;
	}

	void Return(GPUCommandAllocator* allocator, GPUFenceHandle fence) {
		Check(allocator->State == CA_RECORDING);
		allocator->State = CA_READY;
		allocator->FenceExecution(fence);
		PushBack(ReadyAllocators, allocator);
		allocator->ListsRecorded++;
	}

	void RecycleProcessed() {
		for (auto& ready : ReadyAllocators) {
			Check(ready->State == CA_READY);

			if (ready->ListsRecorded) {
				PushBack(PendingAllocators, ready);
				ready->State = CA_PENDING;
				ready = nullptr;
			}
		}
		RemoveAll(ReadyAllocators, [](void*ptr) { return ptr == nullptr; });

		for (auto& pending : PendingAllocators) {
			Check(pending->State == CA_PENDING);
			if (pending->IsCompleted()) {
				pending->State = CA_READY;
				pending->ListsRecorded = 0;
				VerifyHr(pending->D12CommandAllocator->Reset());

				PushBack(ReadyAllocators, pending);
				pending = nullptr;
			}
		}
		RemoveAll(PendingAllocators, [](void*ptr) { return ptr == nullptr; });
	}
};

struct resource_tracking_state {
	u32		resource_state;
	bool	per_subresource_tracking;
};

Hashmap<resource_handle, resource_tracking_state>			GResourceState;
Hashmap<resource_slice_t, D3D12_RESOURCE_STATES>			GSubresourceState;

class ResourceTracker {
public:
	Hashmap<resource_handle, resource_tracking_state>	ExpectedState;
	Hashmap<resource_slice_t, u32>						ExpectedSubresourceState;

	Hashmap<resource_handle, resource_tracking_state>	CurrentState;
	Hashmap<resource_slice_t, u32>						CurrentSubresourceState;

	Array<D3D12_RESOURCE_BARRIER>						QueuedBarriers;
	GPUCommandList*										Owner;

	ResourceTracker(GPUCommandList* list);
	~ResourceTracker();

	void Transition(resource_slice_t resource, D3D12_RESOURCE_STATES after);
	void FireBarriers();
	void EnqueueTransition(ID3D12Resource* resource, u32 subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

	void Clear() {
		Essence::Clear(ExpectedState);
		Essence::Clear(ExpectedSubresourceState);
		Essence::Clear(CurrentState);
		Essence::Clear(CurrentSubresourceState);
		Check(Size(QueuedBarriers) == 0);
	}
};

enum CommandListStateEnum {
	CL_UNASSIGNED,
	CL_RECORDING,
	CL_CLOSED,
	CL_EXECUTED
};

enum RootParameterEnum {
	ROOT_TABLE
};

struct root_table_parameter_t {
	u32 length;
	u32 uav_range_offset;
	u32 cbv_range_offset;
	u32 root_index;
};

struct root_parameter_t {
	RootParameterEnum type;
	union {
		root_table_parameter_t table;
	};
	struct {
		union {
			struct {
				GPU_DESC_HANDLE gpu_handle;
				CPU_DESC_HANDLE cpu_handle;
				u32				src_array_offset;
			} table;
		};
	} binding;
};

struct shader_constantbuffer_t {
	u32 bytesize;
	u32 table_slot;
	u64 param_hash;
};

class PipelineStateBindings;

struct constantbuffer_cpudata_t {
	void*	write_ptr;
	u32		size;
};

const u32 MAX_RTVS = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;

class GPUCommandList {
public:
	// own-ptrs
	OwningComPtr<ID3D12GraphicsCommandList>	D12CommandList;
	// weak-ptrs
	GPUCommandAllocator*		CommandAllocator;
	GPUQueue*					Queue;
	GPUFenceHandle				Fence;
	GPUCommandListPool*			Pool;
	// data
	ResourceNameId				Usage;
	CommandListStateEnum		State;
	ResourceTracker				ResourcesStateTracker;
	GPUProfilerContext			ProfilerContext;
#if GPU_PROFILING
	gpu_sample					Sample;
#endif
	//
	struct {
		Array<CPU_DESC_HANDLE>					DstDescRanges;
		Array<u32>								DstDescSizes;
		Array<CPU_DESC_HANDLE>					SrcDescRanges;
		Array<u32>								SrcDescRangeSizes;
		Hashmap<u64, constantbuffer_cpudata_t>	ConstantBuffers;
		Hashmap<u64, root_parameter_t>			Params;
	} Root;
	struct {
		D3D12_VIEWPORT							Viewport;
		D3D12_RECT								ScissorRect;
		D3D_PRIMITIVE_TOPOLOGY					Topology;
		resource_rtv_t							RTVs[MAX_RTVS];
		resource_dsv_t							DSV;
		u32										NumRenderTargets;
		vertex_factory_handle					VertexFactory;
		D3D12_VERTEX_BUFFER_VIEW				VertexStreams[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		u32										VertexStreamsNum;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC		PipelineDesc;
	} Graphics;
	struct {
		D3D12_COMPUTE_PIPELINE_STATE_DESC		PipelineDesc;
	} Compute;
	PipelineStateBindings*	Bindings;
	PipelineTypeEnum		Type;

	void ResetState() {
		Graphics = {};

		ZeroMemory(&Graphics.PipelineDesc, sizeof(Graphics.PipelineDesc));
		Graphics.PipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.SampleMask = UINT_MAX;
		Graphics.PipelineDesc.SampleDesc.Count = 1;

		Graphics.Topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		Graphics.ScissorRect.left = 0;
		Graphics.ScissorRect.top = 0;
		Graphics.ScissorRect.right = 32768;
		Graphics.ScissorRect.bottom = 32768;

		Compute = {};

		ZeroMemory(&Compute.PipelineDesc, sizeof(Compute.PipelineDesc));

		Clear(Root.DstDescRanges);
		Clear(Root.DstDescSizes);
		Clear(Root.SrcDescRanges);
		Clear(Root.SrcDescRangeSizes);
		Clear(Root.ConstantBuffers);
		Clear(Root.Params);

		Bindings = nullptr;

		Type = {};

		Sample = {};
	}

	GPUCommandList(ResourceNameId usage, GPUCommandAllocator* allocator, GPUQueue *queue, GPUCommandListPool* pool)
		: Usage(usage), CommandAllocator(allocator), Queue(queue), Pool(pool), State(), ResourcesStateTracker(this), Type()
	{
		VerifyHr(GD12Device->CreateCommandList(0, GetD12QueueType(queue->Type), *allocator->D12CommandAllocator, nullptr, IID_PPV_ARGS(D12CommandList.GetInitPtr())));

#if GPU_PROFILING
		ProfilerContext.Profiler = *Queue->Profiler;
#endif
	}

	~GPUCommandList() {
		Check(State == CL_EXECUTED);

		FreeMemory(Root.ConstantBuffers);
		FreeMemory(Root.DstDescRanges);
		FreeMemory(Root.DstDescSizes);
		FreeMemory(Root.Params);
		FreeMemory(Root.SrcDescRanges);
		FreeMemory(Root.SrcDescRangeSizes);
	}
};

class GPUCommandListPool {
public:
	// own-ptrs
	Array<GPUCommandList*>	CommandLists;
	// weak-ptrs
	Array<GPUCommandList*>	FreeCommandLists;
	GPUQueue*				Queue;
	ResourceNameId			Usage;

	GPUCommandListPool(GPUQueue* queue, ResourceNameId usage) :
		Queue(queue),
		Usage(usage)
	{

	}

	~GPUCommandListPool() {
		Check(Size(FreeCommandLists) == Size(CommandLists));

		for (auto ptr : CommandLists) {
			_delete(ptr);
		}

		FreeMemory(CommandLists);
		FreeMemory(FreeCommandLists);
	}

	// returns command list, in recording state
	GPUCommandList* Get(GPUCommandAllocator* allocator) {
		if (Size(FreeCommandLists)) {
			auto free = Back(FreeCommandLists);
			PopBack(FreeCommandLists);

			Check(free->State == CL_EXECUTED);

			free->CommandAllocator = allocator;
			free->D12CommandList->Reset(*allocator->D12CommandAllocator, nullptr);

			return free;
		}
		
		GPUCommandList* list;
		_new(list, Usage, allocator, Queue, this);
		PushBack(CommandLists, list);

		return list;
	}

	// expects closed and executed list
	void Return(GPUCommandList* list) {
		Check(list->Pool == this);

		PushBack(FreeCommandLists, list);
	}
};

GPUQueue::GPUQueue(TextId name, GPUQueueTypeEnum type, u32 adapterIndex) :
	Type(type),
	AdapterIndex(adapterIndex),
	FenceValue(1),
	LastSignaledValue(0),
	LastSignaledFence()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc;
	ZeroMemory(&queueDesc, sizeof(queueDesc));
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	queueDesc.Type = GetD12QueueType(Type);

	VerifyHr(GD12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(D12CommandQueue.GetInitPtr())));
	VerifyHr(GD12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(D12Fence.GetInitPtr())));
	VerifyHr(GD12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(D12SharedFence.GetInitPtr())));

	SetDebugName(*D12CommandQueue, "CommandQueue");

	SyncEvent = CreateEvent();

	DebugName[0] = 0;
	Verify(strncat_s(DebugName, GetString(name), _TRUNCATE) == 0);

#if GPU_PROFILING
	if (Type != COPY_QUEUE) {
		decltype(*Profiler) pprofiler;
		_new(pprofiler);
		Profiler.Reset(pprofiler, GetMallocAllocator());
		Profiler->AttachToQueue(this);
	}
#endif
}

GPUQueue::~GPUQueue() {
	WaitForCompletion(this);

	for (auto kv : CommandListPools) {
		_delete(kv.value);
	}
	for (auto kv : CommandAllocatorPools) {
		_delete(kv.value);
	}

	FreeMemory(CommandListPools);
	FreeMemory(CommandAllocatorPools);

	DestroyEvent(SyncEvent);
}

void GPUQueue::EndFrame() {
	for (auto allocatorsPool : CommandAllocatorPools) {
		allocatorsPool.value->RecycleProcessed();
	}

#if GPU_PROFILING
	if (Type != COPY_QUEUE) {
		Profiler->ReadbackAndFeedProfiler();

		auto cl = GetCommandList(this, NAME_("patchup"));
		Profiler->ResolveFrameProfilingQueries(cl);
		Execute(cl);
	}
#endif
}

ID3D12CommandQueue* GetD12Queue(GPUQueue* queue) {
	return *queue->D12CommandQueue;
}

GPUCommandList* GetCommandList(GPUQueue* queue, ResourceNameId usage) {
	auto ppCommandListPool = Get(queue->CommandListPools, usage);
	auto commandListPool = ppCommandListPool ? *ppCommandListPool : nullptr;
	if (!commandListPool) {
		_new(commandListPool, queue, usage);
		Set(queue->CommandListPools, usage, commandListPool);
	}
	auto ppCommandAllocatorPool = Get(queue->CommandAllocatorPools, usage);
	auto commandAllocatorPool = ppCommandAllocatorPool ? *ppCommandAllocatorPool : nullptr;
	if (!commandAllocatorPool) {
		_new(commandAllocatorPool, queue, usage);
		Set(queue->CommandAllocatorPools, usage, commandAllocatorPool);
	}

	auto allocator = commandAllocatorPool->Get();
	auto commandList = commandListPool->Get(allocator);

	commandList->Fence = CreateFence(queue);
	commandList->State = CL_RECORDING;

	PushBack(allocator->Fences, commandList->Fence);

	commandList->ResetState();

	return commandList;
}

void GPUProfiler::AttachToQueue(GPUQueue* queue) {
	Queue = queue;

	VerifyHr(Queue->D12CommandQueue->GetTimestampFrequency(&QueueFrequency));
	VerifyHr(Queue->D12CommandQueue->GetClockCalibration(&QueueClockGpuCtr, &QueueClockCpuCalibratedCtr));
}

void GPUProfiler::GatherListSamples(Ringbuffer<sample_internal> *rb) {
	while (Size(*rb)) {
		PushBack(Samples, Front(*rb));
		PopFront(*rb);
	}
}

void GPUProfiler::ResolveFrameProfilingQueries(GPUCommandList* list) {
	ScopeLock lock(&CS);

	sample_internal guard;
	guard.timestamp_index_begin = 0xFFFFFFFF;
	guard.timestamp_index_end = 0xFFFFFFFF;

	PushBack(Samples, guard);

	auto index0 = (u32)(QueryResolveIndex % MaxTimestamps);
	auto index1 = (u32)(QueryIssueIndex % MaxTimestamps);

	if (index0 == index1) {
		return;
	}

	if (index1 > index0) {
		u32 size = (u32)(index1 - index0) * sizeof(u64);

		list->D12CommandList->ResolveQueryData(
			*QueryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
			index0, index1 - index0, GetResourceInfo(Readback[WriteIndex])->resource, 0);

		QueryResolveIndex += (index1 - index0);
	}
	else {
		u32 size = (u32)((MaxTimestamps - index0) + index1) * sizeof(u64);

		list->D12CommandList->ResolveQueryData(
			*QueryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
			index0, (u32)(MaxTimestamps - index0), GetResourceInfo(Readback[WriteIndex])->resource, 0);
		list->D12CommandList->ResolveQueryData(
			*QueryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
			0, index1, GetResourceInfo(Readback[WriteIndex])->resource, sizeof(u64)*(MaxTimestamps - index0));

		QueryResolveIndex += (u32)(MaxTimestamps - index0) + index1;
	}

	query_readback_frame_fence fence;
	fence.index_from = index0;
	fence.num = QueryIssueIndex - QueryResolveIndex;
	PushBack(Fences, fence);

	ReadFences[WriteIndex] = GetFence(list);
	WriteIndex = (WriteIndex + 1) % MaxQueuedFrames;
}

void GPUProfiler::ReadbackAndFeedProfiler() {
	if (WriteIndex == ReadIndex && !IsFenceCompleted(ReadFences[ReadIndex])) {
		WaitForCompletion(ReadFences[ReadIndex]);
	}

	while (IsFenceCompleted(ReadFences[ReadIndex]) && Size(Fences)) {
		auto fence = Front(Fences);
		PopFront(Fences);

		void* mappedPtr;
		auto readbackBuffer = GetResourceInfo(Readback[ReadIndex])->resource;
		D3D12_RANGE mappedRange;
		mappedRange.Begin = 0;
		mappedRange.End = sizeof(u64) * fence.num;
		VerifyHr(readbackBuffer->Map(0, &mappedRange, &mappedPtr));

		u64* timestamps = (u64*)mappedPtr;

		struct timing_t {
			cstr	label;
			u32		name_hash;
			u64		start_clocks;
			u64		end_clocks;
		};

		u64 CPUFrequency = rmt_GetCPUFrequency();
		double usScaling = 1000000.0 / CPUFrequency;

		while (Size(Samples)) {
			auto gpu_sample = Front(Samples);
			PopFront(Samples);

			if (gpu_sample.timestamp_index_begin == 0xFFFFFFFF && gpu_sample.timestamp_index_end == 0xFFFFFFFF) {
				// guard block, break loop
				break;
			}

			// read from mapped
			if (gpu_sample.timestamp_index_begin != 0xFFFFFFFF) {
				u64 gpu_begin_ctr = timestamps[gpu_sample.timestamp_index_begin - fence.index_from];
				u64 start_clocks = (gpu_begin_ctr - QueueClockGpuCtr) * CPUFrequency / QueueFrequency;
				rmt_BeginGPUSample(gpu_sample.label, gpu_sample.name_hash, (u64)(start_clocks * usScaling));
			}
			else {
				Check(gpu_sample.timestamp_index_end != 0xFFFFFFFF);
				u64 gpu_end_ctr = timestamps[gpu_sample.timestamp_index_end - fence.index_from];
				u64 end_clocks = (gpu_end_ctr - QueueClockGpuCtr) * CPUFrequency / QueueFrequency;
				rmt_EndGPUSample((u64)(end_clocks * usScaling), Queue->DebugName);
			}
		}

		D3D12_RANGE writtenRange;
		writtenRange.Begin = 1;
		writtenRange.End = 0;
		readbackBuffer->Unmap(0, &writtenRange);

		ReadIndex = (ReadIndex + 1) % MaxQueuedFrames;
	}
}

void GPUProfilerContext::Begin(gpu_sample *sample) {
	u64 index = Profiler->QueryIssueIndex++;
	rmt_PrepareGPUSample(sample->label, sample->rmt_name_hash);
	sample->timestamp_index_begin = (u32)(index % Profiler->MaxTimestamps);
	sample->timestamp_index_end = -1;

	sample->cl->D12CommandList->EndQuery(*Profiler->QueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, sample->timestamp_index_begin % Profiler->MaxTimestamps);

	sample_internal stored;
	stored.label = sample->label;
	stored.name_hash = *sample->rmt_name_hash;
	stored.timestamp_index_begin = sample->timestamp_index_begin;
	stored.timestamp_index_end = 0xFFFFFFFF;

	PushBack(Samples, stored);
}

void GPUProfilerContext::End(gpu_sample *sample) {
	u64 index = Profiler->QueryIssueIndex++;
	sample->timestamp_index_end = (u32)(index % Profiler->MaxTimestamps);

	sample->cl->D12CommandList->EndQuery(*Profiler->QueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, sample->timestamp_index_end % Profiler->MaxTimestamps);

	sample_internal stored;
	stored.label = sample->label;
	stored.name_hash = *sample->rmt_name_hash;
	stored.timestamp_index_begin = 0xFFFFFFFF;
	stored.timestamp_index_end = sample->timestamp_index_end;

	PushBack(Samples, stored);
}

GPUFenceHandle GetFence(GPUCommandList* list) {
	Check(list->State == CL_RECORDING || list->State == CL_CLOSED);
	return list->Fence;
}

GPUFenceHandle GetFence(GPUQueue* queue) {
	return queue->LastSignaledFence;
}

void ResetRootBindingMappings(GPUCommandList* list);

void Close(GPUCommandList* list) {
	Check(list->State == CL_RECORDING);
	list->State = CL_CLOSED;

	list->ResourcesStateTracker.FireBarriers();

	ResetRootBindingMappings(list);
	
	VerifyHr(list->D12CommandList->Close());
}

const u32 RESOURCE_STATE_UNKNOWN = 0xFFFFFFFF;

void Execute(GPUCommandList* list) {
#if GPU_PROFILING
	Check(list->Sample.cl == nullptr);
	list->Queue->Profiler->GatherListSamples(&list->ProfilerContext.Samples);
	Check(Size(list->ProfilerContext.Samples) == 0);
#endif

	if (list->State == CL_RECORDING) {
		Close(list);
	}
	Check(list->State == CL_CLOSED);

	// fix resources states!
	Check(Size(list->ResourcesStateTracker.QueuedBarriers) == 0);

	Array<D3D12_RESOURCE_BARRIER> patchupBarriers(GetThreadScratchAllocator());

	auto EnqueueBarrier = [&](ID3D12Resource* resource, u32 subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = subresource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		PushBack(patchupBarriers, barrier);
	};


	for (auto kv : list->ResourcesStateTracker.ExpectedState) {
		if (kv.value.per_subresource_tracking == 0) {
			auto expected = (D3D12_RESOURCE_STATES)kv.value.resource_state;
			if (GResourceState[kv.key].per_subresource_tracking == 0) {
				// whole to whole
				auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
				auto expected = (D3D12_RESOURCE_STATES)kv.value.resource_state;

				if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key)->heap_type, before, expected)) {
					auto after = GetNextState(list->Queue->Type, before, expected);
					EnqueueBarrier(GetResourceFast(kv.key)->resource, -1, before, after);
					GResourceState[kv.key].resource_state = (u32)after;
				}
			}
			else {
				// subres to whole
				auto subresNum = GetResourceInfo(kv.key)->subresources_num;
				resource_slice_t query = {};
				query.handle = kv.key;
				for (auto subresIndex : MakeRange(subresNum)) {
					query.subresource = subresIndex + 1;

					auto pSubresState = Get(GSubresourceState, query);
					auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
					if (pSubresState) {
						before = *pSubresState;
						Remove(GSubresourceState, query);
					}

					if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key)->heap_type, before, expected, true)) {
						EnqueueBarrier(GetResourceFast(kv.key)->resource, subresIndex, before, expected);
					}
				}

				GResourceState[kv.key].per_subresource_tracking = 0;
				GResourceState[kv.key].resource_state = (u32)expected;
			}

		}
		else if (kv.value.per_subresource_tracking == 1) {
			auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
			auto subresNum = GetResourceInfo(kv.key)->subresources_num;
			if (GResourceState[kv.key].per_subresource_tracking == 0) {
				// whole to subres
				auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;

				bool change = false;

				auto subresNum = GetResourceInfo(kv.key)->subresources_num;
				resource_slice_t query = {};
				query.handle = kv.key;

				for (auto subresIndex : MakeRange(subresNum)) {
					query.subresource = subresIndex + 1;
					auto pExpectedState = Get(list->ResourcesStateTracker.ExpectedSubresourceState, query);

					if (kv.value.resource_state == RESOURCE_STATE_UNKNOWN && pExpectedState == nullptr) {
						continue;
					}

					auto expected = (D3D12_RESOURCE_STATES)kv.value.resource_state;
					if (pExpectedState) {
						expected = (D3D12_RESOURCE_STATES)*pExpectedState;
					}

					if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key)->heap_type, before, expected)) {
						auto after = GetNextState(list->Queue->Type, before, expected);
						EnqueueBarrier(GetResourceFast(kv.key)->resource, subresIndex, before, after);
						GSubresourceState[query] = after;
						change = true;
					}
				}

				if (change) {
					GResourceState[kv.key].per_subresource_tracking = 1;
				}
			}
			else {
				// subres to subres

				auto subresNum = GetResourceInfo(kv.key)->subresources_num;
				resource_slice_t query = {};
				query.handle = kv.key;
				for (auto subresIndex : MakeRange(subresNum)) {
					query.subresource = subresIndex + 1;

					auto pSubresState = Get(GSubresourceState, query);
					auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
					if (pSubresState) {
						before = *pSubresState;
						Remove(GSubresourceState, query);
					}
					auto pExpectedState = Get(list->ResourcesStateTracker.ExpectedSubresourceState, query);

					if (kv.value.resource_state == RESOURCE_STATE_UNKNOWN && pExpectedState == nullptr) {
						continue;
					}

					auto expected = (D3D12_RESOURCE_STATES)kv.value.resource_state;
					if (pExpectedState) {
						expected = (D3D12_RESOURCE_STATES)*pExpectedState;
					}
					Check((u32)expected != RESOURCE_STATE_UNKNOWN);

					if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key)->heap_type, before, expected)) {
						auto after = GetNextState(list->Queue->Type, before, expected);
						EnqueueBarrier(GetResourceFast(kv.key)->resource, subresIndex, before, after);
						GSubresourceState[query] = after;
					}
				}
			}
		}
	}

	//for (auto kv : list->ResourcesStateTracker.ExpectedSubresourceState) {
	//	if (GResourceState[kv.key.handle].per_subresource_tracking == 0) {
	//		auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key.handle].resource_state;
	//		auto expected = (D3D12_RESOURCE_STATES)kv.value;
	//		Check(kv.key.subresource > 0);

	//		if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key.handle)->heap_type, before, expected)) {
	//			auto after = GetNextState(list->Queue->Type, before, expected);

	//			D3D12_RESOURCE_BARRIER barrier;
	//			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//			barrier.Transition.pResource = GetResourceFast(kv.key.handle)->resource;
	//			barrier.Transition.Subresource = kv.key.subresource - 1;
	//			barrier.Transition.StateBefore = before;
	//			barrier.Transition.StateAfter = after;
	//			PushBack(patchupBarriers, barrier);

	//			GSubresourceState[kv.key] = after;
	//			// todo: defer
	//			GResourceState[kv.key.handle].per_subresource_tracking = 1;
	//			Check(0);
	//		}
	//	}
	//	else {
	//		auto pSubresState = Get(GSubresourceState, kv.key);

	//		auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key.handle].resource_state;
	//		if (pSubresState) {
	//			before = *pSubresState;
	//		}
	//		auto expected = (D3D12_RESOURCE_STATES)kv.value;

	//		if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key.handle)->heap_type, before, expected)) {
	//			auto after = GetNextState(list->Queue->Type, before, expected);

	//			D3D12_RESOURCE_BARRIER barrier;
	//			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//			barrier.Transition.pResource = GetResourceFast(kv.key.handle)->resource;
	//			barrier.Transition.Subresource = kv.key.subresource - 1;
	//			barrier.Transition.StateBefore = before;
	//			barrier.Transition.StateAfter = after;
	//			PushBack(patchupBarriers, barrier);

	//			GSubresourceState[kv.key] = after;
	//		}
	//	}
	//}

	//for (auto kv : list->ResourcesStateTracker.ExpectedState) {
	//	auto key = kv.key;

	//	if (kv.value.per_subresource_tracking == 0 && GResourceState[kv.key].per_subresource_tracking == 0) {
	//		Check(kv.value.resource_state != RESOURCE_STATE_UNKNOWN);
	//		auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
	//		auto expected = (D3D12_RESOURCE_STATES)kv.value.resource_state;

	//		if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(key)->heap_type, before, expected)) {
	//			auto after = GetNextState(list->Queue->Type, before, expected);

	//			D3D12_RESOURCE_BARRIER barrier;
	//			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//			barrier.Transition.pResource = GetResourceFast(kv.key)->resource;
	//			barrier.Transition.Subresource = -1;
	//			barrier.Transition.StateBefore = before;
	//			barrier.Transition.StateAfter = after;
	//			PushBack(patchupBarriers, barrier);

	//			GResourceState[kv.key].resource_state = after;
	//		}
	//	}
	//	else if(kv.value.per_subresource_tracking == 0) {
	//		Check(kv.value.resource_state != RESOURCE_STATE_UNKNOWN);
	//		Check(GResourceState[kv.key].per_subresource_tracking != 0);

	//		auto subresNum = GetResourceInfo(kv.key)->subresources_num;
	//		resource_slice_t query = {};
	//		query.handle = kv.key;
	//		for (auto subresIndex : MakeRange(subresNum)) {
	//			query.subresource = subresIndex + 1;

	//			auto pSubresState = Get(GSubresourceState, query);
	//			if (pSubresState && NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(key)->heap_type, *pSubresState, (D3D12_RESOURCE_STATES)kv.value.resource_state)) {
	//				auto before = *pSubresState;
	//				auto after = GetNextState(list->Queue->Type, before, (D3D12_RESOURCE_STATES)kv.value.resource_state);

	//				D3D12_RESOURCE_BARRIER barrier;
	//				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//				barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//				barrier.Transition.pResource = GetResourceFast(kv.key)->resource;
	//				barrier.Transition.Subresource = subresIndex;
	//				barrier.Transition.StateBefore = before;
	//				barrier.Transition.StateAfter = after;
	//				PushBack(patchupBarriers, barrier);
	//			}
	//			if (pSubresState) {
	//				Remove(GSubresourceState, query);
	//			}
	//		}

	//		auto before = (D3D12_RESOURCE_STATES)kv.value.resource_state;
	//		if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(key)->heap_type, before, (D3D12_RESOURCE_STATES)kv.value.resource_state)) {
	//			auto after = GetNextState(list->Queue->Type, before, (D3D12_RESOURCE_STATES)kv.value.resource_state);

	//			D3D12_RESOURCE_BARRIER barrier;
	//			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//			barrier.Transition.pResource = GetResourceFast(kv.key)->resource;
	//			barrier.Transition.Subresource = -1;
	//			barrier.Transition.StateBefore = before;
	//			barrier.Transition.StateAfter = after;
	//			PushBack(patchupBarriers, barrier);

	//			GResourceState[kv.key].resource_state = after;
	//		}

	//		GResourceState[kv.key].per_subresource_tracking = 0;
	//	}
	//	else {
	//		Check(kv.value.per_subresource_tracking == 1);

	//		// subresources were already transitioned
	//		auto before = (D3D12_RESOURCE_STATES)GResourceState[kv.key].resource_state;
	//		if (kv.value.resource_state != RESOURCE_STATE_UNKNOWN && NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(key)->heap_type, before, (D3D12_RESOURCE_STATES)kv.value.resource_state)) {
	//			auto after = GetNextState(list->Queue->Type, before, (D3D12_RESOURCE_STATES)kv.value.resource_state);

	//			Check(after == (D3D12_RESOURCE_STATES)kv.value.resource_state);

	//			D3D12_RESOURCE_BARRIER barrier;
	//			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//			barrier.Transition.pResource = GetResourceFast(kv.key)->resource;
	//			barrier.Transition.Subresource = -1;
	//			barrier.Transition.StateBefore = before;
	//			barrier.Transition.StateAfter = after;
	//			PushBack(patchupBarriers, barrier);

	//			GResourceState[kv.key].resource_state = after;
	//			GResourceState[kv.key].per_subresource_tracking = 0;
	//		}
	//	}
	//}

	// A: we expect whole res in state Y, whole rs in state X
	// B: we expect whole res in Y, some subres in X
	// C: we expect subres in Y, whole res in X
	// D: we expect subres in Y, subres in X
/*
	for (auto kv : list->ResourcesStateTracker.ExpectedState) {
		auto key = kv.key;

		const auto pCurrent = Get(GResourceState, kv.key);
		if (kv.value.per_subresource_tracking == 0) {
			current.per_subresource_tracking
		}

		auto state = GResourceState[kv.key];
		auto expectedState = kv.value;

		if (state.state == D3D12_RESOURCE_STATE_RENDER_TARGET && list->Queue->Type == COPY_QUEUE) {
			state.state = D3D12_RESOURCE_STATE_COMMON;
		}

		auto before = state.state;
		auto after = expectedState.state;

		if (NeedStateChange(list->Queue->Type, GetResourceTransitionInfo(kv.key.handle)->heap_type, before, after)) {
			D3D12_RESOURCE_BARRIER barrier;
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = GetResourceFast(kv.key.handle)->resource;
			barrier.Transition.Subresource = key.subresource - 1;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = GetNextState(list->Queue->Type , before, after);

			PushBack(patchupBarriers, barrier);
		}
	}

	for (auto kv : list->ResourcesStateTracker.ResourceState) {
		Set(GResourceStates, kv.key, kv.value);
	}*/

	Array<ID3D12CommandList*> executionList(GetThreadScratchAllocator());

	GPUCommandList* patchupList = nullptr;
	if (Size(patchupBarriers)) {
		patchupList = GetCommandList(list->Queue, NAME_("Glue"));
		patchupList->D12CommandList->ResourceBarrier((u32)Size(patchupBarriers), patchupBarriers.DataPtr);
		patchupList->D12CommandList->Close();
		patchupList->State = CL_CLOSED;

		PushBack(executionList, (ID3D12CommandList*)*patchupList->D12CommandList);
	}

	PushBack(executionList, (ID3D12CommandList*) *list->D12CommandList);
	auto queue = list->Queue;
	auto signalValue = queue->FenceValue;

	queue->D12CommandQueue->ExecuteCommandLists((u32)Size(executionList), executionList.DataPtr);
	queue->AdvanceFence();

	queue->LastSignaledFence = list->Fence;
	
	Fences[list->Fence.handle].value = signalValue;

	list->State = CL_EXECUTED;
	list->Fence = {};

	list->CommandAllocator->Pool->Return(list->CommandAllocator, queue->LastSignaledFence);
	list->CommandAllocator = nullptr;

	list->Pool->Return(list);
	list->ResourcesStateTracker.Clear();

	if (patchupList) {
		Fences[patchupList->Fence.handle].value = signalValue;

		patchupList->State = CL_EXECUTED;
		patchupList->Fence = {};

		patchupList->CommandAllocator->Pool->Return(patchupList->CommandAllocator, queue->LastSignaledFence);
		patchupList->CommandAllocator = nullptr;

		patchupList->Pool->Return(patchupList);
		Check(Size(patchupList->ResourcesStateTracker.ExpectedState) == 0);
		Check(Size(patchupList->ResourcesStateTracker.ExpectedSubresourceState) == 0);
	}
}

GPUQueue*		CreateQueue(TextId name, GPUQueueTypeEnum type, u32 adapterIndex) {
	GPUQueue* queue;
	_new(queue, name, type, adapterIndex);
	PushBack(GPUQueues, queue);
	return queue;
}

void EndCommandsFrame(GPUQueue* mainQueue, u32 limitGpuBufferedFrames) {
	// fence read with last fence from queue(!)
	GpuDescriptorsAllocator.FenceTemporaryAllocations(GetLastSignaledFence(mainQueue));
	ConstantsAllocator.FenceTemporaryAllocations(GetLastSignaledFence(mainQueue));


	auto frameFence = GetLastSignaledFence(mainQueue);

	PushBack(FrameFences, frameFence);
	while (Size(FrameFences) && IsFenceCompleted(Front(FrameFences))) {
		PopFront(FrameFences);
	}

	while (Size(FrameFences) > limitGpuBufferedFrames) {
		WaitForCompletion(Front(FrameFences));
		PopFront(FrameFences);
	}

	for (auto queue : GPUQueues) {
		queue->EndFrame();
	}

	GpuDescriptorsAllocator.FreeTemporaryAllocations();
	ConstantsAllocator.FreeTemporaryAllocations();
}

void WaitForCompletion(GPUQueue* queue, u64 fenceValue) {
	if (queue->D12Fence->GetCompletedValue() < fenceValue) {
		VerifyHr(queue->D12Fence->SetEventOnCompletion(fenceValue, queue->SyncEvent));
		WaitForSingleObject(queue->SyncEvent, INFINITE);
	}
}

void WaitForCompletion(GPUQueue* queue, u64 fenceValue);

void WaitForCompletion(GPUQueue* queue) {
	WaitForCompletion(queue, queue->LastSignaledValue);
}

void WaitForCompletion() {
	for (auto queue : GPUQueues) {
		WaitForCompletion(queue);
	}
}

ResourceTracker::ResourceTracker(GPUCommandList* list) : Owner(list) {
}

ResourceTracker::~ResourceTracker() {
	FreeMemory(CurrentState);
	FreeMemory(CurrentSubresourceState);
	FreeMemory(ExpectedState);
	FreeMemory(ExpectedSubresourceState);
	FreeMemory(QueuedBarriers);
}

void ResourceTracker::EnqueueTransition(ID3D12Resource* resource, u32 subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = subresource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	PushBack(QueuedBarriers, barrier);
}

// state exists => expected state exists
// state is tracked per subresource => expected might tracked per subresource
// state is tracked per subresource but no entry for subresource => inherit from resource (subres has precedence over resource)

void ResourceTracker::Transition(resource_slice_t slice, D3D12_RESOURCE_STATES desired) {
	if (slice.subresource == 0) {
		const auto pState = Get(CurrentState, slice.handle);
		if (pState && pState->per_subresource_tracking == 0) {
			Check(pState->resource_state != RESOURCE_STATE_UNKNOWN);
			auto before = (D3D12_RESOURCE_STATES)pState->resource_state;
			if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, desired)) {
				auto after = GetNextState(Owner->Queue->Type, before, desired);
				EnqueueTransition(GetResourceFast(slice.handle)->resource, slice.subresource - 1, before, after);
				CurrentState[slice.handle].resource_state = after;
			}
		}
		else if(!pState) {
			ExpectedState[slice.handle].resource_state = (u32)desired;
			ExpectedState[slice.handle].per_subresource_tracking = 0;

			CurrentState[slice.handle].resource_state = (u32)desired;
			CurrentState[slice.handle].per_subresource_tracking = 0;
		}
		else {
			Check(pState && pState->per_subresource_tracking == 1);

			auto subresNum = GetResourceInfo(slice.handle)->subresources_num;
			auto query = slice;

			auto after = pState->resource_state != RESOURCE_STATE_UNKNOWN 
				? GetNextState(Owner->Queue->Type, (D3D12_RESOURCE_STATES)pState->resource_state, desired)
				: desired;

			for (auto subresIndex : MakeRange(subresNum)) {
				query.subresource = subresIndex + 1;
				const auto pSubresState = Get(CurrentSubresourceState, query);
				if (pSubresState) {
					auto before = (D3D12_RESOURCE_STATES)*pSubresState;
					if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, after)) {
						EnqueueTransition(GetResourceFast(slice.handle)->resource, subresIndex, before, after);
					}
					// we go to one state per res, drop rest
					Remove(CurrentSubresourceState, query);
				}
				else if(pState->resource_state != RESOURCE_STATE_UNKNOWN){
					auto before = (D3D12_RESOURCE_STATES)pState->resource_state;
					if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, after, true)) {
						EnqueueTransition(GetResourceFast(slice.handle)->resource, subresIndex, before, after);
					}
				}
			}

			if (pState->resource_state != RESOURCE_STATE_UNKNOWN && NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, (D3D12_RESOURCE_STATES)pState->resource_state, after, true)) {
				//EnqueueTransition(GetResourceFast(slice.handle)->resource, -1, (D3D12_RESOURCE_STATES)pState->resource_state, after);
			}
			else {
				// expect resource to be in after state
				ExpectedState[slice.handle].resource_state = (u32)after;
			}
			CurrentState[slice.handle].resource_state = after;
			CurrentState[slice.handle].per_subresource_tracking = 0;
		}
	}
	else {
		Check(slice.subresource != 0);

		const auto pResState = Get(CurrentState, slice.handle);
		const auto pSubresState = Get(CurrentSubresourceState, slice);
		if (pSubresState) {
			Check(*pSubresState != RESOURCE_STATE_UNKNOWN);

			auto before = (D3D12_RESOURCE_STATES)*pSubresState;
			if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, desired)) {
				auto after = GetNextState(Owner->Queue->Type, before, desired);
				EnqueueTransition(GetResourceFast(slice.handle)->resource, slice.subresource - 1, before, after);
				Set(CurrentSubresourceState, slice, (u32)after);
			}
		}
		else if (pResState && pResState->per_subresource_tracking == 1) {
			if (pResState->resource_state == RESOURCE_STATE_UNKNOWN) {
				Set(ExpectedSubresourceState, slice, (u32)desired);
				Set(CurrentSubresourceState, slice, (u32)desired);
			}
			else {
				Check(pResState->resource_state != RESOURCE_STATE_UNKNOWN);
				auto before = (D3D12_RESOURCE_STATES)pResState->resource_state;
				if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, desired)) {
					auto after = GetNextState(Owner->Queue->Type, before, desired);
					EnqueueTransition(GetResourceFast(slice.handle)->resource, slice.subresource - 1, before, after);
					Set(CurrentSubresourceState, slice, (u32)after);
				}
			}
		}
		else if (pResState && pResState->per_subresource_tracking == 0 && pResState->resource_state != RESOURCE_STATE_UNKNOWN) {
			auto before = (D3D12_RESOURCE_STATES)pResState->resource_state;
			if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(slice.handle)->heap_type, before, desired)) {
				auto after = GetNextState(Owner->Queue->Type, before, desired);
				EnqueueTransition(GetResourceFast(slice.handle)->resource, slice.subresource - 1, before, after);
				Set(CurrentSubresourceState, slice, (u32) after);
				Get(CurrentState, slice.handle)->per_subresource_tracking = 1;
			}
		}
		else if (!pResState) {
			Set(CurrentSubresourceState, slice, (u32)desired);
			Set(ExpectedSubresourceState, slice, (u32)desired);
			resource_tracking_state tracking = {};
			tracking.per_subresource_tracking = 1;
			tracking.resource_state = RESOURCE_STATE_UNKNOWN;
			Set(CurrentState, slice.handle, tracking);
			Set(ExpectedState, slice.handle, tracking);
		}
		else {
			Check(0);
		}
	}
}

void ResourceTracker::FireBarriers() {
	if (Size(QueuedBarriers)) {
		Owner->D12CommandList->ResourceBarrier((u32)Size(QueuedBarriers), QueuedBarriers.DataPtr);
		Essence::Clear(QueuedBarriers);
	}
}

void	 RegisterResource(resource_handle resource, D3D12_RESOURCE_STATES initialState) {
	GResourceState[resource].resource_state = (u32)initialState;
	GResourceState[resource].per_subresource_tracking = 0;
}

void CopyResource(GPUCommandList* list, resource_handle dst, resource_handle src) {
	list->ResourcesStateTracker.Transition(Slice(dst), D3D12_RESOURCE_STATE_COPY_DEST);
	list->ResourcesStateTracker.Transition(Slice(src), D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->ResourcesStateTracker.FireBarriers();
	list->D12CommandList->CopyResource(GetResourceFast(dst)->resource, GetResourceFast(src)->resource);
}

void GPUBeginProfiling(GPUCommandList* list, cstr label, u32* rmt_name_hash) {
#if GPU_PROFILING
	Check(list->Sample.cl == nullptr);
	list->Sample.label = label;
	list->Sample.rmt_name_hash = rmt_name_hash;
	list->Sample.cl = list;
	list->ProfilerContext.Begin(&list->Sample);
#endif
}

void GPUEndProfiling(GPUCommandList* list) {
#if GPU_PROFILING
	Check(list->Sample.cl);
	list->ProfilerContext.End(&list->Sample);
	list->Sample = {};
#endif
}

void ClearRenderTarget(GPUCommandList* list, resource_rtv_t rtv, float4 color) {
	list->ResourcesStateTracker.Transition(rtv.slice, D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourcesStateTracker.FireBarriers();
	float c[4];
	c[0] = color.x;
	c[1] = color.y;
	c[2] = color.z;
	c[3] = color.w;
	list->D12CommandList->ClearRenderTargetView(rtv.cpu_descriptor, c, 0, nullptr);
}

void ClearUnorderedAccess(GPUCommandList* list, resource_uav_t uav) {
	list->ResourcesStateTracker.Transition(uav.slice, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	list->ResourcesStateTracker.FireBarriers();
	u32 values[4] = { 0, 0, 0, 0 };
	auto viewAlloc = GpuDescriptorsAllocator.AllocateTemporary(1);
	GD12Device->CopyDescriptorsSimple(1, GetCPUHandle(viewAlloc), uav.cpu_descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	list->D12CommandList->ClearUnorderedAccessViewUint(GetGPUHandle(viewAlloc), uav.cpu_descriptor, GetResourceFast(uav.slice.handle)->resource, values, 0, nullptr);
}

void ClearDepthStencil(GPUCommandList* list, resource_dsv_t dsv, ClearDepthFlagEnum flags, float depth, u8 stencil, u32 numRects, D3D12_RECT* rects) {
	list->ResourcesStateTracker.Transition(dsv.slice, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	list->ResourcesStateTracker.FireBarriers();

	auto access = DSV_WRITE_ALL;
	access = (flags == CLEAR_STENCIL) ? DSV_READ_ONLY_DEPTH : access;
	access = (flags == CLEAR_DEPTH) ? DSV_READ_ONLY_STENCIL : access;
	list->D12CommandList->ClearDepthStencilView(dsv.cpu_descriptor, (D3D12_CLEAR_FLAGS)flags, depth, stencil, numRects, rects);
}

void QueueWait(GPUQueue* queue, GPUFenceHandle handle) {
	if (FenceGenerations[handle.handle] != handle.generation) {
		Check(IsFenceCompleted(handle));
		return;
	}

	Check(Fences[handle.handle].value);

	// any point in seprating?
	if (queue == Fences[handle.handle].queue) {
		VerifyHr(queue->D12CommandQueue->Wait(*queue->D12Fence, Fences[handle.handle].value));
	}
	else {
		VerifyHr(queue->D12CommandQueue->Wait(*Fences[handle.handle].queue->D12SharedFence, Fences[handle.handle].value));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////

struct root_signature_t {
	u64						hash;
	ID3D12RootSignature*	ptr;
};

Hashmap<u64, root_signature_t> RootSignatures;

root_signature_t GetRootSignature(D3D12_ROOT_SIGNATURE_DESC const& desc,
	Array<D3D12_DESCRIPTOR_RANGE> const* ranges,
	Array<D3D12_ROOT_PARAMETER> const* params
	) {
	Array<D3D12_STATIC_SAMPLER_DESC> samplers(GetThreadScratchAllocator());
	//D3D12_STATIC_SAMPLER_DESC sampler = {};
	//sampler.Filter = 


	// hash only content, not pointers!
	u64 rootHash = 0;
	rootHash = Hash::MurmurHash2_64(ranges->DataPtr, (i32)(ranges->Size * sizeof(ranges->DataPtr[0])), rootHash);
	rootHash = Hash::MurmurHash2_64(params->DataPtr, (i32)(params->Size * sizeof(params->DataPtr[0])), rootHash);
	rootHash = Hash::MurmurHash2_64(&desc.Flags, (i32)sizeof(desc.Flags), rootHash);
	if (desc.pStaticSamplers) {
		rootHash = Hash::MurmurHash2_64(desc.pStaticSamplers, (i32)(desc.NumStaticSamplers * sizeof(desc.pStaticSamplers[0])), rootHash);
	}

	root_signature_t out = {};
	out.hash = rootHash;

	if (Contains(RootSignatures, rootHash)) {
		out.ptr = RootSignatures[rootHash].ptr;
		return out;
	}

	ID3D12RootSignature *rootSignaturePtr;
	ID3DBlob* pOutBlob;
	ID3DBlob* pErrorBlob;
	auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
	if (pErrorBlob) {
		debugf(Format("Root signature errors: %s", (char*)pErrorBlob->GetBufferPointer()));
	}
	VerifyHr(hr);
	VerifyHr(GD12Device->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignaturePtr)));
	ComRelease(pOutBlob);
	ComRelease(pErrorBlob);

	out.ptr = rootSignaturePtr;
	RootSignatures[rootHash] = out;

	return out;
}

struct shader_bind_key_t {
	D3D12_DESCRIPTOR_RANGE_TYPE type;
	u32							reg;
	u32							space;
};

// frequency
// constant buffers: by index (higher index = higher priorty), expect globals
// textures 
// samplers last

struct shader_input_desc_t {
	u32							frequency;
	// srv, uav, cbv, sampler
	D3D12_DESCRIPTOR_RANGE_TYPE type;
	u32							reg;
	u32							space;
	D3D12_SHADER_VISIBILITY		visibility;
	TextId						name;
	u64							hash;
	// 
	u64							paramHashId;
	u32							paramTableSlot;

	inline bool operator < (shader_input_desc_t const& rhs) const {
		if (frequency != rhs.frequency) {
			return frequency > rhs.frequency;
		}
		if (type != rhs.type) {
			return type < rhs.type;
		}
		if (reg != rhs.reg) {
			return reg < rhs.reg;
		}
		return space < rhs.space;
	}
};

struct shader_constantbuffer_desc_t {
	u32	bytesize;
	u64 content_hash;
};

struct shader_constantvariable_desc_t {
	u32		constantbuffer_offset;
	u32		bytesize;
	TextId	constantbuffer_name;
	u64		content_hash;
};

bool GetConstantBuffersAndVariables(
	ID3D12ShaderReflection* shaderReflection,
	Hashmap<TextId, shader_constantbuffer_desc_t>	&cbDict,
	Hashmap<TextId, shader_constantvariable_desc_t>			&varsDict) {

	D3D12_SHADER_DESC shaderDesc;
	VerifyHr(shaderReflection->GetDesc(&shaderDesc));
	auto constantBuffersNum = shaderDesc.ConstantBuffers;
	for (auto i = 0u; i < constantBuffersNum;++i) {
		auto cbReflection = shaderReflection->GetConstantBufferByIndex(i);

		D3D12_SHADER_BUFFER_DESC bufferDesc;
		VerifyHr(cbReflection->GetDesc(&bufferDesc));

		// preparing hash of constant buffer with all variables inside, so we can figure out collisions!
		auto cbDictKey = TEXT_(bufferDesc.Name);
		shader_constantbuffer_desc_t cbInfo;
		cbInfo.content_hash = Hash::MurmurHash2_64(bufferDesc.Name, (u32)strlen(bufferDesc.Name), 0);
		bufferDesc.Name = nullptr;
		cbInfo.content_hash = Hash::MurmurHash2_64(&bufferDesc, (u32)sizeof(bufferDesc), cbInfo.content_hash);
		cbInfo.bytesize = bufferDesc.Size;

		//auto defaultContent = GetThreadScratchAllocator()->Allocate(cbInfo.bytesize, 8);

		if (bufferDesc.Type == D3D_CT_CBUFFER) {
			auto variablesNum = bufferDesc.Variables;

			// prepare final cb hash
			for (auto i = 0u; i < variablesNum; ++i) {
				auto variable = cbReflection->GetVariableByIndex(i);
				D3D12_SHADER_VARIABLE_DESC variableDesc;
				VerifyHr(variable->GetDesc(&variableDesc));
				// to ignore used/unused marking for particular shader
				variableDesc.uFlags &= ~0x2;
				cbInfo.content_hash = Hash::MurmurHash2_64(variableDesc.Name, (u32)strlen(variableDesc.Name), cbInfo.content_hash);
				variableDesc.Name = nullptr;
				cbInfo.content_hash = Hash::MurmurHash2_64(&variableDesc, sizeof(variableDesc), cbInfo.content_hash);
			}

			// add to dict, check if no collision
			if (!Get(cbDict, cbDictKey)) {
				cbDict[cbDictKey] = cbInfo;
			}
			else if (cbDict[cbDictKey].content_hash != cbInfo.content_hash) {
				ConsolePrint(Format("shader state has conflicting constant buffers"));
				return false;
			}

			// gather variables
			for (auto i = 0u; i < variablesNum; ++i) {
				auto variable = cbReflection->GetVariableByIndex(i);
				D3D12_SHADER_VARIABLE_DESC variableDesc;
				VerifyHr(variable->GetDesc(&variableDesc));

				auto varDictKey = TEXT_(variableDesc.Name);

				shader_constantvariable_desc_t varInfo;
				varInfo.constantbuffer_offset = variableDesc.StartOffset;
				varInfo.bytesize = variableDesc.Size;
				varInfo.constantbuffer_name = cbDictKey;

				variableDesc.Name = nullptr;
				auto defaulValue = variableDesc.DefaultValue;
				variableDesc.DefaultValue = nullptr;
				// to ignore used/unused marking for particular shader
				variableDesc.uFlags &= ~0x2;
				varInfo.content_hash = Hash::MurmurHash2_64(&variableDesc, sizeof(variableDesc), 0);

				// add to dict, check if no collision
				if (!Get(varsDict, varDictKey)) {
					varsDict[varDictKey] = varInfo;
				}
				else if (varsDict[varDictKey].content_hash != varInfo.content_hash) {
					ConsolePrint(Format("shader state has conflicting varriable"));
					return false;
				}
			}
		}
	}

	return true;
}

bool GetInputBindingSlots(
	ID3D12ShaderReflection* shaderReflection,
	Hashmap<TextId, shader_input_desc_t> &inputDict,
	D3D12_SHADER_VISIBILITY visibilityFlag
	)
{
	D3D12_SHADER_DESC shaderDesc;
	VerifyHr(shaderReflection->GetDesc(&shaderDesc));

	auto constantBuffersNum = shaderDesc.ConstantBuffers;
	auto resourcesNum = shaderDesc.BoundResources;

	for (auto i = 0u; i < resourcesNum;++i) {
		D3D12_SHADER_INPUT_BIND_DESC bindDesc;
		VerifyHr(shaderReflection->GetResourceBindingDesc(i, &bindDesc));
		// because of trash memory from shader reflection :(
		ZeroMemory(
			(u8*)&bindDesc + offsetof(D3D12_SHADER_INPUT_BIND_DESC, uID) + sizeof(bindDesc.uID),
			sizeof(bindDesc) - offsetof(D3D12_SHADER_INPUT_BIND_DESC, uID) - sizeof(bindDesc.uID));

		shader_input_desc_t bindInfo = {};
		bindInfo.reg = bindDesc.BindPoint;
		bindInfo.space = bindDesc.Space;
		bindInfo.name = TEXT_(bindDesc.Name);
		bindDesc.Name = nullptr;
		bindInfo.hash = Hash::MurmurHash2_64(&bindDesc, sizeof(bindDesc), 0);
		bindInfo.visibility = visibilityFlag;

		bindInfo.paramHashId = -1;
		bindInfo.paramTableSlot = -1;

		switch (bindDesc.Type) {
		case D3D_SIT_CBUFFER:
			{
				bindInfo.frequency = 1;
				bindInfo.type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
			}
			break;
		case D3D_SIT_SAMPLER:
			{
				bindInfo.frequency = 2;
				bindInfo.type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
			}
			break;
		case D3D_SIT_TEXTURE:
			{
				bindInfo.frequency = 0;
				bindInfo.type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			}
			break;
		case D3D_SIT_UAV_RWTYPED:
			{
				bindInfo.frequency = 0;
				bindInfo.type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			}
			break;
		}

		if (Contains(inputDict, bindInfo.name)) {
			auto &ref = inputDict[bindInfo.name];

			if (ref.hash != bindInfo.hash) {
				return false;
			}

			ref.visibility = ref.visibility == visibilityFlag ? ref.visibility : D3D12_SHADER_VISIBILITY_ALL;
		}
		else {
			inputDict[bindInfo.name] = bindInfo;
		}
	}

	return true;
}

typedef GenericHandle32<20, TYPE_ID(PipelineState)> pipeline_handle;

struct shader_binding_t {
	u32 table_slot;
	u64 root_parameter_hash;
};

struct shader_constantvariable_t {
	u32 bytesize;
	u32 byteoffset;
	u64 cb_hash_index;
};

struct graphics_pipeline_root_key {
	shader_handle VS;
	shader_handle PS;
};

struct compute_pipeline_root_key {
	shader_handle CS;
};

struct root_range_offset_t {
	u32 index;
	u32 num;
};

// [start, end)
// appends api root ranges, params desc
// maps binding to root param
// map param hash to info
void GetRootParamsForBindings(
	Array<TextId> const& bindKeys,
	Hashmap<TextId, shader_input_desc_t> &bindInputs,
	u32 start, u32 end,
	Array<D3D12_DESCRIPTOR_RANGE> &rootRangesArray,
	Array<D3D12_ROOT_PARAMETER> &rootParamsArray,
	Hashmap<u64, root_parameter_t> &RootParams,
	Array<root_range_offset_t> &offsetsArray) {

	auto rangeIndex = (u32)Size(rootRangesArray);
	auto currentTableSlot = 0;
	u32 index = start;
	u32 cbvsOffset = 0;
	u32 uavsOffset = 0;

	//right now everything ends in a table
	D3D12_ROOT_PARAMETER currentTable;
	currentTable.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	currentTable.ShaderVisibility = bindInputs.Values[index].visibility;

	// create ranges, figure out slots for bindings
	for (; index < end; ++index) {
		auto key = bindKeys[index];
		D3D12_DESCRIPTOR_RANGE range;
		range.BaseShaderRegister = bindInputs[key].reg;
		range.NumDescriptors = 1;
		range.RegisterSpace = bindInputs[key].space;
		range.RangeType = bindInputs[key].type;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		int currentReg = range.BaseShaderRegister;

		// update bind input info
		bindInputs[key].paramTableSlot = currentTableSlot;
		++currentTableSlot;

		// update table info if necessary
		currentTable.ShaderVisibility = currentTable.ShaderVisibility == bindInputs[key].visibility ?
			currentTable.ShaderVisibility : D3D12_SHADER_VISIBILITY_ALL;
		if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV && range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
			++uavsOffset;
		if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
			++cbvsOffset;

		while (index + 1 < end) {
			auto &next = bindInputs[bindKeys[index + 1]];

			bool continous =
				next.reg == currentReg &&
				next.space == range.RegisterSpace &&
				next.type == range.RangeType;
			if (continous) {
				range.NumDescriptors++;
				currentReg++;
			}
			else {
				break;
			}

			bindInputs[key].paramTableSlot = currentTableSlot;
			++currentTableSlot;

			currentTable.ShaderVisibility = currentTable.ShaderVisibility == bindInputs[key].visibility ?
				currentTable.ShaderVisibility : D3D12_SHADER_VISIBILITY_ALL;
			if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV && range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
				++uavsOffset;
			if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
				++cbvsOffset;
			++index;
		}

		PushBack(rootRangesArray, range);
	}

	root_range_offset_t offsets;
	offsets.index = rangeIndex;
	offsets.num = (u32)Size(rootRangesArray) - rangeIndex;

	auto tableHash = Hash::MurmurHash2_64(rootRangesArray.DataPtr + offsets.index, offsets.num, 0);

	// prepare param info for runtime binding
	root_parameter_t rootParamInfo;
	rootParamInfo.table.length = currentTableSlot;
	rootParamInfo.table.root_index = (u32)Size(rootParamsArray);
	Check(uavsOffset <= (u32)currentTableSlot);
	Check(cbvsOffset <= (u32)currentTableSlot);
	Check(uavsOffset <= cbvsOffset);
	rootParamInfo.table.uav_range_offset = uavsOffset;
	rootParamInfo.table.cbv_range_offset = cbvsOffset;
	RootParams[tableHash] = rootParamInfo;

	PushBack(offsetsArray, offsets);
	PushBack(rootParamsArray, currentTable);

	// fix bindings info
	for (auto i = start; i < end; ++i) {
		bindInputs[bindKeys[i]].paramHashId = tableHash;
	}
}

template<typename K, typename V>
Array<V> GetValuesScratch(Hashmap<K, V>  & Hm) {
	Array<V> A(GetThreadScratchAllocator());

	for (auto kv : Hm) {
		PushBack(A, kv.value);
	}
	return std::move(A);
}

template<typename K, typename V>
Array<K> GetKeysScratch(Hashmap<K, V>  & Hm) {
	Array<K> A(GetThreadScratchAllocator());
	
	for (auto kv : Hm) {
		PushBack(A, kv.key);
	}
	return std::move(A);
}

void DebugPrintRoot(D3D12_ROOT_SIGNATURE_DESC const& desc) {
	ConsolePrint("Root params:\n");
	for (auto i : MakeRange(desc.NumParameters)) {
		desc.pParameters[i].DescriptorTable;
		desc.pParameters[i].ShaderVisibility;

		Check(desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);

		auto visibilityStr = [](D3D12_SHADER_VISIBILITY a) {
			switch (a) {
			case D3D12_SHADER_VISIBILITY_ALL:
				return "ALL";
			case D3D12_SHADER_VISIBILITY_VERTEX:
				return "PIX";
			case D3D12_SHADER_VISIBILITY_PIXEL:
				return "VERT";
			};
			return "?";
		};

		auto rangeStr = [](D3D12_DESCRIPTOR_RANGE_TYPE a) {
			switch (a) {
			case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
				return "t";
			case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
				return "u";
			case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
				return "b";
			case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
				return "s";
			};
			return "?";
		};

		ConsolePrint(Format("[%d]: table, visibility: %s\n", i, visibilityStr(desc.pParameters[i].ShaderVisibility)));

		for (auto r : MakeRange(desc.pParameters[i].DescriptorTable.NumDescriptorRanges)) {
			auto range = desc.pParameters[i].DescriptorTable.pDescriptorRanges[r];
			ConsolePrint(Format("  [%d]: %s%d,%d+%d offset %d\n", 
				r,
				rangeStr(range.RangeType),
				range.BaseShaderRegister,
				range.RegisterSpace,
				range.NumDescriptors,
				range.OffsetInDescriptorsFromTableStart));
		}
	}
}

class PipelineStateBindings {
public:
	u32				CompilationErrors : 1;
	Hashmap<TextId, shader_binding_t>				Texture2DParams;
	Hashmap<TextId, shader_binding_t>				RWTexture2DParams;
	Hashmap<TextId, shader_constantvariable_t>		ConstantVarParams;
	Hashmap<u64, shader_constantbuffer_t>			ConstantBuffers;
	Hashmap<u64, root_parameter_t>					RootParams;

	ID3D12RootSignature* RootSignature;

protected:
	void PrepareInternal(
		Hashmap<TextId, shader_constantvariable_desc_t>& constantVariables,
		Hashmap<TextId, shader_input_desc_t>& bindInputs,
		Hashmap<TextId, shader_constantbuffer_desc_t>& constantBuffers) 
	{
		auto bindKeys = GetKeysScratch(bindInputs);
		quicksort(bindKeys.DataPtr, 0, bindKeys.Size, [&](TextId a, TextId b) -> bool { return bindInputs[a] < bindInputs[b]; });

		Array<D3D12_DESCRIPTOR_RANGE> rootRangesArray(GetThreadScratchAllocator());
		Array<D3D12_ROOT_PARAMETER> rootParamsArray(GetThreadScratchAllocator());
		// pointers may change as we expand array, we will fix them later with offsets
		Array<root_range_offset_t>	offsetsArray(GetThreadScratchAllocator());

		u32 startBindIndex = 0;
		u32 bindIndex = 0;
		while (bindIndex < Size(bindKeys))
		{
			auto tableFreq = bindInputs[bindKeys[bindIndex]].frequency;
			bool isSamplerTable = bindInputs[bindKeys[bindIndex]].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

			while (bindIndex < Size(bindKeys)
				&& bindInputs[bindKeys[bindIndex]].frequency == tableFreq
				&& isSamplerTable == (bindInputs[bindKeys[bindIndex]].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)) {

				bindIndex++;
			}

			// prepare ranges, tables from range [startBindIndex, bindIndex)
			GetRootParamsForBindings(bindKeys, bindInputs,
				startBindIndex, bindIndex,
				rootRangesArray, rootParamsArray, RootParams, offsetsArray);

			startBindIndex = bindIndex;
		}

		Hashmap<TextId, u64> constantBufferNameToHash(GetThreadScratchAllocator());

		// values changed
		auto bindArray = GetValuesScratch(bindInputs);
		for (auto i = 0u; i < Size(bindArray); ++i) {
			if (bindArray[i].type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
				shader_binding_t bindingInfo;
				bindingInfo.root_parameter_hash = bindArray[i].paramHashId;
				bindingInfo.table_slot = bindArray[i].paramTableSlot;
				Texture2DParams[bindArray[i].name] = bindingInfo;
			}
			else if (bindArray[i].type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
				shader_constantbuffer_t cbBindInfo;
				cbBindInfo.param_hash = bindArray[i].paramHashId;
				cbBindInfo.table_slot = bindArray[i].paramTableSlot;
				cbBindInfo.bytesize = constantBuffers[bindArray[i].name].bytesize;

				ConstantBuffers[constantBuffers[bindArray[i].name].content_hash] = cbBindInfo;
				constantBufferNameToHash[bindArray[i].name] = constantBuffers[bindArray[i].name].content_hash;
			}
			else if (bindArray[i].type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
				shader_binding_t bindingInfo;
				bindingInfo.root_parameter_hash = bindArray[i].paramHashId;
				bindingInfo.table_slot = bindArray[i].paramTableSlot;
				RWTexture2DParams[bindArray[i].name] = bindingInfo;
			}
		}

		// map vars to cbuffers
		for (auto kv : constantVariables) {
			auto& param = ConstantVarParams[kv.key];
			param.cb_hash_index = constantBufferNameToHash[kv.value.constantbuffer_name];
			param.bytesize = kv.value.bytesize;
			param.byteoffset = kv.value.constantbuffer_offset;
		}

		// fixing descriptor table pointers
		for (auto i = 0u; i < Size(rootParamsArray); ++i) {
			rootParamsArray[i].DescriptorTable.pDescriptorRanges = rootRangesArray.DataPtr + offsetsArray[i].index;
			rootParamsArray[i].DescriptorTable.NumDescriptorRanges = offsetsArray[i].num;
		}

		D3D12_ROOT_SIGNATURE_DESC rootDesc;
		rootDesc.NumParameters = (u32)Size(rootParamsArray);
		rootDesc.pParameters = rootParamsArray.DataPtr;
		rootDesc.NumStaticSamplers = 0;
		rootDesc.pStaticSamplers = nullptr;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;

		RootSignature = GetRootSignature(rootDesc, &rootRangesArray, &rootParamsArray).ptr;

		if (GVerboseRootSingatures) {
			DebugPrintRoot(rootDesc);
		}

		Trim(Texture2DParams);
		Trim(RWTexture2DParams);
		Trim(ConstantVarParams);
		Trim(ConstantBuffers);
		Trim(RootParams);
	}
};

class GraphicsPipelineStateBindings : public PipelineStateBindings {
public:
	shader_handle	VS;
	shader_handle	PS;
	u32				CompilationErrors : 1;
	u32				NoPixelShader : 1;

	GraphicsPipelineStateBindings() :
		VS({}),
		PS({}),
		CompilationErrors(0),
		NoPixelShader(1)
	{
	}

	void Prepare() {
		CompilationErrors = 0;

		Check(IsValid(VS));

		auto vsBytecode = GetShaderBytecode(VS);
		NoPixelShader = !IsValid(PS);
		auto psBytecode = NoPixelShader ? shader_bytecode_t() : GetShaderBytecode(PS);
		CompilationErrors = (vsBytecode.bytesize == 0) || (!NoPixelShader && psBytecode.bytesize == 0);
		if (CompilationErrors) {
			return;
		}

		Hashmap<TextId, shader_constantvariable_desc_t>	constantVariables(GetThreadScratchAllocator());
		Hashmap<TextId, shader_input_desc_t>			bindInputs(GetThreadScratchAllocator());
		Hashmap<TextId, shader_constantbuffer_desc_t>	constantBuffers(GetThreadScratchAllocator());

		ID3D12ShaderReflection* vsReflection;
		VerifyHr(D3DReflect(vsBytecode.bytecode, vsBytecode.bytesize, IID_PPV_ARGS(&vsReflection)));
		ID3D12ShaderReflection* psReflection = nullptr;
		if (!NoPixelShader) {
			VerifyHr(D3DReflect(psBytecode.bytecode, psBytecode.bytesize, IID_PPV_ARGS(&psReflection)));
		}

		Verify(GetConstantBuffersAndVariables(vsReflection, constantBuffers, constantVariables));
		Verify(GetInputBindingSlots(vsReflection, bindInputs, D3D12_SHADER_VISIBILITY_VERTEX));

		if (psReflection) {
			Verify(GetConstantBuffersAndVariables(psReflection, constantBuffers, constantVariables));
			Verify(GetInputBindingSlots(psReflection, bindInputs, D3D12_SHADER_VISIBILITY_PIXEL));
		}

		if (GVerbosePipelineStates) {
			ConsolePrint(Format("%s\n", (cstr)GetShaderDisplayString(VS)));
			if (!NoPixelShader) {
				ConsolePrint(Format("%s\n", (cstr)GetShaderDisplayString(PS)));
			}
		}

		PrepareInternal(constantVariables, bindInputs, constantBuffers);

		ComRelease(vsReflection);
		ComRelease(psReflection);
	}
};

class ComputePipelineStateBindings : public PipelineStateBindings {
public:
	shader_handle	CS;
	u32				CompilationErrors : 1;

	ComputePipelineStateBindings() :
		CS({}),
		CompilationErrors(0)
	{
	}

	void Prepare() {
		CompilationErrors = 0;

		Check(IsValid(CS));

		auto csBytecode = GetShaderBytecode(CS);
		CompilationErrors = (csBytecode.bytesize == 0);
		if (CompilationErrors) {
			return;
		}

		Hashmap<TextId, shader_constantvariable_desc_t>	constantVariables(GetThreadScratchAllocator());
		Hashmap<TextId, shader_input_desc_t>			bindInputs(GetThreadScratchAllocator());
		Hashmap<TextId, shader_constantbuffer_desc_t>	constantBuffers(GetThreadScratchAllocator());

		ID3D12ShaderReflection* csReflection;
		VerifyHr(D3DReflect(csBytecode.bytecode, csBytecode.bytesize, IID_PPV_ARGS(&csReflection)));
		
		Verify(GetConstantBuffersAndVariables(csReflection, constantBuffers, constantVariables));
		Verify(GetInputBindingSlots(csReflection, bindInputs, D3D12_SHADER_VISIBILITY_ALL));

		if (GVerbosePipelineStates) {
			ConsolePrint(Format("%s\n", (cstr)GetShaderDisplayString(CS)));
		}
		PrepareInternal(constantVariables, bindInputs, constantBuffers);

		ComRelease(csReflection);
	}
};

Hashmap<graphics_pipeline_root_key, GraphicsPipelineStateBindings*> CachedShaderStateBindings;
Hashmap<compute_pipeline_root_key, ComputePipelineStateBindings*>	CachedComputeShaderStateBindings;
RWLock																CachedStateBindingsRWL;

GraphicsPipelineStateBindings* GetPipelineStateBindings(shader_handle VS, shader_handle PS) {
	graphics_pipeline_root_key key;
	key.VS = VS;
	key.PS = PS;

	CachedStateBindingsRWL.LockShared();
	auto pbinding = Get(CachedShaderStateBindings, key);
	if (pbinding) {
		auto binding = *pbinding;
		CachedStateBindingsRWL.UnlockShared();
		return binding;
	}

	CachedStateBindingsRWL.UnlockShared();
	CachedStateBindingsRWL.LockExclusive();
	GraphicsPipelineStateBindings* val;
	_new(val);
	Set(CachedShaderStateBindings, key, val);

	val->VS = VS;
	val->PS = PS;
	val->Prepare();
	CachedStateBindingsRWL.UnlockExclusive();

	return val;
}

ComputePipelineStateBindings* GetComputePipelineStateBindings(shader_handle CS) {
	compute_pipeline_root_key key;
	key.CS = CS;

	CachedStateBindingsRWL.LockShared();
	auto pbinding = Get(CachedComputeShaderStateBindings, key);
	if (pbinding) {
		auto binding = *pbinding;
		CachedStateBindingsRWL.UnlockShared();
		return binding;
	}

	CachedStateBindingsRWL.UnlockShared();
	CachedStateBindingsRWL.LockExclusive();
	ComputePipelineStateBindings* val;
	_new(val);
	Set(CachedComputeShaderStateBindings, key, val);

	val->CS = CS;
	val->Prepare();
	CachedStateBindingsRWL.UnlockExclusive();

	return val;
}

void SetRootParams(GPUCommandList* list) {
	if (Size(list->Root.DstDescRanges) && Size(list->Root.SrcDescRanges)) {
		GD12Device->CopyDescriptors(
			(u32)Size(list->Root.DstDescRanges),
			list->Root.DstDescRanges.DataPtr, list->Root.DstDescSizes.DataPtr,
			(u32)Size(list->Root.SrcDescRanges),
			list->Root.SrcDescRanges.DataPtr, list->Root.SrcDescRangeSizes.DataPtr,
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	for (auto kv : list->Root.ConstantBuffers) {
		auto const& cb = list->Bindings->ConstantBuffers[kv.key];
		auto const& cbData = kv.value;
		auto allocation = ConstantsAllocator.AllocateTemporary(list->Bindings->ConstantBuffers[kv.key].bytesize);

		Check(Contains(list->Root.Params, cb.param_hash));

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = allocation.virtual_address;
		cbvDesc.SizeInBytes = (cb.bytesize | 0xFF) + 1;
		GD12Device->CreateConstantBufferView(&cbvDesc, offseted_handle(list->Root.Params[cb.param_hash].binding.table.cpu_handle, cb.table_slot, GD12CbvSrvUavDescIncrement));

		memcpy(allocation.write_ptr, kv.value.write_ptr, cb.bytesize);
	}

	list->D12CommandList->SetDescriptorHeaps(1, &GpuDescriptorsAllocator.D12DescriptorHeap.Ptr);
	if (list->Type == PIPELINE_GRAPHICS) {
		for (auto kv : list->Root.Params) {
			list->D12CommandList->SetGraphicsRootDescriptorTable(kv.value.table.root_index, kv.value.binding.table.gpu_handle);
		}
	}
	else if (list->Type == PIPELINE_COMPUTE) {
		for (auto kv : list->Root.Params) {
			list->D12CommandList->SetComputeRootDescriptorTable(kv.value.table.root_index, kv.value.binding.table.gpu_handle);
		}
	}
}

void ResetRootBindingMappings(GPUCommandList* list) {
	for (auto kv : list->Root.ConstantBuffers) {
		GetThreadScratchAllocator()->Free(kv.value.write_ptr);
		kv.value.write_ptr = nullptr;
	}
	Clear(list->Root.ConstantBuffers);
	Clear(list->Root.DstDescRanges);
	Clear(list->Root.DstDescSizes);
	Clear(list->Root.SrcDescRanges);
	Clear(list->Root.SrcDescRangeSizes);
	Clear(list->Root.Params);
}

void SetComputeShaderState(GPUCommandList* list, shader_handle cs) {
	auto bindings = GetComputePipelineStateBindings(cs);

	ResetRootBindingMappings(list);

	list->Bindings = bindings;
	list->Type = PIPELINE_COMPUTE;
}

void SetShaderState(GPUCommandList* list, shader_handle vs, shader_handle ps, vertex_factory_handle vertexFactory) {
	auto bindings = GetPipelineStateBindings(vs, ps);

	list->Graphics.VertexFactory = vertexFactory;

	ResetRootBindingMappings(list);

	list->Bindings = bindings;
	list->Type = PIPELINE_GRAPHICS;
}

void SetTopology(GPUCommandList* list, D3D_PRIMITIVE_TOPOLOGY topology) {
	list->Graphics.Topology = topology;
}

struct pipeline_query_t {
	union {
		struct {
			shader_handle						vs;
			shader_handle						ps;
			vertex_factory_handle				vertex_factory;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC	graphics_desc;
		} Graphics;
		struct {
			shader_handle						cs;
			D3D12_COMPUTE_PIPELINE_STATE_DESC	compute_desc;
		} Compute;
	};
	PipelineTypeEnum Type;
};

struct pipeline_t {
	pipeline_query_t	query;
	u64					persistant_hash;
};

Hashmap<u64, ID3D12PipelineState*>		PipelineByHash;
Hashmap<u64, pipeline_t>				PipelineDescriptors;

RWLock									PipelineRWL;

u64 CalculatePipelineQueryHash(pipeline_query_t const* query) {
	Check(query->Type != PIPELINE_UNKNOWN);
	if (query->Type == PIPELINE_GRAPHICS) {
		u64 hash = Hash::MurmurHash2_64(&query->Graphics.graphics_desc, sizeof(query->Graphics.graphics_desc), 0);
		hash = Hash::MurmurHash2_64(&query->Graphics.vs, sizeof(query->Graphics.vs), hash);
		hash = Hash::MurmurHash2_64(&query->Graphics.ps, sizeof(query->Graphics.ps), hash);
		hash = Hash::MurmurHash2_64(&query->Graphics.vertex_factory, sizeof(query->Graphics.vertex_factory), hash);
		return hash;
	}
	if (query->Type == PIPELINE_COMPUTE) {
		u64 hash = Hash::MurmurHash2_64(&query->Compute.compute_desc, sizeof(query->Compute.compute_desc), 0);
		hash = Hash::MurmurHash2_64(&query->Compute.cs, sizeof(query->Compute.cs), hash);
		return hash;
	}
	return -1;
}

ID3D12PipelineState* CreatePipelineState(pipeline_query_t const* query, u64 hash);

void UpdatePipelineStates() {
	for (auto kv : PipelineDescriptors) {
		CreatePipelineState(&PipelineDescriptors[kv.key].query, kv.key);
	}
}

u64	CalculatePipelinePersistantHash(pipeline_query_t const* query) {
	Check(query->Type != PIPELINE_UNKNOWN);
	if (query->Type == PIPELINE_GRAPHICS) {
		auto hash = Hash::Combine_64(GetShaderBytecode(query->Graphics.vs).bytecode_hash, IsValid(query->Graphics.ps) ? GetShaderBytecode(query->Graphics.ps).bytecode_hash : 0);
		hash = Hash::MurmurHash2_64(&query->Graphics.graphics_desc, sizeof(query->Graphics.graphics_desc), hash);
		auto layout = GetInputLayoutDesc(query->Graphics.vertex_factory);
		hash = Hash::MurmurHash2_64(layout.pInputElementDescs, sizeof(layout.pInputElementDescs[0]) * layout.NumElements, hash);
		return hash;
	}
	if (query->Type == PIPELINE_COMPUTE) {
		u64 hash = GetShaderBytecode(query->Compute.cs).bytecode_hash;
		hash = Hash::MurmurHash2_64(&query->Compute.compute_desc, sizeof(query->Compute.compute_desc), hash);
		return hash;
	}
	return -1;
}

ID3D12PipelineState* CreatePipelineState(pipeline_query_t const* query, u64 hash) {
	auto persistantHash = CalculatePipelinePersistantHash(query);
	Check(Contains(PipelineByHash, hash));
	Check(Contains(PipelineDescriptors, hash));

	if (PipelineDescriptors[hash].persistant_hash != persistantHash) {
		ID3D12PipelineState *pipelineState = nullptr;

		if (query->Type == PIPELINE_GRAPHICS) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
			desc = query->Graphics.graphics_desc;
			desc.pRootSignature = GetPipelineStateBindings(query->Graphics.vs, query->Graphics.ps)->RootSignature;
			auto VS = GetShaderBytecode(query->Graphics.vs);
			desc.VS.pShaderBytecode = VS.bytecode;
			desc.VS.BytecodeLength = VS.bytesize;
			auto PS = IsValid(query->Graphics.ps) ? GetShaderBytecode(query->Graphics.ps) : shader_bytecode_t();
			desc.PS.pShaderBytecode = PS.bytecode;
			desc.PS.BytecodeLength = PS.bytesize;
			desc.InputLayout = GetInputLayoutDesc(query->Graphics.vertex_factory);

			VerifyHr(GD12Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState)));
		}
		else if (query->Type == PIPELINE_COMPUTE) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
			desc = query->Compute.compute_desc;
			desc.pRootSignature = GetComputePipelineStateBindings(query->Compute.cs)->RootSignature;
			auto CS = GetShaderBytecode(query->Compute.cs);
			desc.CS.pShaderBytecode = CS.bytecode;
			desc.CS.BytecodeLength = CS.bytesize;

			VerifyHr(GD12Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelineState)));
		}

		if (PipelineByHash[hash] != nullptr) {
			PipelineByHash[hash]->Release();
			PipelineByHash[hash] = nullptr;
		}

		PipelineByHash[hash] = pipelineState;
		PipelineDescriptors[hash].persistant_hash = persistantHash;
		return pipelineState;
	}

	return PipelineByHash[hash];
}

ID3D12PipelineState* GetPipelineState(pipeline_query_t const* query) {
	u64 hash = CalculatePipelineQueryHash(query);

	PipelineRWL.LockShared();

	auto ppipeline = Get(PipelineByHash, hash);

	if (ppipeline) {
		auto pipeline = *ppipeline;
		PipelineRWL.UnlockShared();
		return pipeline;
	}

	PipelineRWL.UnlockShared();

	PipelineRWL.LockExclusive();
	Set(PipelineByHash, hash, {});
	PipelineDescriptors[hash].query = *query;
	auto pipeline = CreatePipelineState(query, hash);
	PipelineRWL.UnlockExclusive();

	return pipeline;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE GetPrimitiveTopologyType(D3D_PRIMITIVE_TOPOLOGY topology) {
	switch (topology)
	{
	case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
	case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	}
	return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

void SetRenderTarget(GPUCommandList* list, u32 index, resource_rtv_t rtv) {
	list->Graphics.RTVs[index] = rtv;

	if (IsValid(rtv.slice.handle)) {
		list->Graphics.NumRenderTargets = max(list->Graphics.NumRenderTargets, index + 1);
	}
	else {
		list->Graphics.RTVs[index] = {};

		i32 maxIndex = -1;
		for (i32 i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			if (IsValid(list->Graphics.RTVs[i].slice.handle)) {
				maxIndex = i;
			}
		}
		list->Graphics.NumRenderTargets = maxIndex + 1;
	}
}

void SetDepthStencil(GPUCommandList* list, resource_dsv_t dsv) {
	if (IsValid(dsv.slice.handle)) {
		list->Graphics.DSV = dsv;
	}
	else{
		list->Graphics.DSV = {};
	}
}

void SetViewport(GPUCommandList* list, float width, float height, float x, float y, float minDepth, float maxDepth) {
	list->Graphics.Viewport.TopLeftX = x;
	list->Graphics.Viewport.TopLeftY = y;
	list->Graphics.Viewport.Width = width;
	list->Graphics.Viewport.Height = height;
	list->Graphics.Viewport.MinDepth = minDepth;
	list->Graphics.Viewport.MaxDepth = maxDepth;
}

void SetScissorRect(GPUCommandList* list, D3D12_RECT rect) {
	list->Graphics.ScissorRect = rect;
}

void SetRasterizer(GPUCommandList* list, D3D12_RASTERIZER_DESC const& desc) {
	list->Graphics.PipelineDesc.RasterizerState = desc;
}

void SetBlendState(GPUCommandList* list, u32 index, D3D12_RENDER_TARGET_BLEND_DESC const& desc) {
	list->Graphics.PipelineDesc.BlendState.RenderTarget[index] = desc;
}

void PreDraw(GPUCommandList* list) {
	auto d12cl = *list->D12CommandList;

	Check(list->Type == PIPELINE_GRAPHICS);

	pipeline_query_t query;
	query.Type = PIPELINE_GRAPHICS;
	query.Graphics.graphics_desc = list->Graphics.PipelineDesc;
	query.Graphics.graphics_desc.DepthStencilState.DepthEnable = IsValid(list->Graphics.DSV);
	query.Graphics.graphics_desc.PrimitiveTopologyType = GetPrimitiveTopologyType(list->Graphics.Topology);
	query.Graphics.graphics_desc.NumRenderTargets = list->Graphics.NumRenderTargets;
	for (auto i = 0; i < _countof(list->Graphics.RTVs); ++i) {
		query.Graphics.graphics_desc.RTVFormats[i] = list->Graphics.RTVs[i].format;
	}
	query.Graphics.graphics_desc.DSVFormat = list->Graphics.DSV.format;

	query.Graphics.vs = static_cast<GraphicsPipelineStateBindings*>(list->Bindings)->VS;
	query.Graphics.ps = static_cast<GraphicsPipelineStateBindings*>(list->Bindings)->PS;
	query.Graphics.vertex_factory = list->Graphics.VertexFactory;

	auto pipelineState = GetPipelineState(&query);

	d12cl->RSSetViewports(1, &list->Graphics.Viewport);
	d12cl->RSSetScissorRects(1, &list->Graphics.ScissorRect);

	d12cl->SetGraphicsRootSignature(list->Bindings->RootSignature);
	SetRootParams(list);
	d12cl->SetPipelineState(pipelineState);
	d12cl->IASetPrimitiveTopology(list->Graphics.Topology);
	d12cl->IASetVertexBuffers(0, list->Graphics.VertexStreamsNum, list->Graphics.VertexStreams);

	CPU_DESC_HANDLE rtvs[MAX_RTVS];
	for (auto i = 0u; i < list->Graphics.NumRenderTargets; ++i) {
		rtvs[i] = list->Graphics.RTVs[i].cpu_descriptor;
	}
	d12cl->OMSetRenderTargets(list->Graphics.NumRenderTargets, rtvs, false, IsValid(list->Graphics.DSV) ? &list->Graphics.DSV.cpu_descriptor : nullptr);

	for (auto i : MakeRange(list->Graphics.NumRenderTargets)) {
		list->ResourcesStateTracker.Transition(list->Graphics.RTVs[i].slice, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	if (IsValid(list->Graphics.DSV)) {
		list->ResourcesStateTracker.Transition(list->Graphics.DSV.slice, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	list->ResourcesStateTracker.FireBarriers();
}

void PostDraw(GPUCommandList* list) {
	for (auto kv : list->Root.ConstantBuffers) {
		GetThreadScratchAllocator()->Free(kv.value.write_ptr);
		kv.value.write_ptr = nullptr;
		Remove(list->Root.Params, kv.key);
	}
	Clear(list->Root.ConstantBuffers);
}

void Draw(GPUCommandList* list, u32 vertexCount, u32 startVertex, u32 instances, u32 startInstance)  {
	PreDraw(list);
	list->D12CommandList->DrawInstanced(vertexCount, instances, startVertex, startInstance);
	PostDraw(list);
}

void DrawIndexed(GPUCommandList* list, u32 indexCount, u32 startIndex, i32 baseVertex, u32 instances, u32 startInstance) {
	PreDraw(list);
	list->D12CommandList->DrawIndexedInstanced(indexCount, instances, startIndex, baseVertex, startInstance);
	PostDraw(list);
}

void PreDispatch(GPUCommandList* list) {
	auto d12cl = *list->D12CommandList;

	Check(list->Type == PIPELINE_COMPUTE);

	pipeline_query_t query;
	query.Type = PIPELINE_COMPUTE;
	query.Compute.compute_desc = list->Compute.PipelineDesc;

	query.Compute.cs = static_cast<ComputePipelineStateBindings*>(list->Bindings)->CS;

	auto pipelineState = GetPipelineState(&query);

	d12cl->SetComputeRootSignature(list->Bindings->RootSignature);
	SetRootParams(list);
	d12cl->SetPipelineState(pipelineState);

	list->ResourcesStateTracker.FireBarriers();
}

void PostDispatch(GPUCommandList* list) {
	PostDraw(list);
}

void Dispatch(GPUCommandList* list, u32 threadGroupX, u32 threadGroupY, u32 threadGroupZ) {
	PreDispatch(list);
	list->D12CommandList->Dispatch(threadGroupX, threadGroupY, threadGroupZ);
	PostDispatch(list);
}

root_parameter_t* PrepareRootParam(GPUCommandList* list, u64 paramHash) {
	Check(Contains(list->Bindings->RootParams, paramHash));
	auto const& param = list->Bindings->RootParams[paramHash];

	auto rootParamPtr = Get(list->Root.Params, paramHash);
	if (!rootParamPtr) {
		list->Root.Params[paramHash].table = param.table;
		
		auto tableDstDescriptors = GpuDescriptorsAllocator.AllocateTemporary(param.table.length);

		list->Root.Params[paramHash].binding.table.gpu_handle = GetGPUHandle(tableDstDescriptors);
		list->Root.Params[paramHash].binding.table.cpu_handle = GetCPUHandle(tableDstDescriptors);

		PushBack(list->Root.DstDescRanges, GetCPUHandle(tableDstDescriptors));

		// copy only srvs and uavs, cbv are created directly on heap
		auto copySlots = param.table.cbv_range_offset;
		PushBack(list->Root.DstDescSizes, copySlots);

		list->Root.Params[paramHash].binding.table.src_array_offset = (u32)Size(list->Root.SrcDescRanges);
		Resize(list->Root.SrcDescRanges, Size(list->Root.SrcDescRanges) + copySlots);
		Resize(list->Root.SrcDescRangeSizes, Size(list->Root.SrcDescRangeSizes) + copySlots);

		Check(param.table.cbv_range_offset <= param.table.length);
		Check(param.table.cbv_range_offset >= 0);
		Check(param.table.length >= 0);

		for (auto i = 0u;i < param.table.uav_range_offset; ++i) {
			list->Root.SrcDescRanges[i] = G_NULL_TEXTURE2D_SRV_DESCRIPTOR;
			list->Root.SrcDescRangeSizes[i] = 1;
		}
		for (auto i = param.table.uav_range_offset;i < copySlots; ++i) {
			list->Root.SrcDescRanges[i] = G_NULL_TEXTURE2D_UAV_DESCRIPTOR;
			list->Root.SrcDescRangeSizes[i] = 1;
		}

		rootParamPtr = Get(list->Root.Params, paramHash);
	}

	return rootParamPtr;
}

upload_allocation_t AllocateSmallUploadMemory(GPUCommandList*, u64 size, u64 alignment) {
	return ConstantsAllocator.AllocateTemporary((u32)size, (u32)alignment);
}

void	SetIndexBuffer(GPUCommandList* list, buffer_location_t stream) {
	D3D12_INDEX_BUFFER_VIEW ibv;
	ibv.BufferLocation = stream.address;
	ibv.Format = stream.stride == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
	ibv.SizeInBytes = stream.size;

	list->D12CommandList->IASetIndexBuffer(&ibv);
}

void	SetVertexStream(GPUCommandList* list, u32 index, buffer_location_t stream) {
	list->Graphics.VertexStreams[index].BufferLocation = stream.address;
	list->Graphics.VertexStreams[index].SizeInBytes = stream.size;
	list->Graphics.VertexStreams[index].StrideInBytes = stream.stride;

	if (stream.address != 0) {
		list->Graphics.VertexStreamsNum = max(list->Graphics.VertexStreamsNum, index + 1);
	}
	else {
		list->Graphics.VertexStreamsNum--;

		for (auto i = index - 1; i >= 0; --i) {
			if (list->Graphics.VertexStreams[index].BufferLocation != 0) {
				break;
			}
			else {
				list->Graphics.VertexStreamsNum--;
			}
		}
	}
}

void*	GetConstantWritePtr(GPUCommandList* list, TextId var, size_t writeSize) {
	if (!Contains(list->Bindings->ConstantVarParams, var)) {
		Warning(Format("constant %s not found\n", (const char*)GetString(var)), true, TYPE_ID("ShaderBindings"));
		return nullptr;
	}

	auto const& constantVar = list->Bindings->ConstantVarParams[var];
	Check(Contains(list->Bindings->ConstantBuffers, constantVar.cb_hash_index));
	auto rootParamPtr = PrepareRootParam(list, list->Bindings->ConstantBuffers[constantVar.cb_hash_index].param_hash);

	if (!Get(list->Root.ConstantBuffers, constantVar.cb_hash_index)) {
		auto const& cbInfo = list->Bindings->ConstantBuffers[constantVar.cb_hash_index];

		constantbuffer_cpudata_t cbData;
		cbData.write_ptr = GetThreadScratchAllocator()->Allocate(cbInfo.bytesize, 16);
		cbData.size = cbInfo.bytesize;

		list->Root.ConstantBuffers[constantVar.cb_hash_index] = cbData;
	}

	Check(constantVar.bytesize <= list->Bindings->ConstantBuffers[constantVar.cb_hash_index].bytesize);
	Check(writeSize <= constantVar.bytesize);

	return pointer_add(list->Root.ConstantBuffers[constantVar.cb_hash_index].write_ptr, constantVar.byteoffset);
}

void SetConstant(GPUCommandList* list, TextId var, const void* srcPtr, size_t srcSize) {
	auto upload = GetConstantWritePtr(list, var, srcSize);
	if (upload) {
		memcpy(upload, srcPtr, srcSize);
	}
}

void TransitionBarrier(GPUCommandList* list, resource_slice_t resource, D3D12_RESOURCE_STATES after) {
	list->ResourcesStateTracker.Transition(resource, after);
}

void FlushBarriers(GPUCommandList* list) {
	list->ResourcesStateTracker.FireBarriers();
}

ID3D12GraphicsCommandList*	GetD12CommandList(GPUCommandList* list) {
	return *list->D12CommandList;
}

void CopyBufferRegion(GPUCommandList* list, resource_handle dst, u64 srcOffset, resource_handle src, u64 dstOffset, u64 size) {
	Check(src != dst);
	list->ResourcesStateTracker.Transition(Slice(dst), D3D12_RESOURCE_STATE_COPY_DEST);
	list->ResourcesStateTracker.Transition(Slice(src), D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->ResourcesStateTracker.FireBarriers();
	list->D12CommandList->CopyBufferRegion(GetResourceFast(dst)->resource, srcOffset, GetResourceFast(src)->resource, dstOffset, size);
}

void SetTexture2D(GPUCommandList* list, TextId slot, resource_srv_t srv) {
	if (!Contains(list->Bindings->Texture2DParams, slot)) {
		Warning(Format("texture2d %s not found\n", (const char*)GetString(slot)), true, TYPE_ID("ShaderBindings"));
		return;
	}
	auto const& binding = list->Bindings->Texture2DParams[slot];
	auto rootParamPtr = PrepareRootParam(list, binding.root_parameter_hash);
	Check(binding.table_slot < rootParamPtr->table.length);

	auto index = rootParamPtr->binding.table.src_array_offset + binding.table_slot;
	list->Root.SrcDescRanges[index] = srv.cpu_descriptor;
	list->Root.SrcDescRangeSizes[index] = 1;

	if (!srv.fixed_state) {
		list->ResourcesStateTracker.Transition(srv.slice, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}


void SetRWTexture2D(GPUCommandList* list, TextId slot, resource_uav_t uav) {
	if (!Contains(list->Bindings->RWTexture2DParams, slot)) {
		Warning(Format("rwtexture2d %s not found\n", (const char*)GetString(slot)), true, TYPE_ID("ShaderBindings"));
		return;
	}
	
	auto const& binding = list->Bindings->RWTexture2DParams[slot];
	auto rootParamPtr = PrepareRootParam(list, binding.root_parameter_hash);
	Check(binding.table_slot < rootParamPtr->table.length);

	auto index = rootParamPtr->binding.table.src_array_offset + binding.table_slot;
	list->Root.SrcDescRanges[index] = uav.cpu_descriptor;
	list->Root.SrcDescRangeSizes[index] = 1;

	list->ResourcesStateTracker.Transition(uav.slice, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}


//////////////////////////////////////////////////////

void ShutdownRenderingEngines() {
	WaitForCompletion();

	call_destructor(&ConstantsAllocator);
	call_destructor(&GpuDescriptorsAllocator);

	for (auto kv : RootSignatures) {
		kv.value.ptr->Release();
	}
	FreeMemory(RootSignatures);

	for (auto kv : CachedShaderStateBindings) {
		_delete(kv.value);
	}
	FreeMemory(CachedShaderStateBindings);
	for (auto kv : CachedComputeShaderStateBindings) {
		_delete(kv.value);
	}
	FreeMemory(CachedComputeShaderStateBindings);

	for (auto kv : PipelineByHash) {
		kv.value->Release();
	}
	FreeMemory(PipelineByHash);
	FreeMemory(PipelineDescriptors);

	for (auto kv : VertexFactoryByHash) {
		GetMallocAllocator()->Free(VertexFactories[kv.value].Elements);
	}

	FreeMemory(VertexFactoryByHash);
	FreeMemory(VertexFactories);

	FreeMemory(FrameFences);
	for (auto ptr : GPUQueues) {
		_delete(ptr);
	}
	FreeMemory(GPUQueues);
	FreeMemory(GResourceState);
	FreeMemory(GSubresourceState);
}

}