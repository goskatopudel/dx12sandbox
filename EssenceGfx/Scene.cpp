#include "Scene.h"
#include "Application.h"
#include "Camera.h"
#include "Scheduler.h"

namespace Essence {

Scene::~Scene() {
	for (auto &state : AnimationStates) {
		GetMallocAllocator()->Free(state.transformations.elements);
		FreeAnimationState(&state.state);
	}
	FreeMemory(AnimationStates);

	FreeMemory(Entities);

	EntitiesNum = 0;
}

scene_entity_h SpawnEntity(Scene& Scene, model_handle model) {
	auto handle = Create(Scene.Entities);
	Scene.EntitiesNum++;

	auto& ref = Scene.Entities[handle];
	ref = {};
	ref.position = float3(0, 0, 0);
	ref.qrotation = float4(0, 0, 0, 1);
	ref.scale = float3(1, 1, 1);
	ref.model = model;

	return handle;
}

void SetScale(Scene& Scene, scene_entity_h entity, float val) {
	Scene.Entities[entity].scale = float3(val, val, val);
}

void SetPosition(Scene& Scene, scene_entity_h entity, float3 position) {
	Scene.Entities[entity].position = position;
}

void KillEntity(Scene& Scene, scene_entity_h entity) {
	KillAnimation(Scene, entity);

	Delete(Scene.Entities, entity);
	Scene.EntitiesNum--;
}

void KillAnimation(Scene& Scene, scene_entity_h entity) {
	auto animHandle = Scene.Entities[entity].animation;

	if (IsValid(animHandle)) {
		Scene.AnimationStates[animHandle].use_counter--;
		if (Scene.AnimationStates[animHandle].use_counter == 0) {
			FreeAnimationState(&Scene.AnimationStates[animHandle].state);
		}
	}
}

void SetAnimation(Scene& Scene, scene_entity_h entity, u32 index, float startTime) {
	auto pRenderData = GetModelRenderData(Scene.Entities[entity].model);

	auto animHandle = Scene.Entities[entity].animation;

	if (IsValid(animHandle)) {
		KillAnimation(Scene, entity);
	}
	else {
		animHandle = Create(Scene.AnimationStates);
		Scene.AnimationStates[animHandle] = {};
		Scene.AnimationStates[animHandle].use_counter++;
		Scene.AnimationStates[animHandle].model = Scene.Entities[entity].model;
		Scene.AnimationStates[animHandle].animation_index = index;
		Scene.AnimationStates[animHandle].state.last_time = startTime;
		Check(GetModelRenderData(Scene.Entities[entity].model)->animations.num > index);

		Scene.Entities[entity].animation = animHandle;

		allocate_array(&Scene.AnimationStates[animHandle].transformations, GetModelRenderData(Scene.Entities[entity].model)->skeleton.bones_num, GetMallocAllocator());
	}

	InitAnimationState(&Scene.AnimationStates[animHandle].state, pRenderData, index);
}

void MirrorAnimation(Scene& Scene, scene_entity_h dstEntity, scene_entity_h srcEntity) {
	Check(Scene.Entities[dstEntity].model == Scene.Entities[srcEntity].model);

	if (IsValid(Scene.Entities[dstEntity].animation)) {
		KillAnimation(Scene, dstEntity);
	}

	Scene.Entities[dstEntity].animation = Scene.Entities[srcEntity].animation;
	Scene.AnimationStates[Scene.Entities[dstEntity].animation].use_counter++;
}

void GetSceneAnimations(Scene* pScene, Array<animation_h>* handlesAcc) {
	for (auto handle : pScene->AnimationStates.Keys()) {
		PushBack(*handlesAcc, handle);
	}
}

void UpdateAnimations(Scene& Scene, float dt) {
	for (auto& animState : Scene.AnimationStates) {
		auto pRenderData = GetModelRenderData(animState.model);

		float currentAnimTime = animState.state.last_time + dt;
		animState.state.last_time += dt;

		calculate_animation(
			&pRenderData->skeleton,
			&pRenderData->animations[animState.animation_index],
			&animState.state,
			currentAnimTime,
			animState.transformations.elements);
	}
}

struct ParallelUpdateAnimationsRange_Payload {
	Scene*			pScene;
	animation_h*	workspace;
	u32				from;
	u32				to;
	float			dt;
};

struct ParallelUpdateAnimationsRoot_Payload {
	Array<ParallelUpdateAnimationsRange_Payload>*	SubtasksData;
};

void ParallelUpdateAnimationsRange(const void* InArgs, Job*) {
	PROFILE_SCOPE(update_anitmations_range);

	auto Args = *(ParallelUpdateAnimationsRange_Payload*)InArgs;

	auto dt = Args.dt;

	for (auto i : MakeRange(Args.from, Args.to)) {
		auto& animState = Args.pScene->AnimationStates[Args.workspace[i]];
		auto pRenderData = GetModelRenderData(animState.model);

		float currentAnimTime = animState.state.last_time + dt;
		animState.state.last_time += dt;

		calculate_animation(
			&pRenderData->skeleton,
			&pRenderData->animations[animState.animation_index],
			&animState.state,
			currentAnimTime,
			animState.transformations.elements);
	}
}

void ParallelUpdateAnimationsRoot(const void* InArgs, Job* job) {
	auto Args = *(ParallelUpdateAnimationsRoot_Payload*)InArgs;

	Job* children[512];
	Check(Size(*Args.SubtasksData) < _countof(children));

	for (auto i : MakeRange(Size(*Args.SubtasksData))) {
		children[i] = CreateChildJob(job, ParallelUpdateAnimationsRange, &(*Args.SubtasksData)[i]);
	}

	RunJobs(children, (u32) Size(*Args.SubtasksData));
}

void ParallelUpdateAnimations(Scene& Scene, float dt) {
	PROFILE_SCOPE(update_anitmations);

	Array<animation_h> workspace(GetMallocAllocator());
	Reserve(workspace, 512);

	for (auto animHandle : Scene.AnimationStates.Keys()) {
		PushBack(workspace, animHandle);
	}

	auto animationsPerBatch = 32;
	Array<ParallelUpdateAnimationsRange_Payload> childWorkspaces(GetMallocAllocator());
	Reserve(childWorkspaces, Size(workspace) / animationsPerBatch + 1);

	u32 N = (u32)Size(workspace);
	for (u32 i = 0; i < N; i += animationsPerBatch) {
		ParallelUpdateAnimationsRange_Payload payload = {};
		payload.pScene = &Scene;
		payload.from = i;
		payload.to = min(N, i + animationsPerBatch);
		payload.dt = dt;
		payload.workspace = workspace.DataPtr;
		PushBack(childWorkspaces, payload);
	}

	ParallelUpdateAnimationsRoot_Payload payload = {};
	payload.SubtasksData = &childWorkspaces;

	auto rootJob = CreateJob(ParallelUpdateAnimationsRoot, &payload);
	RunJobs(&rootJob, 1);

	WaitFor(rootJob, true);
}

void UpdateScene(Scene &Scene, float dt) {
	//UpdateAnimations(Scene, dt);
	ParallelUpdateAnimations(Scene, dt);
}

struct ParallelRenderSceneRange_Payload {
	Array<scene_entity_h>const*			pEntityHandles;
	u32									from;
	u32									to;
	Scene*								pScene;
	forward_render_scene_setup const*	Setup;
	GPUCommandList*						CommandList;
};

struct ParallelRenderSceneRoot_Payload {
	Array<ParallelRenderSceneRange_Payload> const*	SubtasksData;
};

void ParallelRenderSceneRange(const void* InArgs, Job*) {
	PROFILE_SCOPE(render_scene_range);
	auto Args = *(ParallelRenderSceneRange_Payload*)InArgs;

	using namespace DirectX;

	auto pCamera = Args.Setup->pcamera;
	auto drawCmds = Args.CommandList;
	auto const& Scene = *Args.pScene;
	GPU_PROFILE_SCOPE(drawCmds, render_scene_range);

	SetRenderTarget(drawCmds, 0, GetRTV(Args.Setup->buffer));
	SetDepthStencil(drawCmds, GetDSV(Args.Setup->depthbuffer));

	for (auto i : MakeRange(Args.from, Args.to)) {
		auto entity = Args.pScene->Entities[(*Args.pEntityHandles)[i]];

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&entity.scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&entity.qrotation),
				XMLoadFloat3((XMFLOAT3*)&entity.position)
				));

		auto viewProjMatrix = XMMatrixTranspose(
			pCamera->GetViewMatrix()
			* XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f));

		auto renderData = GetModelRenderData(entity.model);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			SetShaderState(drawCmds, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
			SetViewport(drawCmds, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(drawCmds, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(drawCmds, TEXT_("World"), worldMatrix);


			auto transformations = Scene.AnimationStates[entity.animation].transformations;
			SetConstant(drawCmds, TEXT_("BoneTransform"), transformations.elements, sizeof(xmmatrix) * transformations.num);
			SetConstant(drawCmds, TEXT_("ViewProj"), viewProjMatrix);

			buffer_location_t vb;
			vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
			vb.size = renderData->vertices_num * sizeof(mesh_vertex_t);
			vb.stride = sizeof(mesh_vertex_t);

			SetVertexStream(drawCmds, 0, vb);

			buffer_location_t ib;
			ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
			ib.size = renderData->indices_num * sizeof(u32);
			ib.stride = sizeof(u32);

			SetIndexBuffer(drawCmds, ib);

			auto submesh = renderData->submeshes[i];
			DrawIndexed(drawCmds, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}
}

void ParallelRenderSceneRoot(const void* InArgs, Job* job) {
	auto Args = *(ParallelRenderSceneRoot_Payload*)InArgs;

	Job* children[512];
	Check(Size(*Args.SubtasksData) < _countof(children));

	for (auto i : MakeRange(Size(*Args.SubtasksData))) {
		children[i] = CreateChildJob(job, ParallelRenderSceneRange, &(*Args.SubtasksData)[i]);
	}

	RunJobs(children, (u32)Size(*Args.SubtasksData));
}

void ParallelRenderScene(GPUQueue* queue, Scene &Scene, forward_render_scene_setup const* setup) {
	PROFILE_SCOPE(render_scene);

	Array<scene_entity_h> workspace(GetMallocAllocator());
	Reserve(workspace, 2048);

	for (auto entityHandle : Scene.Entities.Keys()) {
		PushBack(workspace, entityHandle);
	}

	const auto objectsPerBatch = 128;
	Array<ParallelRenderSceneRange_Payload> childWorkspaces(GetMallocAllocator());
	Reserve(childWorkspaces, Size(workspace) / objectsPerBatch + 1);

	u32 N = (u32)Size(workspace);
	for (u32 i = 0; i < N; i += objectsPerBatch) {
		ParallelRenderSceneRange_Payload payload = {};
		payload.pScene = &Scene;
		payload.from = i;
		payload.to = min(N, i + objectsPerBatch);
		payload.Setup = setup;
		payload.pEntityHandles = &workspace;
		payload.CommandList = GetCommandList(queue, NAME_("RenderWork"));
		PushBack(childWorkspaces, payload);
	}

	ParallelRenderSceneRoot_Payload payload = {};
	payload.SubtasksData = &childWorkspaces;

	auto rootJob = CreateJob(ParallelRenderSceneRoot, &payload);
	RunJobs(&rootJob, 1);

	WaitFor(rootJob, true);

	if (!Size(childWorkspaces)) {
		return;
	}

	for (auto& payload : childWorkspaces) {
		Execute(payload.CommandList);
	}
};

void RenderScene(Scene &Scene, GPUCommandList* drawCmds, forward_render_scene_setup const* setup) {

	using namespace DirectX;

	for (auto entity : Scene.Entities) {

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&entity.scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&entity.qrotation),
				XMLoadFloat3((XMFLOAT3*)&entity.position)
				));

		auto viewProjMatrix = XMMatrixTranspose(
			setup->pcamera->GetViewMatrix()
			* XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f));

		auto renderData = GetModelRenderData(entity.model);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			SetShaderState(drawCmds, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
			SetViewport(drawCmds, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(drawCmds, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(drawCmds, TEXT_("World"), worldMatrix);


			auto transformations = Scene.AnimationStates[entity.animation].transformations;
			SetConstant(drawCmds, TEXT_("BoneTransform"), transformations.elements, sizeof(xmmatrix) * transformations.num);
			SetConstant(drawCmds, TEXT_("ViewProj"), viewProjMatrix);

			buffer_location_t vb;
			vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
			vb.size = renderData->vertices_num * sizeof(mesh_vertex_t);
			vb.stride = sizeof(mesh_vertex_t);

			SetVertexStream(drawCmds, 0, vb);

			buffer_location_t ib;
			ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
			ib.size = renderData->indices_num * sizeof(u32);
			ib.stride = sizeof(u32);

			SetIndexBuffer(drawCmds, ib);

			auto submesh = renderData->submeshes[i];
			DrawIndexed(drawCmds, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}
}

}