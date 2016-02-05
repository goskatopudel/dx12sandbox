#pragma once

#include "Commands.h"
#include "Resources.h"
#include "Model.h"

namespace Essence {

typedef GenericHandle32<20, TYPE_ID(SceneEntity)>	scene_entity_h;
typedef GenericHandle32<20, TYPE_ID(Animation)>		animation_h;

struct scene_entity_t {
	model_handle	model;
	animation_h		animation;

	float3			position;
	float4			qrotation;
	float3			scale;
};

class Scene {
public:
	struct scene_animation_state_t {
		animation_state_t							state;
		model_handle								model;
		u32											animation_index;
		u32											use_counter;
		array_view<DirectX::XMMATRIX>				transformations;
	};

	Freelist<scene_entity_t, scene_entity_h>		Entities;
	Freelist<scene_animation_state_t, animation_h>	AnimationStates;

	u32												EntitiesNum;

	~Scene();
};

class ICameraControler;

scene_entity_h	SpawnEntity(Scene& Scene, model_handle model);
void			SetScale(Scene& Scene, scene_entity_h entity, float val);
void			SetPosition(Scene& Scene, scene_entity_h entity, float3 position);
void			KillEntity(Scene& Scene, scene_entity_h entity);
void			KillAnimation(Scene& Scene, scene_entity_h entity);
void			SetAnimation(Scene& Scene, scene_entity_h entity, u32 index, float startTime = 0.f);
void			MirrorAnimation(Scene& Scene, scene_entity_h dstEntity, scene_entity_h srcEntity);
void			UpdateAnimations(Scene& Scene, float dt);
void			UpdateScene(Scene &Scene, float dt);

struct forward_render_scene_setup {
	viewport_t			viewport;
	ICameraControler*	pcamera;
	resource_handle		buffer;
	resource_handle		depthbuffer;
};

void			RenderScene(Scene &Scene, GPUCommandList* drawCmds, forward_render_scene_setup const* Setup);
void			ParallelRenderScene(GPUQueue*, Scene &Scene, forward_render_scene_setup const* Setup);
}