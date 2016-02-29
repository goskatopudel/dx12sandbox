// The following ifdef block is the standard way of creating macros which make exporting 
// from a DLL simpler. All files within this DLL are compiled with the SCRATCHPADRUNTIMEDLL_EXPORTS
// symbol defined on the command line. This symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see 
// SCRATCHPADRUNTIMEDLL_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.
#ifdef SCRATCHPADRUNTIMEDLL_EXPORTS
#define SCRATCHPADRUNTIMEDLL_API __declspec(dllexport)
#else
#define SCRATCHPADRUNTIMEDLL_API __declspec(dllimport)
#endif

#include "../Essence/VectorMath.h"

using DrawLine2DFunc = void(*)(Vec2f P0, Vec2f P1, Color4b C0, Color4b C1);
using DrawLine3DFunc = void(*)(Vec3f P0, Vec3f P1, Color4b C0, Color4b C1);

struct ScratchpadInterface {
	DrawLine2DFunc DrawLine2D;
	DrawLine3DFunc DrawLine3D;
};

using ScratchpadUpdateInterfaceFunc = void(*)(ScratchpadInterface);
using ScratchpadRuntimeCodeFunc = void(*)(Vec2f, Vec2f);

extern "C"
{
	SCRATCHPADRUNTIMEDLL_API void ScratchpadRuntimeCode(Vec2f screenres, Vec2f mousepos);
}

extern "C"
{
	SCRATCHPADRUNTIMEDLL_API void ScratchpadUpdateInterface(ScratchpadInterface);
}
