#include "JobScheduler.h"
#include "Essence.h"
#include <thread>

namespace Essence {

u32 GSysPageSize;

void InitPageSize() {
	if (!GSysPageSize) {
		SYSTEM_INFO sSysInfo;
		GetSystemInfo(&sSysInfo);
		GSysPageSize = sSysInfo.dwPageSize;
	}
}

u64 align_value(u64 v, u64 a) {
	return (v + a - 1) & ~(a - 1);
}

struct TaggedHeap {
	void*			SegmentPtr;
	void* volatile	AllocNext;
	u64				Size;
	u64				CommitedSize;
	CriticalSection CritSect;

	void Init(u64 size) {
		InitPageSize();
		Size = size;
		SegmentPtr = VirtualAlloc(nullptr, Size, MEM_RESERVE, PAGE_NOACCESS);
		Check(SegmentPtr);
		Check(((u64)SegmentPtr % GSysPageSize) == 0);
		AllocNext = 0;
		CommitedSize = 0;
	}

	void CommitPages(u64 minSize) {
		ScopeLock lock(&CritSect);
		u64 nextCommitedSize = align_value(minSize, GSysPageSize);
		if (nextCommitedSize > CommitedSize) {
			Check(VirtualAlloc(pointer_add(SegmentPtr, CommitedSize), nextCommitedSize - CommitedSize, MEM_COMMIT, PAGE_READWRITE));
			CommitedSize = nextCommitedSize;
		}
	}

	void*	Allocate(u64 size, u64 alignment) {
		Check(alignment <= GSysPageSize);
		void* prev = (void*)AllocNext;
		void* allocNext = align_forward(prev, alignment);
		while (true) {
			if (pointer_sub(pointer_add(allocNext, size), SegmentPtr) > CommitedSize) {
				CommitPages(pointer_sub(pointer_add(allocNext, size), SegmentPtr));
			}
			void* old = InterlockedCompareExchangePointer(&AllocNext, allocNext, prev);
			if (old == prev) {
				return allocNext;
			}
			prev = old;
			allocNext = align_forward(prev, alignment);
		}
	}

	void Free() {
		AllocNext = SegmentPtr;
	}

	void Trim(u64 size) {
		assert(VirtualFree(align_forward(pointer_add(SegmentPtr, size), GSysPageSize), CommitedSize - align_value(size, GSysPageSize), MEM_DECOMMIT) == NULL);
		CommitedSize = size;
	}

	void FreePhysical() {
		Trim(0);
	}

	void Cleanup() {
		assert(VirtualFree(SegmentPtr, Size, MEM_RELEASE) == NULL);
	}

	TaggedHeap(u64 size) {
		Init(size);
	}

	~TaggedHeap() {
		Cleanup();
	}
};


template<typename T, u32 Size>
struct TSRingbuffer {
	CACHE_ALIGN i64 volatile ReadIndex;
	CACHE_ALIGN i64 volatile WriteIndex;
	CACHE_ALIGN T*	Data;

	TSRingbuffer() : ReadIndex(0), WriteIndex(0), Data(nullptr) {}

	void InitMemory() {
		Data = (T*)GetMallocAllocator()->Allocate(sizeof(T) * Size, alignof(T));
	}

	void FreeMemory() {
		if (Data) {
			GetMallocAllocator()->Free(Data);
			Data = nullptr;
		}
	}

	~TSRingbuffer() {
		FreeMemory();
	}

	void Push(T val) {
		assert(ReadIndex + Size != WriteIndex);
		Data[InterlockedIncrement64(&WriteIndex) % Size] = val;
	}

	bool Pop(T *outVal) {
		i64 read = ReadIndex;
		while (read + 1 <= WriteIndex) {
			i64 prev = InterlockedCompareExchange64(&ReadIndex, read + 1, read);
			if (prev == read) {
				*outVal = Data[(prev + 1) % Size];
				return true;
			}
			read = prev;
		}
		return false;
	}
};

typedef void* fiber_ptr;

struct job_data_t {
	job_desc_t		work;
	job_waitable_id	waitable;
};

struct counter_t {
	volatile u32	value;
	fiber_ptr		fiber;
};

thread_local fiber_ptr								TlsThreadFiber;

static const u32									MAX_WORKERS = 32;
u32													WorkersNum;
volatile u32										ActiveWorkerThreadsNum;
CACHE_ALIGN	volatile u32							Suspended;
HANDLE												WorkerThreads[MAX_WORKERS];
static const u32									MAX_FIBERS = 1023;
fiber_ptr											MainFibres[MAX_FIBERS];
volatile u32										MainFibresUsed;
TSRingbuffer<fiber_ptr, MAX_FIBERS + 1>				FreeMainFibres;
CriticalSection										WorkSection;
ConditionVariable									WorkCondition;
static const u32									MAX_SCHEDULED_JOBS = 1023;
TSRingbuffer<job_data_t, MAX_SCHEDULED_JOBS + 1>	JobQueue;
static const u32									MAX_WAITABLES = 255;
CriticalSection										WaitablesSection;
volatile u32										WaitableCounters[MAX_WAITABLES];
volatile u32										WaitableListNum;
counter_t											Counters[MAX_WAITABLES];
volatile u32										NextCounterIndex;
TSRingbuffer<u32, MAX_WAITABLES + 1>				FreeCounters;

fiber_ptr GetNextMainFiber() {
	fiber_ptr fiber;
	if (FreeMainFibres.Pop(&fiber)) {
		return fiber;
	}

	u32 index = InterlockedIncrement(&MainFibresUsed) - 1;
	Check(index < MAX_FIBERS);
	return MainFibres[index];
}

job_waitable_id GetAndInitWaitable(u32 num) {
	u32 index;
	if (!FreeCounters.Pop(&index)) {
		index = InterlockedIncrement(&NextCounterIndex) - 1;
	}
	InterlockedExchange(&Counters[index].value, num);
	Counters[index].fiber = nullptr;

	job_waitable_id waitable;
	waitable.index = index;
	return waitable;
}

DWORD WINAPI worker_thread_main(void* pVoidParams);
void WINAPI fiber_main(void* pVoidParams);

void InitJobScheduler() {
	WorkersNum = std::thread::hardware_concurrency() - 1;

	JobQueue.InitMemory();
	FreeCounters.InitMemory();
	FreeMainFibres.InitMemory();

	for (u32 i = 0; i < MAX_FIBERS; ++i) {
		MainFibres[i] = CreateFiber(64 * 1024, fiber_main, nullptr);
	}

	WorkerThreads[0] = GetCurrentThread();
	for (u32 i = 0; i < WorkersNum; ++i) {
		WorkerThreads[i + 1] = CreateThread(nullptr, 0, worker_thread_main, nullptr, 0, nullptr);
	}

	ConvertThreadToFiber(nullptr);
	TlsThreadFiber = GetCurrentFiber();
}

void ShutdownJobScheduler() {
	InterlockedExchange(&Suspended, 1);

	{	ScopeLock lock(&WorkSection);
		WorkCondition.WakeAll();
	}

	if (GetCurrentFiber() != TlsThreadFiber) {
		SwitchToFiber(TlsThreadFiber);
	}
	ConvertFiberToThread();

	while (ActiveWorkerThreadsNum > 0) {
		_mm_pause();
	}

	HANDLE currentThread = GetCurrentThread();

	for (u32 i = 0; i < WorkersNum + 1; ++i) {
		if (WorkerThreads[i] != currentThread) {
			WaitForSingleObject(WorkerThreads[i], INFINITE);
			CloseHandle(WorkerThreads[i]);
		}
	}

	JobQueue.FreeMemory();
	FreeCounters.FreeMemory();
	FreeMainFibres.FreeMemory();
}

DWORD WINAPI worker_thread_main(void* pVoidParams) {
	InterlockedIncrement(&ActiveWorkerThreadsNum);
	ConvertThreadToFiber(nullptr);
	TlsThreadFiber = GetCurrentFiber();

	SwitchToFiber(GetNextMainFiber());

	ConvertFiberToThread();
	InterlockedDecrement(&ActiveWorkerThreadsNum);

	return 0;
}

job_waitable_id ScheduleJobs(job_desc_t* jobs, u32 num) {
	job_waitable_id waitable = GetAndInitWaitable(num);

	for (u32 i = 0; i < num; ++i) {
		job_data_t job_data;
		job_data.work = jobs[i];
		job_data.waitable = waitable;

		JobQueue.Push(job_data);
	}

	WorkCondition.WakeAll();

	return waitable;
}

void WaitForCompletion(job_waitable_id waitable) {
	if (Counters[waitable.index].value == 0) {
		return;
	}
	
	Counters[waitable.index].fiber = GetCurrentFiber();
	u32 index = InterlockedIncrement(&WaitableListNum) - 1;
	WaitableCounters[index] = waitable.index;

	SwitchToFiber(GetNextMainFiber());
}

bool IsCompleted(job_waitable_id waitable) {
	return Counters[waitable.index].value == 0;
}

void WINAPI fiber_main(void* pVoidParams) {
	while (!Suspended) {
		job_data_t job_data;
		fiber_ptr fiber = nullptr;

		for (u32 i = 0; i < WaitableListNum; ++i) {
			if (Counters[WaitableCounters[i]].value == 0) {
				ScopeLock lock(&WaitablesSection);
				if (i < WaitableListNum && Counters[WaitableCounters[i]].value == 0) {
					u32 index = WaitableCounters[i];
					swap(WaitableCounters[i], WaitableCounters[WaitableListNum - 1]);
					WaitableListNum--;
					fiber = Counters[index].fiber;
					break;
				}
			}
		}

		if (fiber) {
			FreeMainFibres.Push(GetCurrentFiber());
			SwitchToFiber(fiber);
		}

		if (JobQueue.Pop(&job_data)) {
			job_data.work.func(job_data.work.p_args);
			InterlockedDecrement(&Counters[job_data.waitable.index].value);
		}
		else {
			ScopeLock lock(&WorkSection);

			if (!Suspended) {
				WorkCondition.Wait(&WorkSection, 1);
			}
		}
	}
	if (GetCurrentFiber() != TlsThreadFiber) {
		SwitchToFiber(TlsThreadFiber);
	}
}

}