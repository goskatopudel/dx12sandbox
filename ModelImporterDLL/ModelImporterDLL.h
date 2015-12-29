#pragma once

#ifdef MODELIMPORTERDLL_EXPORTS
#define IMPORTFUNCSDLL_API __declspec(dllexport) 
#else
#define IMPORTFUNCSDLL_API __declspec(dllimport) 
#endif

#include <DirectXMath.h>
#include "Types.h"

namespace Importer
{

const u32 NULL_INDEX = 0xFFFFFFFF;

enum LoadResultEnum {
	UNKOWN = 0,
	IMPORT_ERROR,
	OK
};

struct submesh_definition {
	char	name[128];
	u32		indexCount;
	u32		startIndex;
	u32		baseVertex;
	u32		materialId;
};

struct material_definition {
	char diffuseTexture		[128];
	char normalmapTexture	[128];
	char roughnessTexture	[128];
	char metallicnessTexture[128];
	char alphaTexture		[128];
};

struct bone_definition {
	char						name[128];
	u64							name_hash;
	u32							node_index;
	DirectX::XMFLOAT4X4			offset_matrix;
};

struct animation_node_t {
	char						name[128];
	u64							name_hash;
	u32							parent_index;
	u32							channel_index;
	DirectX::XMFLOAT4X4			local_transform;
};

struct animation_channel_t {
	u32							positions_offset;
	u32							positions_num;
	u32							rotations_offset;
	u32							rotations_num;
};

struct position_key_t {
	DirectX::XMFLOAT3A			value;
	float						time;
};

struct rotation_key_t {
	DirectX::XMFLOAT4			value;
	float						time;
};

struct animation_t {
	char						name[128];
	u64							name_hash;
	float						ticks_per_second;
	float						duration;
								
	u32							channels_offset;
	u32							channels_num;
								
	u32							position_keys_num;
	u32							rotation_keys_num;
};

struct model_definition {
	LoadResultEnum				loadResult;
	const char*					loadErrorMessage;

	u32							verticesNum;
	u32							indicesNum;
	u32							trianglesNum;
	u32							submeshesNum;
	u32							materialsNum;
	u32							bonesNum;
	u32							animationNodesNum;
	u32							animationsNum;
	DirectX::XMFLOAT3			boundingBoxMin;
	DirectX::XMFLOAT3			boundingBoxMax;
	DirectX::XMFLOAT3			boundingSphereCenter;
	float						boundingSphereRadius;

	const u32*					indices;
	const DirectX::XMFLOAT3*	positions;
	const DirectX::XMFLOAT2*	texcoords;
	const DirectX::XMFLOAT2*	texcoords1;
	const DirectX::XMFLOAT3*	normals;
	const DirectX::XMFLOAT3*	tangents;
	const DirectX::XMFLOAT3*	bitangents;
	const DirectX::XMFLOAT4*	colors;
	const DirectX::XMUINT4*		boneIndices;
	const DirectX::XMFLOAT4*	boneWeights;

	const submesh_definition*	submeshes;
	const material_definition*	materials;
	const bone_definition*		bones;

	const animation_node_t*		animationNodes;
	const animation_t*			animations;
	const animation_channel_t*	animationChannels;
	const position_key_t*		animationPositionKeys;
	const rotation_key_t*		animationRotationKeys;
};

typedef void* allocated_memory_handle;

IMPORTFUNCSDLL_API allocated_memory_handle LoadModel(const char* path, model_definition* outData);
IMPORTFUNCSDLL_API void FreeMemory(allocated_memory_handle handle);

}