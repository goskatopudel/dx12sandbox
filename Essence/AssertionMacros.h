#pragma once

#include "GlobalMacros.h"
#include <assert.h>

#define RUN_ASSERTS _DEBUG || _PROFILE

bool handle_assertion_inner_loop(const char* condition, const char* file, int line, const char* function);

#if RUN_ASSERTS

#define Check(x) __Check((x), #x, __FILE__, __LINE__, __FUNCTION__)
#define __Check(COND, COND_MSG, FILE, LINE, FUNC)					\
while(!COND) {														\
	if(!handle_assertion_inner_loop(COND_MSG, FILE, LINE, FUNC)) {	\
		break;														\
	}																\
};

#define Verify(x) __Verify((x), #x, __FILE__, __LINE__, __FUNCTION__) 
#define __Verify(COND, COND_MSG, FILE, LINE, FUNC)						\
if(!COND) {																\
	handle_assertion_inner_loop(COND_MSG, FILE, LINE, FUNC);			\
}

#define VerifyHr(x) __VerifyHr((x), #x, __FILE__, __LINE__, __FUNCTION__) 
#define __VerifyHr(COND, COND_MSG, FILE, LINE, FUNC) 				\
if(COND < 0) {														\
	handle_assertion_inner_loop(COND_MSG, FILE, LINE, FUNC);		\
} 

#else

#define Check(x) {}
#define Verify(x) {x;}
#define VerifyHr(x) {x;}

#endif
