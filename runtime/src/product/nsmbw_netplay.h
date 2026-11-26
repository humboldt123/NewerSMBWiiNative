#pragma once
// Lockstep online play (runtime/src/product/nsmbw_netplay.cpp).
//
// NSMBW_NETPLAY=host:<port>        this machine is player 1 and waits for the other one
// NSMBW_NETPLAY=join:<host>:<port> this machine is player 2
// NSMBW_NETPLAY_DELAY=<frames>     input delay, default 3 (one frame is 16.7 ms)
// NSMBW_DET_HASH_FILE=<path>       write guest-memory hashes every second (works without netplay,
//                                  to check two runs or two machines stay identical)
//
// Netplay turns on deterministic time (guest_clock.h). Both machines must run the same build of the
// same game with the same save.

#include <cstdint>

bool NsmbwNetplayActive();
// Channels the game sees as connected while netplay is on (2).
uint32_t NsmbwNetplayPlayerCount();
// Once per game frame, on the game thread. localHold/localTilt are this machine's player 1 input.
// Fills hold and tilt for every channel with the inputs both machines agreed on for this frame,
// waiting for the other machine if its input has not arrived yet. Also runs the memory-hash check.
void NsmbwNetplayExchange(uint32_t frame, uint32_t localHold, float localTilt, uint32_t holds[4],
                          float tilts[4]);
// Hash check alone, for NSMBW_DET_HASH_FILE without netplay.
void NsmbwDeterminismFrame(uint32_t frame);
