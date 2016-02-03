#ifndef SHADER_TYPES_H__
#define SHADER_TYPES_H__

#ifndef _MSC_VER
#define __HLSL
#endif

#ifdef __HLSL

// HLSL 
typedef matrix  		MATRIX;
typedef float4x3  		FLOAT4X3;
typedef float2  		FLOAT2;
typedef float3  		FLOAT3;
typedef float4  		FLOAT4;
typedef uint2  			UINT2;
typedef uint3  			UINT3;  										

typedef matrix  		MATRIXA;
typedef float4x3  		FLOAT4X3A;
typedef float2  		FLOAT2A;
typedef float3  		FLOAT3A;
typedef float4  		FLOAT4A;
typedef uint2  			UINT2A;
typedef uint3  			UINT3A;

#define ALIGNED_CB_STRUCT	struct 

#define REG(a,b) a##b

#define CONST static const

#else 
// C++

#define CONSTANTBUFFER_ALIGN	16

#define ALIGNED_CB			__declspec(align(CONSTANTBUFFER_ALIGN))
#define ALIGNED_CB_STRUCT	ALIGNED_CB struct
#define MATRIX			DirectX::XMMATRIX
#define FLOAT4X3		DirectX::XMFLOAT4X3
#define FLOAT2			float2
#define FLOAT3			float3
#define FLOAT4			float4
typedef uint32_t		uint;
#define UINT2			uint2
#define UINT3			uint3

#define MATRIXA			ALIGNED_CB DirectX::XMFLOAT4X4
#define FLOAT4X3A		ALIGNED_CB DirectX::XMFLOAT4X3
#define FLOAT2A			ALIGNED_CB float2
#define FLOAT3A			ALIGNED_CB float3
#define FLOAT4A			ALIGNED_CB float4
#define UINT2A			ALIGNED_CB uint2
#define UINT3A			ALIGNED_CB uint3

#define CONST const

#endif

#endif
