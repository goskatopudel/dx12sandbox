#include "UIRendering.h"

#include "Essence.h"
#include "imgui\imgui.h"
#include "Device.h"
#include "Resources.h"
#include "Application.h"

namespace Essence {

vertex_factory_handle UiVertex;

void LazyInit() {
	if (!IsValid(UiVertex)) {
		UiVertex = GetVertexFactory({ VertexInput::POSITION_2_32F, VertexInput::TEXCOORD_32F, VertexInput::COLOR_RGBA_8U });
	}
}

namespace ImGuiGlobals {
GPUCommandList* commandList;
};

void RenderUserInterface(GPUCommandList* commandList) {
	PROFILE_SCOPE(render_ui);
	LazyInit();
	ImGuiGlobals::commandList = commandList;
	ImGui::Render();
}

inline resource_handle TexIdToHandle(void* texID) {
	return *(resource_handle*)(&texID);
}

void RenderImDrawLists(ImDrawData *draw_data) {
	PROFILE_SCOPE(render_ui_record_cmds);

	auto cmdList = ImGuiGlobals::commandList;

	u32 vtxBytesize = 0;
	u32 idxBytesize = 0;
	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		vtxBytesize += cmd_list->VtxBuffer.size() * sizeof(ImDrawVert);
		idxBytesize += cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx);
	}

	auto vtxUpload = AllocateSmallUploadMemory(cmdList, vtxBytesize, 8);
	auto idxUpload = AllocateSmallUploadMemory(cmdList, idxBytesize, 8);
	auto vtxDst = (ImDrawVert*)vtxUpload.write_ptr;
	auto idxDst = (ImDrawIdx*)idxUpload.write_ptr;

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		memcpy(vtxDst, &cmd_list->VtxBuffer[0], cmd_list->VtxBuffer.size() * sizeof(ImDrawVert));
		memcpy(idxDst, &cmd_list->IdxBuffer[0], cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx));
		vtxDst += cmd_list->VtxBuffer.size();
		idxDst += cmd_list->IdxBuffer.size();
	}

	buffer_location_t vb;
	vb.address = vtxUpload.virtual_address;
	vb.size = vtxBytesize;
	vb.stride = sizeof(ImDrawVert);
	SetVertexStream(cmdList, 0, vb);

	buffer_location_t ib;
	ib.address = idxUpload.virtual_address;
	ib.size = idxBytesize;
	ib.stride = sizeof(u16);
	SetIndexBuffer(cmdList, ib);

	using namespace DirectX;

	auto matrix = XMMatrixTranspose(
		XMMatrixOrthographicOffCenterLH(
			0, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y, 0, 0, 1));

	//
	u32 vtxOffset = 0;
	i32 idxOffset = 0;

	D3D12_RASTERIZER_DESC rasterizerMode;
	GetD3D12StateDefaults(&rasterizerMode);
	rasterizerMode.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerMode.DepthClipEnable = true;
	SetRasterizer(cmdList, rasterizerMode);

	D3D12_RENDER_TARGET_BLEND_DESC blendMode = {};
	blendMode.BlendEnable = true;
	blendMode.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendMode.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendMode.BlendOp = D3D12_BLEND_OP_ADD;
	blendMode.SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blendMode.DestBlendAlpha = D3D12_BLEND_ZERO;
	blendMode.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendMode.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	SetBlendState(cmdList, 0, blendMode);

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.size(); cmd_i++) {
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

			SetShaderState(cmdList, SHADER_(Ui, VShader, VS_5_0), SHADER_(Ui, PShader, PS_5_0), UiVertex);

			SetViewport(cmdList, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(cmdList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			SetRenderTarget(cmdList, 0, Slice(GetCurrentBackbuffer()));

			if (pcmd->UserCallback) {
				pcmd->UserCallback(cmd_list, pcmd);
			}
			else {
				D3D12_RECT scissor = { (LONG)pcmd->ClipRect.x, (LONG)pcmd->ClipRect.y, (LONG)pcmd->ClipRect.z, (LONG)pcmd->ClipRect.w };

				if (pcmd->TextureId) {
					SetTexture2D(cmdList, TEXT_("Image"), Slice(TexIdToHandle(pcmd->TextureId)));
				}

				SetConstant(cmdList, TEXT_("Projection"), matrix);
				SetScissorRect(cmdList, scissor);

				DrawIndexed(cmdList, pcmd->ElemCount, idxOffset, vtxOffset);
			}
			idxOffset += pcmd->ElemCount;
		}
		vtxOffset += cmd_list->VtxBuffer.size();
	}
}

}