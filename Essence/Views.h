#pragma once
#include "Collections.h"

namespace Essence {

template<typename T>
struct array_view {
	T*	elements;
	u32 num;

	array_view() = default;
	array_view(T* ptr, u32 size) : elements(ptr), num(size) {};

	T& operator [](i64 index) {
		Check(index >= 0 && index < (i32)num);
		return elements[index];
	}

	T const & operator [](i64 index) const {
		Check(index >= 0 && index < (i32)num);
		return elements[index];
	}
};

template<typename T>
void allocate_array(array_view<T> *a, u32 size, IAllocator* allocator) {
	(*a) = array_view<T>((T*)allocator->Allocate(sizeof(T) * size, alignof(T)), size);
}

template<typename T>
void zero_array(array_view<T> *a) {
	ZeroMemory(a->elements, sizeof(T) * a->num);
}

template<typename T>
Array<T> wrap_c_array(T* ptr, u32 size) {
	Array<T> wrapped;
	wrapped.Allocator = nullptr;
	wrapped.Capacity = wrapped.Size = size;
	wrapped.DataPtr = ptr;
	return std::move(wrapped);
}

}