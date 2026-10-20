#pragma once
// The chapter mutators ([mutators] in the rules): the server rolls a chapter's when it starts and tells every player
// with the mod; their client applies what it draws (the fog).  Main thread.

namespace game::detail
{

    // The start of the line a player's client DLL reads the chapter's mutators from:
    //   @mut@ <generation> <1: announce>|<entry>;<entry>...      (nothing after the |: none)
    // an entry is "fog <density per mille> -" (the map's colour) or "fog <density per mille> <r> <g> <b>", "hunted
    // <zombie class>", or a gameplay mutator's bare id ("scarcity": the server's own, the client only shows its name).
    // The generation changes with every roll, so a client announces a chapter's mutators once.
    constexpr char k_mutator_marker[] = "@mut@";

    // Server: a round started on `map`; `again` is the same map again (a lost round), which keeps the chapter's.
    void mutators_on_round_start(const char* map, bool again);
    // Server: a game event (FireEvent): player_left_start_area starts the horde tide's clock.
    void mutators_on_event(const char* name);
    // Server, every game frame: the line to every player with the mod (at once when it changes, then every few
    // seconds: a client forgets it when it leaves a game), and to the host's own client half.
    void mutators_frame();

    // Client: a server's line (see k_mutator_marker).
    void apply_mutator_line(const char* text);
    // Client, every pump: once the player has been in a game for a moment, what the line asks for is applied, and
    // announced.
    void mutators_client_tick();
    // Client: out of a game (quit, or loading the next map): what was applied goes, so the next map's own fog is read
    // first; the next server says again what it has.
    void mutators_client_left();

} // namespace game::detail
