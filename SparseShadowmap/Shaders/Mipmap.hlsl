Texture2D<uint> 	LowerLevel : register( t0 );
RWTexture2D<uint>	CurrentLevel : register( u0 );

[numthreads( 8, 8, 1 )]
void BuildMinMip( uint3 DTid : SV_DispatchThreadID ) 
{
	uint A = LowerLevel[DTid.xy*2];
	uint B = LowerLevel[DTid.xy*2 + uint2(1,0)];
	uint C = LowerLevel[DTid.xy*2 + uint2(1,1)];
	uint D = LowerLevel[DTid.xy*2 + uint2(0,1)];
	CurrentLevel[DTid.xy] = max(max(A,B),max(C,D));
}