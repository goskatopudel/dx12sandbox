#pragma once
#include "Types.h"

#define TYPE_ID(TYPE_NAME)	Hash::MurmurHash2_64_CompileTime(#TYPE_NAME, length_cstring(#TYPE_NAME), 0)

namespace Hash {

struct hash128__ {
	u64 h;
	u64 l;
};

void MurmurHash3_x86_32(const void * key, int len, u32 seed, void * out);
void MurmurHash3_x86_128(const void * key, int len, u32 seed, void * out);
void MurmurHash3_x64_128(const void * key, int len, u32 seed, void * out);
u64 MurmurHash2_64(const void * key, u64 len, u64 seed);
constexpr u64 MurmurHash2_64_CompileTime(const char *data, size_t len, u64 seed);

inline hash128__ MurmurHash3_x64_128(const void * key, int len, u32 seed) {
	hash128__ v;
	MurmurHash3_x64_128(key, len, seed, &v);
	return v;
}

u32 Combine_32(u32 h1, u32 h2);
u64 Combine_64(u64 h1, u64 h2);

namespace internal
{
	// Murmur hash constants
	constexpr const uint64_t m = 0xc6a4a7935bd1e995ull;
	constexpr const int      r = 47;

	constexpr uint64_t crotate(uint64_t a)
	{
		return a ^ (a >> r);
	}

	constexpr uint64_t cfinalize_h(const char *data, size_t key, uint64_t h)
	{
		return (key != 0) ? cfinalize_h(data, key - 1, (h ^ (uint64_t(data[key - 1]) << (8 * (key - 1))))) : h * m;
	}

	constexpr uint64_t cfinalize(const char *data, size_t len, uint64_t h)
	{
		return (len & 7) ? crotate(crotate(cfinalize_h(data, len & 7, h)) * m)
			: crotate(crotate(h) * m);
	}

	// reinterpret cast is illegal (static is fine) so we have to manually load 64 bit chuncks of string instead
	// of casting char* to uint64_t*
	//
	// TODO - this only works on little endian machines .... fuuuu
	constexpr uint64_t cblock(const char *data, size_t offset = 0)
	{
		return (offset == 7) ? uint64_t(data[offset]) << (8 * offset)
			: (uint64_t(data[offset]) << (8 * offset)) | cblock(data, offset + 1);
	}

	// Mixing function for the hash function
	constexpr uint64_t cmix_h(const char *data, uint64_t h, size_t offset)
	{
		return (h ^ (crotate(cblock(data + offset) * m) * m)) * m;
	}

	// Control function for the mixing
	constexpr uint64_t cmix(const char *data, size_t len, uint64_t h, size_t offset = 0)
	{
		return (offset == (len & ~size_t(7))) ? cfinalize(data + offset, len, h)
			: cmix(data, len, cmix_h(data, h, offset), offset + 8);
	}
}

constexpr u64 MurmurHash2_64_CompileTime(const char *data, size_t len, u64 seed) {
	return internal::cmix(data, len, seed ^ (len * internal::m));
}

}

namespace Essence {

struct string_context_t
{
public:
	struct const_char_wrapper_t
	{
		const_char_wrapper_t(const char* str) : ptr(str) {}
		const char* ptr;
	};

	template <size_t N>
	inline explicit string_context_t(const char(&str)[N]) {
		string = str;
		length = N - 1;
		hash = Hash::MurmurHash2_64(str, N - 1, 0);
	}

	explicit string_context_t(const_char_wrapper_t str);

	const char* string;
	u64			length;
	u64			hash;
};

struct string_case_invariant_context_t
{
public:
	struct const_char_wrapper_t
	{
		const_char_wrapper_t(const char* str) : ptr(str) {}
		const char* ptr;
	};

	template <size_t N>
	inline explicit string_case_invariant_context_t(const char(&str)[N]) {
		string = str;
		length = N;

		auto tmp = ScratchString(str);
		tmp.ToLower();
		hash = Hash::MurmurHash2_64((const char*)tmp, (u32)N, 0);
	}

	explicit string_case_invariant_context_t(const_char_wrapper_t str);

	const char* string;
	u64			length;
	u64			hash;
};

constexpr u64 length_cstring(const char *data, const int idx = 0)
{
	return (data[idx] == '\0') ? idx : length_cstring(data, idx + 1);
}

}
