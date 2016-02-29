Texture2D<float4> 	Image : register( t0 );
RWTexture2D<uint4>	BcData : register( u0 );

groupshared float3 Samples[4][4][4];
groupshared uint   B[8][2][4];
groupshared float3 ColorMin[4];
groupshared float3 ColorMax[4];
groupshared uint   uColorMin[4];
groupshared uint   uColorMax[4];

float3 LinearToSRGB( float3 x )
{
	// This can be 9 cycles faster than the "precise" version
	return x < 0.0031308 ? 12.92 * x : 1.13005 * sqrt(abs(x - 0.00228)) - 0.13448 * x + 0.005719;
}

float3 LinearToSRGB_Exact( float3 x )
{
	return x < 0.0031308 ? 12.92 * x : 1.055 * pow(abs(x), 1.0 / 2.4) - 0.055;
}

[numthreads( 8, 8, 1 )]
void BC1( uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID ) 
{
	uint2 blockTexel = GTid.xy % 4;
	uint groupBlock = dot(GTid.xy / 4, uint2(2, 1));

	float3 Color = Image[DTid.xy].xyz;
	Samples[blockTexel.x][blockTexel.y][groupBlock] = Color;
	
	GroupMemoryBarrierWithGroupSync();

	uint2 blockIndex = DTid.xy / 4;

	uint i, j;

	if(blockTexel.x == 0 & blockTexel.y == 0) {
		ColorMin[groupBlock] = Samples[0][0][groupBlock];

		for(uint i = 0; i<4; ++i) {
			for(uint j = 0; j<4; ++j) {
				ColorMin[groupBlock] = min(ColorMin[groupBlock], Samples[i][j][groupBlock]);
			}
		}

		float3 EncColor = ColorMin[groupBlock];
		uint3 color0  = LinearToSRGB_Exact(EncColor) * 255;
		color0 = color0 / uint3(8, 4, 8);
		uint color0_565 = dot(color0, float3(2048, 32, 1));

		uColorMin[groupBlock] = color0_565;
	}
	if(blockTexel.x == 1 & blockTexel.y == 0) {
		ColorMax[groupBlock] = Samples[0][0][groupBlock];

		for(uint i = 0; i<4; ++i) {
			for(uint j = 0; j<4; ++j) {
				ColorMax[groupBlock] = max(ColorMax[groupBlock], Samples[i][j][groupBlock]);
			}
		}

		float3 EncColor = ColorMax[groupBlock];
		uint3 color1  = LinearToSRGB_Exact(EncColor) * 255;
		color1 = color1 / uint3(8, 4, 8);
		uint color1_565 = dot(color1, float3(2048, 32, 1));

		uColorMax[groupBlock] = color1_565;
	}

	GroupMemoryBarrierWithGroupSync();

	float3 E0 = ColorMax[groupBlock];

	float3 V = ColorMin[groupBlock] - E0;
	float Len = length(V);
	V /= Len;

	float p = dot(V, Color - E0) / Len;

	uint index = (blockTexel.y & 1) * 4 + blockTexel.x;

	uint bits = dot(p > float3(0.25f, 0.5f, 0.75f), float3(1, 2, 4));
	bits = bits == 3 ? 2 : bits;
	bits = bits == 7 ? 3 : bits;

	if(uColorMax[groupBlock] == uColorMin[groupBlock])
		bits = min(bits, 2);
	bits <<= 2 * index;
	B[index][blockTexel.y > 1][groupBlock] = bits;

	GroupMemoryBarrierWithGroupSync();	

	if(blockTexel.x == 0 & blockTexel.y == 0) {
		uint b0 = 0, b1 = 0;

		for(uint i = 0; i<8; ++i) {
			b0 |= B[i][0][groupBlock];
			b1 |= B[i][1][groupBlock];
		}

		BcData[blockIndex] = uint4(uColorMax[groupBlock], uColorMin[groupBlock], b0, b1);
	}
}

// http://www.nvidia.com/object/real-time-ycocg-dxt-compression.html

float3 LinearToYCoCg(float3 rgb) {
	return mul(float3x3(
		0.25f, 0.5f, 0.25f,
		0.5f, 0.f, -0.5f,
		-0.25f, 0.5f, -0.25f),
		rgb);
}

void FindMinMaxColorsBox(float3 block[16], out float3 mincol, out float3 maxcol)
{
    mincol = float3(1, 1, 1);
    maxcol = float3(0, 0, 0);
    
    for (int i = 0; i < 16; i++) {
        mincol = min(mincol, block[i]);
        maxcol = max(maxcol, block[i]);
    }
}

void InsetYBBox(in out float mincol, in out float maxcol)
{
    float inset = (maxcol - mincol) / 32.0 - (16.0 / 255.0) / 32.0;
    mincol = saturate(mincol + inset);
    maxcol = saturate(maxcol - inset);
}

void InsetCoCgBBox(in out float2 mincol, in out float2 maxcol)
{
    float2 inset = (maxcol - mincol) / 16.0 - (8.0 / 255.0) / 16;
    mincol = saturate(mincol + inset);
    maxcol = saturate(maxcol - inset);
}

void SelectDiagonal(float3 block[16], in out float3 mincol, in out float3 maxcol)
{
    float3 center = (mincol + maxcol) * 0.5;
    
    float2 cov = 0;
    for (int i = 0; i < 16; i++)
    {
        float3 t = block[i] - center;
        cov.x += t.x * t.z;
        cov.y += t.y * t.z;
    }
    
    if (cov.x < 0) {
        float temp = maxcol.x;
        maxcol.x = mincol.x;
        mincol.x = temp;
    }
    if (cov.y < 0) {
        float temp = maxcol.y;
        maxcol.y = mincol.y;
        mincol.y = temp;
    }
}

static const float OFFSET = 128.0 / 255.0;

uint GetYCoCgScale(float2 minColor, float2 maxColor)
{
    float2 m0 = abs(minColor - OFFSET);
    float2 m1 = abs(maxColor - OFFSET);
    
    float m = max(max(m0.x, m0.y), max(m1.x, m1.y));
    
    const float s0 = 64.0 / 255.0;
    const float s1 = 32.0 / 255.0;
    
    uint scale = 1;
    if (m < s0) scale = 2;
    if (m < s1) scale = 4;
    
    return scale;
}

uint EmitEndPointsYCoCgDXT5(in out float2 mincol, in out float2 maxcol, int scale)
{
    maxcol = (maxcol - OFFSET) * scale + OFFSET;
    mincol = (mincol - OFFSET) * scale + OFFSET;
    
    InsetCoCgBBox(mincol, maxcol);
    
    maxcol = round(maxcol * float2(31, 63));
    mincol = round(mincol * float2(31, 63));
    
    uint2 imaxcol = maxcol;
    uint2 imincol = mincol;
    
    uint2 output;
    output.x = (imaxcol.r << 11) | (imaxcol.g << 5) | (scale - 1);
    output.y = (imincol.r << 11) | (imincol.g << 5) | (scale - 1);
    
    imaxcol.r = (imaxcol.r << 3) | (imaxcol.r >> 2);
    imaxcol.g = (imaxcol.g << 2) | (imaxcol.g >> 4);
    imincol.r = (imincol.r << 3) | (imincol.r >> 2);
    imincol.g = (imincol.g << 2) | (imincol.g >> 4);
    
    maxcol = (float2)imaxcol * (1.0 / 255.0);
    mincol = (float2)imincol * (1.0 / 255.0);
    
    // Undo rescale.
    maxcol = (maxcol - OFFSET) / scale + OFFSET;
    mincol = (mincol - OFFSET) / scale + OFFSET;
    
    return output.x | (output.y << 16);
}
 
float colorDistance(float2 c0, float2 c1)
{
    return dot(c0-c1, c0-c1);
}

uint EmitIndicesYCoCgDXT5(float3 block[16], float2 mincol, float2 maxcol)
{
    // Compute palette
    float2 c[4];
    c[0] = maxcol;
    c[1] = mincol;
    c[2] = lerp(c[0], c[1], 1.0/3.0);
    c[3] = lerp(c[0], c[1], 2.0/3.0);
    
    // Compute indices
    uint indices = 0;
    for (int i = 0; i < 16; i++)
    {
        // find index of closest color
        float4 dist;
        dist.x = colorDistance(block[i].yz, c[0]);
        dist.y = colorDistance(block[i].yz, c[1]);
        dist.z = colorDistance(block[i].yz, c[2]);
        dist.w = colorDistance(block[i].yz, c[3]);
        
        uint4 b = dist.xyxy > dist.wzzw;
        uint b4 = dist.z > dist.w;
        
        uint index = (b.x & b4) | (((b.y & b.z) | (b.x & b.w)) << 1);
        indices |= index << (i*2);
    }
    
    // Output indices
    return indices;
}

uint2 EmitAlphaIndicesYCoCgDXT5(float3 block[16], float minAlpha, float maxAlpha)
{
    const uint ALPHA_RANGE = 7;
    uint i;
    
    float mid = (maxAlpha - minAlpha) / (2.0 * ALPHA_RANGE);
    
    float ab1 = minAlpha + mid;
    float ab2 = (6 * maxAlpha + 1 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab3 = (5 * maxAlpha + 2 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab4 = (4 * maxAlpha + 3 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab5 = (3 * maxAlpha + 4 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab6 = (2 * maxAlpha + 5 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab7 = (1 * maxAlpha + 6 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    
    uint2 indices = 0;
    
    uint index;
    for (i = 0; i < 6; i++)
    {
        float a = block[i].x;
        index = 1;
        index += (a <= ab1);
        index += (a <= ab2);
        index += (a <= ab3);
        index += (a <= ab4);
        index += (a <= ab5);
        index += (a <= ab6);
        index += (a <= ab7);
        index &= 7;
        index ^= (2 > index);
        indices.x |= index << (3 * i + 16);
    }
    
    indices.y = index >> 1;
    
    for (i = 6; i < 16; i++)
    {
        float a = block[i].x;
        index = 1;
        index += (a <= ab1);
        index += (a <= ab2);
        index += (a <= ab3);
        index += (a <= ab4);
        index += (a <= ab5);
        index += (a <= ab6);
        index += (a <= ab7);
        index &= 7;
        index ^= (2 > index);
        indices.y |= index << (3 * i - 16);
    }
    
    return indices;
}

void SelectYCoCgDiagonal(const float3 block[16], in out float2 minColor, in out float2 maxColor)
{
    float2 mid = (maxColor + minColor) * 0.5;
    
    float cov = 0;
    for (int i = 0; i < 16; i++)
    {
        float2 t = block[i].yz - mid;
        cov += t.x * t.y;
    }
    if (cov < 0) {
        float tmp = maxColor.y;
        maxColor.y = minColor.y;
        minColor.y = tmp;
    }
}

uint EmitAlphaEndPointsYCoCgDXT5(in out float mincol, in out float maxcol)
{
    InsetYBBox(mincol, maxcol);
    
    uint c0 = round(mincol * 255);
    uint c1 = round(maxcol * 255);
    
    return (c0 << 8) | c1;
}
 
float3 SRGBToLinear_Exact( float3 x )
{
	return x < 0.04045 ? x / 12.92 : pow( (abs(x) + 0.055) / 1.055, 2.4 );
}

[numthreads( 8, 8, 1 )]
void BC3( uint3 DTid : SV_DispatchThreadID) {
	
	float3 block[16];

	uint i, j;

	for(i=0; i<4; ++i) {
		for(j=0; j<4; ++j) {
			block[i*4+j] = LinearToYCoCg(LinearToSRGB_Exact(Image[DTid.xy * 4 + uint2(j, i)]).xyz) + float3(0, OFFSET, OFFSET);
		}
	}

	float3 mincol, maxcol;
    FindMinMaxColorsBox(block, mincol, maxcol);

    SelectYCoCgDiagonal(block, mincol.yz, maxcol.yz);
    
    uint scale = GetYCoCgScale(mincol.yz, maxcol.yz);

    uint4 output;
    output.z = EmitEndPointsYCoCgDXT5(mincol.yz, maxcol.yz, scale);
    output.w = EmitIndicesYCoCgDXT5(block, mincol.yz, maxcol.yz);
    
    // Output Y in DXT5 alpha block.
    output.x = EmitAlphaEndPointsYCoCgDXT5(mincol.x, maxcol.x);
    uint2 indices = EmitAlphaIndicesYCoCgDXT5(block, mincol.x, maxcol.x);
    output.x |= indices.x;
    output.y = indices.y;

    //output.z = 0x0000FFFF;
    //output.z = 0xAAAAAAAA;
    //output.w = 0xAAAAAAAA;
    //output.w = 0x1F1F1F1F;

    BcData[DTid.xy] = output;
}