#pragma once
#include "Types.h"
#include <type_traits>

#ifndef __PLACEMENT_NEW_INLINE
	#define __PLACEMENT_NEW_INLINE
	inline void* operator new(size_t _Size, void* _Where);
	inline void operator delete(void*, void*);
	inline void* operator new[](size_t _Size, void* _Where);
	inline void operator delete[](void*, void*);
#endif

#define sizeof_pointed_type(x) sizeof(std::remove_pointer<decltype(x)>::type)

const u32 CACHE_LINE = 64;
#define CACHE_ALIGN __declspec(align(CACHE_LINE))

namespace Essence {

class IAllocator {
public:
	IAllocator() {}
	virtual ~IAllocator() {}
	virtual void* Allocate(size_t size, size_t align) = 0;
	virtual void Free(void*) = 0;
	virtual size_t GetTotalAllocatedSize() const { return 0; }
private:
	IAllocator(const IAllocator& other);
	IAllocator& operator=(const IAllocator& other);
};

// global allocator
void InitMemoryAllocators();
void ShutdownMemoryAllocators();
IAllocator* GetMallocAllocator();
IAllocator* GetScratchAllocator();
IAllocator* GetThreadScratchAllocator();
void FreeThreadAllocator();

template<typename T> void call_destructor(T* ptr);
template<typename T> void call_destructor(T& ptr);

template<class T, class... _Types> inline void make_new(T*& ptr, IAllocator &allocator, _Types &&... _args);
template<class T> inline void make_delete(T*& ptr, IAllocator &allocator);
template<class T, class... _Types> inline void _new(T*& ptr, _Types &&... _args);
template<class T> inline void _delete(T*& ptr);

inline void *align_forward(const void *p, size_t alignment);
inline void *pointer_add(const void *p, size_t bytes);
inline void *pointer_sub(const void *p, size_t bytes);
inline size_t padded_size(size_t size, size_t align);
inline size_t pointer_sub(const void *a, const void *b);


// implementation

template<typename T> void call_destructor(T& ptr) {
	ptr.~T();
}

template<typename T> void call_destructor(T* ptr) {
	ptr->~T();
}

template<class T, class... _Types> inline void make_new(T*& ptr, IAllocator *allocator, _Types &&... _args) {
	ptr = new ((allocator)->Allocate(sizeof(T), __alignof(T))) T(std::forward<_Types>(_args)...);
}

template<class T, class... _Types> inline void make_new(T** ptr, IAllocator *allocator, _Types &&... _args) {
	*ptr = new ((allocator)->Allocate(sizeof(T), __alignof(T))) T(std::forward<_Types>(_args)...);
}

template<typename T>
void allocate_c_array(T *& ptr, IAllocator* allocator, u32 elements) {
	ptr = (T*)allocator->Allocate(sizeof(T) * elements, alignof(T));
};

template<class T> inline void make_delete(T*& ptr, IAllocator *allocator) {
	if (ptr) { 
		(ptr)->~T(); 
		allocator->Free(ptr); 
		ptr = nullptr; 
	}
}

template<class T, class... _Types> inline void _new(T*& ptr, _Types &&... _args) {
	make_new(ptr, GetMallocAllocator(), _args...);
}

template<class T> inline void _delete(T*& ptr) {
	make_delete(ptr, GetMallocAllocator());
}

inline void *align_forward(const void *p, size_t alignment) {
	uintptr_t pi = uintptr_t(p);
	pi = (pi + alignment - 1) & ~(alignment - 1);
	return (void *) pi;
}

inline void *pointer_add(const void *p, size_t bytes) {
	return (void*) ((const char *) p + bytes);
}

inline void *pointer_sub(const void *p, size_t bytes) {
	return (void*) ((const char *) p - bytes);
}

inline size_t padded_size(size_t size, size_t alignment) {
	return (size + alignment - 1) & ~(alignment - 1);
}

inline size_t pointer_sub(const void *a, const void *b) {
	return uintptr_t(a) - uintptr_t(b);
}

inline u64 Kilobytes(u64 bytes) {
	return bytes / 1024;
}

inline u64 Megabytes(u64 bytes) {
	return Kilobytes(bytes) / 1024;
}

}