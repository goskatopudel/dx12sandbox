#include <cstdio>
#include "Memory.h"
#include "AssertionMacros.h"
#include "Files.h"

namespace Essence {

void FreeMemory(file_read_result& read) {
	if (read.data_ptr) {
		Check(read.allocator);
		read.allocator->Free(read.data_ptr);
		read = {};
	}
}

file_read_result ReadFile(const char* filename, IAllocator* allocator) {
	file_read_result result = {};

	char * buffer = nullptr;
	long length = 0;
	FILE * f;
	if (fopen_s(&f, filename, "rb") != 0) {
		result.result = FileNotFound;
		return result;
	}

	if (f)
	{
		fseek(f, 0, SEEK_END);
		length = ftell(f);
		fseek(f, 0, SEEK_SET);
		buffer = (char*)allocator->Allocate(length + 1, 1);
		if (buffer)
		{
			fread(buffer, 1, length, f);
		}
		buffer[length] = 0;
		fclose(f);
		length += 1;

		result.bytesize = length;
	}

	result.data_ptr = buffer;
	result.allocator = allocator;
	return result;
}


}