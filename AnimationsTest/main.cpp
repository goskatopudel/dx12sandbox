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

resource_handle RT_A;
resource_handle RT_B;
resource_handle DepthBuffer;

vertex_factory_handle QuadVertex;
vertex_factory_handle ColoredVertex;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

void CreateTestScene(Scene &Scene, i32 sceneObjectsNum);

void ShowSceneWidget(Scene& Scene) {
	static i32 sceneObjectsNum = 100;

	ImGui::SliderInt("Scene objects", &sceneObjectsNum, 0, 2000);

	if (Scene.EntitiesNum != sceneObjectsNum) {
		call_destructor(Scene);
		Scene = ::Scene();
		CreateTestScene(Scene, sceneObjectsNum);
	}
};

void CreateTestScene(Scene &Scene, i32 sceneObjectsNum) {
	struct available_model {
		model_handle	model;
		float			scale;
	};

	available_model models[] = {
		{ GetModel(NAME_("Models/TestBull.fbx")), 0.025f },
		{ GetModel(NAME_("Models/boblampclean.md5mesh")), 0.15f },
		{ GetModel(NAME_("Models/ninja.mesh")), 0.07f },
	};

	random_generator rng;

	auto generate_position = [&](u32 index) -> float3 {
		u32 rowLen = 50;
		return float3((index % rowLen) * 20.f, 0, (index / rowLen) * 20.f);
	};

	for (auto i : MakeRange((i32)Scene.EntitiesNum, sceneObjectsNum)) {
		auto modelIndex = rng.u32Next() % _countof(models);

		auto renderData = GetModelRenderData(models[modelIndex].model);

		auto entity = SpawnEntity(Scene, models[modelIndex].model);
		auto animIndex = rng.u32Next() % renderData->animations.num;
		SetAnimation(Scene, entity, animIndex, rng.f32Next() * renderData->animations[animIndex].duration / renderData->animations[animIndex].ticks_per_second);
		SetPosition(Scene, entity, generate_position(i));
		SetScale(Scene, entity, models[modelIndex].scale);
	}
}

Scene testScene;

void CreateScreenResources() {
	if (IsValid(RT_A)) {
		Delete(RT_A);
		Delete(RT_B);
		Delete(DepthBuffer);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	RT_A = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt0", float4(0.5f, 0.5f, 0.5f, 1));
	RT_B = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt1", float4(0.5f, 0.5f, 0.5f, 1));
	DepthBuffer = CreateTexture(x, y, DXGI_FORMAT_R24G8_TYPELESS, ALLOW_DEPTH_STENCIL, "depth");
}

void Init() {
	CreateScreenResources();

	QuadVertex = GetVertexFactory({ VertexInput::POSITION_4_32F, VertexInput::TEXCOORD_32F });
	ColoredVertex = GetVertexFactory({ VertexInput::POSITION_3_32F, VertexInput::COLOR_RGBA_8U });

	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));

	CreateTestScene(testScene, 100);
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

	ShowSceneWidget(testScene);

	PROFILE_END; // ui logic

	using namespace Essence;

	static bool copyAtoB;
	const bool swapQueues = true;

	if (true) {
		auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));
		auto copyList = GetCommandList(GGPUCopyQueue, NAME_("CopyWork"));

		ClearRenderTarget(drawList, Slice(RT_A), float4(0.5f, 0.5f, 0.5f, 1));
		ClearDepthStencil(drawList, Slice(DepthBuffer));
		SetShaderState(drawList, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, ColorPS, PS_5_1), {});
		SetRenderTarget(drawList, 0, Slice(RT_A));
		SetViewport(drawList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
		SetTopology(drawList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		static float x = 0;
		x += 1 / 60.f;
		auto fx = x * 3.14f;
		fx = fx > DirectX::XM_2PI ? fx - DirectX::XM_2PI : fx;
		auto color = DirectX::XMColorRGBToSRGB(toSimd(float4(sinf(fx) * 0.5f + 0.5f, 0.001f, 0.001f, 1)));
		color = DirectX::XMVectorSet(0, 0, 0, 1);

		SetConstant(drawList, TEXT_("WriteColor"), color);
		Draw(drawList, 3);

		UpdateScene(testScene, fDeltaTime);
		//RenderScene(testScene, drawList, &FpsCamera);

		/*struct vertex_t {
		float4 position;
		float2 texcoord;
		};

		vertex_t vertices[] = {
		{ float4(0,0,0.999f,1) , float2(0,0) },
		{ float4(0,1,0.999f,1) , float2(1,0) },
		{ float4(1,0,0.999f,1) , float2(0,1) },
		{ float4(1,0,0.999f,1) , float2(0,0) },
		{ float4(0,1,0.999f,1) , float2(1,0) },
		{ float4(1,1,0.999f,1) , float2(0,1) }
		};

		auto VB = AllocateSmallUploadMemory(drawList, sizeof(vertices), 16);
		buffer_location_t vb_location;
		vb_location.address = VB.virtual_address;
		vb_location.stride = sizeof(vertex_t);
		vb_location.size = sizeof(vertices);
		memcpy(VB.write_ptr, &vertices, sizeof(vertices));

		SetShaderState(drawList, SHADER_(Test, VShader, VS_5_1), SHADER_(Test, ColorPS, PS_5_1), QuadVertex);
		SetViewport(drawList, 1200, 768);
		SetTopology(drawList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		SetRenderTarget(drawList, 0, Slice(RT_A));
		SetDepthStencil(drawList, Slice(DepthBuffer));
		SetVertexStream(drawList, 0, vb_location);
		Draw(drawList, 6);*/

		auto clearFinished = GetFence(drawList);
		Execute(drawList);

		auto lastFence = ParallelRenderScene(GGPUMainQueue, testScene, drawList, &FpsCamera);
		lastFence = lastFence.generation ? lastFence : clearFinished;

		QueueWait(GGPUCopyQueue, GetFence(GGPUMainQueue));
		CopyResource(copyList, RT_B, RT_A);
		auto copyFinished = GetFence(copyList);
		Execute(copyList);
		drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));
		QueueWait(GGPUMainQueue, copyFinished);
		CopyResource(drawList, GetCurrentBackbuffer(), RT_B);

		auto drawUiCommands = RenderUserInterface(GGPUMainQueue);
		Execute(drawList);
		Execute(drawUiCommands);

		{
			PROFILE_SCOPE(wait_for_present);
			Present(1);
		}
	}
	else {

		auto drawList = GetCommandList(GGPUMainQueue, NAME_("RenderWork"));
		auto copyList = GetCommandList(GGPUCopyQueue, NAME_("CopyWork"));
		if (copyAtoB) {
			CopyResource(drawList, RT_B, RT_A);
		}
		else {
			CopyResource(swapQueues ? drawList : copyList, RT_A, RT_B);
		}
		copyAtoB = !copyAtoB;

		Close(drawList);
		Execute(copyList);
		Execute(drawList);
	}
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