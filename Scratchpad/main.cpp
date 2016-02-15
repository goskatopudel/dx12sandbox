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

void CreateScreenResources() {
	if (IsValid(RT_A)) {
		Delete(RT_A);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	RT_A = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ALLOW_RENDER_TARGET, "rt0", float4(0, 0, 0, 1));
}

void ReloadCodeFromDLL(cstr dllpath);

void Init() {
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));
}

#include "VectorMath.h"

#include "../ScratchpadRuntimeDLL/ScratchpadRuntimeDLL.h"

void DrawLine2D(Vec2f P0, Vec2f P1, Color4b C0, Color4b C1);
ScratchpadRuntimeCodeFunc	RuntimeCodeFunc;
HMODULE						RuntimeLibrary;
FILETIME					LoadedDLLFiletime;

void ReloadCodeFromDLL(cstr dllpath) {
	HANDLE lockfile = CreateFile("../ScratchpadRuntimeDLL/x64/Debug/pdb.lock", GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, 0, NULL);

	if (lockfile != INVALID_HANDLE_VALUE) {
		CloseHandle(lockfile);
		Sleep(5);
		return;
	}

	HANDLE file = CreateFile(dllpath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, 0, NULL);

	if (file == INVALID_HANDLE_VALUE) {
		return;
	}

	SCOPE_EXIT(CloseHandle(file));

	FILETIME ftCreate, ftAccess, ftWrite;

	// Retrieve the file times for the file.
	if (!GetFileTime(file, &ftCreate, &ftAccess, &ftWrite)) {
		return;
	}

	if (LoadedDLLFiletime.dwHighDateTime == ftWrite.dwHighDateTime
		&& LoadedDLLFiletime.dwLowDateTime == ftWrite.dwLowDateTime) {
		return;
	}

	if (RuntimeLibrary) {
		FreeLibrary(RuntimeLibrary);
		RuntimeLibrary = {};
		RuntimeCodeFunc = {};
	}

	cstr const LocalDllFile = "RuntimeDLLCopy.dll";
	cstr const LocalPdbFile = "Runtime_tmp.pdb";
	u32 Counter = 0;
	while (CopyFile("../ScratchpadRuntimeDLL/x64/Debug/ScratchpadRuntimeDLL.pdb", LocalPdbFile, false) == 0) {
		
	}
	Verify(CopyFile(dllpath, LocalDllFile, false));

	RuntimeLibrary = LoadLibrary(LocalDllFile);
	if (RuntimeLibrary == NULL) {
		return;
	}

	LoadedDLLFiletime = ftWrite;

	ScratchpadInterface exportedInterface = {};
	exportedInterface.DrawLine2D = DrawLine2D;
	
	ScratchpadUpdateInterfaceFunc ScratchpadUpdateInterface = (ScratchpadUpdateInterfaceFunc)GetProcAddress(RuntimeLibrary, "ScratchpadUpdateInterface");

	ScratchpadUpdateInterface(exportedInterface);

	RuntimeCodeFunc = (ScratchpadRuntimeCodeFunc)GetProcAddress(RuntimeLibrary, "ScratchpadRuntimeCode");
}

struct line_vertex_t {
	Vec4f	position;
	u32		color;
};

enum LineDrawType {
	LINE_2D,
};

struct line_batch_t {
	LineDrawType	type;
	u32				num;
};

Array<line_vertex_t>	VB;
Array<line_batch_t>		Batches;
vertex_factory_handle	LineVertex;

void DrawLine2D(Vec2f P0, Vec2f P1, Color4b C0, Color4b C1) {
	line_vertex_t v0;
	v0.position = Vec4f(P0.x, P0.y, 0, 1);
	v0.color = C0.packed_u32;
	line_vertex_t v1;
	v1.position = Vec4f(P1.x, P1.y, 0, 1);
	v1.color = C1.packed_u32;
	PushBack(VB, v0);
	PushBack(VB, v1);

	if (Size(Batches) && Back(Batches).type == LINE_2D) {
		++Back(Batches).num;
	}
	else {
		line_batch_t batch;
		batch.num = 1;
		batch.type = LINE_2D;
		PushBack(Batches, batch);
	}
}

void DrawScratchpad(GPUCommandList* cmdList) {
	using namespace DirectX;

	if (!IsValid(LineVertex)) {
		LineVertex = GetVertexFactory({ VertexInput::POSITION_4_32F, VertexInput::COLOR_RGBA_8U });
	}

	if (RuntimeCodeFunc) {
		Vec2f screenres = Vec2f((float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
		Vec2f mousepos = Vec2f((float)ImGui::GetMousePos().x, (float)ImGui::GetMousePos().y);
		RuntimeCodeFunc( screenres, mousepos );
	}

	auto matrix2D = XMMatrixTranspose(
		XMMatrixOrthographicOffCenterLH(
			0, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y, 0, 0, 1));

	u64 bytesize = sizeof(line_vertex_t) * Size(VB);
	auto uploadHeap = AllocateSmallUploadMemory(cmdList, bytesize, 16);
	buffer_location_t vb;
	vb.address = uploadHeap.virtual_address;
	vb.size = (u32)bytesize;
	vb.stride = sizeof(line_vertex_t);
	memcpy(uploadHeap.write_ptr, VB.DataPtr, bytesize);
	SetVertexStream(cmdList, 0, vb);

	SetShaderState(cmdList, SHADER_(Line, VShader2D, VS_5_1), SHADER_(Line, PShader2D, PS_5_1), LineVertex);
	SetViewport(cmdList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(cmdList, D3D_PRIMITIVE_TOPOLOGY_LINELIST);
	SetRenderTarget(cmdList, 0, GetRTV(RT_A));
	SetConstant(cmdList, TEXT_("ViewProj"), matrix2D);

	u32 vOffset = 0;
	for (u32 i = 0; i < Size(Batches); ++i) {
		Draw(cmdList, Batches[i].num * 2, vOffset);
		vOffset += Batches[i].num * 2;
	}

	Clear(VB);
	Clear(Batches);
}

void Tick(float fDeltaTime) {

	ImGuiIO& io = ImGui::GetIO();
	static float rx = 0;
	static float ry = 0;
	io.MouseDrawCursor = false;
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

	ClearRenderTarget(drawList, GetRTV(RT_A), float4(0, 0, 0, 1));

	ReloadCodeFromDLL("../ScratchpadRuntimeDLL/x64/Debug/ScratchpadRuntimeDLL.dll");
	DrawScratchpad(drawList);

	CopyResource(drawList, GetCurrentBackbuffer(), RT_A);

	RenderUserInterface(drawList);
	Execute(drawList);

	{
		PROFILE_SCOPE(wait_for_present);
		Present();
	}
}

void Shutdown() {
	WaitForCompletion();

	FreeMemory(Batches);
	FreeMemory(VB);

	if (RuntimeLibrary) {
		FreeLibrary(RuntimeLibrary);
	}
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, APP_FLAG_D3D12_DEBUG, APP_PRESENT_DEFAULT);

	return RunApplicationMainLoop();
}
