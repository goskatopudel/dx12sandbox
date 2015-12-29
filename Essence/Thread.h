#pragma once
#include "Types.h"
#include "Memory.h"

#include <windows.h>
#include <synchapi.h>

#include <atomic>

typedef std::atomic_bool		abool;
typedef std::atomic_int16_t		ai16;
typedef std::atomic_int32_t		ai32;
typedef std::atomic_int64_t		ai64;
typedef std::atomic_uint16_t	au16;
typedef std::atomic_uint32_t	au32;
typedef std::atomic_uint64_t	au64;

extern u32 GMainThreadId;

namespace Essence {

u32		GetThreadId();
bool	IsMainThread();
void	SetAsMainThread();

struct CriticalSection {
public:
	CRITICAL_SECTION WinCriticalSection;

	FORCEINLINE CriticalSection() {
		InitializeCriticalSectionAndSpinCount(&WinCriticalSection, 200);
	}

	FORCEINLINE ~CriticalSection() {
		DeleteCriticalSection(&WinCriticalSection);
	}

	FORCEINLINE bool TryLock() {
		return TryEnterCriticalSection(&WinCriticalSection) > 0;
	}

	FORCEINLINE void Lock() {
		EnterCriticalSection(&WinCriticalSection);
	}

	FORCEINLINE void Unlock() {
		LeaveCriticalSection(&WinCriticalSection);
	}
};

struct ScopeLock {
	CriticalSection* Owner;

	FORCEINLINE ScopeLock(CriticalSection* owner) : Owner(owner) {
		Owner->Lock();
	}

	FORCEINLINE ~ScopeLock() {
		Owner->Unlock();
	}

	ScopeLock(ScopeLock const& other) = delete;
};

struct RWLock {
	SRWLOCK WinSRWLock;

	FORCEINLINE RWLock() {
		InitializeSRWLock(&WinSRWLock);
	}

	FORCEINLINE ~RWLock() {
		ReleaseSRWLockExclusive(&WinSRWLock);
	}

	RWLock(const RWLock& other) = delete;

	FORCEINLINE void LockShared() {
		AcquireSRWLockShared(&WinSRWLock);
	}

	FORCEINLINE void UnlockShared() {
		ReleaseSRWLockShared(&WinSRWLock);
	}

	FORCEINLINE void LockExclusive() {
		AcquireSRWLockExclusive(&WinSRWLock);
	}

	FORCEINLINE void UnlockExclusive() {
		ReleaseSRWLockExclusive(&WinSRWLock);
	}
};

struct ReaderScope {
	RWLock* Owner;

	FORCEINLINE ReaderScope(RWLock* owner) : Owner(owner) {
		Owner->LockShared();
	}

	FORCEINLINE ~ReaderScope() {
		Owner->UnlockShared();
	}

	ReaderScope(ReaderScope const& other) = delete;
};

struct ReaderToWriterScope {
	RWLock* Owner;

	FORCEINLINE ReaderToWriterScope(RWLock* owner) : Owner(owner) {
		Owner->UnlockShared();
		Owner->LockExclusive();
	}

	FORCEINLINE ~ReaderToWriterScope() {
		Owner->UnlockExclusive();
		Owner->LockShared();
	}

	ReaderToWriterScope(ReaderToWriterScope const& other) = delete;
};

struct ConditionVariable {
	CONDITION_VARIABLE WinCV;

	ConditionVariable() {
		InitializeConditionVariable(&WinCV);
	}

	FORCEINLINE bool Wait(CriticalSection* CS, int ms = 0xFFFFFFFF) {
		return SleepConditionVariableCS(&WinCV, &CS->WinCriticalSection, ms) != 0;
	}

	FORCEINLINE bool Wait(RWLock* RWL, bool shared = false, int ms = 0xFFFFFFFF) {
		return SleepConditionVariableSRW(&WinCV, &RWL->WinSRWLock, ms, shared ? CONDITION_VARIABLE_LOCKMODE_SHARED : 0) != 0;
	}

	FORCEINLINE void WakeOne() {
		WakeConditionVariable(&WinCV);
	}

	FORCEINLINE void WakeAll() {
		WakeAllConditionVariable(&WinCV);
	}
};

}
