struct VIn
{
	float4 position : POSITION;
	float2 texcoord : TEXCOORD;
};

struct VOut
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

VOut VShader(VIn input)
{
	VOut output;

	output.position = input.position;
	output.texcoord = input.texcoord;

	return output;
}

float4 ColorPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return 0.5f;
}
