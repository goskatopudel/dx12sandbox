#include "Application.h"
#include "Scheduler.h"

namespace Essence {

display_settings_t	GDisplaySettings;
char				GWorkingDir[1024];
size_t				GWorkingDirLength;

void(*GApplicationInitializeFunction)	() = []() {};
void(*GApplicationTickFunction)			() = []() {};
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

i32 ApplicationWinMain() {
	InitMainThread();
	InitProfiler();
	PROFILE_NAME_THREAD("Main");
	InitScheduler();

	SDL_Window* window;
	InitSDL(&window);

	GApplicationInitializeFunction();

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

		GApplicationTickFunction();
	}

	GApplicationShutdownFunction();

	ShutdownSDL(window);

	ShutdownScheduler();
	ShutdownProfiler();
	ShutdownMainThread();
	return 0;
}

}