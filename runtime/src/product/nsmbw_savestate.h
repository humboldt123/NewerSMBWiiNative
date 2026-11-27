#pragma once
// In-memory savestates for rollback (nsmbw_savestate.cpp). Save and Load must be called from the
// same call site on the game's main thread (the start of a game frame in the KPAD sampler):
// Load rewinds every guest thread's native stack too, and execution continues from the matching
// Save, which then returns true.

#include <cstddef>
#include <cstdint>

// Saves the game into `slot`. Returns false right away; returns true again later, when a Load of
// this slot resumes execution here.
bool NsmbwSavestateSave(int slot);
// Rewinds the game to `slot`. Does not return: execution continues in the Save that wrote it.
[[noreturn]] void NsmbwSavestateLoad(int slot);
bool NsmbwSavestateValid(int slot);
// The guest frame number recorded with the slot.
uint32_t NsmbwSavestateFrame(int slot);
// How long the last Save took, in microseconds, and how big the slot is.
uint64_t NsmbwSavestateLastSaveMicros();
size_t NsmbwSavestateSlotBytes(int slot);
// True while a Load is being taken. Callers that must not save (a re-simulated frame) check this.
constexpr int kNsmbwSavestateSlots = 10;
