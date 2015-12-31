#include "Model.h"
#include "Array.h"
#include "Hashmap.h"
#include <DirectXMath.h>
#include "Commands.h"
using namespace DirectX;
#include <Windows.h>
#include "../ModelImporterDLL/ModelImporterDLL.h"

namespace Essence {

const u16 SHORT_NULL_INDEX = 0xFFFF;

Hashmap<ResourceNameId, model_handle>	ModelsByName;
Freelist<model_t, model_handle>			Models;

void FreeModelsMemory() {

	for (auto kv : ModelsByName) {
		GetMallocAllocator()->Free(Models[kv.value].submeshes.elements);

		GetMallocAllocator()->Free(Models[kv.value].skeleton.bone_offsets);
		GetMallocAllocator()->Free(Models[kv.value].skeleton.bone_node_indices);
		GetMallocAllocator()->Free(Models[kv.value].skeleton.node_local_transforms);
		GetMallocAllocator()->Free(Models[kv.value].skeleton.node_parents);
		GetMallocAllocator()->Free(Models[kv.value].skeleton.node_channel_indices);

		for (auto i : MakeRange(Models[kv.value].animations.num)) {
			GetMallocAllocator()->Free(Models[kv.value].animations[i].channels);
			GetMallocAllocator()->Free(Models[kv.value].animations[i].position_keys);
			GetMallocAllocator()->Free(Models[kv.value].animations[i].rotation_keys);
		}

		GetMallocAllocator()->Free(Models[kv.value].animations.elements);
	}

	FreeMemory(ModelsByName);
	FreeMemory(Models);
}

extern GPUQueue* GGPUCopyQueue;

u32 packBoneIndices(uint4 const &v) {
	return ((v.x & 255) << 0) | ((v.y & 255) << 8) | ((v.z & 255) << 16) | ((v.w & 255) << 24);
}

void LoadModel(ResourceNameId name) {
	if (Contains(ModelsByName, name)) {
		return;
	}

	Importer::model_definition modelData;
	auto importedDataHandle = Importer::LoadModel(GetString(name), &modelData);

	Array<mesh_vertex_t> Vertices(GetThreadScratchAllocator());

	u32 maxIndex = 0;

	for (auto i = 0u; i < modelData.verticesNum; ++i) {
		mesh_vertex_t vertex;
		vertex.position = modelData.positions[i];
		vertex.normal = modelData.normals[i];
		vertex.texcoord0 = modelData.texcoords[i];
		vertex.boneIndices = packBoneIndices(modelData.boneIndices[i]);
		vertex.boneWeights = modelData.boneWeights[i];
		PushBack(Vertices, vertex);
	}

	auto copyCommands = GetCommandList(GGPUCopyQueue, NAME_("Copy"));

	model_t model;
	model.vertex_buffer = CreateBuffer(DEFAULT_MEMORY, Size(Vertices) * sizeof(mesh_vertex_t), sizeof(mesh_vertex_t), ALLOW_VERTEX_BUFFER, Format("vertex buffer of %s", (const char*)GetString(name)));
	CopyToBuffer(copyCommands, model.vertex_buffer, Vertices.DataPtr, Size(Vertices) * sizeof(mesh_vertex_t));
	model.index_buffer = CreateBuffer(DEFAULT_MEMORY, modelData.indicesNum * sizeof(u32), sizeof(u32), ALLOW_INDEX_BUFFER, Format("index buffer of %s", (const char*)GetString(name)));
	CopyToBuffer(copyCommands, model.index_buffer, modelData.indices, modelData.indicesNum * sizeof(u32));
	model.vertices_num = modelData.verticesNum;
	model.indices_num = modelData.indicesNum;
	model.submeshes.num = modelData.submeshesNum;
	model.submeshes.elements = (mesh_draw_t*)GetMallocAllocator()->Allocate(sizeof(modelData.submeshes[0]) * modelData.submeshesNum, 8);
	model.vertex_layout = GetVertexFactory({ VertexInput::POSITION_3_32F, VertexInput::NORMAL_32F, VertexInput::TEXCOORD_32F, VertexInput::BONE_INDICES_8U, VertexInput::BONE_WEIGHTS_32F });

	Execute(copyCommands);

	for (auto i = 0u; i < model.submeshes.num; ++i) {
		model.submeshes[i].base_vertex = modelData.submeshes[i].baseVertex;
		model.submeshes[i].index_count = modelData.submeshes[i].indexCount;
		model.submeshes[i].start_index = modelData.submeshes[i].startIndex;
	}

	if (modelData.animationsNum) {
		allocate_array(&model.animations, modelData.animationsNum, GetMallocAllocator());
		zero_array(&model.animations);

		animation_skeleton_t skeleton = {};
		skeleton.bones_num = modelData.bonesNum;
		skeleton.nodes_num = modelData.animationNodesNum;
		allocate_c_array(skeleton.bone_offsets, GetMallocAllocator(), modelData.bonesNum);
		allocate_c_array(skeleton.bone_node_indices, GetMallocAllocator(), modelData.bonesNum);
		allocate_c_array(skeleton.node_local_transforms, GetMallocAllocator(), modelData.animationNodesNum);
		allocate_c_array(skeleton.node_parents, GetMallocAllocator(), modelData.animationNodesNum);
		allocate_c_array(skeleton.node_channel_indices, GetMallocAllocator(), modelData.animationNodesNum);

		for (auto i = 0u; i < skeleton.nodes_num; ++i) {
			skeleton.node_parents[i] = modelData.animationNodes[i].parent_index != Importer::NULL_INDEX ? modelData.animationNodes[i].parent_index : SHORT_NULL_INDEX;
			skeleton.node_local_transforms[i] = XMLoadFloat4x4(&modelData.animationNodes[i].local_transform);
			skeleton.node_channel_indices[i] = modelData.animationNodes[i].channel_index != Importer::NULL_INDEX ? modelData.animationNodes[i].channel_index : SHORT_NULL_INDEX;
		}

		for (auto i = 0u; i < skeleton.bones_num; ++i) {
			skeleton.bone_node_indices[i] = modelData.bones[i].node_index != Importer::NULL_INDEX ? modelData.bones[i].node_index : SHORT_NULL_INDEX;
			skeleton.bone_offsets[i] = XMLoadFloat4x4(&modelData.bones[i].offset_matrix);
		}

		model.skeleton = skeleton;
	}

	for (auto a : MakeRange(modelData.animationsNum)) {
		animation_t animation = {};
		allocate_c_array(animation.channels, GetMallocAllocator(), modelData.animations[a].channels_num);

		allocate_c_array(animation.position_keys, GetMallocAllocator(), modelData.animations[a].position_keys_num);
		allocate_c_array(animation.rotation_keys, GetMallocAllocator(), modelData.animations[a].rotation_keys_num);

		animation.channels_num = modelData.animations[a].channels_num;
		animation.duration = modelData.animations[a].duration;
		animation.ticks_per_second = modelData.animations[a].ticks_per_second;

		auto positionsOffset = 0u;
		auto rotationsOffset = 0u;
		for (auto i = 0u; i < animation.channels_num; ++i) {
			auto importedChannel = modelData.animationChannels[modelData.animations[a].channels_offset + i];

			for (auto j = 0u; j < importedChannel.positions_num; ++j) {
				auto outerIndex = importedChannel.positions_offset + j;
				auto innerIndex = positionsOffset + j;

				animation.position_keys[innerIndex].time = modelData.animationPositionKeys[outerIndex].time;
				animation.position_keys[innerIndex].value = modelData.animationPositionKeys[outerIndex].value;
			}

			for (auto j = 0u; j < importedChannel.rotations_num; ++j) {
				auto outerIndex = importedChannel.rotations_offset + j;
				auto innerIndex = rotationsOffset + j;

				animation.rotation_keys[innerIndex].time = modelData.animationRotationKeys[outerIndex].time;
				animation.rotation_keys[innerIndex].value = modelData.animationRotationKeys[outerIndex].value;
			}

			animation.channels[i].position_keys = animation.position_keys + positionsOffset;
			animation.channels[i].position_keys_num = importedChannel.positions_num;
			animation.channels[i].rotation_keys = animation.rotation_keys + rotationsOffset;
			animation.channels[i].rotation_keys_num = importedChannel.rotations_num;

			positionsOffset += importedChannel.positions_num;
			rotationsOffset += importedChannel.rotations_num;
		}


		model.animations[a] = animation;
	}

	auto handle = Create(Models);
	Models[handle] = model;
	ModelsByName[name] = handle;

	Importer::FreeMemory(importedDataHandle);
}

model_handle		GetModel(ResourceNameId name) {
	LoadModel(name);
	return ModelsByName[name];
}

model_t const*	GetModelRenderData(model_handle handle) {
	return &Models[handle];
}

void InitAnimationState(animation_state_t* AnimationState, model_t const* Model, u32 index) {
	allocate_c_array(AnimationState->last_position_keys, GetMallocAllocator(), Model->animations[index].channels_num);
	allocate_c_array(AnimationState->last_rotation_keys, GetMallocAllocator(), Model->animations[index].channels_num);
	ZeroMemory(AnimationState->last_position_keys, Model->animations[index].channels_num * sizeof(AnimationState->last_position_keys[0]));
	ZeroMemory(AnimationState->last_rotation_keys, Model->animations[index].channels_num * sizeof(AnimationState->last_rotation_keys[0]));
}

void FreeAnimationState(animation_state_t* AnimationState) {
	GetMallocAllocator()->Free(AnimationState->last_position_keys);
	GetMallocAllocator()->Free(AnimationState->last_rotation_keys);
	(*AnimationState) = {};
}

model_t const* GetModelRenderData(ResourceNameId name) {
	return &Models[ModelsByName[name]];
}

void calculate_animation_frames(
	animation_t const* Animation,
	animation_state_t* AnimationState,
	float Time,
	Array<xmmatrix> *outTransforms
	) {
	Array<xmmatrix>& transforms = *outTransforms;

	auto channelsNum = Animation->channels_num;
	Resize(transforms, channelsNum);

	auto duration = Animation->duration;
	float time = duration ? fmodf(Time * Animation->ticks_per_second, duration) : 0;

	for (auto c = 0u; c < channelsNum; ++c) {
		auto channel = Animation->channels[c];

		xmvec position;
		auto p = time > AnimationState->last_scaled_time ? AnimationState->last_position_keys[c] : 0u;
		while ((p + 1) < channel.position_keys_num && channel.position_keys[p + 1].time < time) {
			++p;
		}
		AnimationState->last_position_keys[c] = p;
		{
			auto next = min(p + 1, channel.position_keys_num - 1);

			auto key = channel.position_keys[p];
			auto nextKey = channel.position_keys[next];
			auto diffTime = nextKey.time - key.time;
			float blend = diffTime > 0 ? (time - key.time) / diffTime : 0;
			position = XMVectorLerp(XMLoadFloat3A(&channel.position_keys[p].value), XMLoadFloat3A(&channel.position_keys[next].value), blend);
			position = XMVectorSetW(position, 1.f);
		}

		xmvec qrotation;
		auto r = time > AnimationState->last_scaled_time ? AnimationState->last_rotation_keys[c] : 0u;
		while ((r + 1) < channel.rotation_keys_num && channel.rotation_keys[r + 1].time < time) {
			++r;
		}
		AnimationState->last_rotation_keys[c] = r;
		{
			auto next = min(r + 1, channel.rotation_keys_num - 1);

			auto key = channel.rotation_keys[r];
			auto nextKey = channel.rotation_keys[next];
			auto diffTime = nextKey.time - key.time;
			float blend = diffTime > 0 ? (time - key.time) / diffTime : 0;
			qrotation = XMQuaternionSlerp(XMLoadFloat4(&channel.rotation_keys[r].value), XMLoadFloat4(&channel.rotation_keys[next].value), blend);
			//qrotation = XMQuaternionNormalizeEst(XMVectorLerp(XMLoadFloat4(&channel.rotation_keys[r].value), XMLoadFloat4(&channel.rotation_keys[next].value), blend));
			
		}
		transforms[c] = XMMatrixRotationQuaternion(qrotation);
		transforms[c].r[3] = position;
	}

	AnimationState->last_scaled_time = time;
}

void calculate_animation(
	animation_skeleton_t const* Skeleton,
	animation_t const* Animation,
	animation_state_t* AnimationState,
	float Time,
	Array<xmmatrix> *outNodeTransforms,
	Array<xmmatrix> *outTransforms) {

	Array<xmmatrix> localTransformationMatrices(GetThreadScratchAllocator());
	Resize(localTransformationMatrices, Skeleton->nodes_num);

	Array<xmmatrix> fallbackNodeTransforms(GetThreadScratchAllocator());
	if (outNodeTransforms == nullptr) {
		outNodeTransforms = &fallbackNodeTransforms;
	}
	auto &globalTransformationMatrices = *outNodeTransforms;
	Resize(globalTransformationMatrices, Skeleton->nodes_num);

	Array<xmmatrix> animationMatrices(GetThreadScratchAllocator());
	calculate_animation_frames(Animation, AnimationState, Time, &animationMatrices);

	auto nodesNum = Skeleton->nodes_num;
	// calculate channels
	for (auto i = 0u; i < nodesNum; ++i) {
		localTransformationMatrices[i] = Skeleton->node_channel_indices[i] != SHORT_NULL_INDEX ? animationMatrices[Skeleton->node_channel_indices[i]] : Skeleton->node_local_transforms[i];
	}

	// calculate all nodes starting from root
	globalTransformationMatrices[0] = localTransformationMatrices[0];
	for (auto i = 1u; i < nodesNum; ++i) {
		globalTransformationMatrices[i] = localTransformationMatrices[i] * globalTransformationMatrices[Skeleton->node_parents[i]];
	}

	xmvec determinant;
	xmmatrix globalInverseMatrix = XMMatrixInverse(&determinant, globalTransformationMatrices[0]);
	auto& transformationMatrices = *outTransforms;
	Resize(transformationMatrices, Skeleton->bones_num);

	auto bonesNum = Skeleton->bones_num;
	for (auto i = 0u; i < bonesNum; ++i) {
		transformationMatrices[i] = Skeleton->bone_offsets[i] * globalTransformationMatrices[Skeleton->bone_node_indices[i]] * globalInverseMatrix;
	}
}


void calculate_animation(
	animation_skeleton_t const* Skeleton,
	animation_t const* Animation,
	animation_state_t* AnimationState,
	float Time,
	xmmatrix *outTransforms) {

	Array<xmmatrix> localTransformationMatrices(GetThreadScratchAllocator());
	Resize(localTransformationMatrices, Skeleton->nodes_num);

	Array<xmmatrix> nodeTransforms(GetThreadScratchAllocator());

	Array<xmmatrix> animationMatrices(GetThreadScratchAllocator());
	calculate_animation_frames(Animation, AnimationState, Time, &animationMatrices);

	Array<xmmatrix> globalTransformationMatrices(GetThreadScratchAllocator());
	Resize(globalTransformationMatrices, Skeleton->nodes_num);

	auto nodesNum = Skeleton->nodes_num;
	// calculate channels
	for (auto i = 0u; i < nodesNum; ++i) {
		localTransformationMatrices[i] = Skeleton->node_channel_indices[i] != SHORT_NULL_INDEX ? animationMatrices[Skeleton->node_channel_indices[i]] : Skeleton->node_local_transforms[i];
	}

	// calculate all nodes starting from root
	globalTransformationMatrices[0] = localTransformationMatrices[0];
	for (auto i = 1u; i < nodesNum; ++i) {
		globalTransformationMatrices[i] = localTransformationMatrices[i] * globalTransformationMatrices[Skeleton->node_parents[i]];
	}

	xmvec determinant;
	xmmatrix globalInverseMatrix = XMMatrixInverse(&determinant, globalTransformationMatrices[0]);

	auto bonesNum = Skeleton->bones_num;
	for (auto i = 0u; i < bonesNum; ++i) {
		outTransforms[i] = Skeleton->bone_offsets[i] * globalTransformationMatrices[Skeleton->bone_node_indices[i]] * globalInverseMatrix;
	}

	for (auto i = 0u; i < bonesNum; ++i) {
		outTransforms[i] = XMMatrixTranspose(outTransforms[i]);
	}
}

}
