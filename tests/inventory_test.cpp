// Tests of the inventory model.  A plain program: exit code 0 when every check passes.

#include "inv/inventory.hpp"
#include "inv/melee.hpp"
#include "inv/rules.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    int g_failed = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

    const inv::ItemDef& item(const char* classname)
    {
        const inv::ItemDef* def = inv::find_item(classname);
        if (def == nullptr)
        {
            std::printf("unknown item %s\n", classname);
            std::exit(2);
        }
        return *def;
    }

    void catalogue()
    {
        for (const inv::ItemDef& def : inv::all_items())
        {
            CHECK(def.max_stack >= 1);
            CHECK(inv::find_item(def.classname) == &def);
        }
        CHECK(inv::find_item("weapon_ammo_spawn") == nullptr);
        for (const inv::ItemDef& def : inv::all_items())
            CHECK(!def.weapon() || def.max_stack == 1); // a weapon is one per box: it has its own rounds
    }

    void add_stacks_then_new_slots()
    {
        inv::Inventory inventory({.slots = 3});
        const auto& pills = item("weapon_pain_pills"); // max stack 4
        CHECK(inventory.add(pills, 6) == 6);
        CHECK(inventory.slot(0).count == 4);
        CHECK(inventory.slot(1).count == 2);
        CHECK(inventory.add(pills, 1) == 1); // tops up slot 1
        CHECK(inventory.slot(1).count == 3);
        CHECK(inventory.count_of(pills) == 7);
        CHECK(inventory.used_slots() == 2);
    }

    void add_stops_when_full()
    {
        inv::Inventory inventory({.slots = 2});
        const auto& medkit = item("weapon_first_aid_kit"); // max stack 2
        CHECK(inventory.add(medkit, 10) == 4);
        CHECK(inventory.add(medkit, 1) == 0);
        CHECK(inventory.add(item("weapon_molotov"), 1) == 0);
    }

    void weight_limit()
    {
        inv::Inventory inventory({.slots = 8, .max_weight = 5.f});
        const auto& pipe = item("weapon_pipe_bomb"); // weight 2
        CHECK(inventory.add(pipe, 5) == 2); // 4 of 5
        CHECK(inventory.weight() == 4.f);
        CHECK(inventory.add(item("weapon_pain_pills"), 3) == 1); // 1 left
        CHECK(inventory.add(item("weapon_gascan"), 1) == 0);
    }

    void remove()
    {
        inv::Inventory inventory({.slots = 4});
        const auto& pills = item("weapon_pain_pills");
        inventory.add(pills, 5); // 4 + 1
        CHECK(inventory.remove_from_slot(1, 1) == 1);
        CHECK(inventory.slot(1).empty());
        CHECK(inventory.remove_from_slot(1, 1) == 0);
        CHECK(inventory.remove_item(pills, 10) == 4);
        CHECK(inventory.used_slots() == 0);
        CHECK(inventory.remove_from_slot(-1, 1) == 0);
        CHECK(inventory.remove_from_slot(99, 1) == 0);
    }

    void moving()
    {
        inv::Inventory inventory({.slots = 4});
        const auto& pills = item("weapon_pain_pills");
        const auto& adren = item("weapon_adrenaline");
        inventory.add(pills, 6); // slot 0: 4, slot 1: 2
        inventory.add(adren, 1); // slot 2

        CHECK(inventory.move(2, 3)); // into an empty slot
        CHECK(inventory.slot(2).empty() && inventory.slot(3).def == &adren);

        CHECK(inventory.move(1, 0)); // same item, slot 0 is full: nothing merges, they swap
        CHECK(inventory.slot(0).count == 2 && inventory.slot(1).count == 4);

        inventory.remove_from_slot(1, 2); // slot 1: 2, slot 0: 2
        CHECK(inventory.move(1, 0)); // merges
        CHECK(inventory.slot(0).count == 4 && inventory.slot(1).empty());

        CHECK(inventory.move(0, 3)); // different items swap
        CHECK(inventory.slot(0).def == &adren && inventory.slot(3).def == &pills);

        CHECK(!inventory.move(1, 2)); // empty source
        CHECK(!inventory.move(0, 0));
        CHECK(!inventory.move(0, 9));
    }

    void selection()
    {
        inv::Inventory inventory({.slots = 5});
        inventory.add(item("weapon_molotov"), 1); // slot 0
        inventory.select(2);
        inventory.add(item("weapon_defibrillator"), 1); // slot 1
        inventory.add(item("weapon_gascan"), 1); // slot 2
        inventory.select(0);
        inventory.select_step(1);
        CHECK(inventory.selected() == 1);
        inventory.select_step(1);
        CHECK(inventory.selected() == 2);
        inventory.select_step(1); // wraps past the empty slots
        CHECK(inventory.selected() == 0);
        inventory.select_step(-1);
        CHECK(inventory.selected() == 2);
        inventory.select(9); // ignored
        CHECK(inventory.selected() == 2);

        inv::Inventory empty({.slots = 3});
        empty.select_step(1); // nothing occupied: plain stepping
        CHECK(empty.selected() == 1);
    }

    void text_round_trip()
    {
        {
            inv::Inventory w({.slots = 2, .weapon_slots = 1});
            inv::WeaponState axe;
            axe.melee = "fireaxe";
            axe.wear = 4321;
            CHECK(w.add_weapon(item("weapon_melee"), axe) >= 0);
            inv::Inventory back({.slots = 2, .weapon_slots = 1});
            CHECK(back.from_text(w.to_text()) && back.slot(2).weapon.wear == 4321 && back.slot(2).weapon.melee == "fireaxe");
        }
        inv::Inventory a({.slots = 6});
        a.add(item("weapon_pain_pills"), 6);
        a.add(item("weapon_first_aid_kit"), 1);
        const std::string text = a.to_text();

        inv::Inventory b({.slots = 6});
        CHECK(b.from_text(text));
        for (int i = 0; i < 6; ++i)
        {
            CHECK(a.slot(i).def == b.slot(i).def);
            CHECK(a.slot(i).count == b.slot(i).count);
        }

        CHECK(!b.from_text("garbage"));
        CHECK(b.slot(0).def != nullptr); // a failed load leaves the contents alone

        inv::Inventory c({.slots = 2});
        CHECK(c.from_text("inventory 1\n0 weapon_molotov 99\n1 weapon_nonexistent 1\n7 weapon_gascan 1\nbad line\n"));
        CHECK(c.slot(0).count == item("weapon_molotov").max_stack);
        CHECK(c.slot(1).def == &item("weapon_gascan")); // a box number past the end: into a free box, not lost
    }

    void revision()
    {
        inv::Inventory inventory({.slots = 2});
        const std::uint32_t r0 = inventory.revision();
        inventory.add(item("weapon_molotov"), 1);
        CHECK(inventory.revision() != r0);
        const std::uint32_t r1 = inventory.revision();
        inventory.add(item("weapon_molotov"), 0); // nothing added
        CHECK(inventory.revision() == r1);
    }

    // An item taken in hand keeps its box: it shows there while held, nothing else moves into it, and it comes back there.
    void held_box()
    {
        const inv::ItemDef& medkit = item("weapon_first_aid_kit");
        const inv::ItemDef& upgrade = item("weapon_upgradepack_incendiary");
        inv::Inventory inventory({.slots = 3});
        inventory.add(upgrade, 1); // box 0
        inventory.add(medkit, 1); // box 1
        CHECK(inventory.remove_from_slot(1, 1) == 1); // the kit goes into the hands...
        CHECK(inventory.slot(1).empty());
        CHECK(inventory.set_held(1, medkit)); // ...and box 1 stays its home
        CHECK(!inventory.slot(1).empty() && inventory.slot(1).count == 0 && inventory.held_slot(medkit) == 1);
        CHECK(inventory.add(item("weapon_molotov"), 1) == 1);
        CHECK(inventory.slot(2).def == &item("weapon_molotov")); // a new item does not take the held one's box
        CHECK(inventory.to_text().find("weapon_first_aid_kit") == std::string::npos); // the held one is not saved
        inventory.release_held(1); // stored again
        CHECK(inventory.slot(1).count == 1 && !inventory.slot(1).held);
        CHECK(inventory.set_held(1, medkit));
        inventory.clear_held(1); // used up in the game: only what is stored stays
        CHECK(inventory.slot(1).count == 1 && !inventory.slot(1).held);
        CHECK(inventory.home_for(medkit) == 1);
        CHECK(inventory.set_held(1, medkit) && inventory.remove_from_slot(1, 1) == 1 && !inventory.slot(1).empty());
        inventory.clear_held(1);
        CHECK(inventory.slot(1).empty());
    }

    void weapons()
    {
        const inv::ItemDef& ak = item("weapon_rifle_ak47");
        const inv::ItemDef& melee = item("weapon_melee");
        CHECK(ak.weapon() && melee.weapon() && !item("weapon_molotov").weapon());
        CHECK(ak.game_slot == 0 && melee.game_slot == 1 && item("weapon_pistol").game_slot == 1);

        // 2 item boxes (0, 1), then 3 weapon boxes (2, 3, 4)
        inv::Inventory inventory({.slots = 2, .weapon_slots = 3});
        CHECK(inventory.slot_count() == 2 && inventory.first_weapon_slot() == 2 && inventory.total_slots() == 5);
        CHECK(inventory.add_weapon(ak, {.clip = 17, .ammo = 240}) == 2);
        CHECK(inventory.add_weapon(ak, {.clip = 40, .ammo = 0, .upgrades = 1 | 4, .special = 33}) == 3); // one per box: a second AK gets its own
        CHECK(inventory.add_weapon(melee, {.melee = "katana"}, 3) == -1); // box 3 is taken
        CHECK(inventory.add_weapon(melee, {.melee = "katana"}, 0) == -1); // an item box
        CHECK(inventory.add_weapon(melee, {.melee = "katana"}) == 4);
        CHECK(inventory.add_weapon(melee, {.melee = "fireaxe"}) == -1); // the weapon boxes are full...
        CHECK(!inventory.has_box_for(melee) && inventory.has_box_for(item("weapon_molotov")));
        CHECK(inventory.add(item("weapon_molotov"), 1) == 1 && inventory.slot(0).def == &item("weapon_molotov")); // ...not the item boxes
        CHECK(inventory.slot(2).weapon.clip == 17 && inventory.slot(2).weapon.ammo == 240);
        CHECK(inventory.slot(4).weapon.melee == "katana");

        inv::Inventory copy({.slots = 2, .weapon_slots = 3});
        CHECK(copy.from_text(inventory.to_text()));
        CHECK(copy.slot(3).def == &ak && copy.slot(3).weapon.clip == 40 && copy.slot(3).weapon.ammo == 0);
        CHECK(copy.slot(3).weapon.upgrades == 5 && copy.slot(3).weapon.special == 33); // incendiary + laser, 33 loaded
        CHECK(copy.slot(2).weapon.upgrades == 0 && copy.slot(2).weapon.special == 0);
        CHECK(copy.slot(4).def == &melee && copy.slot(4).weapon.melee == "katana" && copy.slot(4).weapon.clip == -1);

        // a file from before the weapon boxes, with a weapon in an item box: it moves into a weapon box
        inv::Inventory old({.slots = 2, .weapon_slots = 1});
        CHECK(old.from_text("inventory 1\n0 weapon_shotgun_chrome 1 8 51 -\n1 weapon_pain_pills 2\n"));
        CHECK(old.slot(0).empty() && old.slot(1).count == 2 && old.slot(2).def == &item("weapon_shotgun_chrome") &&
              old.slot(2).weapon.ammo == 51 && old.slot(2).weapon.upgrades == 0); // (from before the upgrades)

        CHECK(inventory.remove_from_slot(2, 1) == 1 && inventory.slot(2).empty() && inventory.slot(2).weapon.clip == -1);
        CHECK(inventory.move(4, 2) && inventory.slot(2).weapon.melee == "katana"); // the state moves with it
        CHECK(!inventory.move(2, 1)); // not into an item box

        inv::Inventory heavy({.slots = 3, .max_weight = 6.f});
        CHECK(heavy.add_weapon(ak, {}) == 3 && heavy.add_weapon(ak, {}) == -1); // 4.5 + 4.5 > 6
    }

    // Changes the item table: runs last, and puts the defaults back.
    void rules_file()
    {
        const inv::ItemDef& medkit = item("weapon_first_aid_kit");
        const inv::ItemDef& molotov = item("weapon_molotov");
        const float medkit_weight = medkit.weight;
        const int molotov_stack = molotov.max_stack;

        const inv::Rules rules = inv::apply_rules(R"(
# a server's rules
[limits]
slots = 6
max_weight = 12        # an integer is fine too

[weight]
first_aid_kit = 5.5    # "weapon_" left out
weapon_pain_pills = 0.5

[stack]
molotov = 1
)");
        CHECK(rules.errors.empty());
        CHECK(rules.slots == 6);
        CHECK(rules.max_weight && *rules.max_weight == 12.f);
        CHECK(rules.items_changed == 3);
        CHECK(medkit.weight == 5.5f && item("weapon_pain_pills").weight == 0.5f && molotov.max_stack == 1);

        // the limits work with the new weights: 12 / 5.5 -> two kits
        inv::Inventory inventory({.slots = *rules.slots, .max_weight = *rules.max_weight});
        CHECK(inventory.add(medkit, 5) == 2);
        CHECK(inventory.add(molotov, 2) == 0); // 11 of 12 used, a molotov weighs 2

        // the one in the hands is carried: it counts, and taking it out or putting it back changes nothing
        CHECK(inventory.remove_from_slot(0, 1) == 1 && inventory.set_held(0, medkit));
        CHECK(inventory.weight() == 11.f);
        CHECK(inventory.add(item("weapon_pain_pills"), 3) == 2); // 0.5 each: one point left
        inventory.release_held(0);
        CHECK(inventory.weight() == 12.f && inventory.slot(0).count == 2);

        // mistakes are reported with their line and change nothing else
        const inv::Rules bad = inv::apply_rules("[limits]\nslots = 0\n[weight]\nno_such_item = 1\n[stack]\nmolotov = \"two\"\n[colour]\n");
        CHECK(bad.errors.size() == 4);
        CHECK(!bad.slots && bad.items_changed == 0);
        CHECK(!bad.errors.empty() && bad.errors[0].rfind("line 2:", 0) == 0);
        CHECK(!inv::apply_rules("[limits\nslots = 3").errors.empty()); // does not parse

        // weapons: their weights from the same [weight] table, counted through set_weapons_weight
        const float ak_weight = inv::weapon_weight("weapon_rifle_ak47");
        CHECK(ak_weight > 0.f && inv::weapon_weight("weapon_first_aid_kit") == 0.f);
        const inv::Rules weapons = inv::apply_rules("[weight]\nrifle_ak47 = 7\nweapon_melee = 1.5\n");
        CHECK(weapons.errors.empty() && weapons.items_changed == 2);
        CHECK(inv::weapon_weight("weapon_rifle_ak47") == 7.f && inv::weapon_weight("weapon_melee") == 1.5f);
        inv::Inventory armed({.slots = 4, .max_weight = 10.f});
        armed.set_weapons_weight(inv::weapon_weight("weapon_rifle_ak47") + inv::weapon_weight("weapon_melee"));
        CHECK(armed.weight() == 8.5f);
        CHECK(armed.add(medkit, 1) == 0); // 8.5 + 5.5 > 10
        CHECK(armed.add(item("weapon_pain_pills"), 5) == 3);
        inv::set_item_weight("weapon_rifle_ak47", ak_weight);
        inv::set_item_weight("weapon_melee", 2.f);

        // [infection]
        CHECK(!inv::infection_rules().enabled); // the last rules had none
        const inv::Rules infection = inv::apply_rules(
            "[infection]\nseconds = 90\nhit_seconds = 15\nspecials = [\"hunter\", \"charger\", \"tank\"]\nbots = true\n");
        CHECK(infection.errors.size() == 1); // "tank" is not one of them
        CHECK(inv::infection_rules().enabled && inv::infection_rules().seconds == 90 && inv::infection_rules().hit_seconds == 15);
        CHECK(inv::infection_rules().bots && inv::infection_rules().specials == std::vector<int>({3, 6}));
        CHECK(inv::infection_rules().no_cure); // the default: pills only reset the clock
        CHECK(inv::apply_rules("[infection]\nno_cure = false\n").errors.empty() && !inv::infection_rules().no_cure);
        CHECK(inv::infection_rules().cough); // the default
        CHECK(inv::apply_rules("[infection]\ncough = false\n").errors.empty() && !inv::infection_rules().cough);
        CHECK(!inv::apply_rules("[infection]\ncough = 3\n").errors.empty());
        CHECK(inv::infection_rules().insane && inv::infection_rules().insane_at == 70); // the defaults
        CHECK(inv::apply_rules("[infection]\ninsane = false\ninsane_at = 85\n").errors.empty() && !inv::infection_rules().insane &&
              inv::infection_rules().insane_at == 85);
        CHECK(!inv::apply_rules("[infection]\ninsane_at = 150\n").errors.empty() && inv::infection_rules().insane_at == 70);
        CHECK(!inv::apply_rules("[infection]\ninsane_at = 50.5\n").errors.empty());
        CHECK(!inv::apply_rules("[infection]\nenabled = false\n").errors.size() && !inv::infection_rules().enabled);
        inv::apply_rules("");

        // [stamina]
        CHECK(!inv::stamina_rules().enabled);
        const inv::Rules stamina = inv::apply_rules(
            "[stamina]\nrun = 5\njump = 20.5\nregen = 30\nslow = 50\ndamage = 25\nbots = true\nsprint = 3\nswing_speed = 5\n");
        CHECK(stamina.errors.size() == 2); // no "sprint"; swing_speed is at least 10
        CHECK(inv::stamina_rules().enabled && inv::stamina_rules().run == 5.f && inv::stamina_rules().jump == 20.5f);
        CHECK(inv::stamina_rules().regen == 30.f && inv::stamina_rules().slow == 50.f && inv::stamina_rules().damage == 25.f);
        CHECK(inv::stamina_rules().bots && inv::stamina_rules().swing_speed == 60.f); // the default kept
        CHECK(inv::stamina_rules().swing == 10.f && inv::stamina_rules().recover == 30.f); // the defaults
        CHECK(!inv::apply_rules("[stamina]\nrun = true\n").errors.empty());
        CHECK(!inv::apply_rules("[stamina]\nbots = 1\n").errors.empty());
        CHECK(inv::apply_rules("[stamina]\nenabled = false\n").errors.empty() && !inv::stamina_rules().enabled);
        inv::apply_rules("");
        CHECK(!inv::stamina_rules().enabled);

        // [melee]
        CHECK(!inv::melee_rules().enabled);
        const inv::Rules melee = inv::apply_rules("[melee]\nheavy_hold = 0.8\nheavy_damage = 300\ncleave = 50\nqueue = 0.8\nparry = 1\n");
        CHECK(melee.errors.size() == 2); // no "queue" (there is none), no "parry"
        CHECK(inv::melee_rules().enabled && inv::melee_rules().heavy_hold == 0.8f && inv::melee_rules().heavy_damage == 300.f);
        CHECK(inv::melee_rules().cleave == 50.f && inv::melee_rules().heavy_recovery == 150.f);
        CHECK(inv::melee_rules().light_damage == 40.f && inv::melee_rules().head_damage == 200.f); // the defaults
        CHECK(inv::melee_rules().wind_speed == 65.f);
        CHECK(!inv::apply_rules("[melee]\nwind_speed = 5\n").errors.empty() && inv::melee_rules().wind_speed == 65.f);
        CHECK(inv::apply_rules("[melee]\nhead_damage = 300\nlight_damage = 100\n").errors.empty() &&
              inv::melee_rules().head_damage == 300.f && inv::melee_rules().light_damage == 100.f);
        CHECK(!inv::apply_rules("[melee]\nheavy_hold = 0\n").errors.empty() && inv::melee_rules().heavy_hold == 0.6f);
        CHECK(inv::apply_rules("[melee]\nenabled = false\n").errors.empty() && !inv::melee_rules().enabled);
        inv::apply_rules("");
        CHECK(!inv::melee_rules().enabled);

        // the cleave: 70 % of the one before, never under 30 % of the first
        CHECK(inv::cleave_scale(0, 70.f, 30.f) == 1.f);
        CHECK(std::fabs(inv::cleave_scale(1, 70.f, 30.f) - 0.7f) < 1e-5f && std::fabs(inv::cleave_scale(2, 70.f, 30.f) - 0.49f) < 1e-5f);
        CHECK(std::fabs(inv::cleave_scale(5, 70.f, 30.f) - 0.3f) < 1e-5f);
        CHECK(inv::cleave_scale(3, 100.f, 30.f) == 1.f);

        // the wind-up: the attack is held back while held, given when let go until the weapon swings
        {
            using inv::MeleeInput;
            constexpr int a = inv::k_button_attack, wind = inv::k_button_winding, heavy = inv::k_button_heavy;
            MeleeInput in;
            // buttons, clock, melee in hand, can heavy, next attack, client clock, heavy_hold (no heavy shove)
            const auto cmd = [&](int b, double now, bool melee, bool can, float next, float clock, float hold) {
                inv::MeleeFrame f;
                f.buttons = b;
                f.now = now;
                f.melee = melee;
                f.can_heavy = can;
                f.next_attack = next;
                f.game_time = clock;
                f.hold = hold;
                return in.command(f);
            };
            CHECK(cmd(a, 0.0, false, true, 5.f, 100.f, 0.6f) == a); // no melee weapon: the game's own
            CHECK(cmd(a | 8, 0.0, true, true, 5.f, 100.f, 0.f) == (a | 8)); // [melee] off
            CHECK(cmd(0, 0.0, true, true, 5.f, 100.f, 0.6f) == 0);
            CHECK(cmd(a | 8, 1.0, true, true, 5.f, 100.f, 0.6f) == (8 | wind) && in.state() == MeleeInput::State::winding);
            CHECK(in.peek(a | 8) == (8 | wind));
            CHECK(cmd(a, 1.2, true, true, 5.f, 100.f, 0.6f) == wind);
            CHECK(cmd(0, 1.3, true, true, 5.f, 100.f, 0.6f) == a && !in.heavy()); // let go at 0.3 s: light
            CHECK(cmd(0, 1.35, true, true, 5.f, 100.f, 0.6f) == a); // the weapon has not swung yet
            CHECK(cmd(0, 1.4, true, true, 6.f, 100.f, 0.6f) == 0 && in.state() == MeleeInput::State::idle); // it swung
            // held 0.7 s: heavy
            cmd(a, 2.0, true, true, 6.f, 100.f, 0.6f);
            CHECK(cmd(0, 2.7, true, true, 6.f, 100.f, 0.6f) == (a | heavy) && in.heavy());
            CHECK(cmd(0, 2.8, true, true, 7.f, 100.f, 0.6f) == 0);
            // exhausted when it started: never heavy
            cmd(a, 3.0, true, false, 7.f, 100.f, 0.6f);
            CHECK(cmd(0, 4.0, true, true, 7.f, 100.f, 0.6f) == a);
            CHECK(cmd(0, 4.1, true, true, 8.f, 100.f, 0.6f) == 0);
            // a shove calls it off until the attack is let go
            cmd(a, 5.0, true, true, 8.f, 100.f, 0.6f);
            CHECK(cmd(a | inv::k_button_attack2, 5.1, true, true, 8.f, 100.f, 0.6f) == inv::k_button_attack2);
            CHECK(cmd(a, 5.5, true, true, 8.f, 100.f, 0.6f) == 0 && in.state() == MeleeInput::State::cancelled);
            CHECK(cmd(0, 5.6, true, true, 8.f, 100.f, 0.6f) == 0 && in.state() == MeleeInput::State::idle);
            // no queue: let go while the weapon is still waiting, the swing is dropped after a moment's grace
            cmd(a, 6.0, true, true, 8.f, 100.f, 0.6f);
            CHECK(cmd(0, 6.1, true, true, 8.f, 100.f, 0.6f) == a);
            CHECK(cmd(0, 6.15, true, true, 8.f, 100.f, 0.6f) == a);
            CHECK(cmd(0, 6.3, true, true, 8.f, 100.f, 0.6f) == 0 && in.state() == MeleeInput::State::idle);
            // pressed again before the swing came: winding from then on
            cmd(a, 8.0, true, true, 8.f, 100.f, 0.6f);
            cmd(0, 8.1, true, true, 8.f, 100.f, 0.6f);
            CHECK(cmd(a, 8.15, true, true, 8.f, 100.f, 0.6f) == a);
            CHECK(cmd(a, 8.17, true, true, 9.f, 100.f, 0.6f) == wind && in.winding_since() == 8.15);
            CHECK(cmd(0, 8.9, true, true, 9.f, 100.f, 0.6f) == (a | heavy));
            // the server's correction of the wait (a heavy swing's) while waiting to swing is not the swing
            cmd(a, 10.0, true, true, 9.f, 100.f, 0.6f);
            CHECK(cmd(0, 10.1, true, true, 9.f, 8.f, 0.6f) == a); // ready at 9; the clock is 8
            CHECK(cmd(0, 10.13, true, true, 9.5f, 8.03f, 0.6f) == a); // corrected to 9.5
            CHECK(cmd(0, 10.16, true, true, 9.5f, 8.06f, 0.6f) == a);
            CHECK(cmd(0, 10.19, true, true, 10.5f, 9.5f, 0.6f) == 0); // it swung
            // a weapon switch while releasing drops it
            CHECK(cmd(0, 9.0, false, true, 0.f, 100.f, 0.6f) == 0 && in.state() == MeleeInput::State::idle);
        }


        // the heavy shove, the throw, running out of stamina while winding up
        {
            using inv::MeleeInput;
            constexpr int a = inv::k_button_attack, s2 = inv::k_button_attack2, r = inv::k_button_reload;
            MeleeInput in;
            inv::MeleeFrame f;
            f.melee = true;
            f.hold = 0.6f;
            f.shove_hold = 0.5f;
            f.game_time = 100.f;
            f.next_shove = 3.f;
            const auto at = [&](int b, double now) {
                f.buttons = b;
                f.now = now;
                return in.command(f);
            };
            CHECK(at(s2, 1.0) == 0 && in.shove() == MeleeInput::Shove::holding); // held back
            CHECK(at(0, 1.1) == s2 && !in.heavy_shove()); // a tap: the game's shove when let go
            f.next_shove = 4.f;
            CHECK(at(0, 1.2) == 0 && in.shove() == MeleeInput::Shove::idle); // it shoved
            at(s2, 2.0);
            CHECK(at(0, 2.6) == (s2 | inv::k_button_heavy_shove) && in.heavy_shove()); // held 0.6 s: heavy
            f.next_shove = 5.f;
            at(0, 2.7);
            // the reload key while winding up throws: the throw bit for a moment, the swing called off
            at(a, 3.0);
            CHECK(at(a | r, 3.1) == inv::k_button_throw && in.state() == MeleeInput::State::cancelled && in.thrown_at() == 3.1);
            CHECK(at(a, 3.2) == inv::k_button_throw);
            CHECK(at(a, 3.3) == 0);
            CHECK(at(0, 3.4) == 0 && in.state() == MeleeInput::State::idle);
            // the server does not allow the throw: the reload key while winding up is just the reload key
            at(0, 3.45);
            f.throwing = false;
            at(a, 3.46);
            CHECK(at(a | r, 3.47) == (inv::k_button_winding | r) && in.state() == MeleeInput::State::winding);
            at(a, 3.48);
            f.throwing = true;
            at(0, 3.49);
            f.next_attack += 1.f;
            at(0, 3.495);
            // the reload key alone (a block) is the game's own
            CHECK(at(r, 3.5) == r && in.state() == MeleeInput::State::idle);
            at(0, 3.6);
            // out of stamina while winding up a heavy swing: it goes off by itself, light
            at(a, 4.0);
            f.can_heavy = false;
            CHECK(at(a, 4.5) == a && in.state() == MeleeInput::State::releasing && !in.heavy());
        }

        // the weapon classes: [melee]'s numbers scaled by the class, overridden per weapon
        {
            inv::apply_rules("[melee]\n[melee.weapons.katana]\nclass = \"blunt_1h\"\ndurability = 77\nheavy_hold = 1.5\n"
                             "[melee.weapons.frying_pan]\nstamina = 50\n");
            const inv::MeleeRules& m = inv::melee_rules();
            const inv::MeleeProfile axe = inv::melee_profile("fireaxe", m);
            const inv::MeleeProfile knife = inv::melee_profile("knife", m);
            CHECK(axe.klass == inv::MeleeClass::blade_2h && knife.klass == inv::MeleeClass::blade_1h);
            CHECK(axe.heavy_hold > m.heavy_hold && knife.heavy_hold < m.heavy_hold); // two hands wind up slower
            CHECK(axe.cleave > knife.cleave && knife.durability < axe.durability);
            CHECK(inv::melee_profile("custom_sword", m).klass == inv::MeleeClass::blunt_1h); // unknown: blunt one-handed
            const inv::MeleeProfile katana = inv::melee_profile("katana", m);
            CHECK(katana.klass == inv::MeleeClass::blunt_1h && katana.durability == 77 && katana.heavy_hold == 1.5f);
            CHECK(inv::melee_profile("frying_pan", m).stamina == 50.f);
            CHECK(inv::apply_rules("[melee]\ndurability = 0\n").errors.empty() && inv::melee_profile("fireaxe", inv::melee_rules()).durability == 0);
            CHECK(inv::apply_rules("[melee.weapons.fireaxe]\nclass = \"sword\"\nsharpness = 2\n").errors.size() == 2);
            CHECK(inv::apply_rules("[melee]\nhead_only = 1\n").errors.size() == 1);
            // each feature on or off
            CHECK(inv::melee_rules().blocking && inv::melee_rules().wall_bounce); // the defaults
            CHECK(inv::apply_rules("[melee]\nblocking = false\nwall_bounce = false\nthrowing = false\nheavy = false\n").errors.empty());
            CHECK(!inv::melee_rules().blocking && !inv::melee_rules().wall_bounce && !inv::melee_rules().throwing &&
                  !inv::melee_rules().heavy && inv::melee_rules().deflecting && inv::melee_rules().breaking);
            CHECK(inv::apply_rules("[melee]\nbreaking = 0\n").errors.size() == 1);
            CHECK(inv::apply_rules("[melee]\nqueueing = false\nwall_stagger = false\n").errors.size() == 2); // both gone
            inv::apply_rules("");
            // the overhead heavy swing: straight down first, else a diagonal one
            const int axe_dirs[] = {2, 3}, bat_dirs[] = {2, 7, 4}, pan_dirs[] = {5, 0};
            CHECK(inv::melee_overhead_swing(axe_dirs, 2) == -1 && inv::melee_overhead_swing(bat_dirs, 3) == 1 &&
                  inv::melee_overhead_swing(pan_dirs, 2) == 1 && inv::melee_overhead_swing(nullptr, 0) == -1);
            // wear: durability hits break it
            CHECK(inv::melee_wear_after(0, 1.f, 200) == 50 && inv::melee_wear_after(9990, 1.f, 200) == inv::k_wear_broken);
            CHECK(inv::melee_wear_after(100, 3.f, 0) == 100); // never breaks
        }

        // [ammo_piles]
        CHECK(!inv::ammo_pile_rules().enabled && inv::ammo_pile_rules().kinds.size() == 7);
        CHECK(inv::apply_rules("[ammo_piles]\n").errors.empty() && inv::ammo_pile_rules().enabled &&
              inv::ammo_pile_rules().min_magazines == 1 && inv::ammo_pile_rules().max_magazines == 3);
        const inv::Rules piles =
            inv::apply_rules("[ammo_piles]\nmin_magazines = 2\nmax_magazines = 4\nkinds = [\"smg\", \"auto_shotgun\", \"laser\"]\n");
        CHECK(piles.errors.size() == 1); // no "laser"
        CHECK(inv::ammo_pile_rules().min_magazines == 2 && inv::ammo_pile_rules().max_magazines == 4);
        CHECK((inv::ammo_pile_rules().kinds == std::vector<std::string>{"smg", "autoshotgun"}));
        CHECK(inv::apply_rules("[ammo_piles]\nmin_magazines = 3\nmax_magazines = 2\n").errors.size() == 1 &&
              inv::ammo_pile_rules().max_magazines == 3);
        CHECK(!inv::apply_rules("[ammo_piles]\nmax_magazines = 0\n").errors.empty());
        CHECK(!inv::apply_rules("[ammo_piles]\nmin_magazines = 1.5\n").errors.empty());
        CHECK(inv::apply_rules("[ammo_piles]\nenabled = false\n").errors.empty() && !inv::ammo_pile_rules().enabled);
        CHECK(inv::apply_rules("[ammo_piles]\n").errors.empty() && inv::ammo_pile_rules().replace_map_piles &&
              inv::ammo_pile_rules().extra == 4);
        CHECK(inv::apply_rules("[ammo_piles]\nreplace_map_piles = false\nextra = 10\n").errors.empty() &&
              !inv::ammo_pile_rules().replace_map_piles && inv::ammo_pile_rules().extra == 10);
        CHECK(!inv::apply_rules("[ammo_piles]\nextra = 51\n").errors.empty());
        CHECK(!inv::apply_rules("[ammo_piles]\nreplace_map_piles = 1\n").errors.empty());
        inv::apply_rules("");
        CHECK(!inv::ammo_pile_rules().enabled);

        // [map_guns]
        CHECK(!inv::map_gun_rules().enabled);
        CHECK(inv::apply_rules("[map_guns]\nmin_clip = 0\nmax_clip = 100\n").errors.empty() && inv::map_gun_rules().enabled &&
              inv::map_gun_rules().min_clip == 0 && inv::map_gun_rules().max_clip == 100);
        CHECK(inv::apply_rules("[map_guns]\n").errors.empty() && inv::map_gun_rules().min_clip == 10 &&
              inv::map_gun_rules().max_clip == 60);
        CHECK(inv::apply_rules("[map_guns]\nmin_clip = 70\nmax_clip = 20\n").errors.size() == 1 && inv::map_gun_rules().max_clip == 70);
        CHECK(!inv::apply_rules("[map_guns]\nmax_clip = 150\n").errors.empty());
        CHECK(!inv::apply_rules("[map_guns]\nclip = 5\n").errors.empty());
        CHECK(inv::apply_rules("[map_guns]\n").errors.empty() && inv::map_gun_rules().spawners_once);
        CHECK(inv::apply_rules("[map_guns]\nspawners_once = false\n").errors.empty() && !inv::map_gun_rules().spawners_once);
        CHECK(!inv::apply_rules("[map_guns]\nspawners_once = 1\n").errors.empty());
        CHECK(inv::apply_rules("[map_guns]\n").errors.empty() && inv::map_gun_rules().min_reserve == 0 &&
              inv::map_gun_rules().max_reserve == 1);
        CHECK(inv::apply_rules("[map_guns]\nmin_reserve = 2\nmax_reserve = 3\n").errors.empty() &&
              inv::map_gun_rules().min_reserve == 2 && inv::map_gun_rules().max_reserve == 3);
        CHECK(inv::apply_rules("[map_guns]\nmin_reserve = 3\nmax_reserve = 1\n").errors.size() == 1 &&
              inv::map_gun_rules().max_reserve == 3);
        CHECK(!inv::apply_rules("[map_guns]\nmax_reserve = 21\n").errors.empty());
        inv::apply_rules("");
        CHECK(!inv::map_gun_rules().enabled);

        // [ammo]: the most reserve rounds per ammo type
        CHECK(std::string(inv::ammo_kind("weapon_rifle_desert")) == "assaultrifle" && inv::ammo_kind("weapon_melee") == nullptr);
        CHECK(inv::ammo_max("assaultrifle") == 360 && !inv::ammo_max_set("assaultrifle")); // the game's default
        const inv::Rules ammo = inv::apply_rules("[ammo]\nassault_rifle = 500\nsmg = 300\nlaser = 5\n");
        CHECK(ammo.ammo_changed == 2 && ammo.errors.size() == 1);
        CHECK(inv::ammo_max("assaultrifle") == 500 && inv::ammo_max_set("assaultrifle") && inv::ammo_max("smg") == 300);
        CHECK(inv::ammo_max("shotgun") == 72);
        inv::set_ammo_max("assaultrifle", -1);
        inv::set_ammo_max("smg", -1);

        // [round_weight] and what the reserve weighs
        CHECK(inv::round_weight("assaultrifle") == 0.01f && inv::round_weight("grenadelauncher") == 0.15f);
        CHECK(inv::ammo_magazine("shotgun") == 8 && std::string(inv::ammo_title("smg")) == "SMG rounds");
        CHECK(std::string(inv::ammo_kind_at(3)) == "assaultrifle" && inv::ammo_kind_at(1) == nullptr); // 1: the pistols
        int pool[32] = {};
        pool[3] = 360; // rifle
        pool[7] = 50; // shotgun
        pool[1] = 999; // the pistols' endless reserve weighs nothing
        CHECK(std::fabs(inv::ammo_weight(pool) - (3.6f + 2.f)) < 1e-4f);
        const inv::Rules rounds = inv::apply_rules("[round_weight]\nassault_rifle = 0.02\nlaser = 1\nsmg = -1\n");
        CHECK(rounds.errors.size() == 2 && inv::round_weight("assaultrifle") == 0.02f && inv::round_weight("smg") == 0.006f);
        inv::Inventory loaded({.slots = 2, .weapon_slots = 1});
        loaded.set_ammo_weight(inv::ammo_weight(pool));
        CHECK(std::fabs(loaded.weight() - (7.2f + 2.f)) < 1e-4f);
        inv::apply_rules("");
        CHECK(inv::round_weight("assaultrifle") == 0.01f); // back to the default

        // unloading: which guns, and where the magazine's rounds go
        CHECK(inv::unloadable("weapon_rifle_ak47") && inv::unloadable("weapon_sniper_awp") &&
              inv::unloadable("weapon_grenade_launcher"));
        CHECK(!inv::unloadable("weapon_rifle_m60") && !inv::unloadable("weapon_chainsaw") &&
              !inv::unloadable("weapon_pistol") && !inv::unloadable("weapon_melee") && !inv::unloadable("weapon_gascan"));
        const inv::UnloadSplit all_in = inv::split_unload("assaultrifle", 50, 100, -1.f);
        CHECK(all_in.to_reserve == 50 && all_in.to_ground == 0);
        const inv::UnloadSplit near_max = inv::split_unload("assaultrifle", 50, 340, -1.f); // the most is 360
        CHECK(near_max.to_reserve == 20 && near_max.to_ground == 30);
        const inv::UnloadSplit at_max = inv::split_unload("assaultrifle", 50, 400, -1.f); // over it already (a rules change)
        CHECK(at_max.to_reserve == 0 && at_max.to_ground == 50);
        const inv::UnloadSplit heavy = inv::split_unload("sniperrifle", 30, 0, 0.25f); // 0.02 each: 12 fit
        CHECK(heavy.to_reserve == 12 && heavy.to_ground == 18);
        const inv::UnloadSplit exact = inv::split_unload("shotgun", 8, 0, 0.32f); // 0.04 each, exactly 8
        CHECK(exact.to_reserve == 8 && exact.to_ground == 0);
        const inv::UnloadSplit no_room = inv::split_unload("smg", 50, 0, 0.f);
        CHECK(no_room.to_reserve == 0 && no_room.to_ground == 50);
        const inv::UnloadSplit empty = inv::split_unload("smg", 0, 0, -1.f);
        CHECK(empty.to_reserve == 0 && empty.to_ground == 0);
        inv::apply_rules("[ammo]\nassault_rifle = 60\n");
        CHECK(inv::split_unload("assaultrifle", 40, 30, -1.f).to_reserve == 30); // [ammo] counts
        inv::set_ammo_max("assaultrifle", -1);

        inv::set_item_weight("weapon_first_aid_kit", medkit_weight);
        inv::set_item_weight("weapon_pain_pills", 1.f);
        inv::set_item_stack("weapon_molotov", molotov_stack);
    }
} // namespace

int main()
{
    catalogue();
    add_stacks_then_new_slots();
    add_stops_when_full();
    weight_limit();
    remove();
    moving();
    selection();
    text_round_trip();
    revision();
    held_box();
    weapons();
    rules_file();
    if (g_failed == 0)
        std::printf("all inventory tests passed\n");
    return g_failed == 0 ? 0 : 1;
}
