#pragma once

#include "Types.h"
#include "Functional.h"
#include "Memory.h"
#include "Thread.h"
#include "Strings.h"
#include "Maths.h"
#include "Debug.h"
#include "Collections.h"
#include "Views.h"
#include "Algorithms.h"
#include "Hash.h"
#include "Pointers.h"
#include "Profiler.h"

namespace Essence {

// main thread only!
void InitMainThread();
void ShutdownMainThread();

void InitWorkerThread(u32 index);
void ShutdownWorkerThread();

};