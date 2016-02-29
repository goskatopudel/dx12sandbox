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

using namespace Essence;

resource_handle RT_A;
resource_handle RT_UAV;
resource_handle UA_Depth;
FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

void CreateScreenResources() {
	if (IsValid(RT_A)) {
		Delete(RT_A);
		Delete(RT_UAV);
		Delete(UA_Depth);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	RT_A = CreateTexture2D(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt0", float4(0.5f, 0.5f, 0.5f, 1));
	RT_UAV = CreateTexture2D(x, y, DXGI_FORMAT_R8G8B8A8_UNORM, ALLOW_UNORDERED_ACCESS, "uav0");
	UA_Depth = CreateTexture2D(x, y, DXGI_FORMAT_R32_UINT, ALLOW_UNORDERED_ACCESS, "uav_depth");
}

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));
}

using namespace DirectX;

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

	PROFILE_END; // ui logic

	auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));

	ClearRenderTarget(drawList, GetRTV(RT_A), float4(0.5f, 0.5f, 0.5f, 1));
	CopyResource(drawList, GetCurrentBackbuffer(), RT_A);

	ClearUnorderedAccess(drawList, GetUAV(RT_UAV), float4(0, 0, 0, 0));
	ClearUnorderedAccess(drawList, GetUAV(UA_Depth), 0xFFFFFFFFu);

	auto viewMatrix = CameraControlerPtr->GetViewMatrix();
	auto projMatrix = XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f);
	auto viewProjMatrix = viewMatrix * projMatrix;

	viewMatrix = XMMatrixTranspose(viewMatrix);
	projMatrix = XMMatrixTranspose(projMatrix);
	viewProjMatrix = XMMatrixTranspose(viewProjMatrix);

	TransitionBarrier(drawList, Slice(RT_UAV), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	SetComputeShaderState(drawList, SHADER_(Rasterizer, Rasterize, CS_5_0));
	SetRWTexture2D(drawList, TEXT_("Target"), GetUAV(RT_UAV));
	SetRWTexture2D(drawList, TEXT_("Depth"), GetUAV(UA_Depth));
	SetConstant(drawList, TEXT_("ViewProjection"), viewProjMatrix);
	SetConstant(drawList, TEXT_("ScreenResolution"), float2(GDisplaySettings.resolution.x, GDisplaySettings.resolution.y));
	Dispatch(drawList, (GDisplaySettings.resolution.x + 7) / 8, (GDisplaySettings.resolution.y + 7) / 8, 1);

	SetShaderState(drawList, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
	SetTexture2D(drawList, TEXT_("Image"), GetSRV(RT_UAV));
	SetViewport(drawList, GDisplaySettings.resolution.x, GDisplaySettings.resolution.y);
	SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
	Draw(drawList, 3);

	RenderUserInterface(drawList);
	TransitionBarrier(drawList, Slice(GetCurrentBackbuffer()), D3D12_RESOURCE_STATE_PRESENT);
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
