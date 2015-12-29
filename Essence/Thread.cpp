#include <intrin.h>
#include "Thread.h"
#include "AssertionMacros.h"

u32 GMainThreadId;

namespace Essence {

bool IsMainThread() {
	Check(GMainThreadId);
	return GetThreadId() == GMainThreadId;
}

void SetAsMainThread() {
	GMainThreadId = GetThreadId();
}

u32 GetThreadId() {
	// from handmade hero
	auto TLS = (u8*)__readgsqword(0x30);
	return *(u32*)(TLS + 0x48);
}

}