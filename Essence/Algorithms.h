#pragma once

#include "Types.h"


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

template<typename T>
void swap(T* data, size_t x, size_t y) {
	T tmp = data[x];
	data[x] = data[y];
	data[y] = tmp;
}

template<typename T>
void swap(T& a, T& b) {
	T tmp = a;
	a = b;
	b = tmp;
}

template<typename T, typename Pred>
void insertion_sort(T* data, size_t start, size_t end, Pred pred) {
	for (auto i = start + 1; i < end; ++i) {
		auto j = i;

		while (j > start && pred(data[j], data[j - 1])) {
			swap(data, j, j - 1);
			--j;
		}
	}
}

template<typename T, typename Pred>
void quicksort(T* data, size_t start, size_t end, Pred pred) {
	auto N = end - start;
	if (N > 8) {
		auto pivot = end - 1;
		auto pivotValue = data[pivot];

		auto divider = start;
		for (auto i = start; i<end; ++i) {
			if (pred(data[i], pivotValue)) {
				swap(data, i, divider);
				++divider;
			}
		}

		swap(data, divider, pivot);

		quicksort(data, start, divider, pred);
		quicksort(data, divider + 1, end, pred);
	}
	else if (N > 1) {
		insertion_sort(data, start, end, pred);
	}
}