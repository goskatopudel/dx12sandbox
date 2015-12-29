#include <ctype.h>
#include <cstdio>
#include <cstring>
#include "Strings.h"
#include "Array.h"
#include "Types.h"

namespace Essence {

AString::AString(IAllocator *allocator) : Chars(allocator) {
	PushBack(Chars, (char)0);
}

AString::AString(Array<char> chars) : Chars(chars) {
	Check(Back(Chars) == 0);
}

AString::AString(IAllocator *allocator, const char* src) : AString(allocator) {
	Append(src);
}

AString::AString(AString&& other) {
	Chars = std::move(other.Chars);
}

AString::AString(AString const& src, u64 from, u64 to) : AString(src.Chars.Allocator) {
	Check(from <= to);
	auto len = min(src.Length(), to - from);
	PopBack(Chars);
	Reserve(Chars, len + 1);
	Essence::Append(Chars, &src.Chars[from], len);
	PushBack(Chars, (char)0);
}

u64 AString::Length() const {
	return Size(Chars) - 1;
}

AString::operator const char*() const {
	return Chars.DataPtr;
}

AString& AString::Append(char c) {
	Chars[Size(Chars) - 1] = c;
	PushBack(Chars, (char)0);
	return *this;
}

AString& AString::Append(const char * src) {
	auto len = strlen(src);
	
	PopBack(Chars);
	Essence::Append(Chars, src, (u32)len + 1);

	return *this;
}

AString& AString::Append(const char * src, size_t num) {
	PopBack(Chars);
	Essence::Append(Chars, src, (u32)num);
	Append((char)0);
	return *this;
}

AString& AString::Append(AString const& src) {
	PopBack(Chars);
	Essence::Append(Chars, src.Chars.DataPtr, src.Length() + 1);

	return *this;
}

AString& AString::Append(const wchar_t * src) {
	auto length = wcslen(src);
	char buffer[1024];
	if (length < _countof(buffer) - 1) {
		u64 writtenSize;
		wcstombs_s(&writtenSize, buffer, src, _countof(buffer) - 1);
	}
	else {
		Check(0);
	}

	return Append(buffer, _countof(buffer));
}

AString& AString::ToLower() {
	auto N = Length();
	for (auto i = 0u;i < N;++i) {
		Chars[i] = tolower(Chars[i]);
	}
	return *this;
}

AString& AString::ToUpper() {
	auto N = Length();
	for (auto i = 0u;i < N;++i) {
		Chars[i] = toupper(Chars[i]);
	}
	return *this;
}

bool operator==(AString const& lhs, AString const& rhs) {
	if (lhs.Length() != rhs.Length()) {
		return false;
	}
	const auto N = lhs.Length();
	for (auto i = 0; i < N; ++i) {
		if (lhs.Chars[i] != rhs.Chars[i]) {
			return false;
		}
	}
	return true;
}

bool operator!=(AString const& lhs, AString const& rhs) {
	return !(lhs == rhs);
}

AString ScratchString(const char* str) {
	return std::move(AString(GetThreadScratchAllocator(), str));
}

AString ScratchString(const wchar_t* str) {
	auto obj = AString(GetThreadScratchAllocator());
	obj.Append(str);
	return std::move(obj);
}

void Trim(AString& string) {
	Trim(string.Chars);
}

// clears to single char!
void Clear(AString& string) {
	Clear(string.Chars);
	PushBack(string.Chars, (char)0);
}

void FreeMemory(AString& string) {
	// clear array
	Clear(string.Chars);
	Trim(string);
}

AString Copy(AString const& str, IAllocator* allocator) {
	AString copy(allocator);
	copy.Append(str);
	return std::move(copy);
}

}