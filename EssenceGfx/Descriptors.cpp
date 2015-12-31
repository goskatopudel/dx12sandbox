#include "Descriptors.h"
#include "Array.h"

namespace Essence{

DescriptorAllocator::DescriptorAllocator() : MaxDescriptors(0) {
}

DescriptorAllocator& DescriptorAllocator::operator=(DescriptorAllocator&& other) {
	MaxDescriptors = other.MaxDescriptors;
	Type = other.Type;
	IncrementSize = other.IncrementSize;
	IsShaderVisibleHeap = other.IsShaderVisibleHeap;
	BlocksNum = other.BlocksNum;
	Blocks = std::move(other.Blocks);
	TemporaryBlocks = std::move(other.TemporaryBlocks);
	PendingTemporaryBlocks = std::move(other.PendingTemporaryBlocks);
	NextBlockIndex = other.NextBlockIndex;

	D12DescriptorHeap = std::move(other.D12DescriptorHeap);

	for (auto i : u32Range(BucketsNum)) {
		SuballocatedBlock[i] = other.SuballocatedBlock[i];
		FreeRanges[i] = std::move(other.FreeRanges[i]);
	}
	
	return *this;
}

DescriptorAllocator::DescriptorAllocator(DescriptorAllocator && other) {
	*this = std::move(other);
}

DescriptorAllocator::DescriptorAllocator(u32 size, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisibleHeap) :
	MaxDescriptors(size),
	NextBlockIndex(0),
	Type(type),
	IsShaderVisibleHeap(shaderVisibleHeap)
{
	Check(size % BlockSize == 0);

	D3D12_DESCRIPTOR_HEAP_DESC desc;
	desc.NumDescriptors = size;
	desc.Type = type;
	desc.Flags = shaderVisibleHeap ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	desc.NodeMask = 0;

	VerifyHr(GD12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(D12DescriptorHeap.GetInitPtr())));

	IncrementSize = GD12Device->GetDescriptorHandleIncrementSize(type);

	BlocksNum = size / BlockSize;

	Resize(Blocks, BlocksNum);
	Reserve(TemporaryBlocks, BlocksNum);
	for (auto i : u32Range(BlocksNum)) {
		Blocks[i].fence = {};
		Blocks[i].next_allocation_offset = 0;
	}

	for (auto i : u32Range(BucketsNum)) {
		SuballocatedBlock[i] = NULL_BLOCK;
	}
}

DescriptorAllocator::~DescriptorAllocator() {
	for (auto i : i32Range(BucketsNum)) {
		FreeMemory(FreeRanges[i]);
	}
	FreeMemory(TemporaryBlocks);
	FreeMemory(PendingTemporaryBlocks);
	FreeMemory(Blocks);
}

u16 DescriptorAllocator::AllocateBlock() {
	Check(NextBlockIndex < BlocksNum);
	return NextBlockIndex++;
}

descriptor_allocation_t DescriptorAllocator::Allocate(u32 num) {
	Check(num < BlockSize);

	auto bucket = find_log_2(num);
	auto m = pad_pow_2(num);

	u32 heapOffset = 0;
	if (Size(FreeRanges[bucket])) {
		heapOffset = Back(FreeRanges[bucket]).heap_offset;
		PopBack(FreeRanges[bucket]);
	}
	else {
		if (SuballocatedBlock[bucket] == NULL_BLOCK) {
			SuballocatedBlock[bucket] = AllocateBlock();
		}

		heapOffset = SuballocatedBlock[bucket] * BlockSize + Blocks[SuballocatedBlock[bucket]].next_allocation_offset;
		Blocks[SuballocatedBlock[bucket]].next_allocation_offset += m;

		if (Blocks[SuballocatedBlock[bucket]].next_allocation_offset == BlockSize) {
			SuballocatedBlock[bucket] = NULL_BLOCK;
		}
	}

	descriptor_allocation_t allocation = {};
	allocation.allocator = this;
	allocation.heap_offset = heapOffset;
	allocation.size = num;

	return allocation;
}

void DescriptorAllocator::FenceTemporaryAllocations(GPUFenceHandle fence) {
	for (auto& blockIndex : TemporaryBlocks) {
		if (Blocks[blockIndex].next_allocation_offset != 0) {
			Blocks[blockIndex].fence = fence;
			PushBack(PendingTemporaryBlocks, blockIndex);
		}
	}
	RemoveAll(TemporaryBlocks, [&](u16 index) { return Blocks[index].next_allocation_offset != 0; });
}

void DescriptorAllocator::Free(descriptor_allocation_t allocation) {
	if (!allocation.size) {
		return;
	}
	Check(allocation.allocator == this);
	auto bucket = find_log_2(allocation.size);

	block_suballocation_t range;
	range.heap_offset = allocation.heap_offset;
	PushBack(FreeRanges[bucket], range);
}

void cpu_pause_8() {
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
}

descriptor_allocation_t DescriptorAllocator::AllocateTemporary(u32 num) {
	if (!Size(TemporaryBlocks)) {
		ScopeLock lock(&TemporaryBlocksCS);
		if (!Size(TemporaryBlocks)) {
			PushBack(TemporaryBlocks, AllocateBlock());
			CurrentTemporaryBlockIndex = 0;
		}
	}

	descriptor_allocation_t allocation = {};
	allocation.allocator = this;
	allocation.size = num;

	u16 currentTmpBlockIndex;
	u16 blockToTry;
	u32 blockNextAllocation;
	//{
	//	ScopeLock lock(&TemporaryBlocksCS);
	//	currentTmpBlockIndex = CurrentTemporaryBlockIndex.load();
	//	blockToTry = TemporaryBlocks[currentTmpBlockIndex];
	//	blockNextAllocation = Blocks[blockToTry].next_allocation_offset.load();
	//}

	currentTmpBlockIndex = CurrentTemporaryBlockIndex.load();
	blockToTry = TemporaryBlocks[currentTmpBlockIndex];
	blockNextAllocation = Blocks[blockToTry].next_allocation_offset.load();

	while (true) {
		if (blockNextAllocation + num <= BlockSize) {
			auto expected = blockNextAllocation;
			if (Blocks[blockToTry].next_allocation_offset.compare_exchange_strong(expected, blockNextAllocation + num)) {
				allocation.heap_offset = blockToTry * BlockSize + blockNextAllocation;
				break;
			}
			blockNextAllocation = expected;
		}
		else {
			Check(currentTmpBlockIndex <= Size(TemporaryBlocks));
			if (currentTmpBlockIndex + 1 == Size(TemporaryBlocks)) {
				while (CurrentTemporaryBlockIndex + 1 == Size(TemporaryBlocks)) {
					if (TemporaryBlocksCS.TryLock()) {
						PushBack(TemporaryBlocks, AllocateBlock());
						TemporaryBlocksCS.Unlock();
					}
					else {
						_mm_pause();
					}
				}
			}

			auto expected = currentTmpBlockIndex;
			if (CurrentTemporaryBlockIndex.compare_exchange_strong(expected, currentTmpBlockIndex + 1)) {
				currentTmpBlockIndex = currentTmpBlockIndex + 1;
			}
			else {
				currentTmpBlockIndex = expected;
			}
			Check(currentTmpBlockIndex < Size(TemporaryBlocks));

			blockToTry = TemporaryBlocks[currentTmpBlockIndex];
			blockNextAllocation = Blocks[blockToTry].next_allocation_offset.load();
		}
	}

	return allocation;
}

void DescriptorAllocator::FreeTemporaryAllocations() {
	while (Size(PendingTemporaryBlocks)) {
		if (IsFenceCompleted(Blocks[Front(PendingTemporaryBlocks)].fence)) {
			auto index = Front(PendingTemporaryBlocks);
			PopFront(PendingTemporaryBlocks);
			PushBack(TemporaryBlocks, index);
			Blocks[index].fence = {};
			Blocks[index].next_allocation_offset = 0;
		}
		else {
			break;
		}
	}
	CurrentTemporaryBlockIndex = 0;
}

CPU_DESC_HANDLE DescriptorAllocator::GetCPUHandle(descriptor_allocation_t location, i32 offset = 0) {
	return offseted_handle(location.allocator->D12DescriptorHeap->GetCPUDescriptorHandleForHeapStart(), location.heap_offset + offset, IncrementSize);
}

GPU_DESC_HANDLE DescriptorAllocator::GetGPUHandle(descriptor_allocation_t location, i32 offset = 0) {
	return offseted_handle(location.allocator->D12DescriptorHeap->GetGPUDescriptorHandleForHeapStart(), location.heap_offset + offset, IncrementSize);
}

}