#pragma once

#include "fiber_manager.h"
#if !MKW_WIN32_FIBERS
#include "libco.h"

namespace Fiber {

// Every guest thread's native stack; savestates copy from its saved stack pointer up to the top.
inline constexpr unsigned int kHostFiberStackSize = 16u * 1024u * 1024u;


inline void* CreateHostFiber(void (*entry)()) {
    // libco allocates a fixed stack, unlike CreateFiber's initial commit size.
    return co_create(kHostFiberStackSize, entry);
}

}  // namespace Fiber
#endif
