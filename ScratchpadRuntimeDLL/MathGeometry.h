#pragma once

#include "../Essence/VectorMath.h"

// L0 && L1 are normalized line equations (Ax + By + C)
Vec2f LinesIntersection2D(Vec3f L0, Vec3f L1) {
	Vec3f c = cross(L0, L1);
	return c.xy / c.z;
}

Vec3f LineFromPoints2D(Vec2f P0, Vec2f P1) {
	Vec3f c = cross(Vec3f(P0, -1), Vec3f(P1, -1));
	return c;
}

template<typename T, i32 rows, i32 cols, i32 cols1>
Matrix<T, rows, cols1> operator*(Matrix<T, rows, cols> const& lhs, Matrix<T, cols, cols1> const& rhs) {
	Matrix<T, rows, cols1> o;
	for (i32 c = 0; c < cols1; ++c) {
		for (i32 r = 0; r < rows; ++r) {
			o.data[r][c] = T(0);
			for (i32 i = 0; i < cols; ++i) {
				o.data[r][c] += lhs.data[r][i] * rhs.data[i][c];
			}
		}
	}
	return o;
}


template<typename T, i32 rows, i32 cols>
Vector<T, rows> operator*(Matrix<T, rows, cols> const& lhs, Vector<T, cols> const& rhs) {
	Vector<T, rows> o;
	for (i32 r = 0; r < rows; ++r) {
		o[r] = T(0);
		for (i32 c = 0; c < cols; ++c) {
			o[r] += lhs.data[r][c] * rhs[c];
		}
	}
	return o;
}

template<typename T, i32 rows, i32 cols>
Vector<T, cols> operator*(Vector<T, rows> const& lhs, Matrix<T, rows, cols> const& rhs) {
	Vector<T, cols> o;
	for (i32 c = 0; c < cols; ++c) {
		o[c] = T(0);
		for (i32 r = 0; r < rows; ++r) {
			o[c] += lhs[r] * rhs.data[r][c];
		}
	}
	return o;
}


template <typename T> struct Matrix<T, 2, 2> {
	union {
		T data[2][2];
		struct { Vector<T, 2> row[2]; };
		struct { T _11, _12, _21, _22; };
	};

	constexpr Matrix() = default;
	~Matrix() = default;

	Matrix(T v11, T v12, T v21, T v22) {
		_11 = v11;
		_12 = v12;
		_21 = v21;
		_22 = v22;
	}

	static Matrix<T, 2, 2> Identity() {
		const Matrix<T, 2, 2> identity(T(1), T(0), T(0), T(1));
		return identity;
	}

	static Matrix<T, 2, 2> Rotation(T angle);
};


template <typename T> struct Matrix<T, 2, 3> {
	union {
		T data[2][3];
		struct { Vector<T, 3> row[2]; };
		struct { T 
			_11, _12, _13,
			_21, _22, _23; };
	};

	constexpr Matrix() = default;
	~Matrix() = default;

	Matrix(T v11, T v12, T v13, T v21, T v22, T v23) {
		_11 = v11;
		_12 = v12;
		_13 = v13;
		_21 = v21;
		_22 = v22;
		_23 = v23;
	}

	static Matrix<T, 2, 3> Translation(Vector<T, 2> vT) {
		Matrix<T, 2, 3> o(
			T(1), T(0), T(vT.x), 
			T(0), T(1), T(vT.y));
		return o;
	}
};

template<>
Matrix<float, 2, 2> Matrix<float, 2, 2>::Rotation(float angle) {
	Matrix<float, 2, 2> o;
	o._11 = cosf(angle);
	o._12 = -sinf(angle);
	o._21 = -o._12;
	o._22 = o._11;
	return o;
}


template <typename T> struct Matrix<T, 3, 3> {
	union {
		T data[3][3];
		struct { Vector<T, 3> row[3]; };
		struct {
			T
				_11, _12, _13,
				_21, _22, _23,
				_31, _32, _33;
		};
	};

	constexpr Matrix() = default;
	~Matrix() = default;

	Matrix(T v11, T v12, T v13,
		T v21, T v22, T v23,
		T v31, T v32, T v33,
		T v41, T v42, T v43 ) {
		_11 = v11; _21 = v21; _31 = v31;
		_12 = v12; _22 = v22; _32 = v32;
		_13 = v13; _23 = v23; _33 = v33;
	}

	static Matrix<T, 3, 3> Identity() {
		const Matrix<T, 3, 3> identity(
			T(1), 0, 0,
			0, T(1), 0,
			0, 0, T(1));
		return identity;
	}
};



template <typename T> struct Matrix<T, 3, 4> {
	union {
		T data[3][4];
		struct { Vector<T, 4> row[3]; };
		struct {
			T
				_11, _12, _13, _14,
				_21, _22, _23, _24,
				_31, _32, _33, _34;
		};
	};

	constexpr Matrix() = default;
	~Matrix() = default;

	Matrix(T v11, T v12, T v13, T v14,
		T v21, T v22, T v23, T v24,
		T v31, T v32, T v33, T v34 ) {
		_11 = v11; _21 = v21; _31 = v31; 
		_12 = v12; _22 = v22; _32 = v32; 
		_13 = v13; _23 = v23; _33 = v33; 
		_14 = v14; _24 = v24; _34 = v34; 
	}

	static Matrix<T, 3, 4> Translation(Vector<T, 3> vT) {
		const Matrix<T, 3, 4> o(
			T(1), 0, 0, vT.x,
			0, T(1), 0, vT.y,
			0, 0, T(1), vT.z);
		return o;
	}
};


template <typename T> struct Matrix<T, 4, 4> {
	union {
		T data[4][4];
		struct { Vector<T, 4> row[4]; };
		struct { T 
			_11, _12, _13, _14,
			_21, _22, _23, _24,
			_31, _32, _33, _34, 
			_41, _42, _43, _44; };
	};

	constexpr Matrix() = default;
	~Matrix() = default;

	Matrix(T v11, T v12, T v13, T v14, 
		T v21, T v22, T v23, T v24,
		T v31, T v32, T v33, T v34,
		T v41, T v42, T v43, T v44 ) {
		_11 = v11; _21 = v21; _31 = v31; _41 = v41;
		_12 = v12; _22 = v22; _32 = v32; _42 = v42;
		_13 = v13; _23 = v23; _33 = v33; _43 = v43;
		_14 = v14; _24 = v24; _34 = v34; _44 = v44;
	}

	static Matrix<T, 4, 4> Identity() {
		const Matrix<T, 4, 4> identity(
			T(1), 0, 0, 0,
			0, T(1), 0, 0,
			0, 0, T(1), 0,
			0, 0, 0, T(1));
		return identity;
	}

	static Matrix<T, 4, 4> Translation(Vector<T, 3> vT) {
		const Matrix<T, 4, 4> o(
			T(1), 0, 0, vT.x,
			0, T(1), 0, vT.y,
			0, 0, T(1), vT.z,
			0, 0, 0, T(1));
		return identity;
	}

	static Matrix<T, 4, 4> Scaling(Vector<T, 3> vS) {
		const Matrix<T, 4, 4> o(
			vS.x, 0, 0, 0,
			0, vS.y, 0, 0,
			0, 0, vS.z, 0,
			0, 0, 0, T(1));
		return identity;
	}
};

typedef Matrix<float, 2, 2> Matrix2x2f;
typedef Matrix<float, 2, 3> Matrix2x3f;
typedef Matrix<float, 3, 3> Matrix3x3f;
typedef Matrix<float, 3, 4> Matrix3x4f;
typedef Matrix<float, 4, 4> Matrix4x4f;

