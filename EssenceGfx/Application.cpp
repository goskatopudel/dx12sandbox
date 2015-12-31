#include "Application.h"
#include "Scheduler.h"
#include "Device.h"
#include "Resources.h"
#include "Commands.h"
#include "imgui\imgui.h"
#include "UIRendering.h"
#include "Model.h"

#include <SDL.h>
#include <SDL_syswm.h>

namespace Essence {

display_settings_t	GDisplaySettings;
char				GWorkingDir[1024];
size_t				GWorkingDirLength;
SDL_Window*			SDLWindow;

GPUQueue*			GGPUMainQueue;
GPUQueue*			GGPUCopyQueue;

void(*GApplicationInitializeFunction)	() = []() {};
void(*GApplicationTickFunction)			(float) = [](float) {};
void(*GApplicationShutdownFunction)		() = []() {};
void(*GApplicationKeyDownFunction)		(SDL_Keycode) = [](SDL_Keycode) {};
void(*GApplicationMouseWheelFunction)	(int) = [](int) {};
void(*GApplicationFileDropFunction)		(const char*) = [](const char*) {};
void(*GApplicationTextInputFunction)	(const wchar_t*) = [](const wchar_t*) {};
void(*GApplicationWindowResizeFunction)	() = []() {
};

const char* GetRelativeFilePath(const char* file) {
	return file + GWorkingDirLength + 1;
}

const char* GetFilename(const char* file) {
	return max(file, max(strrchr(file, '\\') + 1, strrchr(file, '/') + 1));
}

void InitSDL(SDL_Window **outSdlWindow) {
	auto currentDirectoryRead = GetCurrentDirectoryA(_countof(GWorkingDir), GWorkingDir);
	Check(currentDirectoryRead > 0 && currentDirectoryRead < _countof(GWorkingDir));
	for (auto i = 0u; i < GWorkingDirLength; ++i) {
		GWorkingDir[i] = tolower(GWorkingDir[i]);
	}
	GWorkingDirLength = currentDirectoryRead;

	SDL_Init(SDL_INIT_EVERYTHING);
	auto sdlWindow = SDL_CreateWindow(GDisplaySettings.window_title,
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		GDisplaySettings.resolution.x, GDisplaySettings.resolution.y,
		SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_RESIZABLE);

	SDL_SysWMinfo info = {};
	SDL_VERSION(&info.version);
	auto getwininfo_ok = SDL_GetWindowWMInfo(sdlWindow, &info);
	SDL_assert(getwininfo_ok);

	GDisplaySettings.hwnd = info.info.win.window;
	*outSdlWindow = sdlWindow;
}

void ShutdownSDL(SDL_Window *sdlWindow) {
	SDL_DestroyWindow(sdlWindow);
	SDL_Quit();
}

void InitApplication(u32 windowWidth, u32 windowHeight, int vsync, ApplicationFlagsEnum flags) {
	GDisplaySettings.resolution.x = windowWidth;
	GDisplaySettings.resolution.y = windowHeight;
	GDisplaySettings.vsync = vsync;

	InitMainThread();
	InitProfiler();
	PROFILE_NAME_THREAD("Main");
	InitScheduler();

	InitSDL(&SDLWindow);

	InitDevice(GDisplaySettings.hwnd, false, flags & APP_D3D12_DEBUG);
	InitRenderingEngines();
	InitResources();

	GGPUMainQueue = CreateQueue(DIRECT_QUEUE);
	GGPUCopyQueue = CreateQueue(COPY_QUEUE);

	CreateSwapChain(GetD12Queue(GGPUMainQueue), 3);

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
	CopyFromCpuToSubresources(GGPUCopyQueue, Slice(fontsTexture), 1, &imageData);
	io.Fonts->TexID = HandleToImGuiTexID(fontsTexture);

	GApplicationInitializeFunction();

	// after issuing init copies waiting for completion
	QueueWait(GGPUMainQueue, GetFence(GGPUCopyQueue));
}

i32 RunApplicationMainLoop() {
	SDL_Event event;
	bool active = true;
	while (active) {
		while (SDL_PollEvent(&event)) {

			switch (event.type) {

			case SDL_QUIT:
				active = false;
				break;
			case SDL_KEYDOWN:
				if (event.key.keysym.sym == SDLK_ESCAPE) {
					active = false;
					break;
				}
				else {
					GApplicationKeyDownFunction(event.key.keysym.sym);
				}
				break;
			case SDL_TEXTINPUT:
				{
					GApplicationTextInputFunction((wchar_t*)event.text.text);
				}
				break;
			case SDL_MOUSEWHEEL:
				{
					GApplicationMouseWheelFunction(event.wheel.y);
				}
				break;
			case SDL_DROPFILE:
				{
					const char* path = event.drop.file;
					GApplicationFileDropFunction(path);
					SDL_free((void*)path);
				}
				break;
			case SDL_WINDOWEVENT:
				{
					switch (event.window.event) {
					case SDL_WINDOWEVENT_RESIZED:
						{
							GDisplaySettings.resolution.x = event.window.data1;
							GDisplaySettings.resolution.y = event.window.data2;
							GApplicationWindowResizeFunction();
						}
						break;
					}
				}
				break;
			}
		}

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

		int x, y;
		auto buttonState = SDL_GetMouseState(&x, &y);
		io.MousePos = ImVec2((float)x, (float)y);
		io.MouseDown[0] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_LEFT));
		io.MouseDown[1] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_RIGHT));
		io.MouseDown[2] = !!(buttonState & SDL_BUTTON(SDL_BUTTON_MIDDLE));

		ImGui::NewFrame();

		GApplicationTickFunction(fDeltaTime);

		EndCommandsFrame(GGPUMainQueue, 3);
	}

	GApplicationShutdownFunction();

	WaitForCompletion();
	FreeModelsMemory();
	ImGui::Shutdown();
	ShutdownRenderingEngines();
	ShutdownResources();
	ShutdownDevice();
	FreeShadersMemory();

	ShutdownSDL(SDLWindow);

	ShutdownScheduler();
	ShutdownProfiler();
	ShutdownMainThread();
	return 0;
}

}