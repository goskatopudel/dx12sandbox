#pragma once

#include "String.h"
#include "Types.h"

#define debugf(x) Essence::ConsolePrint(x)

namespace Essence {

void ConsolePrint(const char* str);

void ClearWarnings(u64 category);
void Warning(const char* message, bool one_time, u64 category);

void FreeWarningsMemory();

}