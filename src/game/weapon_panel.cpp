// The game's own weapon-selection panel (client.dll): hidden, or its item boxes, while the mod draws them.

#include "game/weapon_panel.hpp"

#include "game/sdk.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"

namespace game::detail
{

    using namespace sdk;

    core::Hook<ShouldDrawFn> g_weapon_select;

    bool __fastcall hk_weapon_select_should_draw(void* self, void* edx)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            INV_LOG("CHudWeaponSelection::ShouldDraw called (this %p), replace_native %d", self,
                    core::config().replace_native ? 1 : 0);
        }
        if (core::config().replace_native)
            return false;
        return g_weapon_select.original()(self, edx);
    }

    // CHudWeaponSelection's Paint (panel vtable 0x105D5734 slot 128, client.dll + 0x2D11E0) draws its boxes one
    // after the other, unrolled: the gun (slot 0), the melee / pistol (slot 1), a part that ends with the key label
    // of box 5 and draws the throwable box too (seen in the game: skipping only the next three parts left it), then
    // slots 2..4.  Beside the game's panel the mod draws the item boxes itself, in its grid, so the hook below sits
    // on the first instruction after the melee part (after `push 1; mov ecx, ebx; call 0x102CE980`, its key) and,
    // while g_hide_item_boxes is set, jumps to the code after the slot-4 part (the same stack depth: both are
    // statement boundaries in the same function).
    volatile bool g_hide_item_boxes = false;
    std::uintptr_t g_item_boxes_end = 0;
    core::Hook<ItemBoxesFn> g_item_boxes;
    std::uintptr_t g_item_boxes_original = 0;

    __declspec(naked) void hk_item_boxes()
    {
        __asm {
            cmp byte ptr [g_hide_item_boxes], 0
            je draw
            jmp dword ptr [g_item_boxes_end]
        draw:
            jmp dword ptr [g_item_boxes_original]
        }
    }

    // With nothing in slot 1 (no melee or pistol: seen in the game, the item boxes came back) the melee part is
    // left by `je` straight to its last statement, after the hook above (0x102D17BF -> 0x102D1B59, `mov eax,
    // [ebp-0x64]; push eax; mov ecx, ebx; call 0x102CDDF0`): a second hook there, to the same end.
    core::Hook<ItemBoxesFn> g_item_boxes_empty;
    std::uintptr_t g_item_boxes_empty_original = 0;

    __declspec(naked) void hk_item_boxes_empty()
    {
        __asm {
            cmp byte ptr [g_hide_item_boxes], 0
            je draw
            jmp dword ptr [g_item_boxes_end]
        draw:
            jmp dword ptr [g_item_boxes_empty_original]
        }
    }

    void hide_item_boxes(bool hide)
    {
        g_hide_item_boxes = hide;
    }

    void install_weapon_panel(const mem::ModuleInfo& client)
    {
        // The game's own weapon-selection panel (client.dll, CHudWeaponSelection): hidden while the mod replaces it.
        // ShouldDraw is vtable slot 12 of the panel's CHudElement part (vtable RVA 0x5D5A84).
        const std::uintptr_t should_draw = mem::read_or<std::uintptr_t>(client.base + k_rva_weapon_select_vtable + 4u * 12);
        if (should_draw == client.base + k_rva_weapon_select_should_draw &&
            mem::find_pattern(should_draw, 7, "56 57 6A FF 8B F9 E8") == should_draw)
        {
            if (g_weapon_select.create("WeaponSelectShouldDraw", reinterpret_cast<void*>(should_draw),
                                       hk_weapon_select_should_draw))
            {
                g_weapon_select.enable();
                INV_LOG("game: native weapon panel hook at %p", reinterpret_cast<void*>(should_draw));
            }
        }
        else
            INV_LOG("game: CHudWeaponSelection::ShouldDraw (%p) does not look as expected: the native panel stays",
                    reinterpret_cast<void*>(should_draw));

        // its Paint: the part that draws the item boxes (see hk_item_boxes)
        const std::uintptr_t paint = mem::read_or<std::uintptr_t>(client.base + k_rva_weapon_select_panel_vtable + 4u * 128);
        const std::uintptr_t start =
            paint == client.base + k_rva_weapon_select_paint
                ? mem::find_pattern(paint, 0xD80, "6A 01 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 45 AC DB 45")
                : 0;
        const std::uintptr_t end =
            start != 0 ? mem::find_pattern(start, 0x500, "6A 04 8B CB E8 ?? ?? ?? ?? 83 BD 78 FF FF FF 00 7C") : 0;
        if (start != 0 && end != 0 &&
            g_item_boxes.create("WeaponSelectItemBoxes", reinterpret_cast<void*>(start + 9), hk_item_boxes))
        {
            g_item_boxes_end = end + 9;
            g_item_boxes_original = reinterpret_cast<std::uintptr_t>(g_item_boxes.original());
            g_item_boxes.enable();
            INV_LOG("game: native item boxes from %p to %p", reinterpret_cast<void*>(start + 9),
                    reinterpret_cast<void*>(g_item_boxes_end));

            // the way past the melee part when slot 1 is empty: push 1; mov ecx, ebx; mov [ebp-0x3C], edi;
            // call 0x102CEB20 (the weapon in slot 1); mov [ebp-0x54], eax; test eax, eax; je <target>
            const std::uintptr_t check =
                mem::find_pattern(paint, 0xD80, "6A 01 8B CB 89 7D C4 E8 ?? ?? ?? ?? 89 45 AC 85 C0 0F 84");
            const std::uintptr_t target =
                check != 0 ? check + 0x17 + static_cast<std::uintptr_t>(mem::read_or<std::int32_t>(check + 0x13)) : 0;
            if (target > check && target < start + 9 + 0x100 &&
                mem::find_pattern(target, 7, "8B 45 9C 50 8B CB E8") == target &&
                g_item_boxes_empty.create("WeaponSelectItemBoxesEmpty", reinterpret_cast<void*>(target), hk_item_boxes_empty))
            {
                g_item_boxes_empty_original = reinterpret_cast<std::uintptr_t>(g_item_boxes_empty.original());
                g_item_boxes_empty.enable();
                INV_LOG("game: native item boxes (no melee) from %p", reinterpret_cast<void*>(target));
            }
            else
                INV_LOG("game: the no-melee way into the item boxes (%p) does not look as expected: with slot 1 empty "
                        "they show", reinterpret_cast<void*>(target));
        }
        else
            INV_LOG("game: CHudWeaponSelection::Paint (%p) does not look as expected: its item boxes stay",
                    reinterpret_cast<void*>(paint));
    }

} // namespace game::detail
