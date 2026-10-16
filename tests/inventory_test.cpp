// Tests of the inventory model.  A plain program: exit code 0 when every check passes.

#include "inv/inventory.hpp"

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
        CHECK(inv::find_item("weapon_rifle") == nullptr);
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
        CHECK(c.slot(1).empty());
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
    if (g_failed == 0)
        std::printf("all inventory tests passed\n");
    return g_failed == 0 ? 0 : 1;
}
