#pragma once

#include "Essence.h"

namespace Essence {

typedef i32 Keycode_t;

class GPUQueue;

extern GPUQueue*			GGPUMainQueue;
extern GPUQueue*			GGPUCopyQueue;

struct display_settings_t {
	const char*	window_title;
	uint2		resolution;

	u8			vsync;
	u8			max_gpu_buffered_frames;
	u8			backbuffers_num;
	bool		wait_to_vblank;

	HWND		hwnd;
};

enum ApplicationPresentProfileEnum {
	APP_PRESENT_THROUGHPUT,
	APP_PRESENT_UNTHROTTLED,
	APP_PRESENT_LOWLATENCY,
	APP_PRESENT_VERYLOWLATENCY,
	APP_PRESENT_DEFAULT = APP_PRESENT_THROUGHPUT
};

enum ApplicationFlagsEnum {
	APP_FLAG_NONE = 0,
	APP_FLAG_D3D12_DEBUG = 1
};

extern void(*GApplicationInitializeFunction)	();
extern void(*GApplicationTickFunction)			(float);
extern void(*GApplicationShutdownFunction)		();
extern void(*GApplicationKeyDownFunction)		(Keycode_t);
extern void(*GApplicationMouseWheelFunction)	(int);
extern void(*GApplicationFileDropFunction)		(const char*);
extern void(*GApplicationTextInputFunction)		(const wchar_t*);
extern void(*GApplicationWindowResizeFunction)	();

extern display_settings_t	GDisplaySettings;
extern char					GWorkingDir[1024];
extern size_t				GWorkingDirLength;

void						InitApplication(u32 windowWidth, u32 windowHeight, ApplicationFlagsEnum flags, ApplicationPresentProfileEnum profile);
i32							RunApplicationMainLoop();

}