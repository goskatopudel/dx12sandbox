#pragma once

#include "Collections.h"
#include "Memory.h"

namespace Essence {

template<typename T> Array<T>::Array(IAllocator* allocator) :
	Allocator(allocator),
	Size(0),
	Capacity(0),
	DataPtr(nullptr)
{
}

template<typename T> Array<T>::Array() :
	Array(GetMallocAllocator())
{
}

template<typename T> Array<T>::~Array() {
	if (DataPtr) {
		Check(Capacity);
		Check(Allocator);
		Allocator->Free(DataPtr);
	}
	DataPtr = nullptr;
	Allocator = nullptr;
}

template<typename T> Array<T>::Array(const Array &other) : Array()
{
	*this = other;
}

template<typename T> Array<T>::Array(Array &&other) : Array() {
	*this = other;
}

template<typename T> Array<T>& Array<T>::operator=(const Array &other) {
	if (this != &other) {
		if (Capacity) {
			Check(Allocator);
			FreeMemory(*this);
		}

		Allocator = other.Allocator;
		Append(*this, other.DataPtr, other.Size);
	}
	return *this;
}

template<typename T> Array<T>& Array<T>::operator=(Array &&other) {
	if (this != &other) {
		if (Capacity) {
			Check(Allocator);
			FreeMemory(*this);
			// todo: could optimize for same allocator, try to reuse memory instead of freeing if possible
		}

		Allocator = other.Allocator;
		Size = other.Size;
		Capacity = other.Capacity;
		DataPtr = other.DataPtr;

		other.Allocator = nullptr;
		other.Size = 0;
		other.Capacity = 0;
		other.DataPtr = nullptr;
	}

	return *this;
}

template<typename T> T&			At(Array<T>& A, u64 index) {
	Check(index >= 0 && index < A.Size);
	return A.DataPtr[index];
}

template<typename T> const T&	At(Array<T> const& A, u64 index) {
	Check(index >= 0 && index < A.Size);
	return A.DataPtr[index];
}

template<typename T> const T&	Array<T>::operator[](u64 index) const {
	return At(*this, index);
}

template<typename T> T&			Array<T>::operator[](u64 index) {
	return At(*this, index);
}

template<typename T> T&			Front(Array<T>& A) {
	return At(A, 0);
}

template<typename T> T&			Back(Array<T>& A) {
	return At(A, A.Size - 1);
}

template<typename T> size_t		Size(Array<T> const & A) {
	return A.Size;
}

template<typename T> void		PushBack(Array<T>& A, const T& v) {
	Expand(A, A.Size + 1);
	A.DataPtr[A.Size] = v;
	++A.Size;
}

template<typename T> void		PopBack(Array<T>& A) {
	--A.Size;
}

template<typename T> void		Append(Array<T>& A, const T* v, u64 num) {
	Expand(A, A.Size + num);
	memcpy(A.DataPtr + A.Size, v, num * sizeof(T));
	A.Size += num;
}

template<typename T> void		Trim(Array<T>& A) {
	if (A.DataPtr && A.Size == 0) {
		A.Allocator->Free(A.DataPtr);
		A.DataPtr = nullptr;
		A.Capacity = 0;
	}
	else if (A.Capacity > A.Size) {
		void* new_data = A.Allocator->Allocate(sizeof(T) * A.Size, __alignof(T));
		if (A.DataPtr) {
			memcpy(new_data, A.DataPtr, sizeof(T) * A.Size);
			A.Allocator->Free(A.DataPtr);
		}
		A.DataPtr = (T*)new_data;

		A.Capacity = A.Size;
	}
}

template<typename T> void		Clear(Array<T>& A) {
	A.Size = 0;
}

template<typename T> void		Resize(Array<T>& A, size_t size) {
	if (A.Capacity < size) {
		Reserve(A, size);
	}
	A.Size = size;
}

template<typename T> void		FreeMemory(Array<T>& A) {
	Clear(A);
	Trim(A);
	Check(A.DataPtr == nullptr);
	Check(A.Capacity == 0);
}

template<typename T> void		ResizeAndZero(Array<T>& A, size_t size) {
	Resize(A, size);
	memset(A.DataPtr + A.Size, 0, (A.Capacity - A.Size) * sizeof(T));
}

template<typename T> void		Expand(Array<T>& A, size_t minCapacity) {
	if (A.Capacity < minCapacity) {
		size_t expand = A.Capacity * 2 + 1;
		Reserve(A, max(expand, minCapacity));
	}
}

template<typename T> void		Reserve(Array<T>& A, size_t capacity) {
	//Check(capacity >= A.Capacity);
	if (A.Capacity < capacity) {
		void* new_data = A.Allocator->Allocate(sizeof(T) * capacity, __alignof(T));
		if (A.DataPtr) {
			memcpy(new_data, A.DataPtr, sizeof(T) * A.Size);
			A.Allocator->Free(A.DataPtr);
		}
		A.DataPtr = (T*)new_data;

		A.Capacity = capacity;
	}
}

template<typename T> void		Remove(Array<T>& A, u32 index) {
	Check(A.Size);
	Check(index < A.Size);
	memmove(A.DataPtr + index, A.DataPtr + index + 1, sizeof(T) * (A.Size - index - 1));
	--A.Size;
}

template<typename T> void		RemoveAndSwap(Array<T>& A, u32 index) {
	Check(A.Size);
	Check(index < A.Size);
	A.DataPtr[index] = A.DataPtr[A.Size - 1];
	--A.Size;
}

template<typename T, typename PRED> 
void	RemoveAll(Array<T>& A, PRED Pred) {
	u32 i = 0;
	u32 j = 0;
	auto N = Size(A);
	for (;i < N; ++i) {
		if (Pred(A[i])) {
			j = i + 1;
			for (; j < N; ++j) {
				if (!Pred(A[j])) {
					swap(A[i], A[j]);
					break;
				}
			}
		}
		if (Pred(A[i])) {
			break;
		}
	}
	A.Size = i;
}

//

template<typename T> ArrayIterator<T>& ArrayIterator<T>::operator++() {
	++Index;
	return *this;
}

template<typename T> bool ArrayIterator<T>::operator==(const ArrayIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index == rhs.Index;
}

template<typename T> bool ArrayIterator<T>::operator!=(const ArrayIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index != rhs.Index;
}

template<typename T> T& ArrayIterator<T>::operator*() const {
	return At(*Collection, Index);
}

template<typename T> ArrayConstIterator<T>& ArrayConstIterator<T>::operator++() {
	++Index;
	return *this;
}

template<typename T> bool ArrayConstIterator<T>::operator==(const ArrayConstIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index == rhs.Index;
}

template<typename T> bool ArrayConstIterator<T>::operator!=(const ArrayConstIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index != rhs.Index;
}

template<typename T> const T& ArrayConstIterator<T>::operator*() const {
	return At(*Collection, Index);
}

//

template<typename T> ArrayIterator<T> Array<T>::begin() {
	ArrayIterator<T> iter { this, 0};
	return iter;
}

template<typename T> ArrayIterator<T> Array<T>::end() {
	ArrayIterator<T> iter { this, (u32)Size };
	return iter;
}

template<typename T> ArrayConstIterator<T> Array<T>::cbegin() const {
	ArrayConstIterator<T> iter { this, 0 };
	return iter;
}

template<typename T> ArrayConstIterator<T> Array<T>::cend() const {
	ArrayConstIterator<T> iter { this, (u32)Size };
	return iter;
}

template<typename T>
Array<T> Copy(Array<T> const& A, IAllocator* allocator) {
	Array<T> Copied(allocator);
	Reserve(Copied, A.Capacity);
	Copied.Size = A.Size;
	memcpy(Copied.DataPtr, A.DataPtr, Size(A) * sizeof(T));

	return Copied;
}

}