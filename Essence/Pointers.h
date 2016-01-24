#pragma once

namespace Essence {


template<typename T>
struct OwningComPtr {
	T*		Ptr;

	T**		GetInitPtr() {
		return &Ptr;
	}

	T*		operator*() const {
		return Ptr;
	}

	T*		operator->() const {
		return Ptr;
	}

	OwningComPtr() : Ptr(nullptr) {}
	OwningComPtr(OwningComPtr<T> const& other) = delete;

	OwningComPtr(OwningComPtr<T> && other) {
		*this = other;
	}

	OwningComPtr<T>& operator = (OwningComPtr<T> && other) {
		if (Ptr) {
			Ptr->Release();
		}

		Ptr = other.Ptr;
		other.Ptr = nullptr;

		return *this;
	}

	~OwningComPtr() {
		if (Ptr) {
			Ptr->Release();
			Ptr = nullptr;
		}
	}
};

template<typename T>
struct OwningPtr {
	T*			Ptr;
	IAllocator*	Allocator;

	T**		GetInitPtr() {
		return &Ptr;
	}

	T*		operator*() const {
		return Ptr;
	}

	T*		operator->() const {
		return Ptr;
	}

	OwningPtr() : Ptr(nullptr) {}
	OwningPtr(OwningPtr<T> const& other) = delete;

	OwningPtr(OwningPtr<T> && other) {
		*this = other;
	}

	OwningPtr<T>& operator = (OwningPtr<T> && other) {
		if (Ptr) {
			call_destructor(Ptr);
			Allocator->Free(Ptr);
		}

		Ptr = other.Ptr;
		Allocator = other.Allocator;
		other.Ptr = nullptr;

		return *this;
	}

	~OwningPtr() {
		if (Ptr) {
			call_destructor(Ptr);
			Allocator->Free(Ptr);
			Ptr = nullptr;
		}
	}

	void Reset(T* ptr, IAllocator* allocator) {
		if (Ptr) {
			call_destructor(Ptr);
			Allocator->Free(Ptr);
			Ptr = nullptr;
		}
		Ptr = ptr;
		Allocator = allocator;
	}
};

}