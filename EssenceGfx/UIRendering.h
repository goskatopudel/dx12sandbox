#pragma once

#include "Commands.h"

struct ImDrawData;

namespace Essence {

GPUCommandList* RenderUserInterface(GPUQueue* queue);
void RenderImDrawLists(ImDrawData *draw_data);

}