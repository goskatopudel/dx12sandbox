#pragma once

#include "remotery\Remotery.h"

struct ProfileScopeGuard {
	inline ~ProfileScopeGuard() {
		rmt_EndCPUSample();
	}
};

#define PROFILE_BEGIN(LABEL)		rmt_BeginCPUSample(LABEL);
#define PROFILE_END					rmt_EndCPUSample();
#define TOKENPASTE(x, y) x ## y
#define TOKENPASTE2(x, y) TOKENPASTE(x, y)
#define PROFILE_SCOPE(LABEL)		rmt_BeginCPUSample(LABEL); ProfileScopeGuard TOKENPASTE2(guard__##LABEL##, __LINE__);
#define PROFILE_NAME_THREAD(NAME)	rmt_SetCurrentThreadName(NAME);

void InitProfiler();
void ShutdownProfiler();
