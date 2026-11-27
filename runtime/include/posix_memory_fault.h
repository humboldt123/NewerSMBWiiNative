#pragma once

#if !defined(_WIN32)
#include <csignal>
#include "guest_flat_memory.h"
#include <stdexcept>
#if defined(__APPLE__)
#include <sys/ucontext.h>  // <ucontext.h> needs _XOPEN_SOURCE on macOS
#else
#include <ucontext.h>
#endif
#include <unistd.h>

extern "C" bool GuestFlatDebugWatchTrap(void* faultAddress, void* context) noexcept;
namespace RuntimeSignals {

inline bool (*memoryFaultResolver)(void*, bool) = nullptr;

inline void MemoryFault(int signal, siginfo_t* info, void* context) {
    bool isWrite = false;
#if defined(__x86_64__)
    if (context) {
        isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    // ESR_EL1 bit 6 (WnR) is set when a data abort was caused by a write.
    if (context) {
        isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr & (1u << 6)) != 0;
    }
#endif
    if (info && GuestFlat::CowHandleFault(info->si_addr)) return;
    if (info && GuestFlatDebugWatchTrap(info->si_addr, context)) return;
    if (info && memoryFaultResolver && memoryFaultResolver(info->si_addr, isWrite)) return;
    constexpr char message[] = "[nsmbw] Unhandled native memory fault.\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

inline void InstallMemoryFaultHandler(bool (*resolver)(void*, bool)) {
    memoryFaultResolver = resolver;
    struct sigaction action{};
    action.sa_sigaction = MemoryFault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, nullptr) != 0 || sigaction(SIGBUS, &action, nullptr) != 0) {
        throw std::runtime_error("Could not install the guest memory fault handler.");
    }
}

}  // namespace RuntimeSignals
#endif
