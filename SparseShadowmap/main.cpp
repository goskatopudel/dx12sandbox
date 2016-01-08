#include "Essence.h"
#include "EssenceGfx.h"

#include <DirectXMath.h>
#include "Camera.h"

#include "Model.h"
#include "UIRendering.h"
#include "imgui/imgui.h"
#include "Shader.h"
#include "StatWindows.h"

#include "Hashmap.h"

#include "Scene.h"
#include "Random.h"

#include "SDL.h"

#pragma comment(lib,"SDL2main.lib")

using namespace Essence;

FirstPersonCamera FpsCamera;
ICameraControler *CameraControlerPtr = &FpsCamera;

Scene testScene;
resource_handle SceneColor;
resource_handle DepthBuffer;
resource_handle ShadowLOD;
resource_handle LowResSM;
resource_handle VirtualSM;
resource_handle PagesNeeded;

void CreateScreenResources() {
	if (IsValid(DepthBuffer)) {
		Delete(DepthBuffer);
		Delete(ShadowLOD);
		Delete(SceneColor);
	}

	auto x = GDisplaySettings.resolution.x;
	auto y = GDisplaySettings.resolution.y;

	SceneColor = CreateTexture(x, y, DXGI_FORMAT_R8G8B8A8_UNORM, ALLOW_RENDER_TARGET, "scene_color", float4(0.1f, 0.1f, 0.1f, 1.f));
	ShadowLOD = CreateTexture(x, y, DXGI_FORMAT_R8_UINT, ALLOW_RENDER_TARGET, "shadow_lod");
	DepthBuffer = CreateTexture(x, y, DXGI_FORMAT_R24G8_TYPELESS, ALLOW_DEPTH_STENCIL, "depth");
}

#include "Ringbuffer.h"
#include "d3dx12.h"

struct page_t {
	u32 index;
};

class PagePool {
public:
	struct page_heap_t {
		ID3D12Heap*	D12Heap;
	};

	u32					PagesPerHeap;
	static const u32	D12PageSize = 65536;

	Array<page_heap_t>	Heaps;
	Ringbuffer<page_t>	FreePages;

	PagePool() {
		PagesPerHeap = 128;
	}

	void FreeMemory() {
		for (auto heap : Heaps) {
			ComRelease(heap.D12Heap);
		}
		::FreeMemory(Heaps);
		::FreeMemory(FreePages);
	}

	~PagePool() {
		FreeMemory();
	}

	void AddPages() {
		page_heap_t heap = {};

		D3D12_HEAP_DESC heapDesc = {};
		heapDesc.Alignment = 0;
		heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
		heapDesc.SizeInBytes = PagesPerHeap * D12PageSize;
		heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

		VerifyHr(GD12Device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap.D12Heap)));

		u32 heapIndex = (u32)Size(Heaps);
		PushBack(Heaps, heap);

		for (auto i : u32Range(PagesPerHeap)) {
			page_t page;
			page.index = heapIndex * PagesPerHeap + i;
			PushBack(FreePages, page);
		}
	}

	void Allocate(Array<page_t>& pagesList, u32 num) {
		while (Size(pagesList) < num) {
			if (!Size(FreePages)) {
				AddPages();
			}
			PushBack(pagesList, Front(FreePages));
			PopFront(FreePages);
		}
	}

	void Free(Array<page_t>const& pagesList) {

	}
	
	ID3D12Heap* GetPageHeap(page_t page) {
		return Heaps[page.index / PagesPerHeap].D12Heap;
	}

	u32 GetPageHeapOffset(page_t page) {
		return page.index % PagesPerHeap;
	}
};

void MapMipTailAndDummyPage(resource_handle resource, PagePool* pagePool, GPUQueue* queue) {

	u32 numTiles;
	D3D12_PACKED_MIP_INFO packedMipDesc;
	D3D12_TILE_SHAPE tileShape;

	u32 numSubresTilings = GetResourceInfo(resource)->subresources_num;
	//
	Array<D3D12_SUBRESOURCE_TILING> subresTilings(GetThreadScratchAllocator());
	Resize(subresTilings, numSubresTilings);
	GD12Device->GetResourceTiling(GetResourceInfo(resource)->resource, &numTiles, &packedMipDesc, &tileShape, &numSubresTilings, 0, subresTilings.DataPtr);

	Array<page_t> pages(GetThreadScratchAllocator());
	pagePool->Allocate(pages, packedMipDesc.NumTilesForPackedMips);

	D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
	coordinate.Subresource = packedMipDesc.NumStandardMips;

	D3D12_TILE_REGION_SIZE region = {};
	region.UseBox = false;
	region.NumTiles = 1;
	region.Width = 1;
	D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;

	u32 heapOffset = pagePool->GetPageHeapOffset(pages[0]);
	u32 rangeCount = 1;

	// allocate mip tail
	GetD12Queue(queue)->UpdateTileMappings(
		GetResourceInfo(resource)->resource,
		1,
		&coordinate,
		&region,
		pagePool->GetPageHeap(pages[0]),
		1,
		&flag,
		&heapOffset,
		&rangeCount,
		D3D12_TILE_MAPPING_FLAG_NONE);

	// allocate rest with dummy page
	Clear(pages);
	pagePool->Allocate(pages, 1);

	heapOffset = pagePool->GetPageHeapOffset(pages[0]);
	rangeCount = 1;

	coordinate = {};
	coordinate.Subresource = 0;

	region.UseBox = false;
	region.NumTiles = packedMipDesc.NumStandardMips;

	flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;

	GetD12Queue(queue)->UpdateTileMappings(
		GetResourceInfo(resource)->resource,
		1,
		&coordinate,
		&region,
		pagePool->GetPageHeap(pages[0]),
		1,
		&flag,
		&heapOffset,
		&rangeCount,
		D3D12_TILE_MAPPING_FLAG_NONE);
}

PagePool Pages;

void Init() {
	SpawnEntity(testScene, GetModel(NAME_("models/sibenik.obj")));
	/*auto hairball = SpawnEntity(testScene, GetModel(NAME_("models/hairball.obj")));
	SetPosition(testScene, hairball, float3(50, 0, 0));*/
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));
	LowResSM = CreateTexture(128, 128, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL, "low_res_sm");
	VirtualSM = CreateTexture(16384, 16384, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL | TEX_MIPMAPPED | TEX_VIRTUAL, "virtual_sm");
	PagesNeeded = CreateTexture(16384 / 128, 16384 / 128, DXGI_FORMAT_R32_UINT, ALLOW_UNORDERED_ACCESS | TEX_MIPMAPPED, "vsm_pages");

	MapMipTailAndDummyPage(VirtualSM, &Pages, GGPUMainQueue);
}

void Tick(float fDeltaTime) {

	ImGuiIO& io = ImGui::GetIO();
	static float rx = 0;
	static float ry = 0;
	if (!io.WantCaptureMouse && !io.WantCaptureKeyboard)
	{
		float activeSpeed = 10 * fDeltaTime;

		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT]) {
			activeSpeed *= 10.f;
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LCTRL]) {
			activeSpeed /= 5.f;
		}

		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_W]) {
			CameraControlerPtr->onForward(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_S]) {
			CameraControlerPtr->onBackward(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]) {
			CameraControlerPtr->onRight(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]) {
			CameraControlerPtr->onLeft(activeSpeed);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_Q]) {
			CameraControlerPtr->onRollLeft(fDeltaTime * DirectX::XM_PI);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_E]) {
			CameraControlerPtr->onRollRight(fDeltaTime * DirectX::XM_PI);
		}
		if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE]) {
			CameraControlerPtr->onUp(activeSpeed);
		}
		int xrel = 0;
		int yrel = 0;
		auto mouseState = SDL_GetRelativeMouseState(&xrel, &yrel);
		if (mouseState & SDL_BUTTON(3)) {
			float dx = (float)xrel / (float)GDisplaySettings.resolution.x;
			float dy = (float)yrel / (float)GDisplaySettings.resolution.y;

			CameraControlerPtr->onMouseMove(dy, dx);
		}

		if (mouseState & SDL_BUTTON(1)) {
			float dx = (float)xrel / 500.0f * (float)M_PI;
			float dy = (float)yrel / 500.0f * (float)M_PI;
			ry += -dx;
			rx += dy;
		}
	}

	PROFILE_BEGIN(ui_logic);

	ImGui::Text("Hello, world!");
	ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
	if (ImGui::Button("Recompile shaders")) {
		ReloadShaders();
		UpdatePipelineStates();
		ClearWarnings(TYPE_ID("ShaderBindings"));
	}

	ImGui::ShowTestWindow();

	ShowMemoryInfo();

	PROFILE_END; // ui logic

	using namespace Essence;

	auto depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));
	using namespace DirectX;

	ClearRenderTarget(depthCL, GetRTV(ShadowLOD));
	ClearRenderTarget(depthCL, GetRTV(SceneColor), float4(0.1f, 0.1f, 0.1f, 1.f));
	ClearDepthStencil(depthCL, GetDSV(DepthBuffer));
	ClearDepthStencil(depthCL, GetDSV(LowResSM));
	ClearUnorderedAccess(depthCL, GetUAV(PagesNeeded));

	xmvec lightDirection = XMVector3Normalize(XMVectorSet(1,1,1,0));
	xmmatrix shadowmapMatrix;

	auto viewMatrix = CameraControlerPtr->GetViewMatrix();
	auto projMatrix = XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f);

	auto viewProjMatrix = XMMatrixTranspose(viewMatrix * projMatrix);
	viewMatrix = XMMatrixTranspose(viewMatrix);
	projMatrix = XMMatrixTranspose(projMatrix);

	shadowmapMatrix = XMMatrixLookAtLH(lightDirection * 200, XMVectorZero(), XMVectorSet(0,1,0,1)) * XMMatrixOrthographicLH(64, 64, 1.f, 400);
	shadowmapMatrix = XMMatrixTranspose(shadowmapMatrix);

	for (auto subres : MakeRange(GetResourceInfo(VirtualSM)->subresources_num)) {
		ClearDepthStencil(depthCL, GetDSV(VirtualSM, subres));
	}

	for (auto entity : testScene.Entities) {

		auto worldMatrix = XMMatrixTranspose(
			XMMatrixAffineTransformation(
				XMLoadFloat3((XMFLOAT3*)&entity.scale),
				XMVectorZero(),
				XMLoadFloat4((XMFLOAT4*)&entity.qrotation),
				XMLoadFloat3((XMFLOAT3*)&entity.position)
				));

		auto renderData = GetModelRenderData(entity.model);

		for (auto i : MakeRange(renderData->submeshes.num)) {
			SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, PShader, PS_5_1), renderData->vertex_layout);
			SetRenderTarget(depthCL, 0, GetRTV(SceneColor));
			SetRenderTarget(depthCL, 1, GetRTV(ShadowLOD));
			SetDepthStencil(depthCL, GetDSV(DepthBuffer));
			SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), viewProjMatrix);
			SetConstant(depthCL, TEXT_("DirectionalLightMatrix"), shadowmapMatrix);

			//SetRWTexture2D(depthCL, TEXT_("PagesTexture"), Slice(PagesNeeded));

			buffer_location_t vb;
			vb.address = GetResourceFast(renderData->vertex_buffer)->resource->GetGPUVirtualAddress();
			vb.size = renderData->vertices_num * sizeof(mesh_vertex_t);
			vb.stride = sizeof(mesh_vertex_t);

			SetVertexStream(depthCL, 0, vb);

			buffer_location_t ib;
			ib.address = GetResourceFast(renderData->index_buffer)->resource->GetGPUVirtualAddress();
			ib.size = renderData->indices_num * sizeof(u32);
			ib.stride = sizeof(u32);

			SetIndexBuffer(depthCL, ib);

			auto submesh = renderData->submeshes[i];
			DrawIndexed(depthCL, submesh.index_count, submesh.start_index, submesh.base_vertex);

			//

			SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), {}, renderData->vertex_layout);
			SetRenderTarget(depthCL, 0, {});
			SetRenderTarget(depthCL, 1, {});
			SetViewport(depthCL, (float)128, (float)128);

			SetDepthStencil(depthCL, GetDSV(LowResSM));

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), shadowmapMatrix);

			DrawIndexed(depthCL, submesh.index_count, submesh.start_index, submesh.base_vertex);
		}
	}

	float3 cameraPos;
	XMStoreFloat3(&cameraPos, CameraControlerPtr->Position);
	SetComputeShaderState(depthCL, SHADER_(VirtualSM, PreparePages, CS_5_0));
	SetTexture2D(depthCL, TEXT_("DepthBuffer"), GetSRV(DepthBuffer));
	SetTexture2D(depthCL, TEXT_("ShadowLevel"), GetSRV(ShadowLOD));
	SetRWTexture2D(depthCL, TEXT_("PagesTexture"), GetUAV(PagesNeeded));
	SetConstant(depthCL, TEXT_("ViewMatrix"), viewMatrix);
	SetConstant(depthCL, TEXT_("ProjectionMatrix"), projMatrix);
	SetConstant(depthCL, TEXT_("ShadowmapMatrix"), shadowmapMatrix);
	SetConstant(depthCL, TEXT_("CameraPos"), cameraPos);
	SetConstant(depthCL, TEXT_("Resolution"), float2((float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y));
	Dispatch(depthCL, (GDisplaySettings.resolution.x + 7) / 8, (GDisplaySettings.resolution.y + 7) / 8, 1);

	u32 subresource = 0;
	for (u32 size = 128; size > 1; size >>= 1) {
		u32 target = size / 2;
		SetComputeShaderState(depthCL, SHADER_(Mipmap, BuildMinMip, CS_5_1));

		SetTexture2D(depthCL, TEXT_("LowerLevel"), GetSRV(PagesNeeded, subresource));
		SetRWTexture2D(depthCL, TEXT_("CurrentLevel"), GetUAV(PagesNeeded, subresource+1));
		Dispatch(depthCL, (target + 7) / 8, (target + 7) / 8, 1);
		subresource++;

		/*Execute(depthCL);
		depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));*/
	}

	SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(SceneColor));
	Draw(depthCL, 3);

	//Execute(depthCL);
	//depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));

	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 128.f, 128.f, 128.f + 1.f + 10.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 64.f, 64.f, 128.f + 1.f + 10.f + 1.f + 128.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 1));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 32.f, 32.f, 128.f + 1.f + 10.f + 1.f + 128.f + 64.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 2));
	Draw(depthCL, 3);
	SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, 16.f, 16.f, 128.f + 1.f + 10.f + 1.f + 128.f + 64.f + 32.f, 1.f);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, 3));
	Draw(depthCL, 3);

	SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, LinearizeDepthPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, (float)128, (float)128, 1, 1);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(LowResSM));
	SetConstant(depthCL, TEXT_("Projection_33"), XMVectorGetZ(shadowmapMatrix.r[2]));
	SetConstant(depthCL, TEXT_("Projection_43"), XMVectorGetW(shadowmapMatrix.r[2]));
	Draw(depthCL, 3);

	Execute(depthCL);

	auto mainCL = GetCommandList(GGPUMainQueue, NAME_("main_cl"));
	RenderUserInterface(mainCL);
	Execute(mainCL);

	Present(1);
}

void Shutdown() {
	WaitForCompletion();

	call_destructor(testScene);

	Pages.FreeMemory();
}

int main(int argc, char * argv[]) {
	using namespace Essence;

	GApplicationInitializeFunction = Init;
	GApplicationTickFunction = Tick;
	GApplicationShutdownFunction = Shutdown;

	GApplicationWindowResizeFunction = []() {
		CreateScreenResources();
	};

	InitApplication(1200, 768, 1, APP_D3D12_DEBUG);

	return RunApplicationMainLoop();
}
