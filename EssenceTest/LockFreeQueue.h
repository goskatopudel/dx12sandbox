#pragma once

#include "Essence.h"

struct LockFreeSPSCQueue {
	au64	ReadIndex;
	au64	WriteIndex;
	au64	WriteCompletedIndex;
	static const u32 MAX_SIZE = 4096;
	int		Data[MAX_SIZE];

	void Push(int val) {
		u64 index = WriteIndex.fetch_add(1, std::memory_order_relaxed);
		Data[index] = val;
		WriteCompletedIndex.store(index, std::memory_order_relaxed);
	}

	bool Pop(int* outVal) {
		u64 readIndex = ReadIndex.load(std::memory_order_relaxed);
		u64 writeIndex = WriteCompletedIndex.load(std::memory_order_relaxed);
		if (readIndex == writeIndex) {
			return false;
		}

		*outVal = Data[ReadIndex.fetch_add(1, std::memory_order_relaxed) % MAX_SIZE];
		return true;
	}
};