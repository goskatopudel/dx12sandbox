#pragma once

namespace Essence {

#pragma once
#include "Types.h"

struct random_generator {
	u32 seed;

	random_generator(u32 seed = 0) : seed(seed) {
	}

	u32		u32Next();
	u32		u32Next(u32 to);
	u32		u32Next(u32 from, u32 to);

	float	f32Next(u32 m);
	float	f32Next();
	float	f32Next(float to);
	float	f32Next(float from, float to);
};

}