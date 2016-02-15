cbuffer Frame : register(b0)
{       
    matrix ViewProj;
}

cbuffer Object : register(b1)
{       
    matrix World;
}

cbuffer Material : register(b2)
{
	float BaseColorMult;
	float RoughnessMult;
	float MetalnessMult;
}

Texture2D NormalTexture 	: register(t0);
Texture2D BaseColorTexture 	: register(t1);
Texture2D RoughnessTexture 	: register(t2);
SamplerState Sampler : register(s0);

struct VIn 
{
	float3 	position : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
	float3 	tangent : TANGENT;
	float3 	bitangent : BITANGENT;
};

struct VOut
{
	float4 	position : SV_POSITION;
	float3 	wposition : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
	float3 	tangent : TANGENT;
	float3 	bitangent : BItANGENT;
};

float2 OctWrap( float2 v )
{
    return ( 1.0 - abs( v.yx ) ) * ( v.xy >= 0.0 ? 1.0 : -1.0 );
}
 
float2 Encode( float3 n )
{
    n /= ( abs( n.x ) + abs( n.y ) + abs( n.z ) );
    n.xy = n.z >= 0.0 ? n.xy : OctWrap( n.xy );
    n.xy = n.xy * 0.5 + 0.5;
    return n.xy;
}

VOut VShader(VIn input, uint vertexId : SV_VertexID)
{
	VOut output;

	matrix objectMatrix = World;

	float4 position = float4(input.position, 1);
	position = mul(position, objectMatrix);
	output.wposition = position.xyz;
	position = mul(position, ViewProj);
	output.position = position;
	output.normal = normalize(mul(input.normal, (float3x3) objectMatrix));
	output.tangent = normalize(mul(input.tangent, (float3x3) objectMatrix));
	output.bitangent = normalize(mul(input.bitangent, (float3x3) objectMatrix));
	output.texcoord = input.texcoord;
	return output;
}

void PShader(VOut interpolated, out float4 gbufferA : SV_TARGET0, out float4 gbufferB : SV_TARGET1)
{
	float3 vNt = NormalTexture.Sample(Sampler, interpolated.texcoord).xyz * 2 - 1;
	float3 N = normalize( vNt.x * interpolated.tangent + vNt.y * interpolated.bitangent + vNt.z * interpolated.normal );
	float2 encN = Encode(N);

	uint2 bitpN = (encN.xy * 4095);
	float2 fencNxy = (bitpN.xy >> 4) / 255.f;
	float fencNz = (((bitpN.x & 15) << 4) + (bitpN.y & 15)) / 255.f;

	gbufferA = float4(BaseColorTexture.Sample(Sampler, interpolated.texcoord).xyz * BaseColorMult, MetalnessMult);
	gbufferB = float4(fencNxy, fencNz, RoughnessTexture.Sample(Sampler, interpolated.texcoord).x * RoughnessMult);
}