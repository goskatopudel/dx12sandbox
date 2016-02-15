#include "StatWindows.h"
#include "Commands.h"

#include "Device.h"
#include "imgui\imgui.h"
#include <Psapi.h>

using namespace Essence;

void ShowStatsWindow() {
	ImGui::Begin("Stats");

	auto stats = GetLastFrameStats();

	ImGui::BulletText("Command lists");
	ImGui::Indent();
	ImGui::Text("All / Patchup / Executions: %u / %u / %u", stats->command_lists_num, stats->patchup_command_lists_num, stats->executions_num);
	ImGui::Unindent();

	ImGui::Separator();

	ImGui::BulletText("Commands");
	ImGui::Indent();
	ImGui::Text("Graphics");
	ImGui::Text("PSO changes:\nRootSignature changes:\nRoot params set:\nDrawcalls:"); ImGui::SameLine();
	ImGui::Text("%u\n%u\n%u\n%u",
		stats->command_stats.graphic_pipeline_state_changes,
		stats->command_stats.graphic_root_signature_changes,
		stats->command_stats.graphic_root_params_set,
		stats->command_stats.draw_calls);

	ImGui::Text("Compute");
	ImGui::Text("PSO changes:\nRootSignature changes:\nRoot params set:\nDispatches:"); ImGui::SameLine();
	ImGui::Text("%u\n%u\n%u\n%u",
		stats->command_stats.compute_pipeline_state_changes,
		stats->command_stats.compute_root_signature_changes,
		stats->command_stats.compute_root_params_set,
		stats->command_stats.dispatches);

	ImGui::Text("Common");
	ImGui::Text("Constants: %llu Kb", Kilobytes(stats->command_stats.constants_bytes_uploaded));
	ImGui::Unindent();

	ImGui::End();
}

void ShowMemoryWindow() {
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