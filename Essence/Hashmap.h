#pragma once

#include "Collections.h"
#include "Hash.h"
#include "Array.h"
#include "Memory.h"

namespace Essence {

static const size_t C_62Bits = 0x3fffffffffffffffULL;

template<typename K, typename V> Hashmap<K, V>::Hashmap() :
	Hashmap(GetMallocAllocator())
{
}

template<typename K, typename V> Hashmap<K, V>::Hashmap(IAllocator* allocator) :
	Buckets(allocator),
	Keys(allocator),
	Values(allocator),
	Size(0)
{
}

template<typename K, typename V> Hashmap<K, V>::~Hashmap() {
	Size = 0;
}

template<typename K, typename V> Hashmap<K, V>::Hashmap(const Hashmap<K, V> &other) : Hashmap() {
	*this = other;
}

template<typename K, typename V> Hashmap<K, V>::Hashmap(Hashmap<K, V> && other) : Hashmap() {
	*this = other;
}

template<typename K, typename V> Hashmap<K, V>& Hashmap<K, V>::operator=(const Hashmap<K, V> &other) {

	Buckets = other.Buckets;
	Keys = other.Keys;
	Values = other.Values;
	Size = other.Size;
	return *this;
}

template<typename K, typename V> Hashmap<K, V>&  Hashmap<K, V>::operator=(Hashmap<K, V> && other) {

	Size = other.Size;
	Buckets = std::move(other.Buckets);
	Keys = std::move(other.Keys);
	Values = std::move(other.Values);

	return *this;
}

template<typename K, typename V> V& Hashmap<K, V>::operator[](const K& Key) {
	auto ptr = Get(*this, Key);
	if (ptr) {
		return *ptr;
	}
	Set(*this, Key, V());
	return *Get(*this, Key);
}

template<typename K, typename V> const V& Hashmap<K, V>::operator[](const K& Key) const {
	auto ptr = Get(*this, Key);
	if (ptr) {
		return *ptr;
	}
	Set(*this, Key, V());
	return *Get(*this, Key);
}

template<typename K, typename V> void	Trim(Hashmap<K, V> &Hm) {
	if (Size(Hm)) {
		Rehash(Hm, Size(Hm));
	}
	else {
		FreeMemory(Hm.Buckets);
		FreeMemory(Hm.Keys);
		FreeMemory(Hm.Values);
	}
}

template<typename K, typename V> u64	Size(Hashmap<K, V> const &Hm) {
	return Hm.Size;
}

template<typename K, typename V> void	FreeMemory(Hashmap<K, V> &Hm) {
	Clear(Hm);
	Trim(Hm);
}

template<typename K, typename V> void	Reserve(Hashmap<K, V> &Hm, size_t min_capacity) {
	auto capacity = Size(Hm.buckets_);
	if (capacity < min_capacity) {
		Rehash(Hm, min_capacity);
	}
}

template<typename K, typename V> void	Clear(Hashmap<K, V> &Hm) {
	typedef Hashmap<K, V> Type;
	for (size_t i = 0, iEnd = Size(Hm.Buckets); i < iEnd; ++i) {
		Hm.Buckets[i].state = Type::BucketEmpty;
		Hm.Buckets[i].hash = 0;
	}
	Hm.Size = 0;
}

template<typename K, typename V> bool	Set(Hashmap<K, V>& Hm, K key, const V& val) {
	using namespace Hash;
	typedef Hashmap<K, V> Type;

	if (Size(Hm.Buckets) == 0) {
		Resize(Hm.Buckets, 4);
		Resize(Hm.Keys,	4);
		Resize(Hm.Values, 4);

		memset(Hm.Buckets.DataPtr, 0, sizeof(Type::bucket_t) * 4);
	}

	if (Hm.Size * 3 > Size(Hm.Buckets) * 2) {
		Rehash(Hm, Size(Hm.Buckets) * 2);
	}

	// Hash the key and find the starting bucket
	size_t hash = MurmurHash2_64(&key, sizeof(K), 0) & C_62Bits;
	size_t iBucketStart = hash % Size(Hm.Buckets);

	bool overwrite = false;
	// Search for an unused bucket
	Type::bucket_t * bTarget = nullptr;
	size_t iBucketTarget = 0;
	for (size_t i = iBucketStart, iEnd = Size(Hm.Buckets); i < iEnd; ++i) {
		Type::bucket_t * b = &Hm.Buckets[i];
		if (b->state != Type::BucketFilled) {
			bTarget = b;
			iBucketTarget = i;
			break;
		}
		//else if (b->hash == hash && Hm.Keys[i] == key) {
		else if (b->hash == hash) {
			bTarget = b;
			iBucketTarget = i;
			overwrite = true;
			break;
		}
	}
	if (!bTarget)
	{
		for (size_t i = 0; i < iBucketStart; ++i)
		{
			Type::bucket_t * b = &Hm.Buckets[i];
			if (b->state != Type::BucketFilled) {
				bTarget = b;
				iBucketTarget = i;
				break;
			}
			//else if (b->hash == hash && Hm.Keys[i] == key) {
			else if (b->hash == hash) {
				bTarget = b;
				iBucketTarget = i;
				overwrite = true;
				break;
			}
		}
	}

	Check(bTarget);

	// Store the hash, key, and value in the bucket
	bTarget->hash = hash;
	bTarget->state = Type::BucketFilled;
	Hm.Keys[iBucketTarget] = key;
	Hm.Values[iBucketTarget] = val;

	Hm.Size += overwrite ? 0 : 1;

	return !overwrite;
}

template<typename K, typename V> const V*	Get(Hashmap<K, V> const& Hm, K key);

template<typename K, typename V> V*		Get(Hashmap<K, V>& Hm, K key) {
	return const_cast<V*>(Get(static_cast<const Hashmap<K, V>&>(Hm), key));
}

template<typename K, typename V> bool	Contains(Hashmap<K, V> const& Hm, K key) {
	return Get(Hm, key) != nullptr;
}

template<typename K, typename V> const V*	Get(Hashmap<K, V> const& Hm, K key) {
	using namespace Hash;
	typedef Hashmap<K, V> Type;

	if (Size(Hm.Buckets) == 0) {
		return nullptr;
	}

	size_t hash = MurmurHash2_64(&key, sizeof(K), 0) & C_62Bits;
	size_t iBucketStart = hash % Size(Hm.Buckets);

	// Search the buckets until we hit an empty one
	for (size_t i = iBucketStart, iEnd = Size(Hm.Buckets); i < iEnd; ++i)
	{
		const Type::bucket_t * b = &Hm.Buckets[i];
		switch (b->state)
		{
		case Type::BucketEmpty:
			return nullptr;
		case Type::BucketFilled:
			//if (b->hash == hash && Hm.Keys[i] == key)
			if (b->hash == hash)
				return &Hm.Values[i];
			break;
		default:
			break;
		}
	}
	for (size_t i = 0; i < iBucketStart; ++i)
	{
		const Type::bucket_t * b = &Hm.Buckets[i];
		switch (b->state)
		{
		case Type::BucketEmpty:
			return nullptr;
		case Type::BucketFilled:
			//if (b->hash == hash && Hm.Keys[i] == key)
			if (b->hash == hash)
				return &Hm.Values[i];
			break;
		default:
			break;
		}
	}

	return nullptr;
}

template<typename K, typename V> void	Rehash(Hashmap<K, V>& Hm, size_t bucket_count_new) {
	typedef Hashmap<K, V> Type;
	
	if (Hm.Size == 0) {
		return;
	}

	// Can't rehash down to smaller than current size or initial size
	bucket_count_new = max(max(bucket_count_new, (size_t)Hm.Size), (size_t)4);

	Check(Hm.Keys.Allocator);
	// Build a new set of buckets, keys, and values
	Array<Type::bucket_t>	bucketsNew(Hm.Keys.Allocator);
	Array<K>				keysNew(Hm.Keys.Allocator);
	Array<V>				valuesNew(Hm.Keys.Allocator);

	Resize(bucketsNew,	bucket_count_new);
	Resize(keysNew,		bucket_count_new);
	Resize(valuesNew,	bucket_count_new);

	memset(bucketsNew.DataPtr, 0, sizeof(Type::bucket_t) * bucket_count_new);

	// Walk through all the current elements and insert them into the new buckets
	for (size_t i = 0, iEnd = Size(Hm.Buckets); i < iEnd; ++i)
	{
		Type::bucket_t * b = &Hm.Buckets[i];
		if (b->state != Type::BucketFilled)
			continue;

		// Hash the key and find the starting bucket
		size_t hash = b->hash;
		size_t iBucketStart = hash % bucket_count_new;

		// Search for an unused bucket
		Type::bucket_t * bTarget = nullptr;
		size_t iBucketTarget = 0;
		for (size_t j = iBucketStart; j < bucket_count_new; ++j)
		{
			Type::bucket_t * bNew = &bucketsNew[j];
			if (bNew->state != Type::BucketFilled)
			{
				bTarget = bNew;
				iBucketTarget = j;
				break;
			}
		}
		if (!bTarget)
		{
			for (size_t j = 0; j < iBucketStart; ++j)
			{
				Type::bucket_t * bNew = &bucketsNew[j];
				if (bNew->state != Type::BucketFilled)
				{
					bTarget = bNew;
					iBucketTarget = j;
					break;
				}
			}
		}

		Check(bTarget);

		// Store the hash, key, and value in the bucket
		bTarget->hash = hash;
		bTarget->state = Type::BucketFilled;
		keysNew[iBucketTarget] = Hm.Keys[i];
		valuesNew[iBucketTarget] = Hm.Values[i];
	}

	// Swap the new buckets, keys, and values into place
	Hm.Buckets =	std::move(bucketsNew);
	Hm.Keys =		std::move(keysNew);
	Hm.Values =		std::move(valuesNew);
}

template<typename K, typename V> bool Remove(Hashmap<K, V>& Hm, K key) {
	using namespace Hash;
	typedef Hashmap<K, V> Type;

	size_t hash = MurmurHash2_64(&key, sizeof(K), 0) & C_62Bits;
	size_t iBucketStart = hash % Size(Hm.Buckets);

	// Search the buckets until we hit an empty one
	for (size_t i = iBucketStart, iEnd = Size(Hm.Buckets); i < iEnd; ++i)
	{
		Type::bucket_t * b = &Hm.Buckets[i];
		switch (b->state)
		{
		case Type::BucketEmpty:
			return false;
		case Type::BucketFilled:
			//if (b->hash == hash && Hm.Keys[i] == key)
			if (b->hash == hash)
			{
				b->hash = 0;
				b->state = Type::BucketRemoved;
				--Hm.Size;
				return true;
			}
			break;
		default:
			break;
		}
	}
	for (size_t i = 0; i < iBucketStart; ++i)
	{
		Type::bucket_t * b = &Hm.Buckets[i];
		switch (b->state)
		{
		case Type::BucketEmpty:
			return false;
		case Type::BucketFilled:
			//if (b->hash == hash && Hm.Keys[i] == key)
			if (b->hash == hash)
			{
				b->hash = 0;
				b->state = Type::BucketRemoved;
				--Hm.Size;
				return true;
			}
			break;
		default:
			break;
		}
	}

	return false;
}

template<typename K, typename V> HashmapIterator<K, V>& HashmapIterator<K, V>::operator++() {
	const auto N = Size(Collection->Buckets);
	++Index;
	for (; Index<N; ++Index) {
		const Hashmap<K,V>::bucket_t * b = &Collection->Buckets[Index];
		if (b->state == Hashmap<K, V>::BucketFilled) {
			return *this;
		}
	}

	return *this;
}

template<typename K, typename V> bool HashmapIterator<K, V>::operator==(const HashmapIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index == rhs.Index;
}

template<typename K, typename V> bool HashmapIterator<K, V>::operator!=(const HashmapIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index != rhs.Index;
}

template<typename K, typename V> KeyValue<K, V> HashmapIterator<K, V>::operator*() const {
	KeyValue<K, V> pair{ Collection->Keys[Index], Collection->Values[Index] };
	return pair;
}

template<typename K, typename V> HashmapIterator<K, V> Hashmap<K, V>::begin() {
	const auto N = Buckets.Size;

	HashmapIterator<K, V> iter = { this , (u32)N };
	
	for (size_t i = 0; i<N; ++i) {
		const bucket_t * b = &Buckets[i];
		if (b->state == BucketFilled) {
			iter.Index = (u32)i;
			return iter;
		}
	}

	return iter;
}

template<typename K, typename V> HashmapIterator<K, V> Hashmap<K, V>::end() {
	HashmapIterator<K, V> iter = { this ,  (u32)Buckets.Size };
	return iter;
}

template<typename K, typename V> HashmapConstIterator<K, V>& HashmapConstIterator<K, V>::operator++() {
	const auto N = Size(Collection->Buckets);
	++Index;
	for (; Index<N; ++Index) {
		const Hashmap<K, V>::bucket_t * b = &Collection->Buckets[Index];
		if (b->state == Hashmap<K, V>::BucketFilled) {
			return *this;
		}
	}

	return *this;
}

template<typename K, typename V> bool HashmapConstIterator<K, V>::operator==(const HashmapConstIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index == rhs.Index;
}

template<typename K, typename V> bool HashmapConstIterator<K, V>::operator!=(const HashmapConstIterator& rhs) const {
	Check(Collection == rhs.Collection);
	return Index != rhs.Index;
}

template<typename K, typename V> KeyConstValue<K, V> HashmapConstIterator<K, V>::operator*() const {
	KeyConstValue<K, V> pair{ Collection->Keys[Index], Collection->Values[Index] };
	return pair;
}

template<typename K, typename V> HashmapConstIterator<K, V> Hashmap<K, V>::cbegin() const {
	const auto N = Buckets.Size;

	HashmapConstIterator<K, V> iter = { this , (u32)N };

	for (size_t i = 0; i<N; ++i) {
		const bucket_t * b = &Buckets[i];
		if (b->state == BucketFilled) {
			iter.Index = (u32)i;
			return iter;
		}
	}

	return iter;
}

template<typename K, typename V> HashmapConstIterator<K, V> Hashmap<K, V>::cend() const {
	HashmapConstIterator<K, V> iter = { this ,  (u32)Buckets.Size };
	return iter;
}

}