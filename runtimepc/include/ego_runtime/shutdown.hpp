#pragma once

#include <atomic>

namespace ego_runtime {

std::atomic<bool>& ShutdownRequested();

void InstallShutdownHandlers();

void WaitForShutdown();

const char* ShutdownReason();

}  // namespace ego_runtime
