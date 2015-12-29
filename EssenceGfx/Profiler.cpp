#include "Profiler.h"
#include "AssertionMacros.h"

Remotery *GRMT;

void InitProfiler() {
	Verify(RMT_ERROR_NONE == rmt_CreateGlobalInstance(&GRMT));
}

void ShutdownProfiler() {
	rmt_DestroyGlobalInstance(GRMT);
}
