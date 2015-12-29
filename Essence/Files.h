#pragma once

#include "Types.h"

namespace Essence {

enum ReadFileResult {
	Success = 0,
	FileNotFound
};

struct file_read_result {
	void*			data_ptr;
	u64				bytesize;
	ReadFileResult	result;
	IAllocator*		allocator;
};

file_read_result	ReadFile(const char* filename, IAllocator* allocator = GetThreadScratchAllocator());
void				FreeMemory(file_read_result& read);

}