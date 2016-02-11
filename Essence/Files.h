#pragma once

#include "Types.h"

namespace Essence {

enum ReadFileResult {
	Success = 0,
	FileNotFound
};

struct file_read_result_t {
	void*			data_ptr;
	u64				bytesize;
	ReadFileResult	result;
	IAllocator*		allocator;
};

file_read_result_t	ReadEntireFile(const char* filename, IAllocator* allocator = GetMallocAllocator());
void				FreeMemory(file_read_result_t read);

}