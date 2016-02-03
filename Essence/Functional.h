#pragma once

template <typename F>
struct ScopeExit {
	ScopeExit(F f) : f(f) {}
	~ScopeExit() { f(); }
	F f;
};

template <typename F>
ScopeExit<F> MakeScopeExit(F f) {
	return ScopeExit<F>(f);
};

#define STRING_JOIN2(arg1, arg2) DO_STRING_JOIN2(arg1, arg2)
#define DO_STRING_JOIN2(arg1, arg2) arg1 ## arg2
#define SCOPE_EXIT(code) \
    auto STRING_JOIN2(scope_exit_, __LINE__) = MakeScopeExit([=](){code;})


template<typename T>
struct Range {
	T		from;
	const T	to;

	Range(T to) : from((T)0), to(to) {
	}

	Range(T from, T to) : from(from), to(to) {
	}

	Range begin() const {
		return *this;
	}

	Range end() const {
		return Range(to, to);
	}

	Range& operator++() {
		++from;
		return *this;
	}

	bool operator==(const Range& rhs) const {
		return from == rhs.from && to == rhs.to;
	}

	bool operator!=(const Range& rhs) const {
		return from != rhs.from || to != rhs.to;
	}

	T operator*() const {
		return from;
	}
};

typedef Range<i32> i32Range;
typedef Range<u32> u32Range;

template<typename T> Range<T> MakeRange(T val) { return Range<T>(val); }
template<typename T> Range<T> MakeRange(T A, T B) { return Range<T>(A, B); }