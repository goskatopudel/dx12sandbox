#pragma once

#include "Types.h"
#include <math.h>
#include <DirectXMath.h>

namespace Essence {

typedef DirectX::XMVECTOR	xmvec;
typedef DirectX::XMMATRIX	xmmatrix;

typedef DirectX::XMFLOAT2	float2;
typedef DirectX::XMFLOAT3	float3;
typedef DirectX::XMFLOAT4	float4;
typedef DirectX::XMFLOAT3A	float3a;
typedef DirectX::XMFLOAT4A	float4a;

typedef DirectX::XMUINT2	uint2;
typedef DirectX::XMUINT3	uint3;
typedef DirectX::XMUINT4	uint4;

typedef DirectX::XMINT2		int2;
typedef DirectX::XMINT3		int3;
typedef DirectX::XMINT4		int4;

typedef DirectX::XMFLOAT4X4 float4x4;

inline xmvec toSimd(float2 v) {
	return DirectX::XMLoadFloat2(&v);
}

inline xmvec toSimd(float3 v) {
	return DirectX::XMLoadFloat3(&v);
}

inline float3 toFloat3(xmvec v) {
	float3 r;
	DirectX::XMStoreFloat3(&r, v);
	return r;
}

inline xmvec toSimd(float4 v) {
	return DirectX::XMLoadFloat4(&v);
}

inline xmvec toSimd(uint2 v) {
	return DirectX::XMLoadUInt2(&v);
}

inline xmvec toSimd(uint3 v) {
	return DirectX::XMLoadUInt3(&v);
}

inline xmvec toSimd(uint4 v) {
	return DirectX::XMLoadUInt4(&v);
}

inline xmvec toSimd(int2 v) {
	return DirectX::XMLoadSInt2(&v);
}

inline xmvec toSimd(int3 v) {
	return DirectX::XMLoadSInt3(&v);
}

inline xmvec toSimd(int4 v) {
	return DirectX::XMLoadSInt4(&v);
}

inline xmmatrix toSimd(float4x4 const& v) {
	return DirectX::XMLoadFloat4x4(&v);
}

inline u32 pad_pow_2(u32 v) {
	--v;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	++v;
	return v;
}

inline u32 find_log_2(u32 v) {
	const u32 b[] = { 0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000 };
	const u32 S[] = { 1, 2, 4, 8, 16 };
	int i;

	u32 r = 0; // result of log2(v) will go here
	for (i = 4; i >= 0; i--) // unroll for speed...
	{
		if (v & b[i])
		{
			v >>= S[i];
			r |= S[i];
		}
	}
	return r;
}

}