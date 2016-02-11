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

#include <DirectXMath.h>
using namespace DirectX;

using namespace Essence;

resource_handle RT_A;
resource_handle DepthBuffer;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

void CreateScreenResources() {
	if (IsValid(RT_A)) {
		Delete(RT_A);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	RT_A = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt0", float4(0.5f, 0.5f, 0.5f, 1));
	DepthBuffer = CreateTexture(x, y, DXGI_FORMAT_R24G8_TYPELESS, ALLOW_DEPTH_STENCIL, "depth_buffer");
}

struct render_data_t {
	float3			position;
	float3			scale;
	model_handle	model;
};

model_handle cubemodel;
model_handle cylindermodel;
model_handle icosahedronmodel;
model_handle torusmodel;
model_handle tubemodel;

Array<render_data_t> RenderObjects;
random_generator RNG;
float ObjectsToRender = 100;
float Alpha = 1;
const u32 MaxObjects = 100000;

float3 UniformSpherePoint(float radius) {
	while (true) {
		auto p = float3(RNG.f32Next() * radius * 2.f - radius, RNG.f32Next() * radius * 2.f - radius, RNG.f32Next() * radius * 2.f - radius);
		float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
		if (d < radius) {
			return p;
		}
	}
}

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -250.f), float3(0, 0, 1));

	cubemodel			= GetModel(NAME_("Models/cube.obj"));
	cylindermodel		= GetModel(NAME_("Models/cylinder.fbx"));
	icosahedronmodel	= GetModel(NAME_("Models/icosa.hedron.fbx"));
	torusmodel			= GetModel(NAME_("Models/torus.fbx"));
	tubemodel			= GetModel(NAME_("Models/tube.fbx"));

	model_handle models[] = {
		cubemodel, cylindermodel, icosahedronmodel, torusmodel, tubemodel
	};

	float sphereRadius = 50.f;

	Resize(RenderObjects, MaxObjects);
	for (u32 i = 0; i < MaxObjects; ++i) {
		RenderObjects[i].position = UniformSpherePoint(sphereRadius);
		RenderObjects[i].scale = float3(1, 1, 1);
		RenderObjects[i].model = models[RNG.u32Next() % _countof(models)];
	}
}


void ShowStatsWindow() {
	ImGui::Begin("Stats");

	auto stats = GetLastFrameStats();

	ImGui::BulletText("Command lists");
	ImGui::Indent();
	ImGui::Text("All / Patchup / Executions: %u / %u / %u", stats->command_lists_num, stats->patchup_command_lists_num, stats->executions_num);
	ImGui::Unindent();

	ImGui::Separator();

	ImGui::BulletText("Commands");
	ImGui::Indent();
	ImGui::Text("Graphics");
	ImGui::Text("PSO changes:\nRootSignature changes:\nRoot params set:\nDrawcalls:"); ImGui::SameLine();
	ImGui::Text("%u\n%u\n%u\n%u",
		stats->command_stats.graphic_pipeline_state_changes,
		stats->command_stats.graphic_root_signature_changes,
		stats->command_stats.graphic_root_params_set,
		stats->command_stats.draw_calls);

	ImGui::Text("Compute");
	ImGui::Text("PSO changes:\nRootSignature changes:\nRoot params set:\nDispatches:"); ImGui::SameLine();
	ImGui::Text("%u\n%u\n%u\n%u",
		stats->command_stats.compute_pipeline_state_changes,
		stats->command_stats.compute_root_signature_changes,
		stats->command_stats.compute_root_params_set,
		stats->command_stats.dispatches);

	ImGui::Text("Common");
	ImGui::Text("Constants: %llu Kb", Kilobytes(stats->command_stats.constants_bytes_uploaded));
	ImGui::Unindent();

	ImGui::End();
}

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

	ShowMemoryInfo();
	ShowStatsWindow();

	PROFILE_END; // ui logic

	auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));
	ClearRenderTarget(drawList, GetRTV(RT_A), float4(0.5f, 0.5f, 0.5f, 1));
	ClearDepthStencil(drawList, GetDSV(DepthBuffer));

	float ObjectsDelta = ((1000.f / 60.f - fDeltaTime * 1000.f) * Alpha);
	ObjectsToRender = min(max(ObjectsToRender + ObjectsDelta, 10.f), (float)MaxObjects);

	auto viewProjMatrix = XMMatrixTranspose(
		CameraControlerPtr->GetViewMatrix()
		* XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f));

	SetRenderTarget(drawList, 0, GetRTV(RT_A));
	SetDepthStencil(drawList, GetDSV(DepthBuffer));
	SetViewport(drawList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(drawList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	auto renderData = GetModelRenderData(RenderObjects[0].model);
	SetShaderState(drawList, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
	SetConstant(drawList, TEXT_("ViewProj"), viewProjMatrix);

	u32 N = (u32)ObjectsToRender;
	for (u32 o = 0; o < N; ++o) {
		float3 scale = RenderObjects[o].scale;
		float4 qrotation = float4(0, 0, 0, 1);
		float3 position = RenderObjects[o].position;

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&qrotation),
				XMLoadFloat3((XMFLOAT3*)&position)
				));

		renderData = GetModelRenderData(RenderObjects[o].model);

		SetShaderState(drawList, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);

		buffer_location_t vb;
		vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
		vb.size = renderData->vertices_num * sizeof(mesh_vertex_t);
		vb.stride = sizeof(mesh_vertex_t);
		SetVertexStream(drawList, 0, vb);

		buffer_location_t ib;
		ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
		ib.size = renderData->indices_num * sizeof(u32);
		ib.stride = sizeof(u32);
		SetIndexBuffer(drawList, ib);

		SetConstant(drawList, TEXT_("World"), worldMatrix);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			auto submesh = renderData->submeshes[i];
			DrawIndexed(drawList, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}

	CopyResource(drawList, GetCurrentBackbuffer(), RT_A);
	
	RenderUserInterface(drawList);
	Execute(drawList);

	Present();
}

void Shutdown() {
	WaitForCompletion();
	FreeMemory(RenderObjects);
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, APP_FLAG_D3D12_DEBUG, APP_PRESENT_UNTHROTTLED);

	return RunApplicationMainLoop();
}
