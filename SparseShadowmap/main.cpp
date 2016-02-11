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
#include "Random.h"
#include "SDL.h"
#include "Scene.h"

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
resource_handle PagesNeededPrev[2];
u32				PagesPrevIndex;
resource_handle PagesCPU[2];
GPUFenceHandle	PagesCPUReady[2];
u32				PagesWriteIndex;
u32				PagesReadIndex;

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
	struct pending_fence_t {
		GPUFenceHandle	fence;
		u32				pages_num;
	};

	u32							PagesPerHeap;
	static const u32			D12PageSize = 65536;

	Array<page_heap_t>			Heaps;
	Ringbuffer<page_t>			FreePages;
	Ringbuffer<page_t>			PendingPages;
	Ringbuffer<pending_fence_t>	PendingFences;

	PagePool() {
		PagesPerHeap = 128;
	}

	void FreeMemory() {
		for (auto heap : Heaps) {
			ComRelease(heap.D12Heap);
		}
		::FreeMemory(Heaps);
		::FreeMemory(FreePages);
		::FreeMemory(PendingPages);
		::FreeMemory(PendingFences);
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

	void Free(Array<page_t>const& pagesList, GPUFenceHandle fence) {
		if (Size(pagesList) == 0) {
			return;
		}
		pending_fence_t pendingFence = {};
		pendingFence.fence = fence;
		pendingFence.pages_num = (u32)Size(pagesList);
		PushBack(PendingFences, pendingFence);
		for (auto i : MakeRange(Size(pagesList))) {
			PushBack(PendingPages, pagesList[i]);
		}
	}

	void RecyclePages() {
		while (Size(PendingFences)) {
			if (IsFenceCompleted(Front(PendingFences).fence)) {
				for (auto i : MakeRange(Front(PendingFences).pages_num)) {
					PushBack(FreePages, Front(PendingPages));
					PopFront(PendingPages);
				}
				PopFront(PendingFences);
			}
			else {
				break;
			}
		}
	}

	ID3D12Heap* GetPageHeap(page_t page) {
		return Heaps[page.index / PagesPerHeap].D12Heap;
	}

	u32 GetPageHeapOffset(page_t page) {
		return page.index % PagesPerHeap;
	}
};

struct tile_mapping_t {
	u16		x;
	u16		y;
	u8		level;
};

struct VirtualShadowmapState {
	Hashmap<tile_mapping_t, page_t> mappedPages;
	page_t							dummyPage;
};

struct VirtualSMInfo {
	u32 pagesMapped;
	u32 perMipPages[16];
	u32 mipTailStart;
} GVirtualSMInfo;

void MapMipTailAndDummyPage(resource_handle resource, PagePool* pagePool, GPUQueue* queue, VirtualShadowmapState* State) {

	u32 numTiles;
	D3D12_PACKED_MIP_INFO packedMipDesc;
	D3D12_TILE_SHAPE tileShape;

	u32 numSubresTilings = GetResourceInfo(resource)->subresources_num;
	//
	Array<D3D12_SUBRESOURCE_TILING> subresTilings(GetThreadScratchAllocator());
	Resize(subresTilings, numSubresTilings);
	GD12Device->GetResourceTiling(GetResourceInfo(resource)->resource, &numTiles, &packedMipDesc, &tileShape, &numSubresTilings, 0, subresTilings.DataPtr);

	GVirtualSMInfo.mipTailStart = packedMipDesc.NumStandardMips;

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
	u32 rangeTiles = 1;

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
		&rangeTiles,
		D3D12_TILE_MAPPING_FLAG_NO_HAZARD);

	// allocate rest with dummy page
	Clear(pages);
	pagePool->Allocate(pages, 1);

	page_t dummyPage = pages[0];
	State->dummyPage = dummyPage;

	heapOffset = pagePool->GetPageHeapOffset(dummyPage);

	for (u32 subres : MakeRange(packedMipDesc.NumStandardMips)) {
		D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
		coordinate.Subresource = subres;

		D3D12_TILE_REGION_SIZE region = {};
		region.UseBox = true;
		region.Width = subresTilings[subres].WidthInTiles;
		region.Height = subresTilings[subres].HeightInTiles;
		region.Depth = 1;
		D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;

		rangeTiles = subresTilings[subres].WidthInTiles * subresTilings[subres].HeightInTiles;

		GetD12Queue(queue)->UpdateTileMappings(
			GetResourceInfo(resource)->resource,
			1,
			&coordinate,
			&region,
			pagePool->GetPageHeap(dummyPage),
			1,
			&flag,
			&heapOffset,
			&rangeTiles,
			D3D12_TILE_MAPPING_FLAG_NO_HAZARD);
	}
}

PagePool Pages;
VirtualShadowmapState State;

void Init() {
	auto sponza = SpawnEntity(testScene, GetModel(NAME_("models/sponza.obj")));
	SetScale(testScene, sponza, 0.01f);
	/*auto hairball = SpawnEntity(testScene, GetModel(NAME_("models/hairball.obj")));
	SetPosition(testScene, hairball, float3(50, 0, 0));*/
	CreateScreenResources();
	FpsCamera.setup(float3(0, 0, -50), float3(0, 0, 1));
	LowResSM = CreateTexture(128, 128, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL, "low_res_sm");
	VirtualSM = CreateTexture(16384, 16384, DXGI_FORMAT_R32_TYPELESS, ALLOW_DEPTH_STENCIL | TEX_MIPMAPPED | TEX_VIRTUAL, "virtual_sm");
	PagesNeeded = CreateTexture(16384 / 128, 16384 / 128, DXGI_FORMAT_R32_UINT, ALLOW_UNORDERED_ACCESS | TEX_MIPMAPPED, "vsm_pages");
	for (u32 i = 0; i < _countof(PagesNeededPrev); ++i) {
		PagesNeededPrev[i] = CreateTexture(16384 / 128, 16384 / 128, DXGI_FORMAT_R32_UINT, TEX_MIPMAPPED, "vsm_pages_prev");
	}

	for (auto i : MakeRange(_countof(PagesCPU))) {
		PagesCPU[i] = CreateReadbackBufferForResource(PagesNeeded);
	}

	MapMipTailAndDummyPage(VirtualSM, &Pages, GGPUMainQueue, &State);
}

u32 MapTiles(resource_handle virtualShadowmap, PagePool* pagePool, GPUQueue* queue, VirtualShadowmapState *MappingState) {
	if(PagesReadIndex == PagesWriteIndex)
	{	PROFILE_SCOPE(wait_for_read);
		WaitForCompletion(PagesCPUReady[PagesReadIndex]);
	}

	u32 numTiles;
	D3D12_PACKED_MIP_INFO packedMipDesc;
	D3D12_TILE_SHAPE tileShape;

	u32 numSubres = GetResourceInfo(virtualShadowmap)->subresources_num;
	//
	Array<D3D12_SUBRESOURCE_TILING> subresTilings(GetThreadScratchAllocator());
	Resize(subresTilings, numSubres);
	GD12Device->GetResourceTiling(GetResourceInfo(virtualShadowmap)->resource, &numTiles, &packedMipDesc, &tileShape, &numSubres, 0, subresTilings.DataPtr);

	u8 mipTailStart = packedMipDesc.NumStandardMips;

	Array<subresource_read_info_t> subres(GetThreadScratchAllocator());
	{	PROFILE_SCOPE(wait_for_map);
		MapReadbackBuffer(PagesCPU[PagesReadIndex], PagesNeeded, &subres);
	}

	u8 quadTreeDepth = (u8)Size(subres);

	struct page_quadtree_node_t {
		u16		x;
		u16		y;
		u8		level;
	};

	auto get_mapping = [](page_quadtree_node_t mapping, subresource_read_info_t*__restrict Subresources, u32 depth) {
		u32 level = depth - 1 - mapping.level;
		auto location = (u32*)pointer_add(Subresources[level].data, Subresources[level].row_pitch * mapping.y + mapping.x * sizeof(u32));
		return *location;
	};

	page_quadtree_node_t root;
	root.level = 0;
	root.x = 0;
	root.y = 0;

	// old set, new set
	// deprecated = old - intersection(old, new)
	auto DeprecatedMappings = Copy(MappingState->mappedPages, GetThreadScratchAllocator());
	Array<tile_mapping_t> Requests(GetThreadScratchAllocator());
	/*
		for (u32 l = 0; l < Size(subres); ++l) {
			u32 level = l;
			for (u32 y = 0; y < subres[level].height; ++y) {
				for (u32 x = 0; x < subres[level].width; ++x) {
					u32 val = *(u32*)pointer_add(subres[level].data, subres[level].row_pitch * y + x * sizeof(u32));
					u32 minNeededSubresource = numSubres - 1 - min(val, numSubres - 1);
					if (val && l >= minNeededSubresource) {
						tile_mapping_t mapping;
						mapping.level = l;
						mapping.x = x;
						mapping.y = y;

						auto pMapping = Get(DeprecatedMappings, mapping);
						if (pMapping) {
							Remove(DeprecatedMappings, mapping);
						}
						else {
							PushBack(Requests, mapping);
						}
					}
				}
			}
		}*/

	PROFILE_BEGIN(quadtree_traversal);

	Ringbuffer<page_quadtree_node_t> Queue(GetThreadScratchAllocator());
	PushBack(Queue, root);
	while (Size(Queue)) {
		page_quadtree_node_t front = Front(Queue);
		PopFront(Queue);

		u32 subresource = quadTreeDepth - front.level - 1;
		if (subresource < packedMipDesc.NumStandardMips) {
			tile_mapping_t mapping;
			mapping.level = subresource;
			mapping.x = front.x;
			mapping.y = front.y;

			Check(mapping.x < 128);
			Check(mapping.y < 128);
			Check(mapping.level < packedMipDesc.NumStandardMips);

			auto pMapping = Get(DeprecatedMappings, mapping);
			if (pMapping) {
				Remove(DeprecatedMappings, mapping);
			}
			else {
				PushBack(Requests, mapping);
			}
		}

		page_quadtree_node_t child;
		child.level = front.level + 1;
		u32 childSubresource = subresource - 1;

		for (int i = 0; i < 4; ++i) {
			child.x = front.x * 2 + i % 2;
			child.y = front.y * 2 + i / 2;

			u32 mapping = child.level < quadTreeDepth ? get_mapping(child, subres.DataPtr, quadTreeDepth) : 0;
			// reverse range
			u32 minNeededSubresource = numSubres - 1 - min(mapping, numSubres - 1);
			if (mapping && childSubresource >= minNeededSubresource) {
				if (child.level < quadTreeDepth) {
					PushBack(Queue, child);
				}
			}
		}
	}

	PROFILE_END;

	UnmapReadbackBuffer(PagesCPU[PagesReadIndex]);

	GVirtualSMInfo.pagesMapped -= (u32)Size(DeprecatedMappings);
	Array<page_t> PagesList(GetThreadScratchAllocator());

	{	PROFILE_SCOPE(tiles_unmapping);

		Reserve(PagesList, Size(DeprecatedMappings));
		for (auto kv : DeprecatedMappings) {
			D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
			coordinate.Subresource = kv.key.level;
			coordinate.X = kv.key.x;
			coordinate.Y = kv.key.y;
			coordinate.Z = 0;

			GVirtualSMInfo.perMipPages[kv.key.level]--;

			D3D12_TILE_REGION_SIZE region = {};
			region.UseBox = true;
			region.NumTiles = 1;
			region.Width = 1;
			region.Height = 1;
			region.Depth = 1;
			D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;

			u32 heapOffset = pagePool->GetPageHeapOffset(MappingState->dummyPage);
			u32 rangeTiles = 1;

			// allocate mip tail
			GetD12Queue(queue)->UpdateTileMappings(
				GetResourceInfo(virtualShadowmap)->resource,
				1,
				&coordinate,
				&region,
				pagePool->GetPageHeap(MappingState->dummyPage),
				1,
				&flag,
				&heapOffset,
				&rangeTiles,
				D3D12_TILE_MAPPING_FLAG_NONE);

			PushBack(PagesList, kv.value);
			Remove(MappingState->mappedPages, kv.key);
		}
		pagePool->Free(PagesList, GetLastSignaledFence(queue));
	}

	{	PROFILE_SCOPE(pages_recycling);
		pagePool->RecyclePages();
	}

	Clear(PagesList);
	Reserve(PagesList, Size(Requests));

	{	PROFILE_SCOPE(tiles_mapping);
		pagePool->Allocate(PagesList, (u32)Size(Requests));

		GVirtualSMInfo.pagesMapped += (u32)Size(Requests);

		u32 reqIndex = 0;
		for (auto req : Requests) {
			D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
			coordinate.Subresource = req.level;
			coordinate.X = req.x;
			coordinate.Y = req.y;
			coordinate.Z = 0;

			GVirtualSMInfo.perMipPages[req.level]++;

			D3D12_TILE_REGION_SIZE region = {};
			region.UseBox = true;
			region.NumTiles = 1;
			region.Width = 1;
			region.Height = 1;
			region.Depth = 1;
			D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;

			u32 heapOffset = pagePool->GetPageHeapOffset(PagesList[reqIndex]);
			u32 rangeTiles = 1;

			GetD12Queue(queue)->UpdateTileMappings(
				GetResourceInfo(virtualShadowmap)->resource,
				1,
				&coordinate,
				&region,
				pagePool->GetPageHeap(PagesList[reqIndex]),
				1,
				&flag,
				&heapOffset,
				&rangeTiles,
				D3D12_TILE_MAPPING_FLAG_NONE);

			Set(MappingState->mappedPages, req, PagesList[reqIndex]);

			reqIndex++;
		}
	}

	return (u32)Size(MappingState->mappedPages);
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

	xmvec lightDirection = XMVector3Normalize(XMVectorSet(1, 2, 1, 0));
	xmmatrix shadowmapMatrix;

	auto viewMatrix = CameraControlerPtr->GetViewMatrix();
	auto projMatrix = XMMatrixPerspectiveFovLH(3.14f * 0.25f, (float)GDisplaySettings.resolution.x / (float)GDisplaySettings.resolution.y, 0.01f, 1000.f);

	auto viewProjMatrix = XMMatrixTranspose(viewMatrix * projMatrix);
	viewMatrix = XMMatrixTranspose(viewMatrix);
	projMatrix = XMMatrixTranspose(projMatrix);

	shadowmapMatrix = XMMatrixLookAtLH(lightDirection * 200, XMVectorZero(), XMVectorSet(0, 1, 0, 1)) * XMMatrixOrthographicLH(64, 64, 1.f, 400);
	shadowmapMatrix = XMMatrixTranspose(shadowmapMatrix);

	for (auto i : MakeRange(GetResourceInfo(VirtualSM)->subresources_num)) {
		ClearDepthStencil(depthCL, GetDSV(VirtualSM, i), CLEAR_DEPTH);
	}

	PROFILE_BEGIN(prepass);
	GPU_PROFILE_BEGIN(depthCL, prepass);

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
			SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), SHADER_(Model, ShadowLodPShader, PS_5_1), renderData->vertex_layout);
			SetRenderTarget(depthCL, 0, GetRTV(SceneColor));
			SetRenderTarget(depthCL, 1, GetRTV(ShadowLOD));
			SetDepthStencil(depthCL, GetDSV(DepthBuffer));
			SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), viewProjMatrix);
			SetConstant(depthCL, TEXT_("DirectionalLightMatrix"), shadowmapMatrix);

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

			SetRenderTarget(depthCL, 0, {});
			SetRenderTarget(depthCL, 1, {});

			u32 viewportSize = (u32)GetResourceInfo(VirtualSM)->width;
			for (auto i : MakeRange(15u)) {
				SetShaderState(depthCL, SHADER_(Model, VShader, VS_5_1), {}, renderData->vertex_layout);
				SetViewport(depthCL, (float)(viewportSize >> i), (float)(viewportSize >> i));

				SetDepthStencil(depthCL, GetDSV(VirtualSM, i));

				SetConstant(depthCL, TEXT_("World"), worldMatrix);
				SetConstant(depthCL, TEXT_("ViewProj"), shadowmapMatrix);

				DrawIndexed(depthCL, submesh.index_count, submesh.start_index, submesh.base_vertex);
			}
		}
	}

	GPU_PROFILE_END(depthCL);
	PROFILE_END;

	GPU_PROFILE_BEGIN(depthCL, tile_texture);
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
		SetRWTexture2D(depthCL, TEXT_("CurrentLevel"), GetUAV(PagesNeeded, subresource + 1));
		Dispatch(depthCL, (target + 7) / 8, (target + 7) / 8, 1);
		subresource++;
	}
	GPU_PROFILE_END(depthCL);

	{	GPU_PROFILE_SCOPE(depthCL, copying_to_readback);
		CopyToReadbackBuffer(depthCL, PagesCPU[PagesWriteIndex], PagesNeeded);
	}
	PagesCPUReady[PagesWriteIndex] = GetCompletionFence(depthCL);
	PagesWriteIndex = (PagesWriteIndex + 1) % _countof(PagesCPU);

	// flushing commands 
	Execute(depthCL);
	depthCL = GetCommandList(GGPUMainQueue, NAME_("depth_cl"));

	{	PROFILE_SCOPE(map_tiles);
		MapTiles(VirtualSM, &Pages, GGPUMainQueue, &State);
	}
	ImGui::Text("%u", GVirtualSMInfo.pagesMapped);
	for (auto i : MakeRange(GVirtualSMInfo.mipTailStart)) {
		ImGui::Text("%u: %u", i, GVirtualSMInfo.perMipPages[i]);
	}

/*
	SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, CopyPS, PS_5_1), {});
	SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
	SetDepthStencil(depthCL, {});
	SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
	SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetTexture2D(depthCL, TEXT_("Image"), GetSRV(SceneColor));
	Draw(depthCL, 3);*/

	ClearRenderTarget(depthCL, GetRTV(GetCurrentBackbuffer()));
	ClearDepthStencil(depthCL, GetDSV(DepthBuffer));

	PROFILE_BEGIN(main_pass);
	GPU_PROFILE_BEGIN(depthCL, main_pass);

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
			SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
			SetDepthStencil(depthCL, GetDSV(DepthBuffer));
			SetViewport(depthCL, (float)GDisplaySettings.resolution.x, (float)GDisplaySettings.resolution.y);
			SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			SetConstant(depthCL, TEXT_("World"), worldMatrix);
			SetConstant(depthCL, TEXT_("ViewProj"), viewProjMatrix);
			SetConstant(depthCL, TEXT_("DirectionalLightMatrix"), shadowmapMatrix);
			SetTexture2D(depthCL, TEXT_("Shadowmap"), GetSRV(VirtualSM));
			float3 f3LightDirection = toFloat3(lightDirection);
			SetConstant(depthCL, TEXT_("LightDirection"), f3LightDirection);
			SetTexture2D(depthCL, TEXT_("ShadowMipLookup"), GetSRV(PagesNeededPrev[PagesPrevIndex]));
			SetTexture2D(depthCL, TEXT_("ShadowMipLookupPrev"), GetSRV(PagesNeededPrev[(PagesPrevIndex + 1) % _countof(PagesNeededPrev)]));

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
		}
	}

	GPU_PROFILE_END(depthCL);
	PROFILE_END;

	{	GPU_PROFILE_SCOPE(depthCL, copy_pages);
		PagesPrevIndex = (PagesPrevIndex + 1) % _countof(PagesNeededPrev);
		CopyResource(depthCL, PagesNeededPrev[PagesPrevIndex], PagesNeeded);
	}

	u32 N = GetResourceInfo(PagesNeeded)->subresources_num;
	u32 width = (u32)GetResourceInfo(PagesNeeded)->width;
	u32 height = (u32)GetResourceInfo(PagesNeeded)->height;
	float rowHeight = (float)height;
	float columnWidth = (float)width;

	for (auto column : MakeRange(N)) {
		SetShaderState(depthCL, SHADER_(Vsm, VShader, VS_5_1), SHADER_(Vsm, CopyUintPS, PS_5_1), {});
		SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
		SetDepthStencil(depthCL, {});
		SetViewport(depthCL, (float)128, (float)128, (columnWidth + 1.f) * column, 1.f);
		SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		SetTexture2D(depthCL, TEXT_("Image"), GetSRV(PagesNeeded, column));
		Draw(depthCL, 3);

		SetShaderState(depthCL, SHADER_(Utility, VShader, VS_5_1), SHADER_(Utility, LinearizeDepthPS, PS_5_1), {});
		SetRenderTarget(depthCL, 0, GetRTV(GetCurrentBackbuffer()));
		SetDepthStencil(depthCL, {});
		SetViewport(depthCL, (float)128, (float)128, (columnWidth + 1.f) * column, rowHeight + 1.f);
		SetTopology(depthCL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		SetTexture2D(depthCL, TEXT_("Image"), GetSRV(VirtualSM, column));
		SetConstant(depthCL, TEXT_("Projection_33"), XMVectorGetZ(shadowmapMatrix.r[2]));
		SetConstant(depthCL, TEXT_("Projection_43"), XMVectorGetW(shadowmapMatrix.r[2]));
		Draw(depthCL, 3);

		width >>= (u32) (width > 1);
		height >>= (u32) (height > 1);
	}

	Execute(depthCL);

	auto mainCL = GetCommandList(GGPUMainQueue, NAME_("main_cl"));
	RenderUserInterface(mainCL);
	Execute(mainCL);

	PROFILE_SCOPE(present);

	Present();
}

void Shutdown() {
	WaitForCompletion();

	call_destructor(testScene);
	FreeMemory(State.mappedPages);

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

	InitApplication(1200, 768, APP_FLAG_D3D12_DEBUG, APP_PRESENT_LOWLATENCY);

	return RunApplicationMainLoop();

}
