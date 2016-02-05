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
	float4			qrotation;
	float3			scale;
	model_handle	model;
};

model_handle cubemodel;
model_handle cylindermodel;
model_handle icosahedronmodel;
model_handle torusmodel;
model_handle tubemodel;

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));

	cubemodel			= GetModel(NAME_("Models/cube.obj"));
	cylindermodel		= GetModel(NAME_("Models/cylinder.fbx"));
	icosahedronmodel	= GetModel(NAME_("Models/icosa.hedron.fbx"));
	torusmodel			= GetModel(NAME_("Models/torus.fbx"));
	tubemodel			= GetModel(NAME_("Models/tube.fbx"));
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
		UpdatePipelineStates();
		ClearWarnings(TYPE_ID("ShaderBindings"));
	}

	ImGui::ShowTestWindow();

	ShowMemoryInfo();

	PROFILE_END; // ui logic

	auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));
	ClearRenderTarget(drawList, GetRTV(RT_A), float4(0.5f, 0.5f, 0.5f, 1));
	ClearDepthStencil(drawList, GetDSV(DepthBuffer));

	float3 scale = float3(10, 10, 10);
	float4 qrotation = float4(0, 0, 0, 1);
	float3 position = float3(0, 0, 0);

	auto worldMatrix = XMMatrixTranspose(
		XMMatrixAffineTransformation(
			XMLoadFloat3((XMFLOAT3*)&scale),
			XMVectorZero(),
			XMLoadFloat4((XMFLOAT4*)&qrotation),
			XMLoadFloat3((XMFLOAT3*)&position)
			));

	auto viewProjMatrix = XMMatrixTranspose(
		CameraControlerPtr->GetViewMatrix()
		* XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f));

	auto renderData = GetModelRenderData(icosahedronmodel);

	for (auto i : MakeRange(renderData->submeshes.num)) {
		SetShaderState(drawList, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
		SetRenderTarget(drawList, 0, GetRTV(RT_A));
		SetDepthStencil(drawList, GetDSV(DepthBuffer));
		SetViewport(drawList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
		SetTopology(drawList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		SetConstant(drawList, TEXT_("World"), worldMatrix);
		SetConstant(drawList, TEXT_("ViewProj"), viewProjMatrix);

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

		auto submesh = renderData->submeshes[i];
		DrawIndexed(drawList, submesh.index_count, submesh.start_index, submesh.base_vertex);
	}

	CopyResource(drawList, GetCurrentBackbuffer(), RT_A);
	/*Execute(drawList);
	drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));*/
	
	RenderUserInterface(drawList);
	Execute(drawList);

	Present();
}

void Shutdown() {
	WaitForCompletion();
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, APP_FLAG_D3D12_DEBUG, APP_PRESENT_LOWLATENCY);

	return RunApplicationMainLoop();
}
