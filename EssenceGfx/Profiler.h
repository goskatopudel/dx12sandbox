#pragma once

#include "remotery\Remotery.h"

struct ProfileScopeGuard {
	inline ~ProfileScopeGuard() {
		rmt_EndCPUSample();
	}
};

#define PROFILE_BEGIN(LABEL)	rmt_BeginCPUSample(LABEL);
#define PROFILE_END				rmt_EndCPUSample();
#define PROFILE_SCOPE(LABEL)	rmt_BeginCPUSample(LABEL); ProfileScopeGuard guard__##LABEL##__LINE__;

void InitProfiler();
void ShutdownProfiler();
