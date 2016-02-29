float3 LinearToSRGB( float3 x )
{
	// This can be 9 cycles faster than the "precise" version
	return x < 0.0031308 ? 12.92 * x : 1.13005 * sqrt(abs(x - 0.00228)) - 0.13448 * x + 0.005719;
}

float3 LinearToSRGB_Exact( float3 x )
{
	return x < 0.0031308 ? 12.92 * x : 1.055 * pow(abs(x), 1.0 / 2.4) - 0.055;
}

float3 SRGBToLinear_Exact( float3 x )
{
	return x < 0.04045 ? x / 12.92 : pow( (abs(x) + 0.055) / 1.055, 2.4 );
}

float3 LinearToYCoCg(float3 rgb) {
	return mul(float3x3(
		0.25f, 0.5f, 0.25f,
		0.5f, 0.f, -0.5f,
		-0.25f, 0.5f, -0.25f),
		rgb);
}

float3 YCoCgToLinear(float3 y) {
	return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
}

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

float4 Compress(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	uint2 pos = position.xy;
	bool ch = (pos.x & 1) == (pos.y & 1);

	float4 lin = Image.Sample(Sampler, texcoord);
	lin.xyz = LinearToSRGB_Exact(lin.xyz);
	float3 c = LinearToYCoCg(lin.xyz);
	c.yz += 0.5f;

	return ch ? c.xyyy : c.xzzz;
}

float Filter(float2 a0, float2 a1, float2 a2, float2 a3, float2 a4) {
	float4 lum = float4(a1.x, a2.x, a3.x, a4.x);
	float4 w = 1 - step(30.f/255.f, abs(lum - a0.x));
	float W = dot(w, 1.f);
	W = W == 0 ? W : 1/W;
	return dot(w, float4(a1.y, a2.y, a3.y, a4.y)) * W;
}

float4 Decompress(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	uint2 pos = position.xy;
	uint ch = (pos.x & 1) == (pos.y & 1);

	float2 a0 = Image[pos].xy;
	a0.y -= 0.5f;
	float3 yc = a0.xyy;

	float2 a1 = Image[pos + int2(-1, 0)].xy + float2(0, -0.5f);
	float2 a2 = Image[pos + int2(1, 0)].xy + float2(0, -0.5f);
	float2 a3 = Image[pos + int2(0, 1)].xy + float2(0, -0.5f);
	float2 a4 = Image[pos + int2(0, -1)].xy + float2(0, -0.5f);
	float f = Filter(a0, a1, a2, a3, a4);

	yc.yz = ch ? float2(yc.y, f) : float2(f, yc.z);

	return SRGBToLinear_Exact(YCoCgToLinear(yc.xyz)).xyzz;
}

float4 DecompressBC3YCoCg(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	float4 sample = Image[position.xy];

	const float OFFSET = 128.0 / 255.0;

	float Y = sample.a;
    float scale = 1.0 / ((255.0 / 8.0) * sample.b + 1);
    float Co = (sample.r - OFFSET) * scale;
    float Cg = (sample.g - OFFSET) * scale;
    
    float R = Y + Co - Cg;
    float G = Y + Cg;
    float B = Y - Co - Cg;

	return float4(SRGBToLinear_Exact(float3(R, G, B)), 1);
}