#include "Camera.h"
using namespace DirectX;

namespace Essence {

xmmatrix ICameraControler::GetViewMatrix() {
	return DirectX::XMMatrixLookToLH(Position, Direction, Up);
}

void ICameraControler::onRight(float d) {
	auto right = DirectX::XMVector3Cross(Up, Direction);

	Position += right * d;
}

void ICameraControler::onLeft(float d) {
	auto right = DirectX::XMVector3Cross(Up, Direction);

	Position -= right * d;
}

void ICameraControler::onForward(float d) {
	Position += d * Direction;
}

void ICameraControler::onBackward(float d) {
	Position -= d * Direction;
}

void ICameraControler::onUp(float d) {
	Position += d * Up;
}

void ICameraControler::onMouseMove(float dx, float dy) {
	using namespace DirectX;

	auto right = DirectX::XMVector3Cross(Up, Direction);

	//around right (local x), up/down
	XMVECTOR pitch = XMQuaternionRotationAxis(right, dx);
	//aroud (0,1,0) (y), left/right
	XMVECTOR yaw = XMQuaternionRotationAxis(Up, dy);

	auto dir = XMVector3Rotate(Direction, XMQuaternionMultiply(pitch, yaw));
	right = XMVector3Rotate(right, yaw);
	auto up = XMVector3Cross(dir, right);

	Direction = dir;
	Up = up;
}

void ICameraControler::onRollLeft(float d) {
	Up = XMVector3Rotate(Up, XMQuaternionRotationAxis(Direction, d));
}

void ICameraControler::onRollRight(float d) {
	Up = XMVector3Rotate(Up, XMQuaternionRotationAxis(Direction, -d));
}

void ICameraControler::setup(float3 position, float3 dir, float3 up) {
	Position = toSimd(position);
	Direction = toSimd(dir);
	Up = toSimd(up);
}

void FirstPersonCamera::onRight(float d) {
	auto right = DirectX::XMVector3Cross(Up, Direction);

	Position += right * d;
}

void FirstPersonCamera::onLeft(float d) {
	auto right = DirectX::XMVector3Cross(Up, Direction);

	Position -= right * d;
}

void FirstPersonCamera::onForward(float d) {
	Position += d * Direction;
}

void FirstPersonCamera::onBackward(float d) {
	Position -= d * Direction;
}

void FirstPersonCamera::onUp(float d) {
	Position += d * Up;
}

void FirstPersonCamera::onMouseMove(float dx, float dy) {
	using namespace DirectX;

	auto right = DirectX::XMVector3Cross(Up, Direction);

	//around right (local x), up/down
	XMVECTOR pitch = XMQuaternionRotationAxis(right, dx);
	//aroud (0,1,0) (y), left/right
	XMVECTOR yaw = XMQuaternionRotationAxis(Up, dy);

	auto dir = XMVector3Rotate(Direction, XMQuaternionMultiply(pitch, yaw));
	right = XMVector3Rotate(right, yaw);
	auto up = XMVector3Cross(dir, right);

	Direction = dir;
	Up = up;
}

void FirstPersonCamera::onRollLeft(float d) {
	Up = XMVector3Rotate(Up, XMQuaternionRotationAxis(Direction, d));
}

void FirstPersonCamera::onRollRight(float d) {
	Up = XMVector3Rotate(Up, XMQuaternionRotationAxis(Direction, -d));
}

}