#pragma once
// The seam between the inventory and the game.  The HUD and the hotkeys only talk to this; what really happens in the
// game (taking an item out of the world, putting one back, a use) lives behind it.

#include "inv/inventory.hpp"

#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace game
{

    constexpr int k_native_slots = 6; // primary, secondary, throwable, kit, pills, carried

    // A weapon or item the player carries in the game's own slots (the ones the native weapon panel lists).
    struct NativeWeapon
    {
        std::string classname; // "weapon_rifle_ak47"
        std::string name; // "Rifle Ak47"
        char tag[3] = {}; // two letters for the slot
        bool active = false; // in the player's hands now
        std::string melee; // weapon_melee: which one ("fireaxe" ...), for its icon
        int wear = -1; // weapon_melee: its wear ([melee] durability: 0 .. inv::k_wear_broken; -1: none)
    };

    // A player's inventory: on a game client the local player's, shared by the HUD (render thread) and the game code (main
    // thread); in a dedicated server one per connected player (see game/players.cpp).
    struct Session
    {
        std::mutex mutex;
        inv::Inventory inventory;
        // The game's own slots, refreshed by the backend on the main thread and shown first on the hotbar: always
        // k_native_slots entries, entry i is game slot i (primary, secondary, throwable, kit, pills, carried); an empty
        // classname is an empty slot.  Fixed positions, so the inventory's slots after them never move on the hotbar.
        std::vector<NativeWeapon> native;
        std::string message; // a status line the HUD shows for a few seconds
        ULONGLONG message_until = 0;
        // Client only: the inventory mirrors the one a dedicated server keeps for this player (it is not saved here).
        bool remote = false;
        // With `remote`: what the server counts the stored items as weighing (its rules file may give the items other
        // weights than this DLL's table); -1 otherwise.
        float remote_weight = -1.f;
        // Client only, refreshed on the main thread: the player is a living survivor in a game with no menu open (the
        // hotbar shows and its keys work only then), and the weapon in their hands: rounds in the clip and in reserve
        // (-1: none, e.g. a melee weapon or an item).
        bool playing = true;
        int ammo_clip = -1;
        int ammo_reserve = -1;
        // The player's reserve of every ammo type (m_iAmmo, by the game's index: inv::ammo_index), for what a stored
        // gun would have (the reserve is shared by the guns of a type).
        int ammo_pool[32] = {};
        // The infection ([infection] in the rules): when the player turns (GetTickCount64), 0 when not infected, and the
        // clock's full length, for how strong the effect is.
        ULONGLONG infection_until = 0;
        int infection_total_ms = 0;
        ULONGLONG infection_turned_at = 0; // the clock ran out (GetTickCount64): the screamer
        // Stamina ([stamina] in the rules): 0 .. 1, -1 when there is none; exhausted: it ran out and is not back yet.
        float stamina = -1.f;
        bool stamina_exhausted = false;
        // [melee] (the server's rules): how long the attack is held for a heavy swing, and how long a swing asked for
        // during the last one's wait is kept, in seconds (0: no [melee], the game's own swings).  And the wind-up, from
        // the client's input (main thread): since when (GetTickCount64, 0: not winding), whether it can be heavy, and
        // when a heavy swing was let go (for its flash at the crosshair).
        float melee_hold = 0.f;
        float melee_queue = 0.f;
        float melee_shove_hold = 0.f; // heavy_shove_hold (0: no heavy shove)
        ULONGLONG melee_winding_since = 0;
        bool melee_can_heavy = true;
        ULONGLONG melee_heavy_at = 0;
        // Server side, main thread only: the player's reserve (m_iAmmo, by the game's index) when the chapter ended,
        // waiting for the next chapter: the game brings back only the reserve of the guns in the hands, and a stored
        // gun's reserve is the player's (see "a chapter transition" in game/lifecycle.cpp).
        bool transition_pending = false;
        int transition_reserve[32] = {};
    };
    // The active session: the local player's, or (while the game code serves another player: a dedicated server, or
    // a guest on this listen server) theirs.  The game code uses this one.
    Session& session();
    // The local player's session, always: what the HUD shows and saves.
    Session& local_session();
    // Main thread only: makes `s` the active session (nullptr: the local one again).
    void set_active_session(Session* s);
    // Rebuilds the inventory from the config's limits, keeping what fits.
    void session_apply_limits();
    // The config's limits (boxes, weapon boxes, weight), for a new inventory.
    inv::Limits config_limits();
    // Shows a line in the HUD for a few seconds.  Locks the session itself.
    void notify(const std::string& text);
    // Why `def` did not go into the active inventory: "Too heavy: ..." or "The inventory is full".  Takes the lock.
    std::string refusal(const inv::ItemDef& def);

    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual const char* name() const = 0;
        // True when this talks to the game itself (the stub only edits the list).
        virtual bool connected() const = 0;
        // True while the game is taking typed text (console, chat), so hotkeys must not fire.
        virtual bool typing() const
        {
            return false;
        }

        // Requests.  They return at once; the backend changes the session when the game has done it.  Never call these
        // with the session locked.
        virtual void use(int slot) = 0; // put the item in the player's hands
        virtual void drop(int slot, int count) = 0; // put it on the ground in front of the player
        virtual void store_held() = 0; // take the item the player holds into the inventory
        virtual void pickup_nearby() = 0; // take the item the player is looking at from the world into the inventory
        // Empty the gun in the player's hands, or the one on the ground they look at: its magazine into their reserve.
        virtual void unload_weapon()
        {
        }
        // Drag and drop in the window: the HUD has moved it in the session already; a remote inventory moves it too.
        virtual void move(int /*from*/, int /*to*/)
        {
        }
        virtual void select_weapon(const std::string& classname) // switch to a weapon the player already carries
        {
            (void)classname;
        }
        virtual void drop_weapon(const std::string& classname) // put a carried game weapon on the ground
        {
            (void)classname;
        }
        // Puts `count` (0: all) of the player's reserve rounds of ammo type `kind` ("assaultrifle" ...) on the ground.
        virtual void drop_ammo(const std::string& kind, int count)
        {
            (void)kind;
            (void)count;
        }
        virtual void infect_self() // development: infects the player ([infection] in the rules must be on)
        {
        }

        // On the game's main thread, when the window gets k_msg_pump.
        virtual void pump()
        {
        }
    };

    Backend& backend();
    // Replaces the stub (done by game::init when the game's functions were found).
    void set_backend(Backend* backend);

    constexpr UINT k_msg_pump = WM_APP + 0x49;
    constexpr UINT k_msg_unload = WM_APP + 0x4A; // development: asks the mod to unload itself
    // The game's window: the backend posts k_msg_pump to it to get code run on the main thread.
    void set_window(HWND window);
    void wake_main_thread();

} // namespace game
