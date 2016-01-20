#include <malloc.h>
#include <atomic>
#include <cstdio>
#include "Memory.h"
#include "AssertionMacros.h"
#include "Debug.h"
#include "Thread.h"

namespace Essence {

struct header_t {
	u64 allocation_size;
};

#define MARK_MEMORY 1

static const u8 ALLOC_CLEAR_VAL = 0xCA;
static const u8 FREE_CLEAR_VAL = 0xCF;

static const u32 BORDER_CONSTANT = 0xFFFFFFFF;
static const u64 ALLOCATION_MIN_ALIGNMENT = sizeof(header_t);

class ScratchAllocator;

bool is_aligned(void* ptr, u64 alignment) {
	return ((u64)ptr % alignment) == 0;
}

template<typename T>
void mark_border(T* header, void* ptr) {
	static_assert(sizeof(T) == sizeof(header_t), "header size mismatch");
	auto address = (u32*) (header + 1);
	while (address != ptr) {
		*address++ = BORDER_CONSTANT;
	}
}

template<typename T>
T* find_header(void* ptr) {
	static_assert(sizeof(T) == sizeof(header_t), "header size mismatch");
	auto traverse = (u32*) ptr;
	while (traverse[-1] == BORDER_CONSTANT) {
		--traverse;
	}
	auto header = (T*)traverse - 1;
	Check(is_aligned(header, sizeof(T)));
	return header;
}

class MallocAllocator : public IAllocator {
	std::atomic_uint64_t TotalAllocatedCounter;
	
public:

	MallocAllocator() {
		TotalAllocatedCounter = 0;
	}

	~MallocAllocator() {
		Check(TotalAllocatedCounter == 0);
	}

	void* Allocate(size_t size, size_t alignment) override {
		alignment = max(alignment, ALLOCATION_MIN_ALIGNMENT);
		const auto mallocSize = sizeof(header_t) + size + alignment;
		auto header = (header_t*) malloc(mallocSize);
		header->allocation_size = mallocSize;
		auto ptr = align_forward(header + 1, alignment);
		mark_border(header, ptr);
		TotalAllocatedCounter += mallocSize;

#if MARK_MEMORY
		memset(ptr, ALLOC_CLEAR_VAL, size);
#endif
		return ptr;
	}

	void Free(void* ptr) override {
		if(!ptr) {
			return;
		}

		auto header = find_header<header_t>(ptr);
		TotalAllocatedCounter -= header->allocation_size;

#if MARK_MEMORY
		memset(ptr, FREE_CLEAR_VAL, header->allocation_size - pointer_sub(ptr, header));
#endif

		::free(header);
	}

	size_t GetTotalAllocatedSize() const override {
		return TotalAllocatedCounter;
	}
};

const u32 THREAD_SCRATCH_BUFFER_SIZE = 8 * 1024 * 1024; // 8 MB

CACHE_ALIGN char G_mallocAllocator[sizeof(MallocAllocator)];

IAllocator *GetMallocAllocator() {
	return reinterpret_cast<MallocAllocator*>(G_mallocAllocator);
}

class ScratchAllocator : public IAllocator {
	IAllocator*	BackingAllocator;

	u8*			SegmentBegin;
	u8*			SegmentEnd;
	
	u8*			ReadAddress;
	u8*			WriteAddress;

	i32			AllocationsNum;
	i32			ThreadId;
public:
	ScratchAllocator(IAllocator* allocator, size_t size);
	~ScratchAllocator();
	void* Allocate(size_t size, size_t align) override;
	void Free(void*) override;
	size_t GetTotalAllocatedSize() const override;
	void FixThread(i32 id);
};

char							G_scratchAllocator[sizeof(ScratchAllocator)];
thread_local bool				TL_scratchAllocatorInitialized;
thread_local CACHE_ALIGN char	TL_scratchAllocatorMemory[sizeof(ScratchAllocator)];

#define CHECK_SCRATCH_THREAD_AFFINITY 1
#if CHECK_SCRATCH_THREAD_AFFINITY
ScratchAllocator*				ScratchAllocators[64];
u32								ScratchAllocatorsNum;
CriticalSection					ScratchCS;
#endif

IAllocator* GetScratchAllocator() {
	return reinterpret_cast<ScratchAllocator*>(G_scratchAllocator);
}

IAllocator *GetThreadScratchAllocatorPtr() {
	return reinterpret_cast<ScratchAllocator*>(TL_scratchAllocatorMemory);
}

void FreeThreadAllocator() {
	if (TL_scratchAllocatorInitialized) {
		call_destructor(GetThreadScratchAllocatorPtr());
		TL_scratchAllocatorInitialized = false;
	}
}

void InitMemoryAllocators() {
	auto ptr = G_mallocAllocator;
	new(ptr) MallocAllocator();
	new(GetScratchAllocator()) ScratchAllocator(GetMallocAllocator(), 1024 * 1024 * 16);
	((ScratchAllocator*)GetScratchAllocator())->FixThread(GetThreadId());
}

void ShutdownMemoryAllocators() {
	FreeThreadAllocator();
	call_destructor(GetScratchAllocator());
	call_destructor(GetMallocAllocator());
}

IAllocator *GetThreadScratchAllocator() {
	if (!TL_scratchAllocatorInitialized) {
		TL_scratchAllocatorInitialized = true;
		new(GetThreadScratchAllocatorPtr()) ScratchAllocator(GetMallocAllocator(), THREAD_SCRATCH_BUFFER_SIZE);

#if CHECK_SCRATCH_THREAD_AFFINITY
		ScratchAllocators[ScratchAllocatorsNum] = (ScratchAllocator*)GetThreadScratchAllocatorPtr();
		ScratchAllocatorsNum++;
#endif
	}

	return GetThreadScratchAllocatorPtr();
}

struct scratch_header_t {
	u64	jump : 63;
	u64	freed : 1;
};

ScratchAllocator::ScratchAllocator(IAllocator* allocator, size_t size)
	:	BackingAllocator(allocator)
{
	SegmentBegin = (u8*)allocator->Allocate(size, ALLOCATION_MIN_ALIGNMENT);
	SegmentEnd = SegmentBegin + size;

	ReadAddress = WriteAddress = SegmentBegin;

	AllocationsNum = 0;
	ThreadId = -1;
}

ScratchAllocator::~ScratchAllocator() {
	Check(ReadAddress == WriteAddress);
	BackingAllocator->Free(SegmentBegin);
}

void ScratchAllocator::FixThread(i32 id) {
	ThreadId = id;
}

void* ScratchAllocator::Allocate(size_t size, size_t alignment) {
	if (ThreadId >= 0) {
		Check(ThreadId == GetThreadId());
	}

	if (4 * size > pointer_sub(SegmentEnd, SegmentBegin)) {
		void* ptr = BackingAllocator->Allocate(size, alignment);
		Check(ptr < SegmentBegin || SegmentEnd < ptr);
		return ptr;
	}

	alignment = max(alignment, ALLOCATION_MIN_ALIGNMENT);
	static_assert(sizeof(scratch_header_t) == ALLOCATION_MIN_ALIGNMENT, "header alignment & size mismatch");
	auto pheader = (scratch_header_t*)WriteAddress; 
	Check(is_aligned(WriteAddress, ALLOCATION_MIN_ALIGNMENT));
	auto ptr = align_forward(pheader + 1, alignment);
	mark_border(pheader, min(ptr, SegmentEnd));
	auto next_write_address = align_forward((u8*)ptr + size, ALLOCATION_MIN_ALIGNMENT);

	bool wrapped = ReadAddress > WriteAddress;
	if ((!wrapped && (next_write_address <= SegmentEnd)) || (wrapped && (next_write_address < ReadAddress))) {
		pheader->jump = pointer_sub(next_write_address, pheader);
		pheader->freed = 0;
		Check(pheader->jump < pointer_sub(SegmentEnd, SegmentBegin));
		WriteAddress = next_write_address < SegmentEnd ? (u8*)next_write_address : SegmentBegin;
		Check(ptr >= SegmentBegin && ptr < SegmentEnd);
		Check((u8*)pheader + pheader->jump == next_write_address);

		AllocationsNum++;

		Check(pointer_sub(next_write_address, ptr) >= size);

#if MARK_MEMORY
		memset(ptr, ALLOC_CLEAR_VAL, size);
#endif

		return ptr;
	}
	if (next_write_address > SegmentEnd) {
		if (ReadAddress == WriteAddress) {
			WriteAddress = ReadAddress = SegmentBegin;
		}
		else {
			Check((u8*)(pheader + 1) <= SegmentEnd);
			pheader->jump = pointer_sub(SegmentEnd, pheader);
			pheader->freed = 1;
			Check(pheader->jump < pointer_sub(SegmentEnd, SegmentBegin));
			WriteAddress = SegmentBegin;
			Check((u8*)pheader + pheader->jump == WriteAddress || (u8*)pheader + pheader->jump == SegmentEnd);

			AllocationsNum++;
		}
		return Allocate(size, alignment);
	}

	Check(next_write_address >= ReadAddress);
	ptr = BackingAllocator->Allocate(size, alignment);
	Check(ptr < SegmentBegin || SegmentEnd < ptr);
	return ptr;
}

void ScratchAllocator::Free(void* ptr) {
	if (ThreadId >= 0) {
		Check(ThreadId == GetThreadId());
	}

	if (ptr == nullptr) {
		return;
	}

	if (ptr < SegmentBegin || SegmentEnd < ptr) {
#if CHECK_SCRATCH_THREAD_AFFINITY
		for (u32 i = 0; i < ScratchAllocatorsNum; ++i) {
			bool contained = ptr >= ScratchAllocators[i]->SegmentBegin && ptr < ScratchAllocators[i]->SegmentEnd;
			Check(!contained);
		}
#endif

		BackingAllocator->Free(ptr);
		return;
	}
	
	auto pheader = find_header<scratch_header_t>(ptr);
	Check(pheader + 1 <= ptr);
	pheader->freed = 1;

#if MARK_MEMORY
	memset(ptr, FREE_CLEAR_VAL, pointer_sub((u8*)pheader + pheader->jump, ptr));
#endif

	while((void*)pheader == ReadAddress && ReadAddress != WriteAddress && pheader->freed) {
		ReadAddress += pheader->jump;
		Check(pheader->jump < pointer_sub(SegmentEnd, SegmentBegin));
		ReadAddress = ReadAddress < SegmentEnd ? ReadAddress : SegmentBegin;
		pheader = (scratch_header_t*)ReadAddress;

		AllocationsNum--;

		Check(ReadAddress >= SegmentBegin && ReadAddress < SegmentEnd);
	}
}

size_t ScratchAllocator::GetTotalAllocatedSize() const {	
	Check(0);
	return 0;
}

}