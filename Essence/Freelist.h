#pragma once

#include "Types.h"
#include "Memory.h"
#include "Array.h"

namespace Essence {

static const u32 FreelistEmptyIndex = 0xFFFFFFFF;

template <typename T, typename HANDLE> struct FreelistIterator;
template <typename T, typename HANDLE> struct FreelistKeyIterator;

template<typename T, typename HANDLE>
class Freelist {
public:
	struct Node {
		u32 next_free;
		u32 generation;
	};

	Freelist();
	Freelist(IAllocator *allocator);

	Freelist& operator=(const Freelist& other);

	T&			operator[](HANDLE h);
	const T&	operator[](HANDLE h) const;

	Array<T>	Values;
	Array<Node>	Nodes;
	u32 Free;
	u32 Size;

	FreelistIterator<T, HANDLE>		begin();
	FreelistIterator<T, HANDLE>		end();

	class FreelistKeys {
	public:
		Freelist*	Ptr;

		FreelistKeyIterator<T, HANDLE>		begin();
		FreelistKeyIterator<T, HANDLE>		end();

		FreelistKeys(Freelist* Owner) : Ptr(Owner) {
		}
	};

	FreelistKeys Keys() {
		return FreelistKeys(this);
	}
};

template<typename T, typename HANDLE> 
Freelist<T, HANDLE>::Freelist(IAllocator* allocator) :
	Values(allocator),
	Nodes(allocator),
	Free(FreelistEmptyIndex),
	Size(0)
{
}

template<typename T, typename HANDLE>
Freelist<T, HANDLE>::Freelist() : Freelist(GetMallocAllocator())
{
}

template<typename T, typename HANDLE>
Freelist<T, HANDLE>& Freelist<T, HANDLE>::operator =(Freelist const& other) {

	Nodes = other.Nodes;
	Values = other.Values;
	Size = other.Size;
	Free = other.Free;

	return *this;
}

template<typename T, typename HANDLE>
void FreeMemory(Freelist<T, HANDLE>& Fl) {
	FreeMemory(Fl.Nodes);
	FreeMemory(Fl.Values);

	Fl.Size = 0;
	Fl.Free = FreelistEmptyIndex;
} 

template<typename T, typename HANDLE>
T& Freelist<T, HANDLE>::operator[](HANDLE handle) {
	Check(handle.GetIndex() < Essence::Size(Nodes));
	Check(handle.GetGeneration() == Nodes[handle.GetIndex()].generation);
	return Values[handle.GetIndex()];
}

template<typename T, typename HANDLE>
const T& Freelist<T, HANDLE>::operator[](HANDLE handle) const {
	Check(handle.GetIndex() < Essence::Size(Nodes));
	Check(handle.GetGeneration() == Nodes[handle.GetIndex()].generation);
	return Values[handle.GetIndex()];
}

template<typename T, typename HANDLE>
HANDLE	Create(Freelist<T, HANDLE>& Fl) {
	if (Fl.Size == Size(Fl.Values)) {
		auto capacity = Size(Fl.Values);
		auto new_capacity = capacity * 2 + 1;
		Resize(Fl.Values, new_capacity);
		Resize(Fl.Nodes, new_capacity);

		for (auto i = capacity; i < new_capacity; i++) {
			Fl.Nodes[i].next_free = (u32)(i + 1);
			Fl.Nodes[i].generation = 1;
		}
		Fl.Free = (u32)capacity;
	}

	auto Free = Fl.Free;
	Fl.Free = Fl.Nodes[Fl.Free].next_free;
	Fl.Nodes[Free].next_free = FreelistEmptyIndex;
	Fl.Size++;

	HANDLE h;
	h.index = Free;
	h.generation = Fl.Nodes[Free].generation;
	return h;
}

template<typename T, typename HANDLE>
void	Delete(Freelist<T, HANDLE>& Fl, HANDLE handle) {
	Check(Fl.Size);
	auto index = handle.GetIndex();
	Fl.Nodes[index].next_free = Fl.Free;
	Fl.Nodes[index].generation = HANDLE::NextGeneration(Fl.Nodes[index].generation);
	Fl.Free = index;
	--Fl.Size;
}

template<typename T, typename HANDLE>
bool	Contains(Freelist<T, HANDLE>& Fl, HANDLE handle) {
	if (Size(Fl.Nodes) < handle.GetIndex()) {
		return false;
	}
	return handle.GetGeneration() == Fl.Nodes[handle.GetIndex()].generation;
}

template <typename T, typename HANDLE> struct FreelistIterator {
	u32 Index;
	Freelist<T, HANDLE> * Collection;

	FreelistIterator& operator++();
	
	bool operator==(const FreelistIterator& rhs) const;
	bool operator!=(const FreelistIterator& rhs) const;
	
	T& operator*();
};

template <typename T, typename HANDLE>
bool FreelistIterator<T, HANDLE>::operator==(const FreelistIterator& rhs) const {
	return Collection == rhs.Collection && Index == rhs.Index;
}

template <typename T, typename HANDLE>
bool FreelistIterator<T, HANDLE>::operator!=(const FreelistIterator& rhs) const {
	return Collection != rhs.Collection || Index != rhs.Index;
}

template <typename T, typename HANDLE>
T& FreelistIterator<T, HANDLE>::operator*() { return Collection->Values[Index]; }

template<typename T, typename HANDLE>
FreelistIterator<T, HANDLE>& FreelistIterator<T, HANDLE>::operator++() {
	auto index = Index + 1;
	while (index < Size(Collection->Nodes) && Collection->Nodes[index].next_free != FreelistEmptyIndex) {
		++index;
	}

	Index = index;
	return *this;
}

template<typename T, typename HANDLE> FreelistIterator<T, HANDLE> Freelist<T, HANDLE>::begin() {
	FreelistIterator<T, HANDLE> iter{ 0, this };
	return iter;
}

template<typename T, typename HANDLE> FreelistIterator<T, HANDLE> Freelist<T, HANDLE>::end() {
	FreelistIterator<T, HANDLE> iter{ (u32)Essence::Size(Nodes), this };
	return iter;
}

template <typename T, typename HANDLE> struct FreelistKeyIterator {
	u32 Index;
	Freelist<T, HANDLE> * Collection;

	FreelistKeyIterator& operator++();

	bool operator==(const FreelistKeyIterator& rhs) const;
	bool operator!=(const FreelistKeyIterator& rhs) const;

	HANDLE operator*();
};

template <typename T, typename HANDLE>
bool FreelistKeyIterator<T, HANDLE>::operator==(const FreelistKeyIterator& rhs) const {
	return Collection == rhs.Collection && Index == rhs.Index;
}

template <typename T, typename HANDLE>
bool FreelistKeyIterator<T, HANDLE>::operator!=(const FreelistKeyIterator& rhs) const {
	return Collection != rhs.Collection || Index != rhs.Index;
}

template <typename T, typename HANDLE>
HANDLE FreelistKeyIterator<T, HANDLE>::operator*() { HANDLE h = { Index, Collection->Nodes[Index].generation }; return h; }

template<typename T, typename HANDLE>
FreelistKeyIterator<T, HANDLE>& FreelistKeyIterator<T, HANDLE>::operator++() {
	auto index = Index + 1;
	while (index < Size(Collection->Nodes) && Collection->Nodes[index].next_free != FreelistEmptyIndex) {
		++index;
	}

	Index = index;
	return *this;
}

template<typename T, typename HANDLE> FreelistKeyIterator<T, HANDLE> Freelist<T, HANDLE>::FreelistKeys::begin() {
	FreelistKeyIterator<T, HANDLE> iter{ 0, Ptr };
	return iter;
}

template<typename T, typename HANDLE> FreelistKeyIterator<T, HANDLE> Freelist<T, HANDLE>::FreelistKeys::end() {
	FreelistKeyIterator<T, HANDLE> iter{ (u32)Essence::Size(Ptr->Nodes), Ptr };
	return iter;
}

}
