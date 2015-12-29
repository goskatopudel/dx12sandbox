#include <cstring>
#include "Hash.h"
#include "Strings.h"

namespace Hash {


u32 Combine_32(u32 h1, u32 h2) {
	return h1 + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
}

u64 Combine_64(u64 h1, u64 h2) {
	u64 kmul = 0x9ddfea08eb382d69ull;
	u64 a = (h1 ^ h2) * kmul;
	a ^= (a >> 47);
	u64 b = (h2 ^ a) * kmul;
	b ^= (b >> 47);
	return b * kmul;
}

}

namespace Essence {

string_context_t::string_context_t(const_char_wrapper_t str) {
	string = str.ptr;
	length = strlen(string);
	hash = Hash::MurmurHash2_64(string, (u32)length, 0);
}

string_case_invariant_context_t::string_case_invariant_context_t(const_char_wrapper_t str) {
	string = str.ptr;
	length = strlen(string);

	auto tmp = ScratchString(str.ptr);
	tmp.ToLower();
	hash = Hash::MurmurHash2_64((const char*)tmp, (u32)length, 0);
}

}