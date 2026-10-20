#pragma once
// [ping]: an Apex-style ping.  A player's ping key sends a request to the server, which looks along that player's view,
// finds what they point at (an item, an enemy, or the spot where the view meets the world) and tells every player of
// their team who runs the mod.  Each client keeps the pings it was told of and the HUD draws them over the picture,
// projected with the engine's own world-to-screen matrix.  The request is main thread; the marks are read by the HUD's
// thread too (they have a lock of their own).

#include <string>
#include <vector>
#include <windows.h>

namespace game
{

    enum class PingKind
    {
        Spot, // the world
        Item, // a weapon or item lying there
        Enemy // an infected, a witch, a special
    };

    struct PingMark
    {
        int owner = 0; // the user id of the player who pinged
        bool mine = false;
        PingKind kind = PingKind::Spot;
        float x = 0.f, y = 0.f, z = 0.f;
        ULONGLONG born = 0; // GetTickCount64
        int life_ms = 0;
        std::string label; // "AK-47", "Witch"; empty for a spot
        unsigned serial = 0; // counts up with every mark added
    };

    // The live pings (the expired ones are dropped first), oldest first.
    std::vector<PingMark> ping_marks();
    // How many marks were ever added: the HUD plays a sound when it grows.
    unsigned ping_serial();
    // Forgets every ping (leaving a game).
    void ping_clear();

    // A world point on the screen: normalized device coordinates (-1 .. 1, y up) and whether it is in front of the camera
    // (behind, they point the way off the screen).  False without the engine's matrix.
    bool ping_project(float x, float y, float z, float& ndc_x, float& ndc_y, bool& in_front);
    // Where the camera is, solved out of the same matrix (the clip coordinates x, y and w are all 0 at the eye).
    bool ping_camera(float& x, float& y, float& z);

} // namespace game

namespace game::detail
{

    // The start of the line a player's client DLL reads a ping from:
    //   @png@ <owner user id> <1: their own> <kind 0..2> <life ms> <pings kept per player> <x> <y> <z>|<label>
    constexpr char k_ping_marker[] = "@png@";

    // A ping on an enemy follows it: while the enemy lives, the server sends this player's mark its new place, a line of
    // kind 3 (the other fields but the owner and the place are not read):
    //   @png@ <owner> 0 3 1000 1 <x> <y> <z>
    // and makes it glow through walls for a few seconds.  Called every game frame.
    void ping_frame();

    // The active player's ping request (the HUD's key; or a client's inv_ping), on the server.
    void do_ping();
    // A server's line (see k_ping_marker), on a client.
    void apply_ping_line(const char* text);
    // The server half's trace (UTIL_TraceLine); optional.
    void install_ping();
    // The client half's matrix (after install_client_remote: it needs the engine's client interface); optional.
    void install_ping_client();

} // namespace game::detail
