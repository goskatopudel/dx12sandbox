matrix 	World;
matrix 	ViewProj;

matrix 	BoneTransform[60];

// Texture2D<float4> 	AlbedoTexture 		: register(t0, space0);
// Texture2D<float4> 	AlphaTexture		: register(t0, space1);
// Texture2D<float4> 	NormalmapTexture	: register(t1, space0);
// Texture2D<float4> 	MetalicnessTexture 	: register(t2, space0);
// Texture2D<float4> 	RoughnessTexture 	: register(t3, space0);
// SamplerState    	TextureSampler : register(s0);

struct VIn 
{
	float3 	position : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
	uint4	boneInd : BONE_INDICES;
	float4  boneWeights : BONE_WEIGHTS;  
};

struct VOut
{
	float4 	position : SV_POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
};

VOut VShader(VIn input, uint vertexId : SV_VertexID)
{
	VOut output;

	matrix boneTransform = 0;
	[unroll]
	for(int i=0; i<4; ++i) {
		boneTransform += BoneTransform[input.boneInd[i]] * input.boneWeights[i];
	}

	matrix objectMatrix = mul(boneTransform, World);

	float4 position = float4(input.position, 1);
	position = mul(position, objectMatrix);
	position = mul(position, ViewProj);
	output.position = position;
	output.normal = mul(input.normal, (float3x3) objectMatrix);
	output.texcoord = input.texcoord;
	return output;
}

float4 PShader(VOut interpolated) : SV_TARGET
{
	return float4(normalize(interpolated.normal) * 0.5 + 0.5, 1);
	//return float4(interpolated.texcoord, 0, 1);
}