#pragma once

#include "Types.h"
#include <initializer_list>
#include <math.h>

template <typename T, i32 n>
struct Vector { 
	T data[n]; 

	constexpr Vector() = default;
	~Vector() = default;

	Vector(T v) {
		for (i32 i = 0; i < n; ++i) {
			data[i] = v;
		}
	}

	T& operator[](i32 index) { return data[index] };
	const T& operator[](i32 index) const { return data[index]; };
};

template <typename T, i32 rows, i32 cols>
struct Matrix { T data[rows][cols]; };

template <typename T> struct Vector<T, 2> { 
	union { 
		T data[2]; 
		struct { T x, y; }; 
	};

	constexpr Vector() = default;
	~Vector() = default;

	template<typename K>
	explicit Vector(K v) {
		T _v = (T)v;
		x = _v;
		y = _v;
	}

	Vector(T v) {
		x = v;
		y = v;
	}

	Vector(T vx, T vy) {
		x = vx;
		y = vy;
	}

	Vector(T* va) {
		x = va[0];
		y = va[1];
	}

	T& operator[](i32 index) { return data[index]; };
	const T& operator[](i32 index) const { return data[index]; };
};

template <typename T> struct Vector<T, 3> { 
	union { 
		T data[3]; 
		struct { T x, y, z; }; 
		Vector<T, 2> xy;
	}; 

	constexpr Vector() = default;
	~Vector() = default;

	template<typename K>
	explicit Vector(K v) {
		T _v = (T)v;
		x = _v;
		y = _v;
		z = _v;
	}

	Vector(T v) {
		x = v;
		y = v;
		z = v;
	}

	Vector(Vector<T, 2> vxy, T vz) {
		x = vxy.x;
		y = vxy.y;
		z = vz;
	}

	Vector(T vx, T vy, T vz) {
		x = vx;
		y = vy;
		z = vz;
	}

	Vector(T* va) {
		x = va[0];
		y = va[1];
		z = va[2];
	}

	Vector(std::initializer_list<T> il) {
		auto iter = il.begin();
		data[0] = *iter; iter++;
		data[1] = *iter; iter++;
		data[2] = *iter; iter++;
	}

	T& operator[](i32 index) { return data[index]; };
	const T& operator[](i32 index) const { return data[index]; };
};

template <typename T> struct Vector<T, 4> { 
	union { 
		T data[4]; 
		struct { T x, y, z, w; }; 
		struct { float r, g, b, a; };
		Vector<T, 2> xy;
		Vector<T, 3> xyz;
		u32 packed_u32;
	}; 

	constexpr Vector() = default;
	~Vector() = default;

	template<typename K>
	explicit Vector(K v) {
		T _v = (T)v;
		x = _v;
		y = _v;
		z = _v;
		w = _v;
	}

	Vector(T v) {
		x = v;
		y = v;
		z = v;
		w = v;
	}

	Vector(T vx, T vy, T vz) {
		x = vx;
		y = vy;
		z = vz;
		w = T(1);
	}

	Vector(T vx, T vy, T vz, T vw) {
		x = vx;
		y = vy;
		z = vz;
		w = vw;
	}

	Vector(T* va) {
		x = va[0];
		y = va[1];
		z = va[2];
		w = va[3];
	}

	Vector(std::initializer_list<T> il) {
		auto iter = il.begin();
		data[0] = *iter; iter++;
		data[1] = *iter; iter++;
		data[2] = *iter; iter++;
		data[3] = *iter; iter++;
	}

	T& operator[](i32 index) { return data[index]; };
};

template<typename T, i32 n>
Vector<T, n> operator +(Vector<T, n>const& lhs, Vector<T, n>const& rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] += rhs.data[i];
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator +=(Vector<T, n>& lhs, Vector<T, n>const& rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] += rhs.data[i];
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator +(Vector<T, n>const& lhs, T rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] += rhs;
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator +=(Vector<T, n>& lhs, T rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] += rhs;
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator -(Vector<T, n>const& lhs, Vector<T, n>const& rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] -= rhs.data[i];
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator -=(Vector<T, n>& lhs, Vector<T, n>const& rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] -= rhs.data[i];
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator -(Vector<T, n>const& lhs, T rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] -= rhs;
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator -=(Vector<T, n>& lhs, T rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] -= rhs;
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator *(Vector<T, n>const& lhs, Vector<T, n>const& rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] *= rhs.data[i];
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator *=(Vector<T, n>& lhs, Vector<T, n>const& rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] *= rhs.data[i];
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator *(Vector<T, n>const& lhs, T rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] *= rhs;
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator *=(Vector<T, n>& lhs, T rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] *= rhs;
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator /(Vector<T, n>const& lhs, Vector<T, n>const& rhs) {
	Vector<T, n> v = lhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] /= rhs.data[i];
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator /=(Vector<T, n>& lhs, Vector<T, n>const& rhs) {
	for (i32 i = 0; i < n; ++i) {
		lhs[i] /= rhs.data[i];
	}
	return lhs;
}

template<typename T, i32 n>
Vector<T, n> operator /(Vector<T, n>const& lhs, T rhs) {
	Vector<T, n> v = lhs;
	T rcp = T(1) / rhs;
	for (i32 i = 0; i < n; ++i) {
		v[i] *= rcp;
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n>& operator /=(Vector<T, n>& lhs, T rhs) {
	T rcp = T(1) / rhs;
	for (i32 i = 0; i < n; ++i) {
		lhs[i] *= rcp;
	}
	return lhs;
}

template<typename T, i32 n>
T dot(Vector<T, n>& lhs, Vector<T, n>const& rhs) {
	T v = 0;
	for (i32 i = 0; i < n; ++i) {
		v += lhs[i] * rhs[i];
	}
	return v;
}

template<typename T, i32 n>
Vector<T, n> cross(Vector<T, n>const& lhs, Vector<T, n>const& rhs);

template<typename T>
Vector<T, 3> cross(Vector<T, 3>const& lhs, Vector<T, 3>const& rhs) {
	return { lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x };
}

template<i32 n>
Vector<float, n> normalize(Vector<float, n>const& v) {
	float rcp = 0.f;
	for (i32 i = 0; i < n; ++i) {
		rcp += v[i] * v[i];
	}
	rcp = sqrtf(1.f / rcp);

	Vector<float, n> o = v;
	for (i32 i = 0; i < n; ++i) {
		o.data[i] *= rcp;
	}
	return o;
}

template<i32 n>
float length(Vector<float, n> const& v) {
	float len = 0.f;
	for (i32 i = 0; i < n; ++i) {
		len += v[i] * v[i];
	}
	return sqrtf(len);
}

typedef Vector<float, 2> Vec2f;
typedef Vector<float, 3> Vec3f;
typedef Vector<float, 4> Vec4f;
typedef Vector<u32, 2> Vec2u;
typedef Vector<u32, 3> Vec3u;
typedef Vector<u32, 4> Vec4u;
typedef Vector<i32, 2> Vec2i;
typedef Vector<i32, 3> Vec3i;
typedef Vector<i32, 4> Vec4i;
typedef Vector<u8, 4> Color4b;