// ModelImporterDLL.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"

#include <new>
#include "ModelImporterDLL.h"

#include <assimp/Importer.hpp>     
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <queue>
#include <string>
#include <unordered_map>
#include "Hash.h"

#pragma comment(lib,"assimp.lib")

namespace Importer {

class ModelMemory {
public:
	std::vector<submesh_definition>		submeshes;
	std::vector<material_definition>	materials;
	std::vector<bone_definition>		bones;

	std::vector<animation_node_t>		animationNodes;
	std::vector<animation_t>			animations;
	std::vector<animation_channel_t>	animationChannels;
	std::vector<position_key_t>			animationPositions;
	std::vector<rotation_key_t>			animationRotations;

	std::vector<uint32_t>				indices;
	std::vector<DirectX::XMFLOAT3>		positions;
	std::vector<DirectX::XMFLOAT2>		texcoords;
	std::vector<DirectX::XMFLOAT2>		texcoords1;
	std::vector<DirectX::XMFLOAT3>		normals;
	std::vector<DirectX::XMFLOAT3>		tangents;
	std::vector<DirectX::XMFLOAT3>		bitangents;
	std::vector<DirectX::XMFLOAT4>		colors;
	std::vector<DirectX::XMUINT4>		boneIndices;
	std::vector<DirectX::XMFLOAT4>		boneWeights;
};

allocated_memory_handle LoadModel(const char* path, model_definition* outModelDefinition) {
	Assimp::Importer importer;

	ZeroMemory(outModelDefinition, sizeof(*outModelDefinition));

	const aiScene* scene = importer.ReadFile(path, aiProcess_CalcTangentSpace | aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType
		| aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes | aiProcess_OptimizeGraph | aiProcess_ValidateDataStructure 
		| aiProcess_ConvertToLeftHanded
		| aiProcess_LimitBoneWeights | aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals
		);

	if (!scene) {
		outModelDefinition->loadResult = IMPORT_ERROR;
		outModelDefinition->loadErrorMessage = importer.GetErrorString();
		return nullptr;
	}

	ModelMemory *modelData = new ModelMemory();
	auto& imported = *modelData;

	// submeshes

	uint32_t baseVertex = 0;
	uint32_t startIndex = 0;
	for (auto i = 0u; i< scene->mNumMeshes; ++i) {
		auto mesh = scene->mMeshes[i];

		auto submesh = submesh_definition();
		sprintf_s(submesh.name, mesh->mName.C_Str());
		submesh.baseVertex = baseVertex;
		submesh.indexCount = mesh->mNumFaces * 3;
		submesh.startIndex = startIndex;
		submesh.materialId = mesh->mMaterialIndex;

		imported.submeshes.push_back(submesh);

		startIndex += mesh->mNumFaces * 3;
		baseVertex += mesh->mNumVertices;
	}

	// materials

	for (auto i = 0u; i < scene->mNumMaterials; ++i) {
		auto material = scene->mMaterials[i];

		material_definition mat_def = {};
		aiString str;
		if (material->GetTextureCount(aiTextureType_DIFFUSE)) {
			
			material->GetTexture(aiTextureType_DIFFUSE, 0, &str);
			sprintf_s(mat_def.diffuseTexture, "%s", str.C_Str());
		}
		if (material->GetTextureCount(aiTextureType_AMBIENT)) {

			material->GetTexture(aiTextureType_AMBIENT, 0, &str);
			sprintf_s(mat_def.metallicnessTexture, "%s", str.C_Str());
		}
		if (material->GetTextureCount(aiTextureType_HEIGHT)) {

			material->GetTexture(aiTextureType_HEIGHT, 0, &str);
			sprintf_s(mat_def.normalmapTexture, "%s", str.C_Str());
		}
		if (material->GetTextureCount(aiTextureType_SHININESS)) {

			material->GetTexture(aiTextureType_SHININESS, 0, &str);
			sprintf_s(mat_def.roughnessTexture, "%s", str.C_Str());
		}
		if (material->GetTextureCount(aiTextureType_OPACITY)) {

			material->GetTexture(aiTextureType_OPACITY, 0, &str);
			sprintf_s(mat_def.alphaTexture, "%s", str.C_Str());
		}
	}

	// bones, collect phase

	std::unordered_map<u64, aiNode*> nodeByBoneNameHash;

	for (auto i = 0u; i < scene->mNumMeshes; ++i) {
		auto mesh = scene->mMeshes[i];
		if (mesh->HasBones()) {
			auto B = mesh->mNumBones;
			for (auto b = 0u; b < B;++b) {
				auto bone = mesh->mBones[b];
				auto name_hash = Hash::MurmurHash2_64(bone->mName.C_Str(), bone->mName.length, 0);
				nodeByBoneNameHash[name_hash] = scene->mRootNode->FindNode(bone->mName.C_Str());
			}
		}
	}

	// animations, channels

	std::unordered_map<u64, u32> channelIndexByNameHash;

	u32 positionsKeys = 0;
	u32 rotationKeys = 0;

	using namespace DirectX;

	// if any animations, collect channels
	if (scene->mNumAnimations) {
		auto sceneAnimation = scene->mAnimations[0];
		for (auto c = 0u; c < sceneAnimation->mNumChannels; ++c) {
			auto channel = sceneAnimation->mChannels[c];
			auto nameHash = Hash::MurmurHash2_64(channel->mNodeName.C_Str(), channel->mNodeName.length, 0);

			channelIndexByNameHash[nameHash] = c;

			/*animation_channel_t anim_channel = {};
			anim_channel.positions_offset = (u32)imported.animationPositions.size();
			anim_channel.positions_num = channel->mNumPositionKeys;
			anim_channel.rotations_offset = (u32)imported.animationRotations.size();
			anim_channel.rotations_num = channel->mNumRotationKeys;*/
			assert(channel->mNodeName.C_Str());

			/*imported.animationChannels.push_back(anim_channel);

			for (auto k = 0u; k < channel->mNumPositionKeys; ++k) {
				auto v = channel->mPositionKeys[k].mValue;
				
				position_key_t key;
				key.value = XMFLOAT3A((float)v.x, (float)v.y, (float)v.z);
				key.time = (float)channel->mPositionKeys[k].mTime;

				imported.animationPositions.push_back(key);
			}
			for (auto k = 0u; k < channel->mNumRotationKeys; ++k) {
				auto v = channel->mRotationKeys[k].mValue;

				rotation_key_t key;
				key.value = XMFLOAT4((float)v.x, (float)v.y, (float)v.z, (float)v.w);
				key.time = (float)channel->mRotationKeys[k].mTime;

				imported.animationRotations.push_back(key);
			}

			for (auto k = 0u; k < channel->mNumScalingKeys; ++k) {
				auto v = channel->mScalingKeys[k].mValue;
				assert(v.x >= 0.98f && v.y >= 0.98f && v.z >= 0.98f);
			}*/
		}

		// verify channels
		for (auto a = 1u; a < scene->mNumAnimations; ++a) {
			auto sceneAnimation = scene->mAnimations[a];

			for (auto c = 0u; c < sceneAnimation->mNumChannels; ++c) {
				auto channel = sceneAnimation->mChannels[c];
				auto nameHash = Hash::MurmurHash2_64(channel->mNodeName.C_Str(), channel->mNodeName.length, 0);
				auto entry = channelIndexByNameHash.find(nameHash);
				assert(entry != channelIndexByNameHash.end() && entry->second == c);
			}
		}
	}

	auto channelsCounter = 0;
	for (auto i = 0u; i < scene->mNumAnimations; ++i) {
		auto sceneAnimation = scene->mAnimations[i];

		animation_t animation = {};
		sprintf_s(animation.name, "%s", sceneAnimation->mName.C_Str());
		animation.name_hash = Hash::MurmurHash2_64(sceneAnimation->mName.C_Str(), sceneAnimation->mName.length, 0);
		animation.duration = (float)sceneAnimation->mDuration;
		animation.ticks_per_second = sceneAnimation->mTicksPerSecond != 0 ? (float)sceneAnimation->mTicksPerSecond : 25.f;
		animation.channels_offset = channelsCounter;
		animation.channels_num = sceneAnimation->mNumChannels;
		channelsCounter += animation.channels_num;

		auto position_keys_num = (u32)imported.animationPositions.size();
		auto rotation_keys_num = (u32)imported.animationRotations.size();

		for (auto c = 0u; c < sceneAnimation->mNumChannels; ++c) {
			auto channel = sceneAnimation->mChannels[c];

			animation_channel_t anim_channel = {};
			anim_channel.positions_offset = (u32)imported.animationPositions.size();
			anim_channel.positions_num = channel->mNumPositionKeys;
			anim_channel.rotations_offset = (u32)imported.animationRotations.size();
			anim_channel.rotations_num = channel->mNumRotationKeys;
			assert(channel->mNodeName.C_Str());

			imported.animationChannels.push_back(anim_channel);

			for (auto k = 0u; k < channel->mNumPositionKeys; ++k) {
				auto v = channel->mPositionKeys[k].mValue;

				position_key_t key;
				key.value = XMFLOAT3A((float)v.x, (float)v.y, (float)v.z);
				key.time = (float)channel->mPositionKeys[k].mTime;

				imported.animationPositions.push_back(key);
			}
			for (auto k = 0u; k < channel->mNumRotationKeys; ++k) {
				auto v = channel->mRotationKeys[k].mValue;

				rotation_key_t key;
				key.value = XMFLOAT4((float)v.x, (float)v.y, (float)v.z, (float)v.w);
				key.time = (float)channel->mRotationKeys[k].mTime;

				imported.animationRotations.push_back(key);
			}

			for (auto k = 0u; k < channel->mNumScalingKeys; ++k) {
				auto v = channel->mScalingKeys[k].mValue;
				assert(v.x >= 0.98f && v.y >= 0.98f && v.z >= 0.98f);
			}
		}

		animation.position_keys_num = (u32)imported.animationPositions.size() - position_keys_num;
		animation.rotation_keys_num = (u32)imported.animationRotations.size() - rotation_keys_num;

		imported.animations.push_back(animation);
	}

	// nodes, skeleton hierarchy

	auto startNode = scene->mRootNode;
	auto currentNode = scene->mRootNode;

	using namespace DirectX;
	std::unordered_map<u64, u32> nodeIndexBoneNameHash;

	auto make_node = [&](aiNode* inNode, u32 parent_index) {
		animation_node_t node = {};
		node.parent_index = parent_index;
		sprintf_s(node.name, "%s", inNode->mName.C_Str());

		auto localTransform = XMFLOAT4X4(&inNode->mTransformation.a1);
		XMStoreFloat4x4(&node.local_transform, XMMatrixTranspose(XMLoadFloat4x4(&localTransform)));

		auto nameHash = Hash::MurmurHash2_64(inNode->mName.C_Str(), inNode->mName.length, 0);
		node.name_hash = nameHash;
		auto findResult = channelIndexByNameHash.find(nameHash);
		node.channel_index = findResult != channelIndexByNameHash.end() ? findResult->second : NULL_INDEX;

		return node;
	};

	std::queue<decltype(scene->mRootNode)> nodesQueue;
	nodesQueue.push(scene->mRootNode);

	u32 parent_index = NULL_INDEX;
	imported.animationNodes.push_back(make_node(nodesQueue.front(), parent_index));
	nodeIndexBoneNameHash[imported.animationNodes.back().name_hash] = (u32)imported.animationNodes.size() - 1;

	while (!nodesQueue.empty()) {
		auto currentNode = nodesQueue.front();
		nodesQueue.pop();
		++parent_index;

		for (auto c = 0u; c < currentNode->mNumChildren; ++c) {
			auto nodeChild = currentNode->mChildren[c];
			nodesQueue.push(nodeChild);

			imported.animationNodes.push_back(make_node(nodeChild, parent_index));
			nodeIndexBoneNameHash[imported.animationNodes.back().name_hash] = (u32)imported.animationNodes.size() - 1;
		}
	}

	imported.indices.reserve(startIndex);
	imported.positions.resize(baseVertex);
	imported.texcoords.resize(baseVertex);
	imported.normals.resize(baseVertex);
	imported.tangents.resize(baseVertex);
	imported.bitangents.resize(baseVertex);
	imported.boneIndices.resize(baseVertex, XMUINT4(0, 0, 0, 0));
	imported.boneWeights.resize(baseVertex, XMFLOAT4(0, 0, 0, 0));

	using namespace DirectX;

	bool hasPositions = true;
	bool hasNormals = true;
	bool hasTangents = true;
	bool hasTextureCoords = true;

	u32 bone_offset = 0;
	for (auto i = 0u; i< scene->mNumMeshes; ++i) {
		auto mesh = scene->mMeshes[i];

		if (i == 0) {
			hasPositions = mesh->HasPositions();
			hasNormals = mesh->HasNormals();
			hasTangents = mesh->HasTangentsAndBitangents();
			hasTextureCoords = mesh->HasTextureCoords(0);
		}

		auto V = mesh->mNumVertices;
		auto vertex_offset = imported.submeshes[i].baseVertex;

		if (mesh->HasPositions() && hasPositions) {
			for (auto v = 0u; v < V; ++v) {
				imported.positions[vertex_offset + v] = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
			}
		}
		if (mesh->HasNormals() && hasNormals) {
			for (auto v = 0u; v < V; ++v) {
				imported.normals[vertex_offset + v] = XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
			}
		}
		if (mesh->HasTangentsAndBitangents() && hasTangents) {
			for (auto v = 0u; v < V; ++v) {
				imported.tangents[vertex_offset + v] = XMFLOAT3(mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z);
				imported.bitangents[vertex_offset + v] = XMFLOAT3(mesh->mBitangents[v].x, mesh->mBitangents[v].y, mesh->mBitangents[v].z);
			}
		}
		if (mesh->HasTextureCoords(0) && hasTextureCoords) {
			for (auto v = 0u; v < V; ++v) {
				imported.texcoords[vertex_offset + v] = XMFLOAT2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y);
			}
		}

		if (mesh->HasBones()) {
			/*imported.boneIndices.resize(V);
			imported.boneWeights.resize(V);*/
			std::vector<int> vertexBonesCtr(V);
			vertexBonesCtr.resize(V);

			auto vertexOffset = imported.submeshes[i].baseVertex;

			auto B = mesh->mNumBones;
			for (auto b = 0u; b < B;++b) {				
				auto bone = mesh->mBones[b];

				bone_definition bone_def = {};
				sprintf_s(bone_def.name, "%s", bone->mName.C_Str());
				bone_def.name_hash = Hash::MurmurHash2_64(bone_def.name, strlen(bone_def.name), 0);
				auto offsetMatrix = XMFLOAT4X4(&bone->mOffsetMatrix.a1);
				XMStoreFloat4x4(&bone_def.offset_matrix, XMMatrixTranspose(XMLoadFloat4x4(&offsetMatrix)));
				assert(nodeIndexBoneNameHash.find(bone_def.name_hash) != nodeIndexBoneNameHash.end());
				bone_def.node_index = nodeIndexBoneNameHash[bone_def.name_hash];
				imported.bones.push_back(bone_def);

				auto W = bone->mNumWeights;
				for (auto w = 0u; w < W; ++w) {
					auto vertexId = bone->mWeights[w].mVertexId + vertex_offset;
					auto weight = bone->mWeights[w].mWeight;

					auto innerIndex = vertexBonesCtr[bone->mWeights[w].mVertexId]++;
					assert(innerIndex < 4);

					switch (innerIndex) {
					case 0:
						imported.boneIndices[vertexId].x = b + bone_offset;
						imported.boneWeights[vertexId].x = weight;
						break;
					case 1:
						imported.boneIndices[vertexId].y = b + bone_offset;
						imported.boneWeights[vertexId].y = weight;
						break;
					case 2:
						imported.boneIndices[vertexId].z = b + bone_offset;
						imported.boneWeights[vertexId].z = weight;
						break;
					case 3:
						imported.boneIndices[vertexId].w = b + bone_offset;
						imported.boneWeights[vertexId].w = weight;
						break;
					}
					
				}
			}

			bone_offset += B;
		}

		auto F = mesh->mNumFaces;
		for (auto f = 0u; f < F; ++f) {
			for (auto k = 0u; k<mesh->mFaces[f].mNumIndices; ++k) {
				imported.indices.push_back(mesh->mFaces[f].mIndices[k]);
				assert(mesh->mFaces[f].mNumIndices == 3);
			}
		}
	}

	XMVECTOR vmin, vmax;
	vmin = XMLoadFloat3(imported.positions.data() + 0);
	vmax = vmin;
	for (auto i = 0u; i<imported.positions.size(); ++i) {
		vmin = XMVectorMin(vmin, XMLoadFloat3(imported.positions.data() + i));
		vmax = XMVectorMax(vmax, XMLoadFloat3(imported.positions.data() + i));
	}

	outModelDefinition->verticesNum = baseVertex;
	outModelDefinition->indicesNum = startIndex;

	outModelDefinition->trianglesNum = startIndex / 3;

	outModelDefinition->submeshesNum = (uint32_t)imported.submeshes.size();
	outModelDefinition->submeshes = imported.submeshes.data();

	outModelDefinition->indices = imported.indices.data();

	outModelDefinition->positions = imported.positions.data();
	outModelDefinition->normals = imported.normals.data();
	outModelDefinition->tangents = imported.tangents.data();
	outModelDefinition->bitangents = imported.bitangents.data();
	outModelDefinition->texcoords = imported.texcoords.data();
	outModelDefinition->boneIndices = imported.boneIndices.data();
	outModelDefinition->boneWeights = imported.boneWeights.data();

	outModelDefinition->bonesNum = (u32)imported.bones.size();
	outModelDefinition->bones = imported.bones.data();

	outModelDefinition->materialsNum = (u32)imported.materials.size();
	outModelDefinition->materials = imported.materials.data();

	outModelDefinition->animationNodes = imported.animationNodes.data();
	outModelDefinition->animationNodesNum = (u32)imported.animationNodes.size();
	outModelDefinition->animations = imported.animations.data();
	outModelDefinition->animationsNum = (u32)imported.animations.size();
	outModelDefinition->animationChannels = imported.animationChannels.data();
	outModelDefinition->animationPositionKeys = imported.animationPositions.data();
	outModelDefinition->animationRotationKeys = imported.animationRotations.data();

	XMStoreFloat3(&outModelDefinition->boundingBoxMin, vmin);
	XMStoreFloat3(&outModelDefinition->boundingBoxMax, vmax);

	return modelData;
}

void FreeMemory(allocated_memory_handle handle) {
	if (handle != nullptr) {
		delete (ModelMemory*)handle;
	}
}

}