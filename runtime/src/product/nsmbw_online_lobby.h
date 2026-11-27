#pragma once
// Going online from the options menu (Options > Online). The lobby runs inside a normal, local game:
// the joiner says hello, both sides check they run the same game, the host sends its save, and then
// both games restart straight into an online session (nsmbw_netplay.cpp), on a copy of the NAND
// holding the host's save. Online play needs both games to start identical, which a game already
// running locally can't be turned into; a restart can.

#include <cstdint>
#include <string>
#include <vector>

namespace nsmbw_online_lobby {

enum class Phase {
    Idle,
    Hosting,       // waiting for player 2
    Joining,       // saying hello to the host
    SendingSave,   // host: player 2 is here, sending the save
    ReceivingSave, // joiner: getting the host's save
    Starting,      // both: restarting into the session
    Failed,
};

inline constexpr uint16_t kDefaultPort = 7777;

void Host(bool rollback);
// `address` is "host" or "host:port".
void Join(const std::string& address);
void Cancel();
// Runs the lobby; call once per frame while the menu is open.
void Poll();

Phase CurrentPhase();
// One line for the menu: what is happening, or why it failed.
std::string Status();
// This machine's IPv4 addresses, for the host to read out (Tailscale's 100.x first).
std::vector<std::string> LocalAddresses();

// In an online session (started from the lobby or by NSMBW_NETPLAY).
bool InSession();
// Restarts the game locally, ending the session.
void LeaveSession();

}  // namespace nsmbw_online_lobby
