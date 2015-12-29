#include "Random.h"

namespace Essence {

u32 wang32(u32 seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	return seed;
}

u32		random_generator::u32Next() {
	seed = wang32(seed);
	return seed;
}

u32		random_generator::u32Next(u32 to) {
	return u32Next() % to;
}

u32		random_generator::u32Next(u32 from, u32 to) {
	return u32Next() % (from - to) + from;
}

// returns [0,1) float from integer
float make_float(u32 m) {
	const u32 ieeeMantissa = 0x007FFFFFu;
	const u32 ieeeOne = 0x3F800000u;

	m &= ieeeMantissa;
	m |= ieeeOne;

	return *((float*)&m) - 1;
}
		
float	random_generator::f32Next() {
	return make_float(u32Next());
}

float	random_generator::f32Next(float to) {
	return f32Next() * to;
}

float	random_generator::f32Next(float from, float to) {
	return f32Next() * (to - from) + from;
}

}