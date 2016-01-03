struct VOut
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

VOut VShader(uint vertexId : SV_VertexID)
{
	VOut output;
	output.position.x = (float)(vertexId / 2) * 4 - 1;
	output.position.y = (float)(vertexId % 2) * 4 - 1;
	output.position.z = 1;
	output.position.w = 1;

	output.texcoord.x = (float)(vertexId / 2) * 2;
	output.texcoord.y= 1 - (float)(vertexId % 2) * 2;

	return output;
}

Texture2D<float4> 	Image : register(t0, space0);
SamplerState    	Sampler : register(s0);

float4 				WriteColor;
float 				WriteDepth;

float4 CopyPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return Image.Sample(Sampler, texcoord);
}

float4 ColorPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return WriteColor;
}

float DepthPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_Depth
{
	return WriteDepth;
}

float 				Projection_33;
float 				Projection_43;

float4 LinearizeDepthPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return Projection_43 / (Image.Sample(Sampler, texcoord).r - Projection_33);
}