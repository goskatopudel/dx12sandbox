#pragma once
#include "Maths.h"

namespace Essence {

class ICameraControler {
public:
	xmvec Position;
	xmvec Up;
	xmvec Direction;

	xmmatrix GetViewMatrix();

	virtual void onRight(float d);
	virtual void onLeft(float d);
	virtual void onForward(float d);
	virtual void onBackward(float d);
	virtual void onUp(float d);
	virtual void onMouseMove(float dx, float dy);
	virtual void onRollLeft(float d);
	virtual void onRollRight(float d);

	void setup(float3 position, float3 dir, float3 up = float3(0, 1, 0));
};

class FirstPersonCamera : public ICameraControler {
public:
	void onRight(float d) override;

	void onLeft(float d) override;

	void onForward(float d) override;

	void onBackward(float d) override;

	void onUp(float d) override;

	void onMouseMove(float dx, float dy) override;

	void onRollLeft(float d) override;

	void onRollRight(float d) override;
};

}