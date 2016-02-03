matrix 	ViewProj;

struct VIn 
{
	float4 	position : POSITION;
	float4	color	: COLOR;
};

struct VOut
{
	float4 	position : SV_POSITION;
	float4	color : COLOR;
};

VOut VShader2D(VIn input)
{
	VOut output;

	float4 position = input.position;
	position = mul(position, ViewProj);
	output.position = position;
	output.color = input.color;

	return output;
}

float4 PShader2D(VOut interpolated) : SV_TARGET
{
	return interpolated.color;
}