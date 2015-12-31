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


namespace Essence {


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

bool NeedStateChange(GPUQueueTypeEnum queueType, ResourceHeapType heapType, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	if (heapType != DEFAULT_MEMORY) {
		return false;
	}

	if (queueType == DIRECT_QUEUE) {
		return (after != before) && ((after & before) == 0);
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

	Hashmap<ResourceNameId, GPUCommandListPool*>		CommandListPools;
	Hashmap<ResourceNameId, GPUCommandAllocatorPool*>	CommandAllocatorPools;

	// data
	GPUQueueTypeEnum	Type;
	u32					AdapterIndex;
	HANDLE				SyncEvent;
	u64					FenceValue;
	u64					LastSignaledValue;
	GPUFenceHandle		LastSignaledFence;

	GPUQueue(GPUQueueTypeEnum type, u32 adapterIndex);
	~GPUQueue();

	u64					GetCompletedValue();
	void				AdvanceFence();
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
		// todo: collapse
		PushBack(Fences, handle);
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

	void Return(GPUCommandAllocator* allocator) {
		Check(allocator->State == CA_RECORDING);
		allocator->State = CA_READY;
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


struct resource_state_t {
	D3D12_RESOURCE_STATES	state;
};

Hashmap<resource_slice_t, resource_state_t>			GResourceStates;

class ResourceTracker {
public:
	Hashmap<resource_slice_t, resource_state_t>		ExpectedState;
	Hashmap<resource_slice_t, resource_state_t>		ResourceState;
	Array<D3D12_RESOURCE_BARRIER>					QueuedBarriers;
	GPUCommandList*									Owner;

	ResourceTracker(GPUCommandList* list);
	~ResourceTracker();

	void Transition(resource_slice_t resource, D3D12_RESOURCE_STATES after);
	void FireBarriers();

	void Clear() {
		Essence::Clear(ExpectedState);
		Essence::Clear(ResourceState);
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

class GraphicsPipelineStateBindings;

struct constantbuffer_cpudata_t {
	void*	write_ptr;
	u32		size;
};

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
		DXGI_FORMAT								RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
		CPU_DESC_HANDLE							RTVDescriptors[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
		DXGI_FORMAT								DSVFormat;
		CPU_DESC_HANDLE							DSVDescriptor;
		u32										NumRenderTargets;
		vertex_factory_handle					VertexFactory;
		D3D12_VERTEX_BUFFER_VIEW				VertexStreams[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		u32										VertexStreamsNum;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC		PipelineDesc;
	} Graphics;
	GraphicsPipelineStateBindings*	Bindings;

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

		Clear(Root.DstDescRanges);
		Clear(Root.DstDescSizes);
		Clear(Root.SrcDescRanges);
		Clear(Root.SrcDescRangeSizes);
		Clear(Root.ConstantBuffers);
		Clear(Root.Params);

		Bindings = nullptr;
	}

	GPUCommandList(ResourceNameId usage, GPUCommandAllocator* allocator, GPUQueue *queue, GPUCommandListPool* pool)
		: Usage(usage), CommandAllocator(allocator), Queue(queue), Pool(pool), State(), ResourcesStateTracker(this)
	{
		VerifyHr(GD12Device->CreateCommandList(0, GetD12QueueType(queue->Type), *allocator->D12CommandAllocator, nullptr, IID_PPV_ARGS(D12CommandList.GetInitPtr())));
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

GPUQueue::GPUQueue(GPUQueueTypeEnum type, u32 adapterIndex) :
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

ID3D12CommandQueue*		GetD12Queue(GPUQueue* queue) {
	return *queue->D12CommandQueue;
}

GPUCommandList*			GetCommandList(GPUQueue* queue, ResourceNameId usage) {
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

GPUFenceHandle			GetFence(GPUCommandList* list) {
	Check(list->State == CL_RECORDING || list->State == CL_CLOSED);
	return list->Fence;
}

GPUFenceHandle			GetFence(GPUQueue* queue) {
	return queue->LastSignaledFence;
}

void ResetRootBindingMappings(GPUCommandList* list);

void					Close(GPUCommandList* list) {
	Check(list->State == CL_RECORDING);
	list->State = CL_CLOSED;

	list->ResourcesStateTracker.FireBarriers();

	ResetRootBindingMappings(list);
	
	VerifyHr(list->D12CommandList->Close());
}

void					Execute(GPUCommandList* list) {
	if (list->State == CL_RECORDING) {
		Close(list);
	}
	Check(list->State == CL_CLOSED);

	// fix resources states!
	Check(Size(list->ResourcesStateTracker.QueuedBarriers) == 0);

	Array<D3D12_RESOURCE_BARRIER> patchupBarriers(GetThreadScratchAllocator());
	for (auto kv : list->ResourcesStateTracker.ExpectedState) {
		auto key = kv.key;

		auto state = GResourceStates[kv.key];
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
	}

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

	list->CommandAllocator->Pool->Return(list->CommandAllocator);
	list->CommandAllocator = nullptr;

	list->Pool->Return(list);
	list->ResourcesStateTracker.Clear();

	if (patchupList) {
		Fences[patchupList->Fence.handle].value = signalValue;

		patchupList->State = CL_EXECUTED;
		patchupList->Fence = {};

		patchupList->CommandAllocator->Pool->Return(patchupList->CommandAllocator);
		patchupList->CommandAllocator = nullptr;

		patchupList->Pool->Return(patchupList);
		Check(Size(patchupList->ResourcesStateTracker.ResourceState) == 0);
	}
}

GPUQueue*		CreateQueue(GPUQueueTypeEnum type, u32 adapterIndex) {
	GPUQueue* queue;
	_new(queue, type, adapterIndex);
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
		for (auto allocatorsPool : queue->CommandAllocatorPools) {
			allocatorsPool.value->RecycleProcessed();
		}
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
	FreeMemory(ResourceState);
}

void ResourceTracker::Transition(resource_slice_t resource, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_STATES before;
	auto pState = Get(ResourceState, resource);
	if (pState) {
		before = pState->state;
	}
	else {
		before = GetResourceTransitionInfo(resource.handle)->default_state;
	}

	if (!Contains(ExpectedState, resource)) {
		ExpectedState[resource].state = before;
	}
	ResourceState[resource].state = after;

	if (NeedStateChange(Owner->Queue->Type, GetResourceTransitionInfo(resource.handle)->heap_type, before, after)) {
		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = GetResourceFast(resource.handle)->resource;
		barrier.Transition.Subresource = resource.subresource - 1;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = GetNextState(Owner->Queue->Type, before, after);

		PushBack(QueuedBarriers, barrier);
	}
}

void ResourceTracker::FireBarriers() {
	if (Size(QueuedBarriers)) {
		Owner->D12CommandList->ResourceBarrier((u32)Size(QueuedBarriers), QueuedBarriers.DataPtr);
		Essence::Clear(QueuedBarriers);
	}
}

void	 RegisterResource(resource_handle resource, D3D12_RESOURCE_STATES initialState) {
	GResourceStates[Slice(resource)].state = initialState;
}

void CopyResource(GPUCommandList* list, resource_handle dst, resource_handle src) {
	list->ResourcesStateTracker.Transition(Slice(dst), D3D12_RESOURCE_STATE_COPY_DEST);
	list->ResourcesStateTracker.Transition(Slice(src), D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->ResourcesStateTracker.FireBarriers();
	list->D12CommandList->CopyResource(GetResourceFast(dst)->resource, GetResourceFast(src)->resource);
}

void ClearRenderTarget(GPUCommandList* list, resource_slice_t resource, float4 color) {
	list->ResourcesStateTracker.Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourcesStateTracker.FireBarriers();
	float c[4];
	c[0] = color.x;
	c[1] = color.y;
	c[2] = color.z;
	c[3] = color.w;
	list->D12CommandList->ClearRenderTargetView(GetRTV(resource).cpu_descriptor, c, 0, nullptr);
}

void ClearDepthStencil(GPUCommandList* list, resource_slice_t resource, ClearDepthFlagEnum flags, float depth, u8 stencil) {
	list->ResourcesStateTracker.Transition(resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	list->ResourcesStateTracker.FireBarriers();

	auto access = DSV_WRITE_ALL;
	access = (flags == CLEAR_STENCIL) ? DSV_READ_ONLY_DEPTH : access;
	access = (flags == CLEAR_DEPTH) ? DSV_READ_ONLY_STENCIL : access;
	list->D12CommandList->ClearDepthStencilView(GetDSV(resource, access).cpu_descriptor, (D3D12_CLEAR_FLAGS)flags, depth, stencil, 0, nullptr);
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
				cbInfo.content_hash = Hash::MurmurHash2_64(variableDesc.Name, (u32)strlen(variableDesc.Name), cbInfo.content_hash);
				variableDesc.Name = nullptr;
				cbInfo.content_hash = Hash::MurmurHash2_64(&variableDesc, sizeof(variableDesc), cbInfo.content_hash);
			}

			// add to dict, check if no collision
			if (!Get(cbDict, cbDictKey)) {
				cbDict[cbDictKey] = cbInfo;
			}
			else if (cbDict[cbDictKey].content_hash != cbInfo.content_hash) {
				Format("shader state has conflicting constant buffers");
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
				varInfo.content_hash = Hash::MurmurHash2_64(&variableDesc, sizeof(variableDesc), 0);

				// add to dict, check if no collision
				if (!Get(varsDict, varDictKey)) {
					varsDict[varDictKey] = varInfo;
				}
				else if (varsDict[varDictKey].content_hash != varInfo.content_hash) {
					Format("shader state has conflicting varriable");
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

class GraphicsPipelineStateBindings {
public:
	shader_handle	VS;
	shader_handle	PS;
	u32				CompilationErrors : 1;
	u32				NoPixelShader : 1;
	Hashmap<TextId, shader_binding_t>				Texture2DParams;
	Hashmap<TextId, shader_constantvariable_t>		ConstantVarParams;
	Hashmap<u64, shader_constantbuffer_t>			ConstantBuffers;
	Hashmap<u64, root_parameter_t>					RootParams;

	ID3D12RootSignature* RootSignature;

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

		auto& vsBytecode = GetShaderBytecode(VS);
		auto& psBytecode = GetShaderBytecode(PS);
		NoPixelShader = !IsValid(PS);
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

		auto bindArray = GetValuesScratch(bindInputs);
		auto bindKeys = GetKeysScratch(bindInputs);
		quicksort(bindArray.DataPtr, 0, bindArray.Size, [](shader_input_desc_t a, shader_input_desc_t b) -> bool { return a < b; });

		Array<D3D12_DESCRIPTOR_RANGE> rootRangesArray(GetThreadScratchAllocator());
		Array<D3D12_ROOT_PARAMETER> rootParamsArray(GetThreadScratchAllocator());
		// pointers may change as we expand array, we will fix them later with offsets
		Array<root_range_offset_t>	offsetsArray(GetThreadScratchAllocator());

		u32 startBindIndex = 0;
		u32 bindIndex = 0;
		while (bindIndex < Size(bindArray))
		{
			auto tableFreq = bindArray[bindIndex].frequency;
			bool isSamplerTable = bindArray[bindIndex].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

			while (bindIndex < Size(bindArray)
				&& bindArray[bindIndex].frequency == tableFreq
				&& isSamplerTable == (bindArray[bindIndex].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)) {

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
		bindArray = GetValuesScratch(bindInputs);
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

		ComRelease(vsReflection);
		ComRelease(psReflection);

		Trim(Texture2DParams);
		Trim(ConstantVarParams);
		Trim(ConstantBuffers);
		Trim(RootParams);
	}
};

Hashmap<graphics_pipeline_root_key, GraphicsPipelineStateBindings*> CachedShaderStateBindings;
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
	for (auto kv : list->Root.Params) {
		list->D12CommandList->SetGraphicsRootDescriptorTable(kv.value.table.root_index, kv.value.binding.table.gpu_handle);
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

void SetShaderState(GPUCommandList* list, shader_handle vs, shader_handle ps, vertex_factory_handle vertexFactory) {
	auto bindings = GetPipelineStateBindings(vs, ps);

	list->Graphics.VertexFactory = vertexFactory;

	ResetRootBindingMappings(list);

	list->Bindings = bindings;
}

void SetTopology(GPUCommandList* list, D3D_PRIMITIVE_TOPOLOGY topology) {
	list->Graphics.Topology = topology;
}

struct pipeline_query_t {
	struct {
		shader_handle						vs;
		shader_handle						ps;
		vertex_factory_handle				vertex_factory;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC	graphics_desc;
	} Graphics;
};

struct pipeline_t {
	pipeline_query_t	query;
	u64					persistant_hash;
};

Hashmap<u64, ID3D12PipelineState*>		PipelineByHash;
Hashmap<u64, pipeline_t>				PipelineDescriptors;

RWLock									PipelineRWL;

u64 CalculatePipelineQueryHash(pipeline_query_t const* query) {
	u64 seed = Hash::MurmurHash2_64(&query->Graphics.graphics_desc, sizeof(query->Graphics.graphics_desc), 0);
	seed = Hash::MurmurHash2_64(&query->Graphics.vs, sizeof(query->Graphics.vs), seed);
	seed = Hash::MurmurHash2_64(&query->Graphics.ps, sizeof(query->Graphics.ps), seed);
	seed = Hash::MurmurHash2_64(&query->Graphics.vertex_factory, sizeof(query->Graphics.vertex_factory), seed);
	return seed;
}

ID3D12PipelineState* CreatePipelineState(pipeline_query_t const* query, u64 hash);

void UpdatePipelineStates() {
	for (auto kv : PipelineDescriptors) {
		CreatePipelineState(&PipelineDescriptors[kv.key].query, kv.key);
	}
}

u64	CalculatePipelinePersistantHash(pipeline_query_t const* query) {
	auto hash = Hash::Combine_64(GetShaderBytecode(query->Graphics.vs).bytecode_hash, GetShaderBytecode(query->Graphics.ps).bytecode_hash);
	hash = Hash::MurmurHash2_64(&query->Graphics.graphics_desc, sizeof(query->Graphics.graphics_desc), hash);
	auto layout = GetInputLayoutDesc(query->Graphics.vertex_factory);
	hash = Hash::MurmurHash2_64(layout.pInputElementDescs, sizeof(layout.pInputElementDescs[0]) * layout.NumElements, hash);
	return hash;
}

ID3D12PipelineState* CreatePipelineState(pipeline_query_t const* query, u64 hash) {
	auto persistantHash = CalculatePipelinePersistantHash(query);
	Check(Contains(PipelineByHash, hash));
	Check(Contains(PipelineDescriptors, hash));

	if (PipelineDescriptors[hash].persistant_hash != persistantHash) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
		desc = query->Graphics.graphics_desc;
		desc.pRootSignature = GetPipelineStateBindings(query->Graphics.vs, query->Graphics.ps)->RootSignature;
		auto VS = GetShaderBytecode(query->Graphics.vs);
		desc.VS.pShaderBytecode = VS.bytecode;
		desc.VS.BytecodeLength = VS.bytesize;
		auto PS = GetShaderBytecode(query->Graphics.ps);
		desc.PS.pShaderBytecode = PS.bytecode;
		desc.PS.BytecodeLength = PS.bytesize;
		desc.InputLayout = GetInputLayoutDesc(query->Graphics.vertex_factory);

		ID3D12PipelineState *pipelineState = nullptr;
		VerifyHr(GD12Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState)));

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
	// calculate fast hash
	// if present get
	// else
	// calculte persistant hash
	// 


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

void SetRenderTarget(GPUCommandList* list, u32 index, resource_slice_t resource) {
	auto rtv = GetRTV(resource);
	list->Graphics.NumRenderTargets = max(list->Graphics.NumRenderTargets, index + 1);
	list->Graphics.RTVFormats[index] = rtv.format;
	list->Graphics.RTVDescriptors[index] = rtv.cpu_descriptor;
}

void SetDepthStencil(GPUCommandList* list, resource_slice_t resource) {
	auto dsv = GetDSV(resource);
	list->Graphics.DSVFormat = dsv.format;
	list->Graphics.DSVDescriptor = dsv.cpu_descriptor;
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

	pipeline_query_t query;
	query.Graphics.graphics_desc = list->Graphics.PipelineDesc;
	query.Graphics.graphics_desc.DepthStencilState.DepthEnable = list->Graphics.DSVFormat != DXGI_FORMAT_UNKNOWN;
	query.Graphics.graphics_desc.PrimitiveTopologyType = GetPrimitiveTopologyType(list->Graphics.Topology);
	query.Graphics.graphics_desc.NumRenderTargets = list->Graphics.NumRenderTargets;
	memcpy(&query.Graphics.graphics_desc.RTVFormats, list->Graphics.RTVFormats, sizeof(list->Graphics.RTVFormats));
	query.Graphics.graphics_desc.DSVFormat = list->Graphics.DSVFormat;

	query.Graphics.vs = list->Bindings->VS;
	query.Graphics.ps = list->Bindings->PS;
	query.Graphics.vertex_factory = list->Graphics.VertexFactory;

	auto pipelineState = GetPipelineState(&query);

	d12cl->RSSetViewports(1, &list->Graphics.Viewport);
	d12cl->RSSetScissorRects(1, &list->Graphics.ScissorRect);

	d12cl->SetGraphicsRootSignature(list->Bindings->RootSignature);
	SetRootParams(list);
	d12cl->SetPipelineState(pipelineState);
	d12cl->IASetPrimitiveTopology(list->Graphics.Topology);
	d12cl->IASetVertexBuffers(0, list->Graphics.VertexStreamsNum, list->Graphics.VertexStreams);
	d12cl->OMSetRenderTargets(list->Graphics.NumRenderTargets, list->Graphics.RTVDescriptors, false, list->Graphics.DSVFormat != DXGI_FORMAT_UNKNOWN ? &list->Graphics.DSVDescriptor : nullptr);
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

		for (auto i = 0u;i < copySlots; ++i) {
			list->Root.SrcDescRanges[i] = G_NULL_TEXTURE2D_DESCRIPTOR;
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

void SetTexture2D(GPUCommandList* list, TextId slot, resource_slice_t resource) {
	Check(Contains(list->Bindings->Texture2DParams, slot));
	auto const& binding = list->Bindings->Texture2DParams[slot];
	auto rootParamPtr = PrepareRootParam(list, binding.root_parameter_hash);
	Check(binding.table_slot < rootParamPtr->table.length);

	auto index = rootParamPtr->binding.table.src_array_offset + binding.table_slot;
	list->Root.SrcDescRanges[index] = GetResourceFast(resource.handle)->srv;
	list->Root.SrcDescRangeSizes[index] = 1;
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
	FreeMemory(GResourceStates);
}

}