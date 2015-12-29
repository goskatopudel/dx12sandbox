#pragma once
#include "Device.h"
#include "Commands.h"
#include "Ringbuffer.h"

namespace Essence {

class DescriptorAllocator;

struct descriptor_allocation_t {
	u32						heap_offset;
	u32						size;
	DescriptorAllocator*	allocator;
};

class DescriptorAllocator {
public:
	const u32 BlockSize = 256;

	class BlockData {
	public:
		au32			next_allocation_offset;
		GPUFenceHandle	fence;
	};

	u32									MaxDescriptors;
	static const u32					BucketsNum = 16;
	Array<BlockData>					Blocks;
	u32									BlocksNum;

	OwningComPtr<ID3D12DescriptorHeap>	D12DescriptorHeap;
	u32									NextBlockIndex;

	D3D12_DESCRIPTOR_HEAP_TYPE			Type;
	bool								IsShaderVisibleHeap;
	u32									IncrementSize;

	struct block_suballocation_t {
		u32 heap_offset;
	};

	static const u16 NULL_BLOCK = 0xFFFFu;

	u16								SuballocatedBlock[BucketsNum];
	Array<block_suballocation_t>	FreeRanges[BucketsNum];

	Array<u16>						TemporaryBlocks;
	au16							CurrentTemporaryBlockIndex;
	Ringbuffer<u16>					PendingTemporaryBlocks;
	CriticalSection					TemporaryBlocksCS;

	DescriptorAllocator();
	DescriptorAllocator(DescriptorAllocator const& other) = delete;
	DescriptorAllocator& operator=(DescriptorAllocator&& other);
	DescriptorAllocator(DescriptorAllocator && other);
	DescriptorAllocator(u32 size, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisibleHeap);
	~DescriptorAllocator();

	u16 AllocateBlock();
	descriptor_allocation_t Allocate(u32 num);
	void Free(descriptor_allocation_t allocation);
	descriptor_allocation_t AllocateTemporary(u32 num);
	void FenceTemporaryAllocations(GPUFenceHandle);
	void FreeTemporaryAllocations();

	CPU_DESC_HANDLE GetCPUHandle(descriptor_allocation_t location, i32 offset);
	GPU_DESC_HANDLE GetGPUHandle(descriptor_allocation_t location, i32 offset);
};

inline CPU_DESC_HANDLE GetCPUHandle(descriptor_allocation_t location, i32 offset = 0) {
	return location.allocator->GetCPUHandle(location, offset);
}

inline GPU_DESC_HANDLE GetGPUHandle(descriptor_allocation_t location, i32 offset = 0) {
	return location.allocator->GetGPUHandle(location, offset);
}

}