#include "Device.h"
#include "Commands.h"
#include "Hashmap.h"
#include "Array.h"
#include "Descriptors.h"
#include "Ringbuffer.h"
#include "Freelist.h"
#include "Application.h"

#include <d3d12shader.h>
#include <d3dcompiler.h>
#include "d3dx12.h"

#include "remotery\Remotery.h"

#define GPU_PROFILING 1
#define COLLECT_RENDER_STATS 1

namespace Essence {

const bool GVerbosePipelineStates = true;
const bool GVerboseRootSingatures = true;
const bool ForceStateChange = false;

commands_stats_t& operator += (commands_stats_t& lhs, commands_stats_t const& rhs) {
	lhs.graphic_pipeline_state_changes += rhs.graphic_pipeline_state_changes;
	lhs.graphic_root_signature_changes += rhs.graphic_root_signature_changes;
	lhs.graphic_root_params_set += rhs.graphic_root_params_set;
	lhs.draw_calls += rhs.draw_calls;
	lhs.compute_pipeline_state_changes += rhs.compute_pipeline_state_changes;
	lhs.compute_root_signature_changes += rhs.compute_root_signature_changes;
	lhs.compute_root_params_set += rhs.compute_root_params_set;
	lhs.dispatches += rhs.dispatches;
	lhs.constants_bytes_uploaded += rhs.constants_bytes_uploaded;
	return lhs;
}

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

bool NeedStateChange(GPUQueueEnum queueType, ResourceHeapType heapType, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool exclusive = false) {
	if (heapType != DEFAULT_MEMORY) {
		return false;
	}

	if (queueType == GPUQueueEnum::Direct) {
		return (after != before) && ((after & before) == 0 || exclusive);
	}
	else if (queueType == GPUQueueEnum::Copy) {
		return false;
	}

	Check(0);
	return false;
}

D3D12_RESOURCE_STATES GetNextState(GPUQueueEnum queueType, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	if (IsExclusiveState(after) || IsExclusiveState(before)) {
		return after;
	}

	Check(before != after);

	return before | after;
}

void GetD3D12StateDefaults(D3D12_RASTERIZER_DESC *pDest) {
	*pDest = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
}

void GetD3D12StateDefaults(D3D12_DEPTH_STENCIL_DESC *pDest) {
	*pDest = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
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

		Check(block_byte_offset + size <= block->Size);

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
	static const u32						MAX_QUEUED_PROFILER_FRAMES = 3;
	u32										ReadIndex;
	u32										WriteIndex;
	GPUFenceHandle							ReadFences[MAX_QUEUED_PROFILER_FRAMES];
	resource_handle							Readback[MAX_QUEUED_PROFILER_FRAMES];

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

		for (u32 i = 0; i < MAX_QUEUED_PROFILER_FRAMES; ++i) {
			Readback[i] = CreateBuffer(READBACK_MEMORY, sizeof(u64) * (MaxPendingQueries + MAX_QUEUED_PROFILER_FRAMES - 1) / MAX_QUEUED_PROFILER_FRAMES, BUFFER_NO_FLAGS, "readback buffer");
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

D3D12_COMMAND_LIST_TYPE GetD12QueueType(GPUQueueEnum type) {
	switch (type) {
	case GPUQueueEnum::Compute:
		return D3D12_COMMAND_LIST_TYPE_COMPUTE;
	case GPUQueueEnum::Copy:
		return D3D12_COMMAND_LIST_TYPE_COPY;
	case GPUQueueEnum::Direct:
		return D3D12_COMMAND_LIST_TYPE_DIRECT;
	default:
		Check(0);
	}
	return D3D12_COMMAND_LIST_TYPE_DIRECT;
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
DescriptorAllocator			CpuConstantsDescriptorsCacheAllocator;
GPUFenceHandle				CreateFence(GPUQueue* queue);
Ringbuffer<GPUFenceHandle>	FrameFences;

d12_stats_t					LastFrameStats;
d12_stats_t					FrameStats;

void				InitRenderingEngines() {
	GpuDescriptorsAllocator = std::move(DescriptorAllocator(512 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true));
	CpuConstantsDescriptorsCacheAllocator = std::move(DescriptorAllocator(512 * 1024, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false));
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
	GPUQueueEnum		Type;
	u32					AdapterIndex;
	u64					FenceValue;
	u64					LastSignaledValue;
	GPUFenceHandle		LastSignaledFence;
	char				DebugName[64];

	GPUQueue(TextId name, GPUQueueEnum type, u32 adapterIndex);
	~GPUQueue();

	u64					GetCompletedValue();
	void				AdvanceFence();
	void				EndFrame();
};

GPUFenceHandle GetLastSignaledFence(GPUQueue* queue);

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
	GPUQueueEnum				Type;
	Array<GPUFenceHandle>		Fences;
	GPUCommandAllocatorPool*	Pool;
	u32							ListsRecorded;
	CommandAllocatorStateEnum	State;

	GPUCommandAllocator(GPUQueueEnum type, ResourceNameId usage, GPUCommandAllocatorPool* pool) :
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
		if (!Size(Fences) || Back(Fences) != handle) {
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
	ROOT_TABLE,
	ROOT_CONSTANTS,
	ROOT_VIEW
};

struct root_table_parameter_t {
	u32 length;
	u32 uav_range_offset;
	u32 cbv_range_offset;
	u32 root_index;
};

struct root_parameter_meta_t {
	RootParameterEnum	type;
	union {
		root_table_parameter_t table;
	};
};

struct root_parameter_bind_t {
	u32	commited : 1;
	u32 constants_commited : 1;
	u32 src_array_offset : 30;
	struct {
		union {
			struct {
				GPU_DESC_HANDLE gpu_handle;
				CPU_DESC_HANDLE cpu_handle;
				CPU_DESC_HANDLE cbv_cpu_handle;
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
	u32		size : 31;
	u32		commited : 1;
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
#if COLLECT_RENDER_STATS
	commands_stats_t			Stats;
#endif
	//
	struct {
		ID3D12RootSignature*					D12Signature;
		Array<CPU_DESC_HANDLE>					SrcDescRanges;
		Array<u32>								SrcDescRangeSizes;
		Hashmap<u64, constantbuffer_cpudata_t>	ConstantBuffers;
		Hashmap<u64, root_parameter_bind_t>		Params;
	} Root;
	struct {
		shader_handle							VS;
		shader_handle							PS;
		u32										CommitedPipeline : 1;
		u32										CommitedRS : 1;
		u32										CommitedRT : 1;
		u32										CommitedDS : 1;
		u32										CommitedVB : 1;
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
		shader_handle							CS;
		u32										CommitedPipeline : 1;
		D3D12_COMPUTE_PIPELINE_STATE_DESC		PipelineDesc;
	} Compute;
	struct {
		ID3D12DescriptorHeap*	DescriptorHeaps[2];
		ID3D12PipelineState*	PSO;
	} Common;
	PipelineStateBindings*		Bindings;
	PipelineTypeEnum			Type;

	void ResetState() {
		Graphics = {};

		ZeroMemory(&Graphics.PipelineDesc, sizeof(Graphics.PipelineDesc));
		Graphics.PipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		Graphics.PipelineDesc.SampleMask = UINT_MAX;
		Graphics.PipelineDesc.SampleDesc.Count = 1;

		Graphics.ScissorRect.left = 0;
		Graphics.ScissorRect.top = 0;
		Graphics.ScissorRect.right = 32768;
		Graphics.ScissorRect.bottom = 32768;

		Compute = {};

		ZeroMemory(&Compute.PipelineDesc, sizeof(Compute.PipelineDesc));

		Root.D12Signature = nullptr;
		Clear(Root.SrcDescRanges);
		Clear(Root.SrcDescRangeSizes);
		Clear(Root.ConstantBuffers);
		Clear(Root.Params);

		Common = {};

		Bindings = nullptr;

		Type = {};

#if GPU_PROFILING
		Sample = {};
#endif
#if COLLECT_RENDER_STATS
		Stats = {};
#endif
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

GPUQueue::GPUQueue(TextId name, GPUQueueEnum type, u32 adapterIndex) :
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

	DebugName[0] = 0;
	Verify(strncat_s(DebugName, GetString(name), _TRUNCATE) == 0);

#if GPU_PROFILING
	if (Type != GPUQueueEnum::Copy) {
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
}

void GPUQueue::EndFrame() {
	for (auto allocatorsPool : CommandAllocatorPools) {
		allocatorsPool.value->RecycleProcessed();
	}

#if GPU_PROFILING
	if (Type != GPUQueueEnum::Copy) {
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

	ReadFences[WriteIndex] = GetCompletionFence(list);
	WriteIndex = (WriteIndex + 1) % MAX_QUEUED_PROFILER_FRAMES;
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

		ReadIndex = (ReadIndex + 1) % MAX_QUEUED_PROFILER_FRAMES;
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

GPUFenceHandle GetCompletionFence(GPUCommandList* list) {
	Check(list->State == CL_RECORDING || list->State == CL_CLOSED);
	return list->Fence;
}

GPUFenceHandle GetLastSignaledFence(GPUQueue* queue) {
	return queue->LastSignaledFence;
}

void ResetRootBindingMappings(GPUCommandList* list);

void Close(GPUCommandList* list) {
	Check(list->State == CL_RECORDING);
	list->State = CL_CLOSED;

	list->ResourcesStateTracker.FireBarriers();

	for (auto kv : list->Root.ConstantBuffers) {
		GetThreadScratchAllocator()->Free(kv.value.write_ptr);
		kv.value.write_ptr = nullptr;
	}
	Clear(list->Root.ConstantBuffers);
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
#if COLLECT_RENDER_STATS
	FrameStats.command_stats += list->Stats;
	FrameStats.command_lists_num++;
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

	Array<ID3D12CommandList*> executionList(GetThreadScratchAllocator());

	GPUCommandList* patchupList = nullptr;
	if (Size(patchupBarriers)) {
		patchupList = GetCommandList(list->Queue, NAME_("Glue"));
		patchupList->D12CommandList->ResourceBarrier((u32)Size(patchupBarriers), patchupBarriers.DataPtr);
		patchupList->D12CommandList->Close();
		patchupList->State = CL_CLOSED;

		PushBack(executionList, (ID3D12CommandList*)*patchupList->D12CommandList);
#if COLLECT_RENDER_STATS
		FrameStats.command_lists_num++;
		FrameStats.patchup_command_lists_num++;
#endif
	}

	PushBack(executionList, (ID3D12CommandList*)*list->D12CommandList);
	auto queue = list->Queue;
	auto signalValue = queue->FenceValue;

	queue->D12CommandQueue->ExecuteCommandLists((u32)Size(executionList), executionList.DataPtr);
	queue->AdvanceFence();

#if COLLECT_RENDER_STATS
	FrameStats.executions_num++;
#endif

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

GPUQueue*		CreateQueue(TextId name, GPUQueueEnum type, u32 adapterIndex) {
	GPUQueue* queue;
	_new(queue, name, type, adapterIndex);
	PushBack(GPUQueues, queue);
	return queue;
}

void EndCommandsFrame(GPUQueue* mainQueue) {
	u8 limitGpuBufferedFrames = GDisplaySettings.max_gpu_buffered_frames;

	// fence read with last fence from queue(!)
	GpuDescriptorsAllocator.FenceTemporaryAllocations(GetLastSignaledFence(mainQueue));
	CpuConstantsDescriptorsCacheAllocator.FenceTemporaryAllocations(GetLastSignaledFence(mainQueue));
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
	CpuConstantsDescriptorsCacheAllocator.FreeTemporaryAllocations();
	ConstantsAllocator.FreeTemporaryAllocations();

#if COLLECT_RENDER_STATS
	LastFrameStats = FrameStats;
	FrameStats = {};
#endif
}

void WaitForCompletion(GPUQueue* queue, u64 fenceValue) {
	if (queue->D12Fence->GetCompletedValue() < fenceValue) {
		thread_local HANDLE SyncEvent = INVALID_HANDLE_VALUE;
		if (SyncEvent == INVALID_HANDLE_VALUE) {
			SyncEvent = CreateEvent();
		}

		VerifyHr(queue->D12Fence->SetEventOnCompletion(fenceValue, SyncEvent));
		WaitForSingleObject(SyncEvent, INFINITE);
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
		else if (!pState) {
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
				else if (pState->resource_state != RESOURCE_STATE_UNKNOWN) {
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
				Set(CurrentSubresourceState, slice, (u32)after);
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

void ClearUnorderedAccess(GPUCommandList* list, resource_uav_t uav, float4 val) {
	list->ResourcesStateTracker.Transition(uav.slice, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	list->ResourcesStateTracker.FireBarriers();
	float values[4] = { val.x, val.y, val.z, val.w };
	auto viewAlloc = GpuDescriptorsAllocator.AllocateTemporary(1);
	GD12Device->CopyDescriptorsSimple(1, GetCPUHandle(viewAlloc), uav.cpu_descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	list->D12CommandList->ClearUnorderedAccessViewFloat(GetGPUHandle(viewAlloc), uav.cpu_descriptor, GetResourceFast(uav.slice.handle)->resource, values, 0, nullptr);
}

void ClearUnorderedAccess(GPUCommandList* list, resource_uav_t uav, u32 val) {
	list->ResourcesStateTracker.Transition(uav.slice, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	list->ResourcesStateTracker.FireBarriers();
	u32 values[4] = { val, val, val, val };
	auto viewAlloc = GpuDescriptorsAllocator.AllocateTemporary(1);
	GD12Device->CopyDescriptorsSimple(1, GetCPUHandle(viewAlloc), uav.cpu_descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	list->D12CommandList->ClearUnorderedAccessViewUint(GetGPUHandle(viewAlloc), uav.cpu_descriptor, GetResourceFast(uav.slice.handle)->resource, values, 0, nullptr);
}

void ClearDepthStencil(GPUCommandList* list, resource_dsv_t dsv, ClearDSEnum flags, float depth, u8 stencil, u32 numRects, D3D12_RECT* rects) {
	list->ResourcesStateTracker.Transition(dsv.slice, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	list->ResourcesStateTracker.FireBarriers();

	auto access = DSV_WRITE_ALL;
	access = (flags == ClearDSEnum::Stencil) ? DSV_READ_ONLY_DEPTH : access;
	access = (flags == ClearDSEnum::Depth) ? DSV_READ_ONLY_STENCIL : access;
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

// shader indepedant information about constant buffer
// based on size & variables
struct constantbuffer_meta_t {
	u32	bytesize;
	u64 content_hash;
};

// shader indepedant information about constant variable
// based on size & name, mapped to specific constantbuffer
struct constantvariable_meta_t {
	u32		constantbuffer_offset;
	u32		bytesize;
	TextId	constantbuffer_name;
	u64		content_hash;
};

bool GetConstantBuffersAndVariables(
	ID3D12ShaderReflection*							shaderReflection,
	Hashmap<TextId, constantbuffer_meta_t>	&cbDict,
	Hashmap<TextId, constantvariable_meta_t>	&varsDict) {

	D3D12_SHADER_DESC shaderDesc;
	VerifyHr(shaderReflection->GetDesc(&shaderDesc));
	auto constantBuffersNum = shaderDesc.ConstantBuffers;
	for (auto i = 0u; i < constantBuffersNum;++i) {
		auto cbReflection = shaderReflection->GetConstantBufferByIndex(i);

		D3D12_SHADER_BUFFER_DESC bufferDesc;
		VerifyHr(cbReflection->GetDesc(&bufferDesc));

		// preparing hash of constant buffer with all variables inside, so we can figure out collisions!
		auto cbDictKey = TEXT_(bufferDesc.Name);
		constantbuffer_meta_t cbInfo;
		cbInfo.content_hash = Hash::MurmurHash2_64(bufferDesc.Name, (u32)strlen(bufferDesc.Name), 0);
		bufferDesc.Name = nullptr;
		cbInfo.content_hash = Hash::MurmurHash2_64(&bufferDesc, (u32)sizeof(bufferDesc), cbInfo.content_hash);
		cbInfo.bytesize = bufferDesc.Size;

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

				constantvariable_meta_t varInfo;
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

		// no idea why this bit is set for texture
		bool isUsed = !!(bindDesc.uFlags & D3D_SVF_INTERFACE_POINTER);

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

		const u32 STATIC_SAMPERS_REG = 0;
		if (bindDesc.Type == D3D_SIT_SAMPLER && bindDesc.BindPoint < 1) {
			continue;
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

// maps view to rootparam for specific signature
// if view is tabl we specify slot, if not slot can be ignored
struct shader_binding_t {
	u32 table_slot;
	u64 root_parameter_hash;
};

// maps variable to rootparam for specific signature
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

// helper structure
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
	Hashmap<u64, root_parameter_meta_t> &RootParams,
	Array<root_range_offset_t> &offsetsArray) {

	auto rangeIndex = (u32)Size(rootRangesArray);
	auto currentTableSlot = 0;
	u32 index = start;
	u32 cbvsOffset = 0;
	u32 uavsOffset = 0;

	//right now everything ends in a table
	D3D12_ROOT_PARAMETER currentTable;
	currentTable.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	currentTable.ShaderVisibility = bindInputs[bindKeys[index]].visibility;

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
	root_parameter_meta_t rootParamInfo;
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
	u32 rootSize = 0;
	ConsolePrint("Root params:\n");
	for (auto i : MakeRange(desc.NumParameters)) {
		switch (desc.pParameters[i].ParameterType) {
		case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
			rootSize += 1;
			break;
		case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
			rootSize += desc.pParameters[i].Constants.Num32BitValues;
			break;
		case D3D12_ROOT_PARAMETER_TYPE_CBV:
		case D3D12_ROOT_PARAMETER_TYPE_SRV:
		case D3D12_ROOT_PARAMETER_TYPE_UAV:
			rootSize += 2;
			break;
		default:
			Check(0);
		}

		auto visibilityStr = [](D3D12_SHADER_VISIBILITY a) {
			switch (a) {
			case D3D12_SHADER_VISIBILITY_ALL:
				return "ALL";
			case D3D12_SHADER_VISIBILITY_VERTEX:
				return "VERT";
			case D3D12_SHADER_VISIBILITY_PIXEL:
				return "PIX";
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

		if (desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
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
		else if (desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
			ConsolePrint("?\n");
		}
		else {
			ConsolePrint("?\n");
		}
	}
	ConsolePrint(Format("%u / 64 DWORDS\n", rootSize));
}

class PipelineStateBindings {
public:
	Hashmap<TextId, shader_binding_t>				Texture2DParams;
	Hashmap<TextId, shader_binding_t>				RWTexture2DParams;
	Hashmap<TextId, shader_constantvariable_t>		ConstantVarParams;
	Hashmap<u64, shader_constantbuffer_t>			ConstantBuffers;
	Hashmap<u64, root_parameter_meta_t>				RootParams;

	ID3D12RootSignature* RootSignature;

	void Prepare(
		Hashmap<TextId, constantvariable_meta_t>& constantVariables,
		Hashmap<TextId, shader_input_desc_t>& bindInputs,
		Hashmap<TextId, constantbuffer_meta_t>& constantBuffers)
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

		D3D12_STATIC_SAMPLER_DESC StaticSamplers[5];
		StaticSamplers[0].ShaderRegister = 0;
		StaticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
		StaticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[0].MipLODBias = 0;
		StaticSamplers[0].MaxAnisotropy = 16;
		StaticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		StaticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		StaticSamplers[0].MinLOD = 0.f;
		StaticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		StaticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		StaticSamplers[0].RegisterSpace = 0;

		StaticSamplers[1].ShaderRegister = 0;
		StaticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		StaticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[1].MipLODBias = 0;
		StaticSamplers[1].MaxAnisotropy = 16;
		StaticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		StaticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		StaticSamplers[1].MinLOD = 0.f;
		StaticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
		StaticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		StaticSamplers[1].RegisterSpace = 1;

		StaticSamplers[2].ShaderRegister = 0;
		StaticSamplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		StaticSamplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		StaticSamplers[2].MipLODBias = 0;
		StaticSamplers[2].MaxAnisotropy = 16;
		StaticSamplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		StaticSamplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		StaticSamplers[2].MinLOD = 0.f;
		StaticSamplers[2].MaxLOD = D3D12_FLOAT32_MAX;
		StaticSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		StaticSamplers[2].RegisterSpace = 2;

		StaticSamplers[3].ShaderRegister = 0;
		StaticSamplers[3].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		StaticSamplers[3].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[3].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[3].MipLODBias = 0;
		StaticSamplers[3].MaxAnisotropy = 16;
		StaticSamplers[3].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		StaticSamplers[3].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		StaticSamplers[3].MinLOD = 0.f;
		StaticSamplers[3].MaxLOD = D3D12_FLOAT32_MAX;
		StaticSamplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		StaticSamplers[3].RegisterSpace = 3;

		StaticSamplers[4].ShaderRegister = 0;
		StaticSamplers[4].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		StaticSamplers[4].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[4].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[4].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		StaticSamplers[4].MipLODBias = 0;
		StaticSamplers[4].MaxAnisotropy = 16;
		StaticSamplers[4].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		StaticSamplers[4].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		StaticSamplers[4].MinLOD = 0.f;
		StaticSamplers[4].MaxLOD = D3D12_FLOAT32_MAX;
		StaticSamplers[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		StaticSamplers[4].RegisterSpace = 4;

		D3D12_ROOT_SIGNATURE_DESC rootDesc;
		rootDesc.NumParameters = (u32)Size(rootParamsArray);
		rootDesc.pParameters = rootParamsArray.DataPtr;
		rootDesc.NumStaticSamplers = _countof(StaticSamplers);
		rootDesc.pStaticSamplers = StaticSamplers;
		/*rootDesc.NumStaticSamplers = 0;
		rootDesc.pStaticSamplers = nullptr;*/
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

// todo: order independant
u64 CalculateBindingsHash(Hashmap<TextId, constantvariable_meta_t>& constantVariables,
	Hashmap<TextId, shader_input_desc_t>& bindInputs,
	Hashmap<TextId, constantbuffer_meta_t>& constantBuffers) {

	u64 hash = 0;

	for (auto kv : constantVariables) {
		hash = Hash::MurmurHash2_64(&kv.key, sizeof(kv.key), hash);
		hash = Hash::MurmurHash2_64(&kv.value, sizeof(kv.value), hash);
	}

	for (auto kv : bindInputs) {
		hash = Hash::MurmurHash2_64(&kv.key, sizeof(kv.key), hash);
		hash = Hash::MurmurHash2_64(&kv.value, sizeof(kv.value), hash);
	}

	for (auto kv : constantBuffers) {
		hash = Hash::MurmurHash2_64(&kv.key, sizeof(kv.key), hash);
		hash = Hash::MurmurHash2_64(&kv.value, sizeof(kv.value), hash);
	}

	return hash;
}

bool LoadShadersMetadataFromReflection(
	shader_handle VS, shader_handle PS,
	Hashmap<TextId, constantvariable_meta_t>& constantVariables,
	Hashmap<TextId, shader_input_desc_t>& bindInputs,
	Hashmap<TextId, constantbuffer_meta_t>& constantBuffers)
{
	u32 compilationErrors = 0;
	u32 noPixelShader = 0;

	Check(IsValid(VS));

	auto vsBytecode = GetShaderBytecode(VS);
	noPixelShader = !IsValid(PS);
	auto psBytecode = noPixelShader ? shader_bytecode_t() : GetShaderBytecode(PS);
	compilationErrors = (vsBytecode.bytesize == 0) || (!noPixelShader && psBytecode.bytesize == 0);
	if (compilationErrors) {
		return false;
	}

	ID3D12ShaderReflection* vsReflection;
	VerifyHr(D3DReflect(vsBytecode.bytecode, vsBytecode.bytesize, IID_PPV_ARGS(&vsReflection)));
	ID3D12ShaderReflection* psReflection = nullptr;
	if (!noPixelShader) {
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
		if (!noPixelShader) {
			ConsolePrint(Format("%s\n", (cstr)GetShaderDisplayString(PS)));
		}
	}

	ComRelease(vsReflection);
	ComRelease(psReflection);

	return true;
}

bool LoadShadersMetadataFromReflection(
	shader_handle CS,
	Hashmap<TextId, constantvariable_meta_t>& constantVariables,
	Hashmap<TextId, shader_input_desc_t>& bindInputs,
	Hashmap<TextId, constantbuffer_meta_t>& constantBuffers)
{
	u32 compilationErrors = 0;

	Check(IsValid(CS));

	auto csBytecode = GetShaderBytecode(CS);
	compilationErrors = (csBytecode.bytesize == 0);
	if (compilationErrors) {
		return false;
	}

	ID3D12ShaderReflection* csReflection;
	VerifyHr(D3DReflect(csBytecode.bytecode, csBytecode.bytesize, IID_PPV_ARGS(&csReflection)));

	Verify(GetConstantBuffersAndVariables(csReflection, constantBuffers, constantVariables));
	Verify(GetInputBindingSlots(csReflection, bindInputs, D3D12_SHADER_VISIBILITY_ALL));

	if (GVerbosePipelineStates) {
		ConsolePrint(Format("%s\n", (cstr)GetShaderDisplayString(CS)));
	}

	ComRelease(csReflection);

	return true;
}

Hashmap<u64, PipelineStateBindings*>							CachedBindings;
Hashmap<graphics_pipeline_root_key, PipelineStateBindings*>		CachedGraphicsBindings;
Hashmap<compute_pipeline_root_key, PipelineStateBindings*>		CachedComputeBindings;
Hashmap<graphics_pipeline_root_key, u64>						CachedGraphicsBindingHash;
Hashmap<compute_pipeline_root_key, u64>							CachedComputeBindingHash;
RWLock															CachedStateBindingsRWL;

PipelineStateBindings* GetPipelineStateBindings(shader_handle VS, shader_handle PS) {
	graphics_pipeline_root_key key;
	key.VS = VS;
	key.PS = PS;

	CachedStateBindingsRWL.LockShared();
	auto pbinding = Get(CachedGraphicsBindings, key);
	if (pbinding) {
		auto binding = *pbinding;
		CachedStateBindingsRWL.UnlockShared();
		return binding;
	}

	CachedStateBindingsRWL.UnlockShared();

	Hashmap<TextId, constantvariable_meta_t>	constantVariables(GetThreadScratchAllocator());
	Hashmap<TextId, shader_input_desc_t>		bindInputs(GetThreadScratchAllocator());
	Hashmap<TextId, constantbuffer_meta_t>		constantBuffers(GetThreadScratchAllocator());

	LoadShadersMetadataFromReflection(VS, PS, constantVariables, bindInputs, constantBuffers);
	u64 hashkey = CalculateBindingsHash(constantVariables, bindInputs, constantBuffers);

	CachedStateBindingsRWL.LockExclusive();

	pbinding = Get(CachedBindings, hashkey);
	if (pbinding) {
		auto binding = *pbinding;

		CachedGraphicsBindings[key] = binding;

		CachedStateBindingsRWL.UnlockExclusive();
		return binding;
	}

	PipelineStateBindings* val;
	_new(val);
	Set(CachedBindings, hashkey, val);
	CachedGraphicsBindings[key] = val;

	val->Prepare(constantVariables, bindInputs, constantBuffers);

	CachedStateBindingsRWL.UnlockExclusive();

	return val;
}

PipelineStateBindings* GetPipelineStateBindings(shader_handle CS) {
	compute_pipeline_root_key key;
	key.CS = CS;

	CachedStateBindingsRWL.LockShared();
	auto pbinding = Get(CachedComputeBindings, key);
	if (pbinding) {
		auto binding = *pbinding;
		CachedStateBindingsRWL.UnlockShared();
		return binding;
	}

	CachedStateBindingsRWL.UnlockShared();

	Hashmap<TextId, constantvariable_meta_t>	constantVariables(GetThreadScratchAllocator());
	Hashmap<TextId, shader_input_desc_t>		bindInputs(GetThreadScratchAllocator());
	Hashmap<TextId, constantbuffer_meta_t>		constantBuffers(GetThreadScratchAllocator());

	LoadShadersMetadataFromReflection(CS, constantVariables, bindInputs, constantBuffers);
	u64 hashkey = CalculateBindingsHash(constantVariables, bindInputs, constantBuffers);

	CachedStateBindingsRWL.LockExclusive();

	pbinding = Get(CachedBindings, hashkey);
	if (pbinding) {
		auto binding = *pbinding;

		CachedComputeBindings[key] = binding;

		CachedStateBindingsRWL.UnlockExclusive();
		return binding;
	}

	PipelineStateBindings* val;
	_new(val);
	Set(CachedBindings, hashkey, val);
	CachedComputeBindings[key] = val;

	val->Prepare(constantVariables, bindInputs, constantBuffers);

	CachedStateBindingsRWL.UnlockExclusive();

	return val;
}

void SetDescriptorHeaps(GPUCommandList* list, ID3D12DescriptorHeap* viewsHeap, ID3D12DescriptorHeap* samplersHeap) {
	Check(viewsHeap);

	if (list->Common.DescriptorHeaps[0] == viewsHeap && list->Common.DescriptorHeaps[1] == samplersHeap) {
		return;
	}

	list->Common.DescriptorHeaps[0] = viewsHeap;
	list->Common.DescriptorHeaps[1] = samplersHeap;

	list->D12CommandList->SetDescriptorHeaps(samplersHeap ? 2 : 1, list->Common.DescriptorHeaps);
}

void SetComputeShaderState(GPUCommandList* list, shader_handle cs) {
	if (list->Compute.CS != cs || ForceStateChange) {
		list->Compute.CS = cs;

		auto bindings = GetPipelineStateBindings(cs);

		if (bindings != list->Bindings) {
			ResetRootBindingMappings(list);

			list->Bindings = bindings;
			list->Type = PIPELINE_COMPUTE;

			list->D12CommandList->SetComputeRootSignature(list->Bindings->RootSignature);
#if COLLECT_RENDER_STATS
			list->Stats.compute_root_signature_changes++;
#endif
		}

		list->Compute.CommitedPipeline = 0;
	}
}

void SetShaderState(GPUCommandList* list, shader_handle vs, shader_handle ps, vertex_factory_handle vertexFactory) {
	if (list->Graphics.VS != vs || list->Graphics.PS != ps || list->Graphics.VertexFactory != vertexFactory || ForceStateChange) {
		if (list->Graphics.Topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			SetTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		list->Graphics.VS = vs;
		list->Graphics.PS = ps;
		list->Graphics.VertexFactory = vertexFactory;

		list->Graphics.CommitedPipeline = 0;

		auto bindings = GetPipelineStateBindings(vs, ps);
		if (bindings != list->Bindings) {
			ResetRootBindingMappings(list);

			list->Bindings = bindings;
			list->Type = PIPELINE_GRAPHICS;

			if (list->Root.D12Signature != list->Bindings->RootSignature) {
				list->Root.D12Signature = list->Bindings->RootSignature;
				list->D12CommandList->SetGraphicsRootSignature(list->Bindings->RootSignature);
#if COLLECT_RENDER_STATS
				list->Stats.graphic_root_signature_changes++;
#endif
			}
		}
	}
}

void SetTopology(GPUCommandList* list, D3D_PRIMITIVE_TOPOLOGY topology) {
	if (list->Graphics.Topology != topology || ForceStateChange) {
		list->Graphics.CommitedPipeline = 0;

		list->D12CommandList->IASetPrimitiveTopology(topology);
		list->Graphics.Topology = topology;
	}
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

void FlushShaderChanges() {
	Hashmap<graphics_pipeline_root_key, i32> InvalidGraphicBindings;
	Hashmap<compute_pipeline_root_key, i32> InvalidComputeBindings;
	Hashmap<PipelineStateBindings*, i32> InvalidPSOs;

	for (auto kv : CachedGraphicsBindings) {
		if (GetShaderMetadata(kv.key.VS).recompiled || GetShaderMetadata(kv.key.PS).recompiled) {
			Set(InvalidPSOs, kv.value, 1);
			Set(InvalidGraphicBindings, kv.key, 1);
		}
	}
	for (auto kv : CachedComputeBindings) {
		if (GetShaderMetadata(kv.key.CS).recompiled) {
			Set(InvalidPSOs, kv.value, 1);
			Set(InvalidComputeBindings, kv.key, 1);
		}
	}

	for (auto kv : InvalidGraphicBindings) {
		Remove(CachedGraphicsBindingHash, kv.key);
		Remove(CachedGraphicsBindings, kv.key);
	}
	for (auto kv : InvalidComputeBindings) {
		Remove(CachedComputeBindingHash, kv.key);
		Remove(CachedComputeBindings, kv.key);
	}
	// todo-optimize: clear cache from old objects

	for (auto kv : PipelineDescriptors) {
		// todo-optimize: check if shaders changed
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
			desc.pRootSignature = GetPipelineStateBindings(query->Compute.cs)->RootSignature;
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

// todo: check if different, check if different format for PSO
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

	list->Graphics.CommitedRT = 0;
	list->Graphics.CommitedPipeline = 0;
}

// todo: check if different, check if different format for PSO
void SetDepthStencil(GPUCommandList* list, resource_dsv_t dsv) {
	if (IsValid(dsv.slice.handle)) {
		list->Graphics.DSV = dsv;
	}
	else {
		list->Graphics.DSV = {};
	}

	list->Graphics.CommitedRT = 0;
	list->Graphics.CommitedPipeline = 0;
}

// todo: check if different
void SetViewport(GPUCommandList* list, float width, float height, float x, float y, float minDepth, float maxDepth) {
	list->Graphics.CommitedRS = 0;

	list->Graphics.Viewport.TopLeftX = x;
	list->Graphics.Viewport.TopLeftY = y;
	list->Graphics.Viewport.Width = width;
	list->Graphics.Viewport.Height = height;
	list->Graphics.Viewport.MinDepth = minDepth;
	list->Graphics.Viewport.MaxDepth = maxDepth;
}

// todo: check if different
void SetViewport(GPUCommandList* list, viewport_t vp) {
	list->Graphics.CommitedRS = 0;

	list->Graphics.Viewport.TopLeftX = vp.x;
	list->Graphics.Viewport.TopLeftY = vp.y;
	list->Graphics.Viewport.Width = vp.width;
	list->Graphics.Viewport.Height = vp.height;
	list->Graphics.Viewport.MinDepth = vp.mindepth;
	list->Graphics.Viewport.MaxDepth = vp.maxdepth;
}

// todo: check if different
void SetScissorRect(GPUCommandList* list, D3D12_RECT rect) {
	list->Graphics.CommitedRS = 0;
	list->Graphics.ScissorRect = rect;
}

void SetRasterizerState(GPUCommandList* list, D3D12_RASTERIZER_DESC const& desc) {
	list->Graphics.CommitedPipeline = 0;
	list->Graphics.PipelineDesc.RasterizerState = desc;
}

void SetDepthStencilState(GPUCommandList* list, D3D12_DEPTH_STENCIL_DESC const& desc) {
	list->Graphics.CommitedPipeline = 0;
	list->Graphics.PipelineDesc.DepthStencilState = desc;
}

void SetBlendState(GPUCommandList* list, u32 index, D3D12_RENDER_TARGET_BLEND_DESC const& desc) {
	list->Graphics.CommitedPipeline = 0;
	list->Graphics.PipelineDesc.BlendState.RenderTarget[index] = desc;
}

void SetRootParams(GPUCommandList* list);

void PreDraw(GPUCommandList* list) {
	auto d12cl = *list->D12CommandList;

	Check(list->Type == PIPELINE_GRAPHICS);

	if (!list->Graphics.CommitedPipeline || ForceStateChange) {
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

		query.Graphics.vs = list->Graphics.VS;
		query.Graphics.ps = list->Graphics.PS;
		query.Graphics.vertex_factory = list->Graphics.VertexFactory;

		auto pipelineState = GetPipelineState(&query);

		if (list->Common.PSO != pipelineState || ForceStateChange) {
			d12cl->SetPipelineState(pipelineState);
			list->Common.PSO = pipelineState;
#if COLLECT_RENDER_STATS
			list->Stats.graphic_pipeline_state_changes++;
#endif
		}

		list->Graphics.CommitedPipeline = 1;
	}

	if (!list->Graphics.CommitedRS || ForceStateChange) {
		d12cl->RSSetViewports(1, &list->Graphics.Viewport);
		d12cl->RSSetScissorRects(1, &list->Graphics.ScissorRect);
		list->Graphics.CommitedRS = 1;
	}

	SetRootParams(list);

	if (!list->Graphics.CommitedVB || ForceStateChange) {
		d12cl->IASetVertexBuffers(0, list->Graphics.VertexStreamsNum, list->Graphics.VertexStreams);
		list->Graphics.CommitedVB = 1;
	}

	if (!list->Graphics.CommitedRT || ForceStateChange) {
		for (auto i : MakeRange(list->Graphics.NumRenderTargets)) {
			list->ResourcesStateTracker.Transition(list->Graphics.RTVs[i].slice, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		if (IsValid(list->Graphics.DSV)) {
			list->ResourcesStateTracker.Transition(list->Graphics.DSV.slice, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		}

		CPU_DESC_HANDLE rtvs[MAX_RTVS];
		for (auto i = 0u; i < list->Graphics.NumRenderTargets; ++i) {
			rtvs[i] = list->Graphics.RTVs[i].cpu_descriptor;
		}
		d12cl->OMSetRenderTargets(list->Graphics.NumRenderTargets, rtvs, false, IsValid(list->Graphics.DSV) ? &list->Graphics.DSV.cpu_descriptor : nullptr);
		list->Graphics.CommitedRT = 1;
	}

	list->ResourcesStateTracker.FireBarriers();
}

void PostDraw(GPUCommandList* list) {
	for (auto kv : list->Root.ConstantBuffers) {
		kv.value.commited = 1;
	}
}

void Draw(GPUCommandList* list, u32 vertexCount, u32 startVertex, u32 instances, u32 startInstance) {
	PreDraw(list);
#if COLLECT_RENDER_STATS
	list->Stats.draw_calls++;
#endif
	list->D12CommandList->DrawInstanced(vertexCount, instances, startVertex, startInstance);
	PostDraw(list);
}

void DrawIndexed(GPUCommandList* list, u32 indexCount, u32 startIndex, i32 baseVertex, u32 instances, u32 startInstance) {
	PreDraw(list);
#if COLLECT_RENDER_STATS
	list->Stats.draw_calls++;
#endif
	list->D12CommandList->DrawIndexedInstanced(indexCount, instances, startIndex, baseVertex, startInstance);
	PostDraw(list);
}

void PreDispatch(GPUCommandList* list) {
	auto d12cl = *list->D12CommandList;

	Check(list->Type == PIPELINE_COMPUTE);

	if (!list->Compute.CommitedPipeline || ForceStateChange) {

		pipeline_query_t query;
		query.Type = PIPELINE_COMPUTE;
		query.Compute.compute_desc = list->Compute.PipelineDesc;

		query.Compute.cs = list->Compute.CS;

		auto pipelineState = GetPipelineState(&query);

		if (list->Common.PSO != pipelineState) {
			d12cl->SetPipelineState(pipelineState);
			list->Common.PSO = pipelineState;
#if COLLECT_RENDER_STATS
			list->Stats.compute_pipeline_state_changes++;
#endif
		}

		list->Compute.CommitedPipeline = 1;
	}

	SetRootParams(list);

	list->ResourcesStateTracker.FireBarriers();
}

void PostDispatch(GPUCommandList* list) {
	PostDraw(list);
}

void Dispatch(GPUCommandList* list, u32 threadGroupX, u32 threadGroupY, u32 threadGroupZ) {
	PreDispatch(list);
#if COLLECT_RENDER_STATS
	list->Stats.dispatches++;
#endif
	list->D12CommandList->Dispatch(threadGroupX, threadGroupY, threadGroupZ);
	PostDispatch(list);
}

root_parameter_bind_t* PrepareRootParam(GPUCommandList* list, u64 paramHash) {
	Check(Contains(list->Bindings->RootParams, paramHash));
	auto const& param = list->Bindings->RootParams[paramHash];

	auto rootParamPtr = Get(list->Root.Params, paramHash);
	if (!rootParamPtr) {
		list->Root.Params[paramHash] = {};
		rootParamPtr = Get(list->Root.Params, paramHash);
		rootParamPtr->commited = 0;
		rootParamPtr->constants_commited = 1;
		rootParamPtr->binding.table.cbv_cpu_handle = {};

		rootParamPtr->src_array_offset = (u32)Size(list->Root.SrcDescRanges);

		// first subtable has srvs and uavs
		auto firstSubtableSlotsNum = param.table.cbv_range_offset;

		Check(rootParamPtr->src_array_offset == (u32)Size(list->Root.SrcDescRanges));
		Resize(list->Root.SrcDescRanges, Size(list->Root.SrcDescRanges) + firstSubtableSlotsNum);
		Resize(list->Root.SrcDescRangeSizes, Size(list->Root.SrcDescRangeSizes) + firstSubtableSlotsNum);

		if (param.table.cbv_range_offset != param.table.length) {
			rootParamPtr->constants_commited = 0;
		}

		Check(param.table.cbv_range_offset <= param.table.length);
		Check(param.table.cbv_range_offset >= 0);
		Check(param.table.length >= 0);

		// fill with null srvs
		for (auto i = 0u; i < param.table.uav_range_offset; ++i) {
			list->Root.SrcDescRanges[i] = G_NULL_TEXTURE2D_SRV_DESCRIPTOR;
			list->Root.SrcDescRangeSizes[i] = 1;
		}
		for (auto i = param.table.uav_range_offset; i < firstSubtableSlotsNum; ++i) {
			list->Root.SrcDescRanges[i] = G_NULL_TEXTURE2D_UAV_DESCRIPTOR;
			list->Root.SrcDescRangeSizes[i] = 1;
		}
	}
	else if (rootParamPtr->commited == 1) {
		rootParamPtr->commited = 0;
	}

	return rootParamPtr;
}

void SetRootParams(GPUCommandList* list) {
	SetDescriptorHeaps(list, GpuDescriptorsAllocator.D12DescriptorHeap.Ptr, nullptr);
	for (auto kv : list->Root.Params) {
		if (kv.value.commited == 0) {
			Check(Contains(list->Bindings->RootParams, kv.key));
			auto const& param = list->Bindings->RootParams[kv.key];

			// we need to allocate new table and copy data inside
			// allocate GPU visible heap
			auto tableDstDescriptors = GpuDescriptorsAllocator.AllocateTemporary(param.table.length);

			kv.value.binding.table.gpu_handle = GetGPUHandle(tableDstDescriptors);
			kv.value.binding.table.cpu_handle = GetCPUHandle(tableDstDescriptors);

			GD12Device->CopyDescriptors(
				1,
				&kv.value.binding.table.cpu_handle, &param.table.cbv_range_offset,
				// only up to cbv 
				param.table.cbv_range_offset,
				list->Root.SrcDescRanges.DataPtr + kv.value.src_array_offset, list->Root.SrcDescRangeSizes.DataPtr + kv.value.src_array_offset,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			if (kv.value.constants_commited == 0) {
				// allocate constants mini-table & copy prev data

				Check(param.table.cbv_range_offset < param.table.length);
				auto oldHandle = kv.value.binding.table.cbv_cpu_handle;
				kv.value.binding.table.cbv_cpu_handle = GetCPUHandle(CpuConstantsDescriptorsCacheAllocator.AllocateTemporary(param.table.length - param.table.cbv_range_offset));

				u32 cbvsNum = param.table.length - param.table.cbv_range_offset;
				Check(cbvsNum);

				// copy old table if necessary
				if (oldHandle.ptr) {
					GD12Device->CopyDescriptors(
						1, &kv.value.binding.table.cbv_cpu_handle, &cbvsNum,
						1, &oldHandle, &cbvsNum,
						D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				}
				/*kv.value.constants_commited = 1;*/
			}
			/*kv.value.commited = 1;*/
		}
	}

	// go through all constant buffers, create view and set to table if needed
	for (auto kv : list->Root.ConstantBuffers) {
		auto& cbData = kv.value;
		if (cbData.commited == 0) {
			auto const& cb = list->Bindings->ConstantBuffers[kv.key];
			auto allocation = ConstantsAllocator.AllocateTemporary(list->Bindings->ConstantBuffers[kv.key].bytesize);
			auto const& param = list->Bindings->RootParams[cb.param_hash];

			Check(Contains(list->Root.Params, cb.param_hash));
			Check(list->Root.Params[cb.param_hash].commited == 0);
			Check(list->Root.Params[cb.param_hash].constants_commited == 0);

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
			cbvDesc.BufferLocation = allocation.virtual_address;
			cbvDesc.SizeInBytes = (cb.bytesize | 0xFF) + 1;

			Check(cb.table_slot - param.table.cbv_range_offset >= 0);
			Check(cb.table_slot - param.table.cbv_range_offset < param.table.length - param.table.cbv_range_offset);

			// create view in cpu mini table
			GD12Device->CreateConstantBufferView(&cbvDesc,
				offseted_handle(list->Root.Params[cb.param_hash].binding.table.cbv_cpu_handle,
					cb.table_slot - param.table.cbv_range_offset,
					GD12CbvSrvUavDescIncrement));

			memcpy(allocation.write_ptr, kv.value.write_ptr, cb.bytesize);
#if COLLECT_RENDER_STATS
			list->Stats.constants_bytes_uploaded += cb.bytesize;
#endif
			cbData.commited = 1;
		}
	}

	// second pass to copy second part of table
	auto copy_cbv_descriptors = [](GPUCommandList* list, decltype(*list->Root.Params.begin()) & kv) {
		auto const& param = list->Bindings->RootParams[kv.key];

		if (kv.value.constants_commited == 0) {
			CPU_DESC_HANDLE dst = offseted_handle(kv.value.binding.table.cpu_handle, param.table.cbv_range_offset, GD12CbvSrvUavDescIncrement);
			u32 cbvsNum = param.table.length - param.table.cbv_range_offset;
			Check(cbvsNum);

			GD12Device->CopyDescriptors(
				1,
				&dst, &cbvsNum,
				1,
				&kv.value.binding.table.cbv_cpu_handle, &cbvsNum,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			kv.value.constants_commited = 1;
		}
	};

	// merge second pass with param set
	if (list->Type == PIPELINE_GRAPHICS) {
		for (auto kv : list->Root.Params) {
			if (kv.value.commited == 0) {
				copy_cbv_descriptors(list, kv);
				Check(Contains(list->Bindings->RootParams, kv.key));

				list->D12CommandList->SetGraphicsRootDescriptorTable(list->Bindings->RootParams[kv.key].table.root_index, kv.value.binding.table.gpu_handle);
				kv.value.commited = 1;
#if COLLECT_RENDER_STATS
				list->Stats.graphic_root_params_set++;
#endif
			}
		}
	}
	else if (list->Type == PIPELINE_COMPUTE) {
		for (auto kv : list->Root.Params) {
			if (kv.value.commited == 0) {
				copy_cbv_descriptors(list, kv);

				list->D12CommandList->SetComputeRootDescriptorTable(list->Bindings->RootParams[kv.key].table.root_index, kv.value.binding.table.gpu_handle);
				kv.value.commited = 1;
#if COLLECT_RENDER_STATS
				list->Stats.compute_root_params_set++;
#endif
			}
		}
	}
}

void ResetRootBindingMappings(GPUCommandList* list) {
	Clear(list->Root.SrcDescRanges);
	Clear(list->Root.SrcDescRangeSizes);
	Clear(list->Root.Params);
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
	if (list->Graphics.VertexStreams[index].BufferLocation == stream.address
		&& list->Graphics.VertexStreams[index].SizeInBytes == stream.size
		&& list->Graphics.VertexStreams[index].StrideInBytes == stream.stride) {
		return;
	}

	list->Graphics.CommitedVB = 0;

	list->Graphics.VertexStreams[index].BufferLocation = stream.address;
	list->Graphics.VertexStreams[index].SizeInBytes = stream.size;
	list->Graphics.VertexStreams[index].StrideInBytes = stream.stride;

	if (stream.address != 0) {
		list->Graphics.VertexStreamsNum = max(list->Graphics.VertexStreamsNum, index + 1);
	}
	else {
		list->Graphics.VertexStreamsNum--;

		for (i32 i = index - 1; i >= 0; --i) {
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

	auto pCB = Get(list->Root.ConstantBuffers, constantVar.cb_hash_index);
	if (!pCB) {
		auto const& cbInfo = list->Bindings->ConstantBuffers[constantVar.cb_hash_index];

		constantbuffer_cpudata_t cbData;
		cbData.write_ptr = GetThreadScratchAllocator()->Allocate(cbInfo.bytesize, 16);
		cbData.size = cbInfo.bytesize;
		cbData.commited = 0;

		list->Root.ConstantBuffers[constantVar.cb_hash_index] = cbData;
		list->Root.Params[cbInfo.param_hash].commited = 0;
	}
	// we need to indicate need for new view & new table, also copy previous data
	// optimize: no copy if single param
	else if (pCB->commited) {
		pCB->commited = 0;
		auto const& cbInfo = list->Bindings->ConstantBuffers[constantVar.cb_hash_index];
		list->Root.Params[cbInfo.param_hash].commited = 0;
		list->Root.Params[cbInfo.param_hash].constants_commited = 0;

		constantbuffer_cpudata_t cbData;
		cbData.write_ptr = GetThreadScratchAllocator()->Allocate(cbInfo.bytesize, 16);
		cbData.size = cbInfo.bytesize;
		cbData.commited = 0;

		// copy & free prev
		memcpy(cbData.write_ptr, list->Root.ConstantBuffers[constantVar.cb_hash_index].write_ptr, cbInfo.bytesize);
		GetThreadScratchAllocator()->Free(list->Root.ConstantBuffers[constantVar.cb_hash_index].write_ptr);
		list->Root.ConstantBuffers[constantVar.cb_hash_index] = cbData;
	}

	Check(constantVar.bytesize <= list->Bindings->ConstantBuffers[constantVar.cb_hash_index].bytesize);
	Check(writeSize <= constantVar.bytesize);

	return pointer_add(list->Root.ConstantBuffers[constantVar.cb_hash_index].write_ptr, constantVar.byteoffset);
}

// no checking if data is same, if we overwrite already commited buffer it will be mirrored
// app code is responsible to avoid commiting same data (only set changed constants!)
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
	Check(binding.table_slot < list->Bindings->RootParams[binding.root_parameter_hash].table.length);

	auto index = rootParamPtr->src_array_offset + binding.table_slot;
	if (list->Root.SrcDescRanges[index].ptr != srv.cpu_descriptor.ptr) {
		list->Root.SrcDescRanges[index] = srv.cpu_descriptor;
		list->Root.SrcDescRangeSizes[index] = 1;
		rootParamPtr->commited = 0;
	}

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
	Check(binding.table_slot < list->Bindings->RootParams[binding.root_parameter_hash].table.length);

	auto index = rootParamPtr->src_array_offset + binding.table_slot;
	list->Root.SrcDescRanges[index] = uav.cpu_descriptor;
	list->Root.SrcDescRangeSizes[index] = 1;

	list->ResourcesStateTracker.Transition(uav.slice, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}


//////////////////////////////////////////////////////

void ShutdownRenderingEngines() {
	WaitForCompletion();

	call_destructor(&ConstantsAllocator);
	call_destructor(&GpuDescriptorsAllocator);
	call_destructor(&CpuConstantsDescriptorsCacheAllocator);

	for (auto kv : RootSignatures) {
		kv.value.ptr->Release();
	}
	FreeMemory(RootSignatures);

	for (auto kv : CachedBindings) {
		_delete(kv.value);
	}
	FreeMemory(CachedBindings);
	FreeMemory(CachedGraphicsBindings);
	FreeMemory(CachedComputeBindings);
	FreeMemory(CachedGraphicsBindingHash);
	FreeMemory(CachedComputeBindingHash);

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

d12_stats_t const* GetLastFrameStats() {
	return &LastFrameStats;
}

}