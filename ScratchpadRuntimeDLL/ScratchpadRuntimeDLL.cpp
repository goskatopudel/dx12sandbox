// ScratchpadRuntimeDLL.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "ScratchpadRuntimeDLL.h"
#include "MathGeometry.h"

DrawLine2DFunc DrawLine2D;

SCRATCHPADRUNTIMEDLL_API void ScratchpadUpdateInterface(ScratchpadInterface I) {
	DrawLine2D = I.DrawLine2D;
}

Vec2f Center;
Vec2f Scale = 1.f;

auto toScreenPos = [](Vec2f pos2D) {
	return pos2D * Vec2f(1.f, -1.f) + Center;
};

auto toPlanePos = [](Vec2f screen2D) {
	return (screen2D - Center) * Vec2f(1.f, -1.f);
};

auto markPoint2D = [](Vec2f point, Color4b color) {
	DrawLine2D(point - Vec2f(4, 4), point + Vec2f(4, 4), color, color);
	DrawLine2D(point - Vec2f(-4, 4), point + Vec2f(-4, 4), color, color);
};

auto drawLine = [](Vec3f eq, Color4b color) {
	Vec2f P0 = eq.xy * eq.z + Vec2f(eq.y, -eq.x) * 1000.f;
	Vec2f P1 = eq.xy * eq.z - Vec2f(eq.y, -eq.x) * 1000.f;

	DrawLine2D(toScreenPos(P0), toScreenPos(P1), color, color);
};

SCRATCHPADRUNTIMEDLL_API void ScratchpadRuntimeCode(Vec2f screenres, Vec2f mousepos) {
	Center = screenres * 0.5f;

	Color4b White = 255;
	Color4b Black(0);
	Color4b Red = { 255, 0, 0, 255 };
	Color4b Green = { 0, 255, 0, 255 };
	Color4b Blue = { 0, 0, 255, 255 };
	Color4b Yellow = { 255, 255, 0, 255 };
	Color4b Violet = { 255, 0, 255, 255 };

	Vec2f Point = { 200.f, 100.f };

	//DrawLine2D(toScreenPos(Vec2f(0.f)), mousepos, Red, Green);

	auto line = LineFromPoints2D(Point, toPlanePos(mousepos));
	line /= length(line.xy);

	//DrawLine2D(mousepos, toScreenPos(Point), Green, Green);

	//DrawLine2D(mousepos, mousepos + line.xy * 10.f, Red, Red);
	
	Vec2f P0 = line.xy * line.z + Vec2f(line.y, -line.x) * 1000.f;
	Vec2f P1 = line.xy * line.z - Vec2f(line.y, -line.x) * 1000.f;

	DrawLine2D(toScreenPos(P0), toScreenPos(P1), Blue, Blue);

	drawLine(Vec3f(1, 0, 0), Red);
	drawLine(Vec3f(0, 1, 0), Red);

	markPoint2D(toScreenPos(0.f), White);
	markPoint2D(toScreenPos({100, 100}), Violet);

	markPoint2D(toScreenPos(line.xy * line.z), Violet);
	markPoint2D(toScreenPos(Point), Yellow);

	auto R = Matrix2x2f::Rotation(3.14f * (mousepos.x / 100.f));
	auto T = Matrix2x3f::Translation(Vec2f(100, 10));

	auto P = R * T * Vec3f(0, 0, 1);
	markPoint2D(toScreenPos(P), Blue);

	/*Vec3f Line(0.f, 1.f, 1.f);
	Vec3f Line1(mousepos, 0.f);
	Line1.xy = normalize(Line1.xy);

	Vec3f cc = cross(Vec3f(0.f, 1.f, 2), Vec3f(1,0,-2) );*/

	/*DrawLine2D(Vec2f(0, 0), screenres, {255, 255, 0, 255}, 255);
	DrawLine2D(Vec2f(1, 100), screenres + Vec2f(1, 0), { 255, 1, 128, 255 }, 255);*/

	markPoint2D(mousepos, Red);
}