#pragma once
#include "Types.h"
#include "String.h"
#include "Hash.h"
#include "Collections.h"
#include "AssertionMacros.h"
#include <cstdio>

namespace Essence {

class IAllocator;

typedef const char* cstr;

struct AString {
	Array<char> Chars;

	AString(IAllocator *allocator);
	AString(IAllocator *allocator, const char* src);
	AString(Array<char> chars);

	// copy
	AString(AString const& other, u64 from = 0, u64 to = -1);
	AString(AString&& other);

	u64 Length() const;

	operator const char *() const;

	AString& Append(char c);
	AString& Append(const char * src);
	AString& Append(const char * src, size_t num);
	AString& Append(const wchar_t * src);
	AString& Append(AString const& src);

	AString& ToLower();
	AString& ToUpper();
};

// equals
bool operator==(AString const& lhs, AString const& rhs);
bool operator!=(AString const& lhs, AString const& rhs);

void Trim(AString& string);
void Clear(AString& string);
void FreeMemory(AString& string);
AString Copy(AString const& str, IAllocator* allocator);
AString ScratchString(const char* str);
AString ScratchString(const wchar_t* str);

inline void FormattedAppend(AString& str, const char *format) {
	str.Append(format);
}

// todo: typesafty :(
template<typename T, typename... Args>
void FormattedAppend(AString& str, const char* format, T value, Args... args) {
	auto current = format;
	// move to first %
	while (*current != 0 && *current != '%') {
		++current;
	}
	if (*current != 0) {
		if (*(current + 1) == '%') {
			// double %%
			str.Append(format, (current - format) + 1);
			FormattedAppend(str, current + 2, value, args...);
		}
		else {
			++current;
			//find next %
			while (*current != 0 && *current != '%') {
				++current;
			}

			Check(str.Chars.Allocator);
			auto formatBuffer = (char*)str.Chars.Allocator->Allocate((current - format) + 1, 1);
			memcpy(formatBuffer, format, (current - format));
			formatBuffer[(current - format)] = 0;

			size_t startSize = 512;
			char* formattedBuffer = nullptr;
			while (true) {
				formattedBuffer = (char*)str.Chars.Allocator->Allocate(startSize, 1);
				// format using sprintf_s, exponential growth if we can't fit in
				auto result = _snprintf_s(formattedBuffer, startSize, startSize - 1, formatBuffer, value);
				//Check(result != -1);
				if (result != -1) {
					break;
				}
				str.Chars.Allocator->Free(formattedBuffer);
				startSize *= 2;
			}

			str.Append(formattedBuffer);
			str.Chars.Allocator->Free(formattedBuffer);
			str.Chars.Allocator->Free(formatBuffer);

			FormattedAppend(str, current, args...);
		}
	}
	else {
		// hit end of format string
		FormattedAppend(str, format);
	}
}

template<typename... Args>
AString Format(const char* format, Args... args) {
	AString scratchStr(GetThreadScratchAllocator());
	FormattedAppend(scratchStr, format, args...);
	return std::move(scratchStr);
}

struct ResourceNameId {
	u64 key;

	// TODO: check collisions in debug
	inline bool operator ==(ResourceNameId rhs) const {
		return key == rhs.key;
	}

	inline bool operator !=(ResourceNameId rhs) const {
		return key != rhs.key;
	}
};

struct TextId {
	u64 index;

	inline bool operator ==(TextId rhs) const {
		return index == rhs.index;
	}

	inline bool operator !=(TextId rhs) const {
		return index != rhs.index;
	}
};

void FreeStringsMemory();

AString GetString(ResourceNameId);
AString GetString(TextId);

ResourceNameId	GetResourceNameId(string_case_invariant_context_t);
TextId			GetTextId(string_context_t);

}

#define NAME_(x) (Essence::GetResourceNameId(string_case_invariant_context_t(x)))
#define TEXT_(x) (Essence::GetTextId(string_context_t(x)))