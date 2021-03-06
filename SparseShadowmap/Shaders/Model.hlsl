matrix 	World;
matrix 	ViewProj;

matrix 				DirectionalLightMatrix;
RWTexture2D<uint> 	PagesTexture : register( u1 );

struct VIn 
{
	float3 	position : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
};

struct VOut
{
	float4 	position : SV_POSITION;
	float3 	wposition : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
};

VOut VShader(VIn input, uint vertexId : SV_VertexID)
{
	VOut output;

	matrix objectMatrix = World;

	float4 position = float4(input.position, 1);
	position = mul(position, objectMatrix);
	output.wposition = position.xyz;
	position = mul(position, ViewProj);
	output.position = position;
	output.normal = mul(input.normal, (float3x3) objectMatrix);
	output.texcoord = input.texcoord;
	return output;
}

void ShadowLodPShader(VOut interpolated, out float4 outColor : SV_TARGET0, out uint outLOD : SV_TARGET1)
{
	float4 smpos = mul(float4(interpolated.wposition, 1), DirectionalLightMatrix);
	smpos /= smpos.w;

	float2 smuv = saturate(smpos.xy * 0.5 + 0.5);

	float2 dx = ddx(smuv);
	float2 dy = ddy(smuv);

	float lod = floor(-log2(sqrt(abs(dx.x*dy.y-dx.y*dy.x))));

	float color = pow((lod - 10) / 10, 2.2f);

	float2 page_texture_res = 128;
	uint2 page_texel = smuv.xy * page_texture_res;

	outLOD = (uint) lod;
	outColor = float4(frac(smuv.xy * 256.f * 32.f * 2.f), 0, 0);
}

float3 HueToRgb(float H) {
    float R = abs(H * 6 - 3) - 1;
    float G = 2 - abs(H * 6 - 2);
    float B = 2 - abs(H * 6 - 4);
    return saturate(float3(R,G,B));
}

float3 HsvToRgb(float3 HSV) {
    float3 RGB = HueToRgb(HSV.x);
    return ((RGB - 1) * HSV.y + 1) * HSV.z;
}

Texture2D<float> 	Shadowmap : register( t0 );
Texture2D<uint>		ShadowMipLookup : register( t1, space0 );
Texture2D<uint>		ShadowMipLookupPrev : register( t1, space1 );
SamplerState    	Sampler : register(s0);
float3 				LightDirection;

void PShader(VOut interpolated, out float4 outColor : SV_TARGET0)
{
	float4 smpos = mul(float4(interpolated.wposition, 1), DirectionalLightMatrix);
	smpos /= smpos.w;
	smpos.y *= -1;
	float2 smuv = saturate(smpos.xy * 0.5 + 0.5);
	uint2 page = smuv.xy * 128;
	uint2 lmip = uint2(ShadowMipLookup[page].r, ShadowMipLookupPrev[page].r);

	uint width, height, mipLevels;
	Shadowmap.GetDimensions(0, width, height, mipLevels);
	float2 mip = clamp((float2)mipLevels.xx - 1 - (float2)lmip, 0, mipLevels.xx - 1);

	float smz = Shadowmap.SampleLevel(Sampler, smuv, max(mip.x, mip.y)).r;

	float lit = smz > (smpos.z - 0.0001f);
	float light = saturate(dot(normalize(interpolated.normal), LightDirection));

	outColor = (lit * light + 0.05f);
}
