#pragma once

#include "Collections.h"

namespace Essence {

inline u32 next_pow2_size(u32 v) {
	--v;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	++v;
	return v;
}

template<typename T> Ringbuffer<T>::Ringbuffer() : Ringbuffer(GetMallocAllocator()) {}

template<typename T> Ringbuffer<T>::Ringbuffer(IAllocator *allocator) :
	Buffer(allocator), End(0), Begin(0), Size(0) {
}

template<typename T> void	Trim(Ringbuffer<T>& Rb) {
	if (Rb.Size == 0) {
		FreeMemory(Rb.Buffer);
	}
}

template<typename T> Ringbuffer<T>& Ringbuffer<T>::operator=(Ringbuffer const& other) {
	Buffer = other.Buffer;
	End = other.End;
	Begin = other.Begin;
	Size = other.Size;

	return *this;
}

template<typename T> Ringbuffer<T>& Ringbuffer<T>::operator=(Ringbuffer&& other) {
	Buffer = std::move(other.Buffer);
	End = other.End;
	Begin = other.Begin;
	Size = other.Size;

	return *this;
}

template<typename T> void	Clear(Ringbuffer<T>& Rb) {
	Rb.Size = 0;
	Rb.Begin = 0;
	Rb.End = 0;
}

template<typename T> void	FreeMemory(Ringbuffer<T>& Rb) {
	Clear(Rb);
	Trim(Rb);
}

template<typename T> u64	 Size(Ringbuffer<T>const& Rb) {
	return Rb.Size;
}

template<typename T> u64	Capacity(Ringbuffer<T> const& Rb) {
	return Size(Rb.Buffer);
}

template<typename T> T		Back(Ringbuffer<T>const& Rb) {
	Check(Size(Rb));
	auto c = Capacity(Rb);
	return Rb.Buffer[(Rb.End + c - 1) % c];
}

template<typename T> T		Front(Ringbuffer<T>const& Rb) {
	Check(Rb.Begin < Capacity(Rb));
	Check(Size(Rb));
	return Rb.Buffer[Rb.Begin];
}

template<typename T> T const& At(Ringbuffer<T> const& Rb, u32 index) {
	Check(Size(Rb));
	return Rb.Buffer[(Rb.Begin + index) % Capacity(r)];
}

template<typename T> void	PushBack(Ringbuffer<T>& Rb, T const& v) {
	if (Rb.Size == Size(Rb.Buffer)) {
		Reserve(Rb, Rb.Size + 1);
	}
	Rb.Buffer[Rb.End] = v;
	Rb.End = (Rb.End + 1) % Capacity(Rb);
	++Rb.Size;
}

template<typename T> void	PopBack(Ringbuffer<T>& Rb) {
	Check(Size(Rb));
	auto c = (u32)Capacity(Rb);
	Rb.End = (Rb.End + c - 1) % c;
	--Rb.Size;
}

template<typename T> void	PushFront(Ringbuffer<T>& Rb, T const& v) {
	if (Rb.Size == Size(Rb.Buffer)) {
		Reserve(Rb, Rb.Size + 1);
	}
	auto c = (u32)Capacity(Rb);
	Rb.Begin = (Rb.Begin + c - 1) % c;
	Rb.Buffer[Rb.Begin] = v;

	++Rb.Size;
	Check(Rb.Begin < Capacity(Rb))
}

template<typename T> void	PopFront(Ringbuffer<T>& Rb) {
	Check(Size(Rb));
	Rb.Begin = (Rb.Begin + 1) % Capacity(Rb);
	--Rb.Size;
}

template<typename T> void	Reserve(Ringbuffer<T>& Rb, u32 min_capacity) {
	auto new_capacity = next_pow2_size(min_capacity + 1);

	auto c = (u32)Capacity(Rb);
	Resize(Rb.Buffer, new_capacity);

	if (Size(Rb)) {
		auto b = Rb.Begin;
		auto e = Rb.End;

		if (e == b && e == 0) {
			Rb.End = c;
		}
		else if (e <= b) {
			auto mov = c - b;
			memmove(Rb.Buffer.DataPtr + new_capacity - mov, Rb.Buffer.DataPtr + b, mov * sizeof(T));
			Rb.Begin = new_capacity - mov;
		}
	}

	Check(Rb.Begin < Capacity(Rb));
}


}