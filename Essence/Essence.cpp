#include "Essence.h"
#include "Profiler.h"

namespace Essence {

void InitMainThread() {
	Essence::InitMemoryAllocators();
	SetAsMainThread();
}

void ShutdownMainThread() {
	Check(IsMainThread());
	FreeStringsMemory();
	FreeWarningsMemory();
	Essence::ShutdownMemoryAllocators();
}

const char* WorkerThreadNames[] = {
	"Worker #0",
	"Worker #1",
	"Worker #2",
	"Worker #3",
	"Worker #4",
	"Worker #5",
	"Worker #6",
	"Worker #7",
	"Worker #8",
	"Worker #9",
	"Worker #10",
	"Worker #11",
	"Worker #12",
	"Worker #13",
	"Worker #14",
	"Worker #15",
	"Worker #16+",
};

void InitWorkerThread(u32 index) {
	PROFILE_NAME_THREAD(WorkerThreadNames[min(index, _countof(WorkerThreadNames) - 1)]);
}

void ShutdownWorkerThread() {
	FreeThreadAllocator();
}

}