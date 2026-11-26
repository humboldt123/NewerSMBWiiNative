#pragma once
// One place that picks the host's SIMD header. x86-64 gets the real SSE/AVX intrinsics. ARM64
// (Apple Silicon) gets sse2neon, which spells the same _mm_* names in NEON, so the paired-single
// and quantizer code stays one source.
//
// sse2neon's _mm_getcsr/_mm_setcsr only carry the rounding mode, but the runtime keeps guest
// FPSCR[NI] in MXCSR's FTZ|DAZ bits. So on ARM64 those two are replaced with versions that also
// map FTZ|DAZ onto FPCR.FZ (one bit there, which flushes both inputs and outputs).

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "sse2neon.h"

inline unsigned int MkwArmGetCsr() noexcept
{
    unsigned int csr = _MM_GET_ROUNDING_MODE();
    if (_MM_GET_FLUSH_ZERO_MODE() == _MM_FLUSH_ZERO_ON)
        csr |= (1u << 15) | (1u << 6);  // FTZ | DAZ
    return csr;
}

inline void MkwArmSetCsr(unsigned int csr) noexcept
{
    _MM_SET_ROUNDING_MODE(csr & 0x6000u);  // MXCSR.RC
    _MM_SET_FLUSH_ZERO_MODE((csr & ((1u << 15) | (1u << 6))) != 0 ? _MM_FLUSH_ZERO_ON
                                                                  : _MM_FLUSH_ZERO_OFF);
}

#define _mm_getcsr MkwArmGetCsr
#define _mm_setcsr MkwArmSetCsr
#else
#error "Unsupported host architecture: need x86-64 or ARM64"
#endif
