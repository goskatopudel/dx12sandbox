#include "Essence.h"
#include "EssenceGfx.h"
#include <DirectXMath.h>
#include "Camera.h"
#include "Model.h"
#include "UIRendering.h"
#include "imgui/imgui.h"
#include "Shader.h"
#include "StatWindows.h"
#include "Hashmap.h"
#include "Random.h"
#include "SDL.h"

#pragma comment(lib,"SDL2main.lib")

using namespace DirectX;
using namespace Essence;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

resource_handle DepthBuffer;
resource_handle GBufferA;
resource_handle GBufferB;
resource_handle LBuffer;

resource_handle AlbedoTex;
resource_handle NormalTex;
resource_handle RoughnessTex;
resource_handle SkyboxTex;

resource_handle WhiteTex;
resource_handle FlatNormalmapTex;

model_handle SphereModel;
model_handle CubeModel;
model_handle CylinderModel;
model_handle MatTesterModel;

struct pbr_material_def_t {
	resource_handle base_color_texture;
	resource_handle normalmap_texture;
	resource_handle roughness_texture;
	resource_handle metalness_texture;

	float base_color_mult;
	float roughness_mult;
	float metalness_mult;
};

pbr_material_def_t MakeDefaultMaterial() {
	pbr_material_def_t mat;
	mat.base_color_texture = WhiteTex;
	mat.normalmap_texture = FlatNormalmapTex;
	mat.roughness_texture = WhiteTex;
	mat.metalness_texture = WhiteTex;
	mat.base_color_mult = 0.5f;
	mat.roughness_mult = 0.5f;
	mat.metalness_mult = 0.f;
	return mat;
}

struct scene_object_t {
	Vec3f position;
	float scale;
	Vec4f rotation_quat;

	model_handle model;
	pbr_material_def_t material;
};

Array<scene_object_t> SceneObjects;

void InitScene() {
	Vec4f rotation;
	XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(&rotation), XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XM_PIDIV2));

	scene_object_t door = {};
	door.model = GetModel(NAME_("Models/cube.obj"));
	door.scale = 50.f;
	door.position = Vec3f(0, -50.f, 0);
	door.rotation_quat = Vec4f(0, 0, 0, 1);
	door.material = MakeDefaultMaterial();

	PushBack(SceneObjects, door);

	scene_object_t plane_1 = {};
	plane_1.model = GetModel(NAME_("Models/wall.doorway.thin.fbx"));
	plane_1.scale = 10;
	plane_1.position = Vec3f(0, 5.5f, 30.f);
	plane_1.rotation_quat = rotation;
	plane_1.material = MakeDefaultMaterial();

	PushBack(SceneObjects, plane_1);

	for (int y = 0; y < 7; y++) {
		for (int x = 0; x < 7; x++) {
			scene_object_t matTester = {};
			matTester.model = GetModel(NAME_("Models/MatTester.obj"));
			matTester.scale = 0.1f;
			matTester.position = Vec3f(-16.f + 2 * 16.f / 7 * x, 0, -16.f + 2 * 16.f / 7 * y) + Vec3f(0, 0, 5);
			matTester.rotation_quat = Vec4f(0, 0, 0, 1);
			matTester.material = MakeDefaultMaterial();
			matTester.material.roughness_mult = (float)x / 7.f;
			matTester.material.metalness_mult = (float)y / 7.f;

			PushBack(SceneObjects, matTester);
		}
	}

	random_generator RNG;

	model_handle models[] = {
		GetModel(NAME_("Models/wall.window.thin.fbx")),
		GetModel(NAME_("Models/wall.cross.thin.fbx")),
		GetModel(NAME_("Models/wall.T.thin.fbx")),
		GetModel(NAME_("Models/wall.thin.fbx")),
		GetModel(NAME_("Models/pyramid.fbx")),
	};

	float scales[] = { 2.f, 3.f, 4.f };

	auto rotations_quat = [] (u32 axis, float angle) {
		Vec4f q;
		XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(&q), XMQuaternionRotationAxis(XMVectorSet(axis == 0, axis == 1, axis == 2, 0), angle));
		return q;
	};

	Vec4f rotations[] = {
		rotations_quat(1, XM_PIDIV4 * 0),
		rotations_quat(1, XM_PIDIV4 * 1),
		rotations_quat(1, XM_PIDIV4 * 2),
		rotations_quat(1, XM_PIDIV4 * 3),
		rotations_quat(1, XM_PIDIV4 * 4),
		rotations_quat(1, XM_PIDIV4 * 5),
		rotations_quat(1, XM_PIDIV4 * 6),
		rotations_quat(1, XM_PIDIV4 * 7),
	};

	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 4; x++) {
			scene_object_t figure = {};
			figure.model = models[RNG.u32Next() % _countof(models)];
			figure.scale = scales[RNG.u32Next() % _countof(scales)];
			figure.position = Vec3f(20.f + x * 40.f / 5.f, figure.scale * 0.5f, -43.f + 180.f * y / 15.f);
			figure.rotation_quat = rotations[RNG.u32Next() % _countof(rotations)];
			figure.material = MakeDefaultMaterial();
			figure.material.roughness_mult = (float)0.5f;
			figure.material.metalness_mult = 0.f;

			PushBack(SceneObjects, figure);
		}
	}
}

struct directinal_light_def_t {
	Vec3f direction;
	Vec3f intensity;
};

void CreateScreenResources() {
	if (IsValid(DepthBuffer)) {
		Delete(GBufferA);
		Delete(GBufferB);
		Delete(LBuffer);
		Delete(DepthBuffer);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	DepthBuffer = CreateTexture2D(x, y, DXGI_FORMAT_R24G8_TYPELESS, ALLOW_DEPTH_STENCIL, "depth_buffer");
	// COLOR, METALNESS
	GBufferA = CreateTexture2D(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "GBufferA");
	// NORMAL, ROUGHNESS
	GBufferB = CreateTexture2D(x, y, DXGI_FORMAT_R8G8B8A8_UNORM, ALLOW_RENDER_TARGET, "GBufferB");
	// 
	LBuffer = CreateTexture2D(x, y, DXGI_FORMAT_R16G16B16A16_FLOAT, ALLOW_RENDER_TARGET, "LBuffer");
}

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));

	auto initialCopies = GetCommandList(GGPUCopyQueue, NAME_("Copy"));

	SphereModel = GetModel(NAME_("Models/cube.sphere.16.fbx"));
	CubeModel = GetModel(NAME_("Models/cube.obj"));
	SphereModel = GetModel(NAME_("Models/cylinder.fbx"));
	MatTesterModel = GetModel(NAME_("Models/MatTester.obj"));

	WhiteTex = CreateTexture2D(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, NO_TEXTURE_FLAGS, "White");
	const u32 whiteData = 0xFFFFFFFF;
	D3D12_SUBRESOURCE_DATA data = {};
	data.pData = &whiteData;
	data.RowPitch = sizeof(u32);
	data.SlicePitch = sizeof(u32);
	CopyFromCpuToSubresources(initialCopies, Slice(WhiteTex), 1, &data);

	FlatNormalmapTex = CreateTexture2D(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, NO_TEXTURE_FLAGS, "FlatNormalmap");
	const u32 flatNormalmapData = 0xFFFF7F7F;
	data = {};
	data.pData = &flatNormalmapData;
	data.RowPitch = sizeof(u32);
	data.SlicePitch = sizeof(u32);
	CopyFromCpuToSubresources(initialCopies, Slice(FlatNormalmapTex), 1, &data);

	AlbedoTex = LoadDDSFromFile(TEXT_("Textures/Sponza_Bricks_a_Albedo.DDS"), initialCopies, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).resource;
	NormalTex = LoadDDSFromFile(TEXT_("Textures/Sponza_Bricks_a_Normal.DDS"), initialCopies, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).resource;
	RoughnessTex = LoadDDSFromFile(TEXT_("Textures/Sponza_Bricks_a_Roughness.DDS"), initialCopies, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).resource;
	SkyboxTex = LoadDDSFromFile(TEXT_("Textures/output_skybox.dds"), initialCopies, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).resource;

	InitScene();

	Execute(initialCopies);
}

enum GBufferDisplayDebugEnum {
	DEBUG_DISABLED,
	DEBUG_DEPTH,
	DEBUG_ALBEDO,
	DEBUG_NORMALS,
	DEBUG_ROUGHNESS,
	DEBUG_METALNESS,
};

GBufferDisplayDebugEnum ShowGBufferDebugWindow() {
	ImGui::Begin("GBuffer");

	static GBufferDisplayDebugEnum chosenValue = DEBUG_DISABLED;

	if (ImGui::RadioButton("Disable", chosenValue == DEBUG_DISABLED)) {
		chosenValue = DEBUG_DISABLED;
	}
	if (ImGui::RadioButton("Depth", chosenValue == DEBUG_DEPTH)) {
		chosenValue = DEBUG_DEPTH;
	}
	if (ImGui::RadioButton("Albedo", chosenValue == DEBUG_ALBEDO)) {
		chosenValue = DEBUG_ALBEDO;
	}
	if (ImGui::RadioButton("Normals", chosenValue == DEBUG_NORMALS)) {
		chosenValue = DEBUG_NORMALS;
	}
	if (ImGui::RadioButton("Roughness", chosenValue == DEBUG_ROUGHNESS)) {
		chosenValue = DEBUG_ROUGHNESS;
	}
	if (ImGui::RadioButton("Metalness", chosenValue == DEBUG_METALNESS)) {
		chosenValue = DEBUG_METALNESS;
	}
	ImGui::End();

	return chosenValue;
};

void Tick(float fDeltaTime) {

	ImGuiIO& io = ImGui::GetIO();
	static float rx = 0;
	static float ry = 0;
	if (!io.WantCaptureMouse && !io.WantCaptureKeyboard)
	{
		float activeSpeed = 10 * fDeltaTime;

		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT]) {
			activeSpeed *= 10.f;
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LCTRL]) {
			activeSpeed /= 5.f;
		}

		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_W]) {
			CameraControlerPtr->onForward(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_S]) {
			CameraControlerPtr->onBackward(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]) {
			CameraControlerPtr->onRight(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]) {
			CameraControlerPtr->onLeft(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_Q]) {
			CameraControlerPtr->onRollLeft(fDeltaTime * DirectX::XM_PI);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_E]) {
			CameraControlerPtr->onRollRight(fDeltaTime * DirectX::XM_PI);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE]) {
			CameraControlerPtr->onUp(activeSpeed);
		}
		int xrel = 0;
		int yrel = 0;
		auto mouseState = SDL_GetRelativeMouseState(&xrel, &yrel);
		if (mouseState & SDL_BUTTON(3)) {
			float dx = (float)xrel / (float)GDisplaySettings.resolution.x;
			float dy = (float)yrel / (float)GDisplaySettings.resolution.y;

			CameraControlerPtr->onMouseMove(dy, dx);
		}

		if (mouseState & SDL_BUTTON(1)) {
			float dx = (float)xrel / 500.0f * (float)M_PI;
			float dy = (float)yrel / 500.0f * (float)M_PI;
			ry += -dx;
			rx += dy;
		}
	}

	PROFILE_BEGIN(ui_logic);

	ImGui::Text("Hello, world!");
	ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
	if (ImGui::Button("Recompile shaders")) {
		ReloadShaders();
		ClearWarnings(TYPE_ID("ShaderBindings"));
	}

	ImGui::ShowTestWindow();

	ShowMemoryWindow();
	auto displayMode =  ShowGBufferDebugWindow();

	PROFILE_END; // ui logic

	auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));

	ClearRenderTarget(drawList, GetRTV(GBufferA));
	ClearRenderTarget(drawList, GetRTV(GBufferB));
	ClearDepthStencil(drawList, GetDSV(DepthBuffer));

	SetRenderTarget(drawList, 0, GetRTV(GBufferA));
	SetRenderTarget(drawList, 1, GetRTV(GBufferB));
	SetDepthStencil(drawList, GetDSV(DepthBuffer));
	SetViewport(drawList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(drawList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	auto viewMatrix = CameraControlerPtr->GetViewMatrix();
	auto projMatrix = XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f);
	auto viewProjMatrix = viewMatrix * projMatrix;

	viewMatrix = XMMatrixTranspose(viewMatrix);
	projMatrix = XMMatrixTranspose(projMatrix);
	viewProjMatrix = XMMatrixTranspose(viewProjMatrix);
	xmvec tmp;
	auto invViewMatrix = XMMatrixTranspose(XMMatrixInverse(&tmp, viewMatrix));

	for (auto &object : SceneObjects) {

		auto renderData = GetModelRenderData(object.model);
		SetShaderState(drawList, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
		SetConstant(drawList, TEXT_("ViewProj"), viewProjMatrix);

		float3 scale = float3(object.scale, object.scale, object.scale);
		float4 qrotation = float4(&object.rotation_quat.x);
		float3 position = float3(&object.position.x);

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&qrotation),
				XMLoadFloat3((XMFLOAT3*)&position)
				));

		buffer_location_t vb;
		vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
		vb.size = renderData->vertices_num * renderData->vertex_stride;
		vb.stride = renderData->vertex_stride;
		SetVertexStream(drawList, 0, vb);

		buffer_location_t ib;
		ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
		ib.size = renderData->indices_num * renderData->index_stride;
		ib.stride = renderData->index_stride;
		SetIndexBuffer(drawList, ib);

		SetConstant(drawList, TEXT_("World"), worldMatrix);
		SetTexture2D(drawList, TEXT_("BaseColorTexture"), GetSRV(object.material.base_color_texture));
		SetTexture2D(drawList, TEXT_("NormalTexture"), GetSRV(object.material.normalmap_texture));
		SetTexture2D(drawList, TEXT_("RoughnessTexture"), GetSRV(object.material.roughness_texture));

		SetConstant(drawList, TEXT_("BaseColorMult"), object.material.base_color_mult);
		SetConstant(drawList, TEXT_("RoughnessMult"), object.material.roughness_mult);
		SetConstant(drawList, TEXT_("MetalnessMult"), object.material.metalness_mult);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			auto submesh = renderData->submeshes[i];
			DrawIndexed(drawList, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}

	SetVertexStream(drawList, 0, {});
	SetRenderTarget(drawList, 1, {});

	// light
	SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
	SetShaderState(drawList, SHADER_(LightPass, VShader, VS_5_1), SHADER_(LightPass, PShader, PS_5_1), {});
	SetDepthStencil(drawList, {});
	SetTexture2D(drawList, TEXT_("DepthBuffer"), GetSRV(DepthBuffer));
	SetTexture2D(drawList, TEXT_("GBufferA"), GetSRV(GBufferA));
	SetTexture2D(drawList, TEXT_("GBufferB"), GetSRV(GBufferB));
	SetConstant(drawList, TEXT_("Projection"), projMatrix);
	SetConstant(drawList, TEXT_("View"), viewMatrix);
	SetConstant(drawList, TEXT_("InvView"), invViewMatrix);
	SetConstant(drawList, TEXT_("LightIntensity"), float3(1,1,1));
	SetConstant(drawList, TEXT_("LightDirection"), normalize(Vec3f(1,-2,1)));
	Draw(drawList, 3);

	// skybox
	SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
	SetShaderState(drawList, SHADER_(Skybox, VShader, VS_5_1), SHADER_(Skybox, PShader, PS_5_1), {});
	D3D12_DEPTH_STENCIL_DESC depthStencilState;
	GetD3D12StateDefaults(&depthStencilState);
	depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
	SetDepthStencilState(drawList, depthStencilState);
	SetDepthStencil(drawList, GetDSV(DepthBuffer));
	SetConstant(drawList, TEXT_("Projection"), projMatrix);
	SetConstant(drawList, TEXT_("View"), viewMatrix);
	SetTexture2D(drawList, TEXT_("Skybox"), GetSRV(SkyboxTex));
	Draw(drawList, 3);

	if (displayMode == DEBUG_DEPTH) {
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(DebugMode, VShader, VS_5_1), SHADER_(DebugMode, PShader_Depth, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("DepthBuffer"), GetSRV(DepthBuffer));
		SetDepthStencil(drawList, {});
		Draw(drawList, 3);
	}
	else if (displayMode == DEBUG_ALBEDO) {
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(DebugMode, VShader, VS_5_1), SHADER_(DebugMode, PShader_Albedo, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("GBufferA"), GetSRV(GBufferA));
		SetDepthStencil(drawList, {});
		Draw(drawList, 3);
	}
	else if (displayMode == DEBUG_NORMALS) {
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(DebugMode, VShader, VS_5_1), SHADER_(DebugMode, PShader_Normals, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("GBufferB"), GetSRV(GBufferB));
		SetDepthStencil(drawList, {});
		Draw(drawList, 3);

	}
	else if (displayMode == DEBUG_ROUGHNESS) {
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(DebugMode, VShader, VS_5_1), SHADER_(DebugMode, PShader_Roughness, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("GBufferB"), GetSRV(GBufferB));
		SetDepthStencil(drawList, {});
		Draw(drawList, 3);

	}
	else if (displayMode == DEBUG_METALNESS) {
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(DebugMode, VShader, VS_5_1), SHADER_(DebugMode, PShader_Metalness, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("GBufferA"), GetSRV(GBufferA));
		SetDepthStencil(drawList, {});
		Draw(drawList, 3);
	}

	RenderUserInterface(drawList);
	TransitionBarrier(drawList, Slice(GetCurrentBackbuffer()), D3D12_RESOURCE_STATE_PRESENT);
	Execute(drawList);

	Present();
}

void Shutdown() {
	WaitForCompletion();
	FreeMemory(SceneObjects);
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, APP_FLAG_NONE, APP_PRESENT_LOWLATENCY);

	return RunApplicationMainLoop();
}
