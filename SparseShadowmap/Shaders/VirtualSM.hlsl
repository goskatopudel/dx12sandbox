Texture2D<float>	DepthBuffer : register( t0 );
Texture2D<uint>		ShadowLevel	: register( t1 );
RWTexture2D<uint> 	PagesTexture : register( u0 );

matrix 				ViewMatrix;
matrix 				ProjectionMatrix;
matrix				ShadowmapMatrix;
float2				Resolution;
float3 				CameraPos;

[numthreads( 8, 8, 1 )]
void PreparePages( uint3 DTid : SV_DispatchThreadID )
{
	float depth = DepthBuffer[DTid.xy].r;
	uint level = ShadowLevel[DTid.xy].r;

	float2 uv = ((float2)DTid.xy + 0.5f) / Resolution;

	const float ray_x = 1./ProjectionMatrix._11;
	const float ray_y = 1./ProjectionMatrix._22;
	float3 screen_ray = float3(lerp( -ray_x, ray_x, uv.x ), lerp( -ray_y, ray_y, 1-uv.y ), 1.);
	float3 V = mul(screen_ray, transpose((float3x3)ViewMatrix));
	float linear_depth = ProjectionMatrix._43 / (depth.r - ProjectionMatrix._33);

	float3 world_pos = linear_depth * V + CameraPos;

	float4 smpos = mul(float4(world_pos, 1), ShadowmapMatrix);
	smpos /= smpos.w;

	float2 smuv = saturate(smpos.xy * 0.5 + 0.5);

	uint2 page = smuv.xy * 128;

	uint old_level;
	InterlockedMax(PagesTexture[page], (uint)level, old_level);
}
