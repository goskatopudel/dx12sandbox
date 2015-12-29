#pragma once

#include "Essence.h"

#include <SDL.h>
#include <SDL_syswm.h>

namespace Essence {

struct display_settings_t {
	const char*	window_title;
	uint2		resolution;
	bool		vsync;

	HWND		hwnd;
};

extern void(*GApplicationInitializeFunction)	()				;
extern void(*GApplicationTickFunction)			()				;
extern void(*GApplicationShutdownFunction)		()				;
extern void(*GApplicationKeyDownFunction)		(SDL_Keycode)	;
extern void(*GApplicationMouseWheelFunction)	(int)			;
extern void(*GApplicationFileDropFunction)		(const char*)	;
extern void(*GApplicationTextInputFunction)		(const wchar_t*);
extern void(*GApplicationWindowResizeFunction)	()				;

extern display_settings_t	GDisplaySettings;
extern char					GWorkingDir[1024];
extern size_t				GWorkingDirLength;

i32							ApplicationWinMain();

}