#pragma once
// Savestates for rollback (runtime/src/product/nsmbw_savestate.cpp). A savestate is all mapped guest
// memory, every guest thread's native stack, and the host-side runtime state the guest can observe
// later. Each runtime module that owns such state exposes a Save/Load pair below, which writes or
// reads its private state through these helpers in a fixed order.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace SaveState {

struct Writer {
    std::vector<uint8_t>& out;
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <typename T> void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Pod() needs a trivially copyable type");
        Bytes(&v, sizeof(T));
    }
    template <typename T> void Vector(const std::vector<T>& v) {
        Pod(static_cast<uint64_t>(v.size()));
        if (!v.empty()) Bytes(v.data(), v.size() * sizeof(T));
    }
    template <typename T> void Deque(const std::deque<T>& d) {
        Pod(static_cast<uint64_t>(d.size()));
        for (const T& e : d) Pod(e);
    }
    template <typename T> void Set(const std::unordered_set<T>& s) {
        std::vector<T> v(s.begin(), s.end());
        Vector(v);
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    void Bytes(void* dst, size_t n) {
        if (static_cast<size_t>(end - p) < n) throw std::runtime_error("savestate truncated");
        std::memcpy(dst, p, n);
        p += n;
    }
    template <typename T> void Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Pod() needs a trivially copyable type");
        Bytes(&v, sizeof(T));
    }
    template <typename T> void Vector(std::vector<T>& v) {
        uint64_t n = 0;
        Pod(n);
        v.resize(static_cast<size_t>(n));
        if (n != 0) Bytes(v.data(), static_cast<size_t>(n) * sizeof(T));
    }
    template <typename T> void Deque(std::deque<T>& d) {
        uint64_t n = 0;
        Pod(n);
        d.clear();
        for (uint64_t i = 0; i < n; ++i) {
            T e{};
            Pod(e);
            d.push_back(e);
        }
    }
    template <typename T> void Set(std::unordered_set<T>& s) {
        std::vector<T> v;
        Vector(v);
        s = std::unordered_set<T>(v.begin(), v.end());
    }
};

}  // namespace SaveState

// Per-module hooks, called in this order by the savestate core.
void VI_SaveState(SaveState::Writer&);
void VI_LoadState(SaveState::Reader&);
void OsSleep_SaveState(SaveState::Writer&);
void OsSleep_LoadState(SaveState::Reader&);
void OsInterrupt_SaveState(SaveState::Writer&);
void OsInterrupt_LoadState(SaveState::Reader&);
void Audio_SaveState(SaveState::Writer&);
void Audio_LoadState(SaveState::Reader&);
void AX_SaveState(SaveState::Writer&);
void AX_LoadState(SaveState::Reader&);
void NandAsync_SaveState(SaveState::Writer&);
void NandAsync_LoadState(SaveState::Reader&);
void PpcHelpers_SaveState(SaveState::Writer&);
void PpcHelpers_LoadState(SaveState::Reader&);
void GxHle_SaveState(SaveState::Writer&);
void GxHle_LoadState(SaveState::Reader&);
void Fibers_SaveState(SaveState::Writer&);
void Fibers_LoadState(SaveState::Reader&);
void NsmbwInput_SaveState(SaveState::Writer&);
void NsmbwInput_LoadState(SaveState::Reader&);
void NsmbwP1Character_SaveState(SaveState::Writer&);
void NsmbwP1Character_LoadState(SaveState::Reader&);
