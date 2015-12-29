#include "Strings.h"
#include "Hashmap.h"
#include "Array.h"
#include "Thread.h"
#include "Memory.h"

using namespace Hash;

namespace Essence {

struct memory_block_t {
	void*	ptr;
	size_t	free_offset;
	size_t	size;
};

struct string_t {
	void*	ptr;
	u64		length;
};

Hashmap<TextId, string_t>			TextHashIndex;
RWLock								TextRWLock;
Hashmap<ResourceNameId, string_t>	NameHashIndex;
RWLock								NameRWLock;

Array<memory_block_t>				TextMemoryBlocks;
Array<memory_block_t>				NameMemoryBlocks;

IAllocator*							BlocksAllocator = GetMallocAllocator();

// 64 KB blocks
const u64 BLOCK_SIZE = 64 * 1024;

void FreeStringsMemory() {

	for (auto block : TextMemoryBlocks) {
		BlocksAllocator->Free(block.ptr);
	}
	for (auto block : NameMemoryBlocks) {
		BlocksAllocator->Free(block.ptr);
	}

	FreeMemory(TextHashIndex);
	FreeMemory(TextHashIndex);
	FreeMemory(NameHashIndex);
	FreeMemory(NameHashIndex);
	FreeMemory(TextMemoryBlocks);
	FreeMemory(NameMemoryBlocks);
}

void* StoreStringData(Array<memory_block_t>& blocksArray, void* srcData, size_t bytesize) {
	string_t string_data = {};

	if (bytesize > BLOCK_SIZE) {
		memory_block_t new_block = {};
		new_block.ptr = BlocksAllocator->Allocate(bytesize, 16);
		new_block.size = bytesize;
		new_block.free_offset = bytesize;
		memcpy(new_block.ptr, srcData, bytesize);
		PushBack(blocksArray, new_block);

		return new_block.ptr;
	}

	for (auto& block : blocksArray) {
		if (block.free_offset + bytesize < block.size) {
			auto free_offset = block.free_offset;
			block.free_offset += bytesize;
			memcpy(pointer_add(block.ptr, free_offset), srcData, bytesize);
			return pointer_add(block.ptr, free_offset);
		}
	}

	memory_block_t new_block = {};
	new_block.ptr = BlocksAllocator->Allocate(BLOCK_SIZE, 16);
	new_block.size = BLOCK_SIZE;
	new_block.free_offset = bytesize;
	memcpy(new_block.ptr, srcData, bytesize);
	PushBack(blocksArray, new_block);

	return new_block.ptr;
}

TextId GetTextId(string_context_t text) {
	TextId id;
	id.index = text.hash;

	ReaderScope read(&TextRWLock);

	auto ptr = Get(TextHashIndex, id);
	if (!ptr) {
		ReaderToWriterScope write(&TextRWLock);

		ptr = Get(TextHashIndex, id);
		if (!ptr) {
			// store with 0
			string_t new_string = {};
			new_string.ptr = StoreStringData(TextMemoryBlocks, (void*)text.string, text.length + 1);
			new_string.length = text.length;
			Set(TextHashIndex, id, new_string);
		}
	}

	return id;
}

AString GetString(TextId id) {
	ReaderScope read(&TextRWLock);
	auto ptr = Get(TextHashIndex, id);
	return ScratchString(ptr ? (const char*)ptr->ptr : "");
}

ResourceNameId GetResourceNameId(string_case_invariant_context_t name) {
	ResourceNameId id;
	id.key = name.hash;

	ReaderScope read(&NameRWLock);

	auto ptr = Get(NameHashIndex, id);
	if (!ptr) {
		ReaderToWriterScope write(&NameRWLock);

		ptr = Get(NameHashIndex, id);
		if (!ptr) {
			// store with 0
			string_t new_string = {};
			new_string.ptr = StoreStringData(NameMemoryBlocks, (void*)name.string, name.length + 1);
			new_string.length = name.length;
			Set(NameHashIndex, id, new_string);
		}
	}

	return id;
}

AString GetString(ResourceNameId id) {
	ReaderScope read(&NameRWLock);
	auto ptr = Get(NameHashIndex, id);
	return ScratchString(ptr ? (const char*)ptr->ptr : "");
}

}