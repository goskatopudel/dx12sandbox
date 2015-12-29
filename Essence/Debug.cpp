#include <Windows.h>
#include "Debug.h"
#include "Hashmap.h"
#include "Thread.h"

namespace Essence {

void ConsolePrint(const char* str) {
	OutputDebugStringA(str);
}

Hashmap<u64, u64>	WarningsIssued;
RWLock				WarningsRWL;

void FreeWarningsMemory() {
	FreeMemory(WarningsIssued);
}

void ClearWarnings(u64 category) {
	WarningsRWL.LockExclusive();
	Array<u64> RemoveList;

	for (auto kv : WarningsIssued) {
		if (kv.value == category) {
			PushBack(RemoveList, kv.key);
		}
	}
	for (auto k : RemoveList) {
		Remove(WarningsIssued, k);
	}
	WarningsRWL.UnlockExclusive();
}

void Warning(const char* message, bool one_time, u64 category) {
	auto hash = Hash::MurmurHash2_64(message, strlen(message), 0);

	bool print = !one_time;

	if (one_time) {
		WarningsRWL.LockShared();
		if (!Contains(WarningsIssued, hash)) {
			print = true;
			WarningsRWL.UnlockShared();
			WarningsRWL.LockExclusive();
			WarningsIssued[hash] = category;
			WarningsRWL.UnlockExclusive();
		}
		else {
			WarningsRWL.UnlockShared();
		}
	}

	if (print) {
		ConsolePrint("WARNING: ");
		ConsolePrint(message);
	}
}


}