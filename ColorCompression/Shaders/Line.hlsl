matrix 	ViewProj;

struct VIn 
{
	float3 	position : POSITION;
	float4	color	: COLOR;
};

struct VOut
{
	float4 	position : SV_POSITION;
	float4	color : COLOR;
};

VOut VShader(VIn input)
{
	VOut output;

	float4 position = float4(input.position, 1);
	position = mul(position, ViewProj);
	output.position = position;
	output.color = input.color;

	return output;
}

float4 PShader(VOut interpolated) : SV_TARGET
{
	return interpolated.color;
}