// te_netplay.h -- naser949's two-player mod, ONLINE (host/netplay.cpp). Two machines run the same game from the same state
// and exchange only the pads: the host's controller is pad 0 on both, the joiner's pad 1 on both (the two-player mod's
// second player), every boundary one field and one pair of samples ("lockstep"). A session starts from a save state the
// host makes and sends (te_savestate.h: both machines load it, so both start from the same bytes), and every few seconds
// the two compare a hash of the game's memory; when it differs the host sends its state again.
//
// The player's side is Options > Mods > online: "host a lobby" listens and puts the lobby's code (the host's address on
// the network the two share -- Radmin VPN, ZeroTier, a LAN) on the screen and in the clipboard, "join with the copied
// code" connects to the code in the clipboard, "leave" ends it. Nothing here runs unless one of those rows is chosen.
#pragma once
#include <string>

#include "te_services.h"

namespace te::netplay {

// The mod's row on Options > Mods and its notices (after te::savestate_keys::Install: `root` is the game folder)
void Install(const std::string& root);
// A session is running (or its state is being loaded): the clock fires one field a boundary whatever the wall did, and a
// load's reads land as under the deterministic profile -- what the two machines must agree on
bool Lockstep();
// Which pad's view this machine shows while a session runs (0 the host, 1 the joiner); -1 without a session
int LocalPad();
// The pads' sampler (te_input.cpp), once a boundary: pad 0's sample given this machine's own; then pad 1's (false: no
// session, the second controller as before)
Pads::Sample Pad0(const Pads::Sample& local);
bool Pad1(Pads::Sample& out);
// The runner's answers for the states this module asked for (runner/main.cpp, beside te::savestate_keys')
void Saved(const std::string& path, bool ok, const std::string& error);
void Loaded(const std::string& path, bool ok, const std::string& error);

}  // namespace te::netplay
