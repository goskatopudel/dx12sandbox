#include "StatWindows.h"

#include "Device.h"
#include "imgui\imgui.h"
#include <Psapi.h>

using namespace Essence;

void ShowMemoryInfo() {
	ImGui::Begin("Memory");

	auto localMemory = GetLocalMemoryInfo();
	auto nonLocalMemory = GetNonLocalMemoryInfo();

	ImGui::BulletText("Device memory");
	ImGui::Indent();
	ImGui::Text("Local memory");
	ImGui::Text("Budget:\nCurrent usage:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(localMemory.Budget), Megabytes(localMemory.CurrentUsage));
	ImGui::Text("Non-Local memory");
	ImGui::Text("Budget:\nCurrent usage:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(nonLocalMemory.Budget), Megabytes(nonLocalMemory.CurrentUsage));
	ImGui::Unindent();

	ImGui::Separator();

	ImGui::BulletText("Process memory");
	PROCESS_MEMORY_COUNTERS pmc;
	Verify(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)));
	ImGui::Indent();
	ImGui::Text("Working set:\nPagefile:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb", Megabytes(pmc.WorkingSetSize), Megabytes(pmc.PagefileUsage));
	ImGui::Unindent();

	ImGui::Separator();

	ImGui::BulletText("System memory");
	PERFORMANCE_INFORMATION perfInfo;
	Verify(GetPerformanceInfo(&perfInfo, sizeof(perfInfo)));
	ImGui::Indent();
	ImGui::Text("Commited total:\nPhysical total:\nPhysical available:"); ImGui::SameLine();
	ImGui::Text("%llu Mb\n%llu Mb\n%llu Mb"
		, Megabytes(perfInfo.CommitTotal * perfInfo.PageSize)
		, Megabytes(perfInfo.PhysicalTotal * perfInfo.PageSize)
		, Megabytes(perfInfo.PhysicalAvailable * perfInfo.PageSize));
	ImGui::Unindent();

	ImGui::End();
}