#pragma once

#include "AssertionMacros.h"
#include "Memory.h"

namespace Essence {

template<typename T> struct ArrayIterator;
template<typename T> struct ArrayConstIterator;

template<typename T> struct Array {
	Array();
	Array(IAllocator* allocator);
	~Array();
	Array(const Array &other);
	Array &operator=(const Array &other);
	Array(Array &&other);
	Array &operator=(Array &&other);

	T&						operator[](u64 index);
	const T&				operator[](u64 index) const;

	ArrayIterator<T>		begin();
	ArrayIterator<T>		end();
	ArrayConstIterator<T>	cbegin() const;
	ArrayConstIterator<T>	cend() const;

	IAllocator*		Allocator;
	size_t					Size;
	size_t					Capacity;
	T*						DataPtr;
};

template<typename T> struct ArrayIterator {
	Array<T>* const	Collection;
	u32				Index;

	ArrayIterator& operator++();
	bool operator==(const ArrayIterator& rhs) const;
	bool operator!=(const ArrayIterator& rhs) const;

	T& operator*() const;
};

template<typename T> struct ArrayConstIterator {
	Array<T> const* const	Collection;
	u32				Index;

	ArrayConstIterator& operator++();
	bool operator==(const ArrayConstIterator& rhs) const;
	bool operator!=(const ArrayConstIterator& rhs) const;

	const T& operator*() const;
};

template<typename K, typename V> struct HashmapIterator;
template<typename K, typename V> struct HashmapConstIterator;

// http://www.reedbeta.com/blog/2015/01/12/data-oriented-hash-table/
// DO2

template<typename K, typename V>
struct Hashmap {
	Hashmap();
	Hashmap(IAllocator* allocator);
	~Hashmap();
	Hashmap(const Hashmap& other);
	Hashmap(Hashmap&& other);
	Hashmap& operator=(const Hashmap& other);
	Hashmap& operator=(Hashmap&& other);

	V&					operator[](const K& Key);
	const V&			operator[](const K& Key) const;

	HashmapIterator<K, V>		begin();
	HashmapIterator<K, V>		end();
	HashmapConstIterator<K, V>	cbegin() const;
	HashmapConstIterator<K, V>	cend() const;

	enum BucketStateEnum
	{
		BucketEmpty,
		BucketFilled,
		BucketRemoved,
	};

	struct bucket_t
	{
		u64	hash : 62;
		u64	state : 2;
	};

	Array<bucket_t>		Buckets;
	Array<K>			Keys;
	Array<V>			Values;
	size_t				Size;
};

template<typename K, typename V> struct KeyValue {
	K	key;
	V&	value;
};

template<typename K, typename V> struct HashmapIterator {
	Hashmap<K, V>*	const	Collection;
	u32						Index;

	HashmapIterator& operator++();
	bool operator==(const HashmapIterator& rhs) const;
	bool operator!=(const HashmapIterator& rhs) const;

	KeyValue<K, V> operator*() const;
};

template<typename K, typename V> struct KeyConstValue {
	K	key;
	V const&	value;
};

template<typename K, typename V> struct HashmapConstIterator {
	Hashmap<K, V> const* const	Collection;
	u32							Index;

	HashmapConstIterator& operator++();
	bool operator==(const HashmapConstIterator& rhs) const;
	bool operator!=(const HashmapConstIterator& rhs) const;

	KeyConstValue<K, V> operator*() const;
};

template<typename T>
struct Ringbuffer {
	Array<T>	Buffer;
	u32			End;
	u32			Begin;
	u32			Size;

	Ringbuffer();
	Ringbuffer(IAllocator *allocator);

	Ringbuffer& operator=(Ringbuffer const& other);
	Ringbuffer& operator=(Ringbuffer && other);
};


template<u32 IndexBits, u64 TypeId = 0xFFFFFFFFFFFFFFFFull>
struct GenericHandle32 {
	static const u32 GenerationBits = 32 - IndexBits;

	u32	index : IndexBits;
	u32	generation : GenerationBits;

	static const u32 IndexMask = (1 << IndexBits) - 1;
	static const u32 GenerationMask = ~IndexMask;
	static const u32 GenerationMax = (1 << GenerationBits) - 1;

	inline u32 GetIndex() const {
		return index & IndexMask;
	}

	inline u32 GetGeneration() const {
		return generation;
	}

	inline static u32 NextGeneration(u32 generation) {
		generation = (generation + 1) % GenerationMax;
		return generation > 0 ? generation : 1;
	}

	bool operator == (GenericHandle32<IndexBits, TypeId> other) const {
		return index == other.index && generation == other.generation;
	}

	bool operator != (GenericHandle32<IndexBits, TypeId> other) const {
		return index != other.index || generation != other.generation;
	}
};

template<u32 IndexBits, u64 TypeId>
bool IsValid(GenericHandle32<IndexBits, TypeId> handle) {
	return handle.GetGeneration() != 0;
}

template<u32 IndexBits, u32 TypeId>
bool operator == (GenericHandle32<IndexBits, TypeId> const& lhs, GenericHandle32<IndexBits, TypeId> const& rhs) {
	return (lhs.index == rhs.index) && (lhs.gen == rhs.gen);
}

template<u32 IndexBits, u32 TypeId>
bool operator != (GenericHandle32<IndexBits, TypeId> const& lhs, GenericHandle32<IndexBits, TypeId> const& rhs) {
	return (lhs.index != rhs.index) || (lhs.gen != rhs.gen);
}

}
