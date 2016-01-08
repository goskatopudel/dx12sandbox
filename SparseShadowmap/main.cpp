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

#include "Scene.h"
#include "Random.h"

#include "SDL.h"

#pragma comment(lib,"SDL2main.lib")

using namespace Essence;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

Scene testScene;
resource_handle SceneColor;
resource_handle DepthBuffer;
resource_handle ShadowLOD;
resource_handle LowResSM;
resource_handle VirtualSM;
resource_handle PagesNeeded;

void CreateScreenResources() {
	if (IsValid(DepthBuffer)) {
		Delete(DepthBuffer);
		Delete(ShadowLOD);
		Delete(SceneColor);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	SceneColor = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM, ALLOW_RENDER_TARGET, "scene_color", float4(0.1f, 0.1f, 0.1f, 1.f));
	ShadowLOD = CreateTexture(x, y, DXGI_FORMAT_R8_UINT, ALLOW_RENDER_TARGET, "shadow_lod");
	DepthBuffer = CreateTexture(x, y, DXGI_FORMAT_R24G8_TYPELESS, ALLOW_DEPTH_STENCIL, "depth");
}

void Init() {
	SpawnEntity(testScene, GetModel(NAME_("models/sibenik.obj")));
	/*auto hairball = SpawnEntity(testScene, GetModel(NAME_("models/hairball.obj")));
	SetPosition(testScene, hairball, float3(50, 0, 0));*/
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));
	LowResSM = CreateTexture(128, 128, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL, "low_res_sm");
	VirtualSM = CreateTexture(16384, 16384, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL | TEX_MIPMAPPED | TEX_VIRTUAL, "virtual_sm");
	PagesNeeded = CreateTexture(16384 / 128, 16384 / 128, DXGI_FORMAT_R32_UINT, ALLOW_UNORDERED_ACCESS | TEX_MIPMAPPED, "vsm_pages");
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

	using namespace Essence;

	auto depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));
	using namespace DirectX;

	ClearRenderTarget(depthCL, GetRTV(ShadowLOD));
	ClearRenderTarget(depthCL, GetRTV(SceneColor), float4(0.1f, 0.1f, 0.1f, 1.f));
	ClearDepthStencil(depthCL, GetDSV(DepthBuffer));
	ClearDepthStencil(depthCL, GetDSV(LowResSM));
	ClearUnorderedAccess(depthCL, GetUAV(PagesNeeded));

	xmvec lightDirection = XMVector3Normalize(XMVectorSet(1,1,1,0));
	xmmatrix shadowmapMatrix;

	auto viewMatrix = CameraControlerPtr->GetViewMatrix();
	auto projMatrix = XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f);

	auto viewProjMatrix = XMMatrixTranspose(viewMatrix * projMatrix);
	viewMatrix = XMMatrixTranspose(viewMatrix);
	projMatrix = XMMatrixTranspose(projMatrix);

	shadowmapMatrix = XMMatrixLookAtLH(lightDirection * 200, XMVectorZero(), XMVectorSet(0,1,0,1)) * XMMatrixOrthographicLH(64, 64, 1.f, 400);
	shadowmapMatrix = XMMatrixTranspose(shadowmapMatrix);

	for (auto entity : testScene.Entities) {

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&entity.scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&entity.qrotation),
				XMLoadFloat3((XMFLOAT3*)&entity.position)
				));

		auto renderData = GetModelRenderData(entity.model);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
			SetRenderTarget(depthCL, 0, GetRTV(SceneColor));
			SetRenderTarget(depthCL, 1, GetRTV(ShadowLOD));
			SetDepthStencil(depthCL, GetDSV(DepthBuffer));
			SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), viewProjMatrix);
			SetConstant(depthCL, TEXT_("DirectionalLightMatrix"), shadowmapMatrix);

			//SetRWTexture2D(depthCL, TEXT_("PagesTexture"), Slice(PagesNeeded));

			buffer_location_t vb;
			vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
			vb.size = renderData->vertices_num * sizeof(mesh_vertex_t);
			vb.stride = sizeof(mesh_vertex_t);

			SetVertexStream(depthCL, 0, vb);

			buffer_location_t ib;
			ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
			ib.size = renderData->indices_num * sizeof(u32);
			ib.stride = sizeof(u32);

			SetIndexBuffer(depthCL, ib);

			auto submesh = renderData->submeshes[i];
			DrawIndexed(depthCL, submesh.index_count, submesh.start_index, submesh.base_vertex);

			//

			SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), {}, renderData->vertex_layout);
			SetRenderTarget(depthCL, 0, {});
			SetRenderTarget(depthCL, 1, {});
			SetViewport(depthCL, (float)128, (float)128);

			SetDepthStencil(depthCL, GetDSV(LowResSM));

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), shadowmapMatrix);

			DrawIndexed(depthCL, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}

	float3 cameraPos;
	XMStoreFloat3(&cameraPos, CameraControlerPtr->Position);
	SetComputeShaderState(depthCL, SHADER_(VirtualSM, PreparePages, CS_5_0));
	SetTexture2D(depthCL, TEXT_("DepthBuffer"), GetSRV(DepthBuffer));
	SetTexture2D(depthCL, TEXT_("ShadowLevel"), GetSRV(ShadowLOD));
	SetRWTexture2D(depthCL, TEXT_("PagesTexture"), GetUAV(PagesNeeded));
	SetConstant(depthCL, TEXT_("ViewMatrix"), viewMatrix);
	SetConstant(depthCL, TEXT_("ProjectionMatrix"), projMatrix);
	SetConstant(depthCL, TEXT_("ShadowmapMatrix"), shadowmapMatrix);
	SetConstant(depthCL, TEXT_("CameraPos"), cameraPos);
	SetConstant(depthCL, TEXT_("Resolution"), float2((float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y));
	Dispatch(depthCL, (GDisplaySettings.resolution.x + 7) / 8, (GDisplaySettings.resolution.y + 7) / 8, 1);

	u32 subresource = 0;
	for (u32 size = 128; size > 1; size >>= 1) {
		u32 target = size / 2;
		SetComputeShaderState(depthCL, SHADER_(Mipmap, BuildMinMip, CS_5_1));

		SetTexture2D(depthCL, TEXT_("LowerLevel"), GetSRV(PagesNeeded, subresource));
		SetRWTexture2D(depthCL, TEXT_("CurrentLevel"), GetUAV(PagesNeeded, subresource+1));
		Dispatch(depthCL, (target + 7) / 8, (target + 7) / 8, 1);
		subresource++;

		Execute(depthCL);
		depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));
	}

	SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(SceneColor));
	Draw(depthCL, 3);

	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 128.f, 128.f, 128.f + 1.f + 10.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 64.f, 64.f, 128.f + 1.f + 10.f + 1.f + 128.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 1));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 32.f, 32.f, 128.f + 1.f + 10.f + 1.f + 128.f + 64.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 2));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 16.f, 16.f, 128.f + 1.f + 10.f + 1.f + 128.f + 64.f + 32.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 3));
	Draw(depthCL, 3);

	SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, LinearizeDepthPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, (float)128, (float)128, 1, 1);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(LowResSM));
	SetConstant(depthCL, TEXT_("Projection_33"), XMVectorGetZ(shadowmapMatrix.r[2]));
	SetConstant(depthCL, TEXT_("Projection_43"), XMVectorGetW(shadowmapMatrix.r[2]));
	Draw(depthCL, 3);

	Execute(depthCL);

	auto mainCL = GetCommandList(GGPUMainQueue, NAME_("main_cl"));
	RenderUserInterface(mainCL);
	Execute(mainCL);

	Present(1);
}

void Shutdown() {
	WaitForCompletion();

	call_destructor(testScene);
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, 1, APP_D3D12_DEBUG);

	return RunApplicationMainLoop();
}
