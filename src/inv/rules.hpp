#pragma once
// The rules file (inventory_mod.toml next to the DLL; on a dedicated server it sets the rules for every player): how
// many boxes a player has, how much they can carry and what each item weighs.
//
//   [limits]
//   slots = 8
//   weapon_slots = 2           # boxes for stored guns and melee weapons
//   max_weight = 20.0          # 0: no weight limit
//
//   [weight]                   # per item or weapon; "weapon_" may be left out
//   first_aid_kit = 4.0
//   pain_pills = 1
//   rifle_ak47 = 4.5           # the gun and the melee / pistol count too
//
//   [stack]                    # how many fit in one box
//   molotov = 2
//
//   [ammo]                     # the most reserve rounds of each ammo type (the game's ammo_<type>_max)
//   assault_rifle = 360
//
//   [round_weight]             # what one reserve round weighs (360 rifle rounds at 0.01: 3.6)
//   assault_rifle = 0.01

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inv
{

    struct Rules
    {
        std::optional<int> slots;
        std::optional<float> max_weight;
        std::optional<int> weapon_slots;
        int items_changed = 0; // weights and stack sizes set
        int ammo_changed = 0; // [ammo] maxima set (set_ammo_max)
        std::vector<std::string> errors; // "line 3: ..."
    };

    // Reads the rules and applies the item weights and stack sizes to the item table (set_item_weight /
    // set_item_stack); the limits are returned for the caller to use.  A file that does not parse changes nothing.
    Rules apply_rules(std::string_view text);

    // [infection]: a survivor knocked down by the infected, grabbed by a special (smoker, hunter, jockey, charger) or
    // vomited on by a boomer is infected; when the clock runs out they die and a random special infected rises where
    // they stood, and they cannot be revived (defibrillator, rescue closet) before the next chapter.  Pain pills put
    // the clock back to its full time (no_cure, the default) or cure it.
    //
    //   [infection]
    //   enabled = true
    //   seconds = 120                # the clock
    //   hit_seconds = 20             # what each new grab / vomit / knock-down takes off a running clock
    //   specials = ["smoker", "boomer", "hunter", "spitter", "jockey", "charger"]   # what may rise
    //   bots = false                 # the survivor bots too
    //   no_cure = true               # pain pills only put the clock back to the start (false: they cure)
    //   cough = true                 # the infected cough now and then, more often as the clock runs out
    //   insane = true                # near the end they go insane: laugh for no reason, scream
    //   insane_at = 70               # from this much (%) of the clock gone
    struct InfectionRules
    {
        bool enabled = false;
        int seconds = 120;
        int hit_seconds = 20;
        std::vector<int> specials{1, 2, 3, 4, 5, 6}; // the game's zombie classes: 1 smoker ... 6 charger
        bool bots = false;
        bool no_cure = true;
        bool cough = true;
        bool insane = true;
        int insane_at = 70;
    };
    // The last apply_rules' [infection] (the defaults, disabled, without one).
    const InfectionRules& infection_rules();

    // [stamina]: running, jumping, melee swings and shoves use stamina (0 .. 100), which comes back after a moment's
    // rest.  A survivor who runs out is exhausted until it is back to `recover`: slower, slower to swing and shove,
    // and their melee hits do less damage.
    //
    //   [stamina]
    //   enabled = true
    //   run = 8                      # used per second of running
    //   jump = 12                    # per jump
    //   swing = 10                   # per melee swing
    //   shove = 8                    # per shove
    //   regen = 20                   # back per second of rest
    //   regen_delay = 1.0            # seconds after the last use before it comes back
    //   recover = 30                 # exhausted until it is back to this much
    //   slow = 60                    # exhausted: % of the normal speed
    //   swing_speed = 60             # exhausted: % of the normal swing and shove rate
    //   damage = 50                  # exhausted: % of the normal melee damage
    //   bots = false                 # the survivor bots too
    struct StaminaRules
    {
        bool enabled = false;
        float run = 8.f;
        float jump = 12.f;
        float swing = 10.f;
        float shove = 8.f;
        float regen = 20.f;
        float regen_delay = 1.f;
        float recover = 30.f;
        float slow = 60.f;
        float swing_speed = 60.f;
        float damage = 50.f;
        bool bots = false;
    };
    // The last apply_rules' [stamina] (the defaults, disabled, without one).
    const StaminaRules& stamina_rules();

    // [melee]: the melee of No More Room in Hell 2.  Pressing the attack winds the swing up and letting go swings: let
    // go before heavy_hold seconds it is a light swing, after it a heavy one (more damage, more stamina, a longer
    // wait).  A shove while winding up calls the swing off.  A swing asked for during the last one's wait is kept for
    // `queue` seconds and comes as soon as the weapon is ready.  One swing that hits several targets does less to
    // each further one (cleave).  A light swing does not kill a common infected (50 health) by itself: about three to
    // the body, two to the head; a heavy one does.  Winding up needs the mod on the player's client; without it every swing is light.
    //
    //   [melee]
    //   enabled = true
    //   heavy_hold = 0.6             # seconds the attack is held for a heavy swing
    //   heavy_damage = 200           # % of the weapon's damage a heavy swing does
    //   heavy_recovery = 150         # % of the weapon's wait after a heavy swing
    //   heavy_stamina = 25           # with [stamina]: what a heavy swing costs (instead of its `swing`)
    //   light_damage = 40            # % of the weapon's damage a light swing does
    //   head_damage = 200            # % more on a hit to the head (on top of light / heavy)
    //   cleave = 70                  # each further target of one swing takes this % of what the one before took
    //   cleave_min = 30              # but never less than this % of what the first took
    //   queue = 0.8                  # seconds a swing asked for during the last one's wait is kept
    //   wind_speed = 65              # % of the normal speed while winding up a heavy swing (eased in with the charge)
    struct MeleeRules
    {
        bool enabled = false;
        float heavy_hold = 0.6f;
        float heavy_damage = 200.f;
        float heavy_recovery = 150.f;
        float heavy_stamina = 25.f;
        float light_damage = 40.f;
        float head_damage = 200.f;
        float cleave = 70.f;
        float cleave_min = 30.f;
        float queue = 0.8f;
        float wind_speed = 65.f;
    };
    // The last apply_rules' [melee] (the defaults, disabled, without one).
    const MeleeRules& melee_rules();

    // [ammo_piles]: a map's ammo pile is not endless.  It holds a random ammo type of `kinds`, from min_magazines to
    // max_magazines magazines of it; anyone takes as many as they can carry (a bot: only for a gun it has), the rest
    // stays, and an empty pile is gone.  With replace_map_piles each of the map's piles becomes a pack of its rolled type
    // when the round starts (what it holds shows); `extra` more packs are put beside the map's item spots.
    //
    //   [ammo_piles]
    //   enabled = true
    //   min_magazines = 1
    //   max_magazines = 3
    //   kinds = ["assault_rifle", "smg", "shotgun", "auto_shotgun", "hunting_rifle", "sniper_rifle", "grenade_launcher"]
    //   replace_map_piles = true
    //   extra = 4
    struct AmmoPileRules
    {
        bool enabled = false;
        int min_magazines = 1;
        int max_magazines = 3;
        std::vector<std::string> kinds{"assaultrifle", "smg",         "shotgun",        "autoshotgun",
                                       "huntingrifle", "sniperrifle", "grenadelauncher"}; // (inv::ammo_index names)
        bool replace_map_piles = true; // the map's piles become packs when the round starts
        int extra = 4; // packs put beside the map's item spots when the round starts
    };
    // The last apply_rules' [ammo_piles] (the defaults, disabled, without one).
    const AmmoPileRules& ammo_pile_rules();

    // [map_guns]: a gun nobody has held yet (lying in the map, or from a weapon spawner) has only a part of its
    // magazine when it is first picked up: from min_clip to max_clip % of it.  A gun someone has held keeps its own.
    //
    //   [map_guns]
    //   enabled = true
    //   min_clip = 10
    //   max_clip = 60
    //   spawners_once = true         # a weapon spawner (guns, melee weapons) gives one, then it is gone
    //   min_reserve = 0              # what such a gun adds to the reserve: min_reserve..max_reserve magazines
    //   max_reserve = 1              # (the game would fill the reserve)
    struct MapGunRules
    {
        bool enabled = false;
        int min_clip = 10;
        int max_clip = 60;
        bool spawners_once = true;
        int min_reserve = 0;
        int max_reserve = 1;
    };
    // The last apply_rules' [map_guns] (the defaults, disabled, without one).
    const MapGunRules& map_gun_rules();

} // namespace inv
