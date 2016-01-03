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

void PShader(VOut interpolated, out float4 outColor : SV_TARGET0, out uint outLOD : SV_TARGET1)
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

	//uint ulod = clamp(lod, 0.f, 14.f);

	outLOD = (uint) lod;

	// uint old_tmp;
	// //InterlockedMax(PagesTexture[page_texel], ulod, old_tmp);
	// PagesTexture[page_texel] = ulod;

	outColor = float4(frac(smuv.xy * 256.f * 32.f * 2.f), 0, 0);
	//float4(pow(frac(smuv * 100), 2.2f), 0, 0);
}