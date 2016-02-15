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
	output.texcoord.y = 1 - (float)(vertexId % 2) * 2;

	return output;
}

Texture2D<float> 	DepthBuffer : register(t0);
Texture2D<float4> 	GBufferA : register(t1);
Texture2D<float4> 	GBufferB : register(t2);
SamplerState    	Sampler : register(s0);

cbuffer Frame : register(b0) 
{
	matrix Projection;
	matrix View;
	matrix InvView;
}

cbuffer Light : register(b1)
{
	float3 LightIntensity;
	float3 LightDirection;
}

float linearizeDepth(float depth, float proj33, float proj43) {
	return proj43 / (depth - proj33);
}

static const float M_PI = 3.14159265358979323846;
static const float M_1_DIV_PI = 0.318309886183790671538;

float2 OctWrap( float2 v )
{
    return ( 1.0 - abs( v.yx ) ) * ( v.xy >= 0.0 ? 1.0 : -1.0 );
}
 
float3 Decode( float2 encN )
{
    encN = encN * 2.0 - 1.0;
 
    float3 n;
    n.z = 1.0 - abs( encN.x ) - abs( encN.y );
    n.xy = n.z >= 0.0 ? encN.xy : OctWrap( encN.xy );
    n = normalize( n );
    return n;
}

float3 FSchlick(float3 f0, float vh) 
{
	return f0 + (1-f0)*exp2((-5.55473 * vh - 6.98316)*vh);
}

float VSmithSchlick(float nl, float nv, float a) {
	return 1./ ( (nl*(1-a)+a) * (nv*(1-a)+a) );
}

float DGGX(float nh, float a) 
{
	float a2 = max(a*a, 0.00001f);
	float denom = pow(nh*nh * (a2-1) + 1, 2);
	return a2 * M_1_DIV_PI / denom;
}

float OrenNayar(float nl, float nv, float g) {
	float r2 = pow(1-g, 2);
	float A = 1. - 0.5 * r2 / (r2 + 0.57);
	float B = 0.45 * r2 / (r2 + 0.09);
	float C = sqrt((1.0 - nv*nv) * (1.0 - nl*nl)) / max(nv, nl);
	return saturate((A + B * C) * M_1_DIV_PI);
}

float4 PShader(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	float hwDepth = DepthBuffer[position.xy].r;
	if(hwDepth == 1) 
		discard;

	float depth = linearizeDepth(DepthBuffer[position.xy].x, Projection._33, Projection._43);
	const float rayX = 1./Projection._11;
	const float rayY = 1./Projection._22;
	float3 screenRay = float3(lerp( -rayX, rayX, texcoord.x ), lerp( rayY, -rayY, texcoord.y ), 1.);

	float3 V = -mul(screenRay, transpose((float3x3)View));

	float3 wPosition = -V * depth + InvView._14_24_34;

	uint3 bituN = GBufferB[position.xy].xyz * 255;
	float2 pN = (bituN.xy << 4) / 4095.f;
	pN += (uint2(bituN.z >> 4, bituN.z) & 15) / 4095.f;

	float3 N = Decode(pN);

	float Roughness = GBufferB[position.xy].w;
	float Metalness = GBufferA[position.xy].w;
	float3 BaseColor = GBufferA[position.xy].xyz;

	float3 L = -LightDirection;
	float3 H = normalize(L + V);

	float NL = saturate(dot(L, N));
	float NV = saturate(dot(N, V));
	float NH = saturate(dot(N, H));
	float VH = saturate(dot(V, H));	

	const float DIELECTRIC_F0 = 0.04f;
	float3 Albedo = lerp(BaseColor, 0, Metalness);
	float3 F0 = lerp(DIELECTRIC_F0, BaseColor, Metalness);


	float a = Roughness * Roughness;
	float3 specular = DGGX(NH, a) * saturate(VSmithSchlick(NL, NV, a)) * FSchlick(F0, VH) / 4;
	float3 diffuse = Albedo * OrenNayar(NL, NV, 1 - Roughness);

	float3 directLight = M_PI * NL * LightIntensity * (specular + diffuse);

	return float4(directLight, 1);
}
