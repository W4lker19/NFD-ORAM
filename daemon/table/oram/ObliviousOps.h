// Constant-time primitives for the PathORAM client.
//
// The C++ standard does not guarantee that these expressions stay branch-free
// after optimisation, so for production TEE deployment the generated assembly
// must be audited (or replaced with inline asm / volatile barriers). The intent
// here is to remove all *source-level* data-dependent control flow from the
// ORAM client so that an auditor can reason locally and so that "obvious"
// compiler choices (cmov on x86) preserve the property.

#ifndef PORAM_OBLIVIOUS_OPS_H
#define PORAM_OBLIVIOUS_OPS_H

#include <cstdint>
#include <cstddef>

namespace oblivious {

// All-ones mask if a == b, else zero. Constant-time wrt a, b.
static inline uint32_t ct_eq_i32(int32_t a, int32_t b) {
    uint32_t x = (uint32_t)a ^ (uint32_t)b;
    // ((x | -x) >> 31) is 1 iff x != 0
    uint32_t nz = (x | (uint32_t)(0u - x)) >> 31;
    return (uint32_t)0u - (1u ^ nz);
}

// All-ones mask if a < b (unsigned), else zero.
static inline uint32_t ct_lt_u32(uint32_t a, uint32_t b) {
    // Borrow into bit 32 iff a < b.
    return (uint32_t)(((uint64_t)a - (uint64_t)b) >> 32);
}

// Conditional select: returns t if mask is all-ones, f if mask is zero.
static inline uint32_t ct_select_u32(uint32_t mask, uint32_t t, uint32_t f) {
    return (mask & t) | (~mask & f);
}

static inline int32_t ct_select_i32(uint32_t mask, int32_t t, int32_t f) {
    return (int32_t)ct_select_u32(mask, (uint32_t)t, (uint32_t)f);
}

// Conditionally copy `len` bytes from src to dst.
// Always reads every src byte and writes every dst byte; the only
// data-dependent quantity is the byte values themselves.
static inline void ct_memcpy(void* dst, const void* src, size_t len, uint32_t mask) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t m = static_cast<uint8_t>(mask & 0xFFu);
    uint8_t nm = static_cast<uint8_t>(~m);
    for (size_t i = 0; i < len; i++) {
        d[i] = static_cast<uint8_t>((m & s[i]) | (nm & d[i]));
    }
}

}  // namespace oblivious

#endif  // PORAM_OBLIVIOUS_OPS_H
