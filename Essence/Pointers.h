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


}