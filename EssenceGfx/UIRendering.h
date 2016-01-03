#pragma once

#include "Commands.h"

struct ImDrawData;

namespace Essence {

void RenderUserInterface(GPUCommandList* commandList);
void RenderImDrawLists(ImDrawData *draw_data);

}