#include <string.h>
#include "lest.hpp"

#include "Array.h"
#include "Memory.h"

#include "Algorithms.h"

Essence::Array<i32> GArray;

void TestArray(int argc, char * argv[]) {
	using namespace Essence;

	const lest::test specification[] = {
		CASE("array basic operations") {
		Essence::Array<i32> A;
		A = Array<i32>(GetMallocAllocator());
		GArray = Array<i32>(GetMallocAllocator());

		PushBack(A, 10);
		PushBack(GArray, 10);

		Expand(A, 100);

		Resize(GArray, 10000);

		EXPECT(Size(A) == 1);
		EXPECT(A.Capacity == 100);
		EXPECT(Size(GArray) == 10000);
		EXPECT(GArray.Capacity >= 10000u);

		Resize(A, 0);
		Trim(A);
		EXPECT(A.Capacity == 0);

		PushBack(A, 1);
		PushBack(A, 2);
		PushBack(A, 3);
		PushBack(A, 4);
		EXPECT(A[0] == 1);
		EXPECT(Front(A) == 1);
		EXPECT(Back(A) == 4);
		PopBack(A);
		EXPECT(Back(A) == 3);
		Remove(A, 1);
		EXPECT(Back(A) == 3);
		RemoveAndSwap(A, 0);
		EXPECT(Back(A) == 3);
		EXPECT(Front(A) == 3);

		EXPECT(Size(A) == 1);

		Clear(A);

		i32 test[] = { 1, 2, 3, 4, 5, 6 };
		Append(A, test, 6);
		RemoveAll(A, [](i32 val) { return val % 2; });
		EXPECT(Size(A) == 3);
		EXPECT(A[2] == 6);

		Clear(A);

		i32 test1[] = { 1, 2, 2, 3, 3, 3, 4, 5, 6, 6 };
		Append(A, test1, _countof(test1));
		RemoveAll(A, [](i32 val) { return val % 2; });
		EXPECT(Size(A) == 5);
		EXPECT(A[0] == 2);
		EXPECT(A[1] == 2);

		Clear(A);

		for (int i = 0; i<1024; ++i) {
			PushBack(A, i);
		}

		for (int i = 1023; i >= 0; --i) {
			EXPECT(Size(A) == i + 1);
			EXPECT(A[i] == i);
			PopBack(A);
		}

		Clear(GArray);
		i32 data[] = { 6, 5, 4, 3, 2, 1 };
		Append(GArray, data, _countof(data));
		EXPECT(GArray.Size == 6);
		Remove(GArray, 1);
		Remove(GArray, 1);
		Remove(GArray, 1);
		EXPECT(GArray[1] == 2);

		auto sum = 0;
		for (auto x : GArray) {
			sum += x;
		}

		for (auto const& x : GArray) {
			sum += x;
		}

		EXPECT(sum == 18);

		A = GArray;

		Essence::Array<i32> B;
		B = std::move(A);

		sum = 0;
		for (auto x : A) {
			sum += x;
		}
		EXPECT(sum == 0);

		sum = 0;
		for (auto x : B) {
			sum += x;
		}
		EXPECT(sum == 9);

		auto C = Array<i32>(GetScratchAllocator());
		for (auto i = 0; i < 1024 * 1024; ++i) {
			PushBack(C, i);
		}

		FreeMemory(GArray);
	}
	};

	Essence::InitMemoryAllocators();
	lest::run(specification, argc, argv);
	Essence::ShutdownMemoryAllocators();
}

#include "Hashmap.h"
Essence::Hashmap<i32, i32> GHashmap;

void TestHashmap(int argc, char * argv[]) {
	using namespace Essence;

	const lest::test specification[] = {
		CASE("hashmap basic operations") {
			EXPECT(Get(GHashmap, 0) == nullptr);

			GHashmap = Essence::Hashmap<i32, i32>(GetMallocAllocator());

			EXPECT(Set(GHashmap, 0, 1) == true);
			EXPECT(Set(GHashmap, 0, 2) == false);
			EXPECT(Set(GHashmap, 0, 3) == false);
			EXPECT(Set(GHashmap, 1, 3) == true);
			EXPECT(Set(GHashmap, 2, 3) == true);

			EXPECT(Size(GHashmap) == 3);
			
			EXPECT(Remove(GHashmap, 0) == true);
			EXPECT(Remove(GHashmap, 0) == false);

			auto A = Hashmap<i32, i64>(GetThreadScratchAllocator());
			for (auto i = 0; i < 100000; ++i) {
				Set(A, i, (i64)i);
			}
			EXPECT(Size(A) == 100000);

			EXPECT(Remove(A, 50000) == true);
			EXPECT(Remove(A, 50000) == false);

			EXPECT(A[1000] == 1000);

			auto B = std::move(A);

			auto C = Hashmap<i32, i64>(GetMallocAllocator());
			Set(C, 0, 0ll);
			Set(C, 1, 0ll);
			Set(C, 2, 0ll);
			Set(C, 3, 5ll);

			auto sum = 0;
			for (auto kv : C) {
				sum += (i32)kv.value;
			}
			EXPECT(sum == 5);

			sum = 0;
			for (auto const& kv : C) {
				sum += kv.key;
				sum += (i32)kv.value;
			}
			EXPECT(sum == 11);

			FreeMemory(GHashmap);
		}
	};

	Essence::InitMemoryAllocators();
	lest::run(specification, argc, argv);
	Essence::ShutdownMemoryAllocators();
}

#include "Ringbuffer.h"
Essence::Ringbuffer<i32> GRingbuffer;

void TestCollections(int argc, char * argv[]) {
	using namespace Essence;

	const lest::test specification[] = {
		CASE("rigbuffer basic operations") {

			PushBack(GRingbuffer, 1);
			PushBack(GRingbuffer, 2);
			PushBack(GRingbuffer, 3);

			EXPECT(Front(GRingbuffer) == 1);
			EXPECT(Back(GRingbuffer) == 3);

			PopBack(GRingbuffer);

			EXPECT(Front(GRingbuffer) == 1);
			EXPECT(Back(GRingbuffer) == 2);

			EXPECT(Size(GRingbuffer) == 2);

			PushFront(GRingbuffer, 7);
			PushFront(GRingbuffer, 6);
			PushFront(GRingbuffer, 5);

			EXPECT(Front(GRingbuffer) == 5);

			PopFront(GRingbuffer);

			EXPECT(Front(GRingbuffer) == 6);

			FreeMemory(GRingbuffer);
		}
	};

	Essence::InitMemoryAllocators();
	lest::run(specification, argc, argv);
	Essence::ShutdownMemoryAllocators();
}

#include "String.h"
#include "Strings.h"
void TestString(int argc, char * argv[]) {
	using namespace Essence;

	const lest::test specification[] = {
		CASE("string basic operations") {
	
		Essence::AString string(GetThreadScratchAllocator());

		string.Append('a');
		string.Append('b');
		string.Append('c');

		auto b = Copy(string, GetMallocAllocator());

		auto c = Format("b is %s", (const char*)b);
		EXPECT(c == ScratchString("b is abc"));

		auto res1 = NAME_("Texture.cpp");
		auto text = TEXT_("AbAbAb");

		EXPECT(GetString(res1) == ScratchString("Texture.cpp"));
		EXPECT(GetString(text) == ScratchString("AbAbAb"));

		EXPECT(res1 == NAME_("texture.cpp"));

		EXPECT(GetString(Essence::ResourceNameId()) == ScratchString(""));
		EXPECT(GetString(Essence::TextId()) == ScratchString(""));

		FreeMemory(c);
		FreeStringsMemory();
	}
	};

	Essence::InitMemoryAllocators();
	lest::run(specification, argc, argv);
	Essence::ShutdownMemoryAllocators();
}

#include "Thread.h"
#include "Debug.h"
#include "Scheduler.h"

void TestScheduler(int argc, char * argv[]) {
	using namespace Essence;

	SetAsMainThread();

	const lest::test specification[] = {
		CASE("scheduler test 1") {
			InitScheduler();

			auto rootJobFun = [](const void*, Job*) {
				for (auto i = 0; i < 100; ++i) {
					_mm_pause();
				}
			};

			auto rootJob = CreateJob(rootJobFun, nullptr);
			RunJobs(&rootJob, 1);

			WaitFor(rootJob, false);

			ShutdownScheduler();
		}
	};

	const lest::test specification1[] = {
		CASE("scheduler test 2 - 2-level tree") {
		InitScheduler();

		auto rootJobFun = [](const void*, Job* job) {
			auto childJobFun = [](const void*, Job*) {
				for (auto i = 0; i < 1000; ++i) {
					_mm_pause(); _mm_pause();
					_mm_pause(); _mm_pause();
					_mm_pause(); _mm_pause();
					_mm_pause(); _mm_pause();
				}
			};

			Job* children[40];
			for (auto i : i32Range(_countof(children))) {
				children[i] = CreateChildJob(job, childJobFun, nullptr);
			}

			RunJobs(children, _countof(children));
		};

		auto rootJob = CreateJob(rootJobFun, nullptr);
		RunJobs(&rootJob, 1);

		WaitFor(rootJob, true);

		ShutdownScheduler();
	},
		CASE("scheduler test 3 - pipe") {
		InitScheduler();

		struct args_t {
			int x;
			int y;
			Essence::job_function_t func;
		};

		auto recJobFun = [](const void* pargs, Job* job) {
			auto args = (args_t*)pargs;
			for (auto i = 0; i < 1000; ++i) {
				_mm_pause();
			}

			if (args->x > 0) {
				args->x--;
				args->y++;
				auto child = CreateChildJob(job, args->func, args);
				RunJobs(&child, 1);
			}
		};

		const auto N = 20;

		args_t args;
		args.x = N;
		args.y = 0;
		args.func = recJobFun;

		auto rootJob = CreateJob(recJobFun, &args);
		RunJobs(&rootJob, 1);

		WaitFor(rootJob, false);

		EXPECT(args.y == N);

		ShutdownScheduler();
	}
	};

	Essence::InitMemoryAllocators();
	for (auto i : i32Range(20)) {
		lest::run(specification, argc, argv);
	}
	lest::run(specification1, argc, argv);
	Essence::ShutdownMemoryAllocators();
}

#if 1

#include "JobScheduler.h"

using namespace Essence;

void child1(void*) {
	for (int i = 0; i < 100; i++) {
		_mm_pause();
	}
}

void child(void*) {
	for (int i = 0; i < 100; i++) {
		_mm_pause();
	}

	job_desc_t jobs[10];
	for (int i = 0; i < 10; ++i) {
		jobs[i] = { child1, nullptr };
	}
	OutputDebugStringA("child pre\n");
	WaitForCompletion(ScheduleJobs(jobs, 10));
	OutputDebugStringA("child post\n");
}

void root(void*) {
	job_desc_t jobs[10];
	for (int i = 0; i < 10; ++i) {
		jobs[i] = { child, nullptr };
	}
	OutputDebugStringA("root pre\n");
	WaitForCompletion(ScheduleJobs(jobs, 10));
	OutputDebugStringA("root post\n");
	OutputDebugStringA("root pre\n");
	WaitForCompletion(ScheduleJobs(jobs, 10));
	OutputDebugStringA("root post\n");
}

struct root2_params {
	u32 depth;
};

void root2(void* pVoidParams) {
	u32 depth = *(u32*)pVoidParams;
	
	if (depth == 0) {
		OutputDebugStringA("root pre\n");
		job_desc_t jobs[10];
		root2_params params[10];
		for (int i = 0; i < 10; ++i) {
			params[i].depth = 1;
			jobs[i] = { root2, &params[i] };
		}
		WaitForCompletion(ScheduleJobs(jobs, 10));
		OutputDebugStringA("root post\n");
	}
	else if (depth == 1) {
		for (int i = 0; i < 10; ++i) {
			_mm_pause();
		}
		job_desc_t jobs[10];
		root2_params params[10];
		for (int i = 0; i < 10; ++i) {
			params[i].depth = 2;
			jobs[i] = { root2, &params[i] };
		}
		WaitForCompletion(ScheduleJobs(jobs, 10));
	}
	else if (depth == 2) {
		for (int i = 0; i < 100; ++i) {
			_mm_pause();
		}
	}
}

int main(int argc, char * argv[]) {
	/*TestArray(argc, argv);
	TestHashmap(argc, argv);
	TestCollections(argc, argv);
	TestString(argc, argv);
	TestScheduler(argc, argv);*/

	Essence::InitMemoryAllocators();

	

	InitJobScheduler();

	job_desc_t job = { root, nullptr };
	auto waitable = ScheduleJobs(&job, 1);
	WaitForCompletion(waitable);

	root2_params params;
	params.depth = 0;
	job = { root2, &params };
	WaitForCompletion(ScheduleJobs(&job, 1));

	ShutdownJobScheduler();

	Essence::ShutdownMemoryAllocators();

	return 0;
}

#endif