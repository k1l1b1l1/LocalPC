#include "ego_runtime/shutdown.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace ego_runtime {
namespace {

std::atomic<bool> g_shutdown{false};
std::atomic<const char*> g_shutdown_reason{"signal"};

#if defined(_WIN32)
BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_shutdown_reason = "sigint";
        g_shutdown = true;
        return TRUE;
    }
    return FALSE;
}
#else
void SignalHandler(int signum) {
    g_shutdown_reason = (signum == SIGTERM) ? "sigterm" : "sigint";
    g_shutdown = true;
}
#endif

}  // namespace

std::atomic<bool>& ShutdownRequested() { return g_shutdown; }

void InstallShutdownHandlers() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

void WaitForShutdown() {
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

const char* ShutdownReason() { return g_shutdown_reason.load(); }

}  // namespace ego_runtime
