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
FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

resource_handle ColorImage;
resource_handle C0;
resource_handle C1;
resource_handle C1_BC;
resource_handle C2;
resource_handle C2_BC;

void CreateScreenResources() {
	if (IsValid(RT_A)) {
		Delete(RT_A);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	RT_A = CreateTexture2D(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt0", float4(0.5f, 0.5f, 0.5f, 1));
}

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));

	auto initialCopies = GetCommandList(GGPUCopyQueue, NAME_("Copy"));
	ColorImage = LoadDDSFromFile(TEXT_("Images/color1.dds"), initialCopies, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).resource;

	C0 = CreateTexture2D((u32)GetResourceInfo(ColorImage)->width, (u32)GetResourceInfo(ColorImage)->height,
		DXGI_FORMAT_R8G8B8A8_UNORM, ALLOW_RENDER_TARGET, "compressed");

	C1 = CreateTexture2D((u32)(GetResourceInfo(ColorImage)->width + 3) / 4, (u32)(GetResourceInfo(ColorImage)->height + 3) / 4,
		DXGI_FORMAT_R16G16B16A16_UINT, ALLOW_UNORDERED_ACCESS, "compressed bc1 - data");

	C1_BC = CreateTexture2D((u32)GetResourceInfo(ColorImage)->width, (u32)GetResourceInfo(ColorImage)->height,
		DXGI_FORMAT_BC1_UNORM_SRGB, NO_TEXTURE_FLAGS, "compressed bc1");

	C2 = CreateTexture2D((u32)(GetResourceInfo(ColorImage)->width + 3) / 4, (u32)(GetResourceInfo(ColorImage)->height + 3) / 4,
		DXGI_FORMAT_R32G32B32A32_UINT, ALLOW_UNORDERED_ACCESS, "compressed bc1 - data");

	C2_BC = CreateTexture2D((u32)GetResourceInfo(ColorImage)->width, (u32)GetResourceInfo(ColorImage)->height,
		DXGI_FORMAT_BC3_UNORM, NO_TEXTURE_FLAGS, "compressed bc1");

	Execute(initialCopies);
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

	ShowMemoryWindow();

	PROFILE_END; // ui logic

	auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));

	ClearRenderTarget(drawList, GetRTV(RT_A), float4(0.5f, 0.5f, 0.5f, 1));
	CopyResource(drawList, GetCurrentBackbuffer(), RT_A);

	enum class CompressionMode {
		None,
		YCoCg_Interleaved,
		BC3_YCoCg,
		BC1,
	};

	static CompressionMode mode = CompressionMode::None;

	if (ImGui::RadioButton("None (sRGBA)", mode == CompressionMode::None)) {
		mode = CompressionMode::None;
	}
	else if (ImGui::RadioButton("YCoCg Interleaved (1/2 bw)", mode == CompressionMode::YCoCg_Interleaved)) {
		mode = CompressionMode::YCoCg_Interleaved;
	}
	else if (ImGui::RadioButton("BC3 YCoCg (1/4 bw)", mode == CompressionMode::BC3_YCoCg)) {
		mode = CompressionMode::BC3_YCoCg;
	}
	else if (ImGui::RadioButton("BC1 (1/8 bw)", mode == CompressionMode::BC1)) {
		mode = CompressionMode::BC1;
	}

	if (mode == CompressionMode::None) {
		SetViewport(drawList, (float)GetResourceInfo(ColorImage)->width, (float)GetResourceInfo(ColorImage)->height);
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(ColorImage));
		Draw(drawList, 3);
	}
	else if (mode == CompressionMode::YCoCg_Interleaved) {
		SetViewport(drawList, (float)GetResourceInfo(ColorImage)->width, (float)GetResourceInfo(ColorImage)->height);
		SetRenderTarget(drawList, 0, GetRTV(C0));
		SetShaderState(drawList, SHADER_(Compression, VShader, VS_5_1), SHADER_(Compression, Compress, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(ColorImage));
		Draw(drawList, 3);

		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(Compression, VShader, VS_5_1), SHADER_(Compression, Decompress, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(C0));
		Draw(drawList, 3);
	}
	else if (mode == CompressionMode::BC3_YCoCg) {
		SetComputeShaderState(drawList, SHADER_(CompressionBC, BC3, CS_5_1));
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(ColorImage));
		SetRWTexture2D(drawList, TEXT_("BcData"), GetUAV(C2));
		Dispatch(drawList, (u32)(GetResourceInfo(C2)->width + 7) / 8, (u32)(GetResourceInfo(C2)->height + 7) / 8, 1);

		CopyResource(drawList, C2_BC, C2);
		TransitionBarrier(drawList, Slice(C2_BC), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		SetViewport(drawList, (float)GetResourceInfo(ColorImage)->width, (float)GetResourceInfo(ColorImage)->height);
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(Compression, VShader, VS_5_1), SHADER_(Compression, DecompressBC3YCoCg, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(C2_BC));
		Draw(drawList, 3);
	}
	else if (mode == CompressionMode::BC1) {
		SetComputeShaderState(drawList, SHADER_(CompressionBC, BC1, CS_5_1));
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(ColorImage));
		SetRWTexture2D(drawList, TEXT_("BcData"), GetUAV(C1));
		Dispatch(drawList, (u32)(GetResourceInfo(ColorImage)->width + 7) / 8, (u32)(GetResourceInfo(ColorImage)->height + 7) / 8, 1);

		CopyResource(drawList, C1_BC, C1);
		TransitionBarrier(drawList, Slice(C1_BC), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		SetViewport(drawList, (float)GetResourceInfo(ColorImage)->width, (float)GetResourceInfo(ColorImage)->height);
		SetRenderTarget(drawList, 0, GetRTV(GetCurrentBackbuffer()));
		SetShaderState(drawList, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
		SetTexture2D(drawList, TEXT_("Image"), GetSRV(C1_BC));
		Draw(drawList, 3);
	}

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
