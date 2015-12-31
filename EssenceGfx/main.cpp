#include "Shader.h"
#include "Essence.h"

#pragma comment(lib,"SDL2.lib")
#pragma comment(lib,"SDL2main.lib")

#include "Application.h"
#include "Device.h"
#include "Commands.h"
#include "Resources.h"
#include <DirectXMath.h>
#include "Camera.h"

#include "Model.h"
#include "UIRendering.h"
#include "imgui/imgui.h"
#include "Shader.h"

#include "Hashmap.h"

using namespace Essence;

namespace Essence {
GPUQueue* GDrawQueue;
GPUQueue* GCopyQueue;
}

resource_handle RT_A;
resource_handle RT_B;
resource_handle DepthBuffer;

vertex_factory_handle QuadVertex;
vertex_factory_handle ColoredVertex;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

#include "Scene.h"
#include "Random.h"

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

inline void* HandleToTexID(resource_handle handle) {
	u64 val = 0;
	*reinterpret_cast<resource_handle*>(&val) = handle;
	return (void*)val;
}

#include <Psapi.h>

void ShowMemoryInfo() {
	ImGui::Begin("Memory");

	auto localMemory = GetLocalMemoryInfo();
	auto nonLocalMemory = GetNonLocalMemoryInfo();

	ImGui::BulletText("Device memory");
	ImGui::Indent();
	ImGui::Text("Local memory");
	ImGui::Text("Budget:\nCurrent usage:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(localMemory.Budget), Megabytes(localMemory.CurrentUsage));
	ImGui::Text("Non-Local memory");
	ImGui::Text("Budget:\nCurrent usage:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(nonLocalMemory.Budget), Megabytes(nonLocalMemory.CurrentUsage));
	ImGui::Unindent();

	ImGui::Separator();
	
	ImGui::BulletText("Process memory");
	PROCESS_MEMORY_COUNTERS pmc;
	Verify(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)));
	ImGui::Indent();
	ImGui::Text("Working set:\nPagefile:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(pmc.WorkingSetSize), Megabytes(pmc.PagefileUsage));
	ImGui::Unindent();

	ImGui::Separator();

	ImGui::BulletText("System memory");
	PERFORMANCE_INFORMATION perfInfo;
	Verify(GetPerformanceInfo(&perfInfo, sizeof(perfInfo)));
	ImGui::Indent();
	ImGui::Text("Commited total:\nPhysical total:\nPhysical available:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb\n%llu Mb"
		, Megabytes(perfInfo.CommitTotal * perfInfo.PageSize)
		, Megabytes(perfInfo.PhysicalTotal * perfInfo.PageSize)
		, Megabytes(perfInfo.PhysicalAvailable * perfInfo.PageSize));
	ImGui::Unindent();
	
	ImGui::End();
}

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
	InitDevice(GDisplaySettings.hwnd, false, true);
	InitRenderingEngines();
	InitResources();

	GDrawQueue = CreateQueue(DIRECT_QUEUE);
	GCopyQueue = CreateQueue(COPY_QUEUE);

	CreateSwapChain(GetD12Queue(GDrawQueue), 3);

	CreateScreenResources();

	QuadVertex		= GetVertexFactory({ VertexInput::POSITION_4_32F, VertexInput::TEXCOORD_32F});
	ColoredVertex	= GetVertexFactory({ VertexInput::POSITION_3_32F, VertexInput::COLOR_RGBA_8U });

	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));

	CreateTestScene(testScene, 100);

	ImGuiIO& io = ImGui::GetIO();
	io.KeyMap[ImGuiKey_Tab] = SDL_SCANCODE_TAB;
	io.KeyMap[ImGuiKey_LeftArrow] = SDL_SCANCODE_LEFT;
	io.KeyMap[ImGuiKey_RightArrow] = SDL_SCANCODE_RIGHT;
	io.KeyMap[ImGuiKey_UpArrow] = SDL_SCANCODE_UP;
	io.KeyMap[ImGuiKey_DownArrow] = SDL_SCANCODE_DOWN;
	io.KeyMap[ImGuiKey_Home] = SDL_SCANCODE_HOME;
	io.KeyMap[ImGuiKey_End] = SDL_SCANCODE_END;
	io.KeyMap[ImGuiKey_Delete] = SDL_SCANCODE_DELETE;
	io.KeyMap[ImGuiKey_Backspace] = SDL_SCANCODE_BACKSPACE;
	io.KeyMap[ImGuiKey_Enter] = SDL_SCANCODE_RETURN;
	io.KeyMap[ImGuiKey_Escape] = SDL_SCANCODE_ESCAPE;
	io.KeyMap[ImGuiKey_A] = SDL_SCANCODE_A;
	io.KeyMap[ImGuiKey_C] = SDL_SCANCODE_C;
	io.KeyMap[ImGuiKey_V] = SDL_SCANCODE_V;
	io.KeyMap[ImGuiKey_X] = SDL_SCANCODE_X;
	io.KeyMap[ImGuiKey_Y] = SDL_SCANCODE_Y;
	io.KeyMap[ImGuiKey_Z] = SDL_SCANCODE_Z;

	io.RenderDrawListsFn = Essence::RenderImDrawLists;
	io.ImeWindowHandle = GDisplaySettings.hwnd;

	io.Fonts->AddFontDefault();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	io.Fonts->TexID = nullptr;

	D3D12_SUBRESOURCE_DATA imageData;
	imageData.pData = pixels;
	imageData.RowPitch = sizeof(u32) * width;
	imageData.SlicePitch = sizeof(u32) * width * height;

	auto fontsTexture = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, NO_TEXTURE_FLAGS, "font_texture");
	CopyFromCpuToSubresources(GCopyQueue, Slice(fontsTexture), 1, &imageData);
	io.Fonts->TexID = HandleToTexID(fontsTexture);

	// after issuing init copies waiting for completion
	QueueWait(GDrawQueue, GetFence(GCopyQueue));
}

void Tick() {
	static INT64 PreviousTime = 0;
	static INT64 TicksPerSecond = 0;

	INT64 currentTime;
	Verify(QueryPerformanceCounter((LARGE_INTEGER *)&currentTime));

	double DeltaTime = (double)(currentTime - PreviousTime) / (double)TicksPerSecond;

	if (TicksPerSecond == 0) {
		Verify(QueryPerformanceFrequency((LARGE_INTEGER *)&TicksPerSecond));

		DeltaTime = 1. / 60.;
	}
	PreviousTime = currentTime;

	float fDeltaTime = (float)DeltaTime;

	ImGuiIO& io = ImGui::GetIO();
	RECT rect;
	GetClientRect(GDisplaySettings.hwnd, &rect);

	io.DisplaySize = ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top));
	io.DeltaTime = (float)DeltaTime;
	io.MouseDrawCursor = true;
	SDL_ShowCursor(SDL_DISABLE);

	io.KeyShift = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT];
	io.KeyCtrl = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LCTRL];
	io.KeyAlt = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LALT];

	io.KeysDown[SDL_SCANCODE_TAB] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];
	io.KeysDown[SDL_SCANCODE_LEFT] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LEFT];
	io.KeysDown[SDL_SCANCODE_RIGHT] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_RIGHT];
	io.KeysDown[SDL_SCANCODE_UP] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_UP];
	io.KeysDown[SDL_SCANCODE_DOWN] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_DOWN];
	io.KeysDown[SDL_SCANCODE_HOME] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_HOME];
	io.KeysDown[SDL_SCANCODE_END] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_END];
	io.KeysDown[SDL_SCANCODE_DELETE] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_DELETE];
	io.KeysDown[SDL_SCANCODE_RETURN] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_RETURN];
	io.KeysDown[SDL_SCANCODE_ESCAPE] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE];
	io.KeysDown[SDL_SCANCODE_BACKSPACE] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_BACKSPACE];
	io.KeysDown[SDL_SCANCODE_A] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A];
	io.KeysDown[SDL_SCANCODE_C] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_C];
	io.KeysDown[SDL_SCANCODE_V] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_V];
	io.KeysDown[SDL_SCANCODE_X] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_X];
	io.KeysDown[SDL_SCANCODE_Y] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_Y];
	io.KeysDown[SDL_SCANCODE_Z] = !!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_Z];

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

	int x, y;
	auto buttonState = SDL_GetMouseState(&x, &y);
	io.MousePos = ImVec2((float)x, (float)y);
	io.MouseDown[0] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_LEFT));
	io.MouseDown[1] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_RIGHT));
	io.MouseDown[2] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_MIDDLE));

	PROFILE_BEGIN(ui_logic);

	ImGui::NewFrame();

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
		auto drawList = GetCommandList(GDrawQueue, NAME_("RenderWork"));
		auto copyList = GetCommandList(GCopyQueue, NAME_("CopyWork"));

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
		color = DirectX::XMVectorSet(0,0,0,1);

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

		auto lastFence = ParallelRenderScene(GDrawQueue, testScene, drawList, &FpsCamera);
		lastFence = lastFence.generation ? lastFence : clearFinished;

		QueueWait(GCopyQueue, GetFence(GDrawQueue));
		CopyResource(copyList, RT_B, RT_A);
		auto copyFinished = GetFence(copyList);
		Execute(copyList);
		drawList = GetCommandList(GDrawQueue, NAME_("RenderWork"));
		QueueWait(GDrawQueue, copyFinished);
		CopyResource(drawList, GetCurrentBackbuffer(), RT_B);

		auto drawUiCommands = RenderUserInterface(GDrawQueue);
		Execute(drawList);
		Execute(drawUiCommands);

		{
			PROFILE_SCOPE(wait_for_present);
			Present(1);
		}
	}
	else {

		auto drawList = GetCommandList(GDrawQueue, NAME_("RenderWork"));
		auto copyList = GetCommandList(GCopyQueue, NAME_("CopyWork"));
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

	EndCommandsFrame(GDrawQueue, 3);
}

void Shutdown() {
	call_destructor(testScene);
	
	ImGui::Shutdown();

	WaitForCompletion();
	FreeModelsMemory();
	ShutdownRenderingEngines();
	ShutdownResources();
	ShutdownDevice();
	FreeShadersMemory();
}

int main(int argc, char * argv[]) {
	using namespace Essence;
	
	GDisplaySettings.resolution.x = 1200;
	GDisplaySettings.resolution.y = 768;
	GDisplaySettings.vsync = 1;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		WaitForCompletion();
		ResizeSwapChain(GDisplaySettings.resolution.x, GDisplaySettings.resolution.y);
		CreateScreenResources();
	};

	return ApplicationWinMain();
}