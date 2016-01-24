#pragma once

#include "Essence.h"

namespace Essence {

using job_func_t = void(*)(void*);

struct job_desc_t {
	job_func_t	func;
	void*		p_args;
};

struct job_id {
	u32 index;
};

struct job_waitable_id {
	u32 index;
};

job_waitable_id ScheduleJobs(job_desc_t* jobs, u32 num);
void WaitForCompletion(job_waitable_id waitable);
bool IsCompleted(job_waitable_id waitable);
void InitJobScheduler();
void ShutdownJobScheduler();

}
