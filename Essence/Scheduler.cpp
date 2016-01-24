#include "Scheduler.h"
#include "Thread.h"
#include "AssertionMacros.h"
#include <thread>
#include <condition_variable>
#include "Array.h"
#include "Ringbuffer.h"
#include "Debug.h"
#include "Essence.h"
#include "Profiler.h"

namespace Essence {

const u32 MaxWorkerThreads = 32;

CACHE_ALIGN	abool		RunWorkers;

CriticalSection			JobQueueCS;
Ringbuffer<Job*>		JobQueue;

Array<Job*>				DeletionList;
CriticalSection			DeletionListCS;

void FinishJob(Job*);

Job*	TryStealJob() {
	ScopeLock lock(&JobQueueCS);

	Job* job = nullptr;
	if (Size(JobQueue)) {
		job = Front(JobQueue);
		PopFront(JobQueue);
	}

	return job;
}

CriticalSection		WorkCS;
ConditionVariable	WorkCV;
ConditionVariable	CompletionCV;

CACHE_ALIGN struct WorkerThread {
	u32						ThreadId;
	u32						Index;
	std::thread				Thread;

	Job*	TryDequeueJob() {
		ScopeLock lock(&JobQueueCS);

		Job* job = nullptr;
		if (Size(JobQueue)) {
			job = Front(JobQueue);
			PopFront(JobQueue);
		}

		return job;
	}

	void Run() {
		ThreadId = GetThreadId();
		Check(!IsMainThread());

		InitWorkerThread(Index);

		while (RunWorkers) {

			auto pJob = TryDequeueJob();
			
			if (pJob == nullptr) {
				pJob = TryStealJob();
			}

			if (pJob == nullptr) {
				ScopeLock lock(&WorkCS);

				if (RunWorkers) {
					PROFILE_SCOPE(worker_wait_for_work);
					WorkCV.Wait(&WorkCS);
				}
			}
			else {
				pJob->Function(pJob->Arguments, pJob);

				FinishJob(pJob);
			}
		}

		ShutdownWorkerThread();
	};
};

WorkerThread		WorkerThreads[MaxWorkerThreads];
u32					WorkerThreadsNum;

void	InitScheduler() {
	WorkerThreadsNum = std::thread::hardware_concurrency() - 1;

	RunWorkers = true;

	for (auto i = 0u; i< WorkerThreadsNum; ++i) {
		WorkerThreads[i].Index = i;
		WorkerThreads[i].Thread = std::thread(&WorkerThread::Run, WorkerThreads + i);
	}
}

void	ShutdownScheduler() {
	RunWorkers = false;

	{
		ScopeLock lock(&WorkCS);
		WorkCV.WakeAll();
	}

	for (auto i = 0u; i<WorkerThreadsNum; ++i) {
		WorkerThreads[i].Thread.join();
	}

	EndSchedulerFrame();

	FreeMemory(JobQueue);
	FreeMemory(DeletionList);
}

Job*	AllocateJob() {
	auto job = (Job*)GetMallocAllocator()->Allocate(sizeof(Job), alignof(Job));

	ScopeLock lock(&DeletionListCS);
	PushBack(DeletionList, job);

	return job;
}

void	FreeJob(Job* job) {
	GetMallocAllocator()->Free(job);
}

void FinishJob(Job* job) {
	auto prevPending = job->Pending--;
	if (job->Parent && prevPending == 1) {
		FinishJob(job->Parent);
	}

	if (prevPending == 1) {
		// to make sure we don't wake between checking condition and sleeping (deadlock)
		ScopeLock lock(&WorkCS);
		CompletionCV.WakeAll();
	}
}

void EndSchedulerFrame() {
	ScopeLock lock(&DeletionListCS);
	for (auto ptr : DeletionList) {
		FreeJob(ptr);
	}
	Clear(DeletionList);
}

bool	IsJobCompleted(Job* job) {
	return job->Pending == 0;
}

Job*	CreateJob(job_function_t function, const void* arguments) {
	auto job = AllocateJob();
	job->Function = function;
	job->Arguments = arguments;
	job->Parent = nullptr;
	job->Pending = 1;
	return job;
}

Job*	CreateChildJob(Job* parent, job_function_t function, const void* arguments) {
	auto job = AllocateJob();
	job->Function = function;
	job->Arguments = arguments;
	job->Parent = parent;
	job->Pending = 1;
	parent->Pending++;
	return job;
}

void	RunJobs(Job** jobs, u32 num) {
	{
		ScopeLock lock(&JobQueueCS);

		for (auto i = 0u; i < num; ++i) {
			PushBack(JobQueue, jobs[i]);
		}
	}

	WorkCV.WakeAll();
}

void	WaitFor(Job* job, bool actively) {
	if (IsJobCompleted(job)) {
		return;
	}

	if (actively) {
		auto pJob = TryStealJob();
		while (pJob) {
			pJob->Function(pJob->Arguments, pJob);

			FinishJob(pJob);

			if (IsJobCompleted(job)) {
				return;
			}

			pJob = TryStealJob();
		}

		WaitFor(job, false);
	}
	else {
		ScopeLock lock(&WorkCS);
		while (!IsJobCompleted(job)) {
			CompletionCV.Wait(&WorkCS);
		}
	}
}

void	WaitForAll() {
	Check(0);
}

};