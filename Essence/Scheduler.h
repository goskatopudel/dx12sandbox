#pragma once

#include "Types.h"
#include "Thread.h"

namespace Essence {

struct Job;

typedef void(*job_function_t)(const void*, Job*);

struct Job {
	job_function_t	Function;
	const void*		Arguments;
	// 1 for unifinished, +1 for each unfinished child task
	ai32			Pending;
	Job*			Parent;
};

// TaskQueue<task_t>

void	InitScheduler();
void	ShutdownScheduler();

Job*	CreateJob(job_function_t function, const void* arguments);
Job*	CreateChildJob(Job* parent, job_function_t function, const void* arguments);

void	RunJobs(Job** jobs, u32 num);

bool	IsJobCompleted(Job* job);
void	WaitFor(Job* job, bool actively);
void	WaitForAll();

void	EndSchedulerFrame();

};