#pragma once

#include "Types.h"
#include "Resources.h"
#include "Collections.h"
#include "Hash.h"

#include "VectorMath.h"

namespace Essence {

typedef GenericHandle32<20, TYPE_ID(Model)> model_handle;

struct simple_mesh_vertex_t {
	float3	position;
	float3	normal;
	float2	texcoord0;
};

struct mesh_vertex_t {
	float3	position;
	float3	normal;
	float2	texcoord0;
	float3	tangent;
	float3	bitangent;
};

struct animated_mesh_vertex_t {
	float3	position;
	float3	normal;
	float2	texcoord0;
	float3	tangent;
	float3	bitangent;
	u32		boneIndices;
	float4	boneWeights;
};

struct animation_skeleton_t {
	u32					nodes_num;
	u32					bones_num;

	xmmatrix*			node_local_transforms;
	u16*				node_parents;
	u16*				node_channel_indices;
	u16*				bone_node_indices;
	xmmatrix*			bone_offsets;
};

struct position_key_t {
	float3a	value;
	float	time;
};

struct rotation_key_t {
	float4	value;
	float	time;
};

struct animation_channel_t {
	u32						position_keys_num;
	u32						rotation_keys_num;
	position_key_t*			position_keys;
	rotation_key_t*			rotation_keys;
};

struct animation_t {
	float					ticks_per_second;
	float					duration; // in ticks
	u32						channels_num;
	animation_channel_t*	channels;
	position_key_t*			position_keys;
	rotation_key_t*			rotation_keys;
};

struct animation_state_t {
	float	last_time;
	float	last_scaled_time;
	u16*	last_position_keys;
	u16*	last_rotation_keys;
};

struct mesh_draw_t {
	u32 index_count;
	u32 start_index;
	u32 base_vertex;
};

struct model_t {
	resource_handle				vertex_buffer;
	resource_handle				index_buffer;
	vertex_factory_handle		vertex_layout;

	u32							vertex_stride : 16;
	u32							index_stride : 16;

	u32							vertices_num;
	u32							indices_num;

	array_view<mesh_draw_t>		submeshes;
	animation_skeleton_t		skeleton;
	array_view<animation_t>		animations;

	array_view<Vec3f>			raw_positions;
	array_view<u32>				raw_indices;
};

void FreeModelsMemory();
void LoadModel(ResourceNameId name);

void InitAnimationState(animation_state_t*, model_t const*, u32);
void FreeAnimationState(animation_state_t* AnimationState);

// todo-consistency: provide comand list to do the copy on
model_handle		GetModel(ResourceNameId);
model_t const*		GetModelRenderData(model_handle);

void calculate_animation_frames(animation_t const* Animation, animation_state_t* AnimationState, float Time, Array<xmmatrix> *outTransforms);
void calculate_animation(animation_skeleton_t const* Skeleton, animation_t const* Animation, animation_state_t* AnimationState, float Time, Array<xmmatrix> *outNodeTransforms, Array<xmmatrix> *outTransforms);
void calculate_animation(animation_skeleton_t const* Skeleton, animation_t const* Animation, animation_state_t* AnimationState, float Time, xmmatrix *outTransforms);

}