"""Builds inventory_mod_menu.vpk, the addon that puts the mod's settings into the game's own menus, into the addons
folder next to the mod's DLL.

    python -I tools/make_menu_addon.py [--game <Left 4 Dead 2 folder>] [--out <file.vpk>]

It holds:
  resource/UI/L4D360UI/OptionsFlyout.res   the game's Options flyout (main menu and in-game menu), read from the game's
                                           own VPKs, with one more button under "Steam Cloud": "Inventory Mod" (its
                                           command "InventoryModSettings" opens the page below, see overlay.cpp)
  resource/UI/L4D360UI/AudioVideo.res      the page: the Xbox-only AudioVideo page, which the mod takes over (its own
                                           code switched off, see overlay.cpp), laid out like the game's Video page: a
                                           drop-down for each of SETTINGS in two columns, "Inventory window" (the mod's
                                           own window) and Done
  resource/UI/L4D360UI/InvMod_<key>.res    each drop-down's choices; a choice's command is "InvMod <key> <value>"
The mod mounts its addons ahead of the game's files, so these copies are the ones the menus read.

SETTINGS must match k_page_settings in src/ui/overlay.cpp (the keys and, for numbers, the values) and
apply_queued_settings in src/ui/hud.cpp.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "re"))
sys.path.insert(0, HERE)
import vpk  # noqa: E402
from make_rounds_addon import GAME, make_vpk  # noqa: E402

ROOT = os.path.dirname(HERE)
FLYOUT = "resource/ui/l4d360ui/optionsflyout.res"
PAGE = "resource/ui/l4d360ui/audiovideo.res"
OPEN_COMMAND = "InventoryModSettings"
WINDOW_COMMAND = "InventoryModWindow"

ON_OFF = [("1", "On"), ("0", "Off")]
# The keys a key can be (Windows virtual-key codes, as inventory_mod_settings.toml has them): the letters the game does
# not use for moving, use, reload, the last weapon or chat, some punctuation and the editing keys.
KEYS = [("0", "None")] + [(str(ord(c)), c) for c in "BCFGHIJKLMNOPTUVXZ"] + [
    ("219", "["), ("221", "]"), ("220", "Backslash"), ("186", ";"), ("222", "'"), ("188", ","), ("190", "."), ("191", "/"),
    ("45", "Insert"), ("46", "Delete"), ("36", "Home"), ("33", "Page Up"), ("34", "Page Down")]
KEY_COLUMNS = 3
LIMIT_NOTE = " (A host's inventory_mod.toml wins over this; it applies after a restart.)"
# (key, label, tooltip, [(value, label)], column): the settings of inventory_mod_settings.toml the page shows
SETTINGS = [
    ("show_hotbar", "Show the item boxes", "The mod's item boxes beside the game's weapon panel (or its hotbar).", ON_OFF),
    ("inventory_view", "Inventory view", "Slots: the boxes. Wheel: hold the middle mouse button.",
     [("0", "Slots and wheel"), ("1", "Slots only"), ("2", "Wheel only")]),
    ("auto_store", "Store items on use", "E on an item stores it even when you could carry it.", ON_OFF),
    ("replace_native", "Weapon panel", "Beside: the game's panel stays, the boxes under it. Replaced: the mod's hotbar.",
     [("0", "Beside the game's"), ("1", "Replaced")]),
    ("hud_scale", "HUD scale", "The size of the mod's boxes, wheel and window.",
     [("0.75", "75%"), ("1", "100%"), ("1.25", "125%"), ("1.5", "150%"), ("2", "200%"), ("2.5", "250%"), ("3", "300%")]),
    ("infection_effect", "Infection: screen effect", "The picture throbs, sways and drains while infected.", ON_OFF),
    ("infection_cover_game_hud", "Infection: over the game's HUD", "Off: the effect stays under the game's HUD.", ON_OFF),
    ("infection_cover_mod_hud", "Infection: over the mod's HUD", "Off: the mod's boxes and wheel stay clear.", ON_OFF),
    ("infection_sound", "Infection: sound", "The sound muffled and warped, a heartbeat, ringing.", ON_OFF),
    ("infection_screamer", "Infection: screamer", "A face and a scream when the clock runs out.", ON_OFF),
    ("screamer_chance", "Infection: screamer chance", "How often the screamer comes when the clock runs out.",
     [("0", "Never"), ("0.1", "10%"), ("0.25", "25%"), ("0.5", "50%"), ("0.75", "75%"), ("1", "Always")]),
    ("stamina_bar", "Stamina: bar", "A thin bar under the crosshair while stamina is not full.", ON_OFF),
    ("stamina_vignette", "Stamina: vignette", "The screen's edges darken as stamina runs low.", ON_OFF),
    ("stamina_cover_game_hud", "Stamina: over the game's HUD", "Off: the vignette stays under the game's HUD.", ON_OFF),
    ("stamina_cover_mod_hud", "Stamina: over the mod's HUD", "Off: the mod's boxes and wheel stay clear.", ON_OFF),
    ("key_toggle", "Key: inventory window", "Opens and closes the mod's inventory window.", KEYS),
    ("key_prev", "Key: previous item", "Selects the previous box.", KEYS),
    ("key_next", "Key: next item", "Selects the next box.", KEYS),
    ("key_use", "Key: use item", "Takes the selected item in your hands.", KEYS),
    ("key_drop", "Key: drop", "Drops what is in your hands (or the selected box's item).", KEYS),
    ("key_store", "Key: store", "Stores the item or weapon in your hands.", KEYS),
    ("key_pickup", "Key: pick up", "Takes the item or weapon in front of you into the inventory.", KEYS),
    ("key_unload", "Key: unload gun", "Empties the gun in your hands (or the one you look at) into your reserve.", KEYS),
    ("slots", "Item boxes", "The most item boxes." + LIMIT_NOTE,
     [(str(n), str(n)) for n in (4, 6, 8, 10, 12, 16, 20, 24)]),
    ("weapon_slots", "Weapon boxes", "The most boxes for stored guns and melee weapons." + LIMIT_NOTE,
     [(str(n), str(n)) for n in (0, 1, 2, 3, 4, 6, 8)]),
    ("max_weight", "Weight limit", "What you can carry." + LIMIT_NOTE,
     [("0", "None")] + [(str(n), str(n)) for n in (10, 15, 20, 25, 30, 40, 50)]),
    ("melee_ring", "Melee: wind-up ring", "A ring at the crosshair fills while a melee swing winds up.", ON_OFF),
]
# the right-hand column: the keys, the limits and the melee's ring (the left one is full: its buttons end at the frame)
RIGHT = {"key_toggle", "key_prev", "key_next", "key_use", "key_drop", "key_store", "key_pickup", "key_unload", "slots", "weapon_slots",
         "max_weight", "melee_ring"}

FLYOUT_BUTTON = '''
	"BtnInventoryMod"	[$WIN32]
	{
		"ControlName"			"L4D360HybridButton"
		"fieldName"				"BtnInventoryMod"
		"xpos"					"0"
		"ypos"					"120"
		"wide"					"150"
		"tall"					"20"
		"autoResize"			"1"
		"pinCorner"				"0"
		"visible"				"1"
		"enabled"				"1"
		"tabPosition"			"0"
		"proportionalToParent"	"1"
		"navUp"					"BtnCloud"
		"navDown"				"BtnVideo"
		"tooltiptext"			"The inventory mod's settings."
		"labelText"				"Inventory Mod"
		"style"					"FlyoutMenuButton"
		"command"				"%s"
	}
''' % OPEN_COMMAND


def block(text, name):
    """(start, end) of the `"name" [condition] { ... }` block (its braces balanced; not a mention of the name as a
    value, such as another button's "navUp")."""
    match = re.search(r'\n[ \t]*"%s"[ \t]*(\[[^\]]*\])?\s*\{' % re.escape(name), text)
    if match is None:
        raise SystemExit(f"{name}: no block")
    at = match.start() + 1
    open_at = match.end() - 1
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return at, i + 1
    raise SystemExit(f"{name}: no end")


def set_in_block(text, name, key, old, new):
    start, end = block(text, name)
    part = text[start:end]
    pattern = re.compile(r'("%s"\s+)"%s"' % (re.escape(key), re.escape(old)))
    part, n = pattern.subn(lambda m: m.group(1) + '"%s"' % new, part, count=1)
    if n != 1:
        raise SystemExit(f"{name}: no {key} {old}")
    return text[:start] + part + text[end:]


def kv(pairs, indent):
    return "".join('%s"%s"%s"%s"\n' % (indent, k, " " * max(1, 24 - len(k)), v) for k, v in pairs)


def choices_res(key, options):
    """A drop-down's flyout: one button per choice; the keys' long list in columns (a grid read down the columns)."""
    count = len(options)
    columns = KEY_COLUMNS if options is KEYS else 1
    rows = (count + columns - 1) // columns
    width = 150 if columns == 1 else 70
    out = ['"Resource/UI/InvMod_%s.res"\n{\n' % key]
    out.append('\t"PnlBackground"\n\t{\n' + kv([
        ("ControlName", "Panel"), ("fieldName", "PnlBackground"), ("xpos", "0"), ("ypos", "0"), ("zpos", "-1"),
        ("wide", str(columns * width + 6)), ("tall", str(rows * 20 + 5)), ("visible", "1"), ("enabled", "1"),
        ("paintbackground", "1"), ("paintborder", "1")], "\t\t") + "\t}\n")
    for i, (value, label) in enumerate(options):
        column, row = divmod(i, rows)
        out.append('\n\t"Btn%d"\n\t{\n' % i + kv([
            ("ControlName", "L4D360HybridButton"), ("fieldName", "Btn%d" % i), ("xpos", str(column * width)),
            ("ypos", str(row * 20)), ("wide", str(width)), ("tall", "20"), ("autoResize", "1"), ("pinCorner", "0"),
            ("visible", "1"), ("enabled", "1"), ("tabPosition", "0"), ("wrap", "1"),
            ("navUp", "Btn%d" % ((i - 1) % count)), ("navDown", "Btn%d" % ((i + 1) % count)),
            ("navLeft", "Btn%d" % ((i - rows) % count)), ("navRight", "Btn%d" % ((i + rows) % count)),
            ("labelText", label), ("style", "FlyoutMenuButton"), ("command", "InvMod %s %s" % (key, value)),
            ("OnlyActiveUser", "1")], "\t\t") + "\t}\n")
    out.append("}\n")
    return "".join(out)


def page_res():
    """The page: the game's Video page's look, a drop-down per setting in two columns (keys and limits on the right),
    the two buttons under the left one."""
    left = ["Drp_" + key for key, _, _, _ in SETTINGS if key not in RIGHT] + ["BtnWindow", "BtnDone"]
    right = ["Drp_" + key for key, _, _, _ in SETTINGS if key in RIGHT]
    nav = {}
    for column in (left, right):
        for i, n in enumerate(column):
            nav[n] = (column[i - 1], column[(i + 1) % len(column)])
    # left and right: across to the same row of the other column (the last row there when it is shorter)
    across = {}
    for i, n in enumerate(left):
        across[n] = right[min(i, len(right) - 1)]
    for i, n in enumerate(right):
        across[n] = left[min(i, len(left) - 1)]
    out = ['"Resource/UI/AudioVideo.res"\n{\n']
    out.append('\t"AudioVideo"\n\t{\n' + kv([
        ("ControlName", "Frame"), ("fieldName", "AudioVideo"), ("xpos", "0"), ("ypos", "0"), ("wide", "f0"),
        ("tall", "448"), ("autoResize", "0"), ("pinCorner", "0"), ("visible", "1"), ("enabled", "1"),
        ("tabPosition", "0")], "\t\t") + "\t}\n")
    out.append('\n\t"ImgBackground"\n\t{\n' + kv([
        ("ControlName", "L4DMenuBackground"), ("fieldName", "ImgBackground"), ("xpos", "0"), ("ypos", "79"),
        ("zpos", "-1"), ("wide", "f0"), ("tall", "350"), ("autoResize", "0"), ("pinCorner", "0"), ("visible", "1"),
        ("enabled", "1"), ("tabPosition", "0"), ("fillColor", "0 0 0 0")], "\t\t") + "\t}\n")
    rows = {"left": 0, "right": 0}
    for key, label, tip, options in SETTINGS:
        name = "Drp_" + key
        up, down = nav[name]
        side = "right" if key in RIGHT else "left"
        x = "c+15" if side == "right" else "c-300"
        y = 100 + 20 * rows[side]
        rows[side] += 1
        out.append('\n\t"%s"\n\t{\n' % name + kv([
            ("ControlName", "DropDownMenu"), ("fieldName", name), ("xpos", x), ("ypos", str(y)), ("zpos", "3"),
            ("wide", "285"), ("tall", "15"), ("visible", "1"), ("enabled", "1"), ("usetitlesafe", "0"),
            ("tabPosition", "0"), ("navUp", up), ("navDown", down), ("navLeft", across[name]),
            ("navRight", across[name])], "\t\t"))
        out.append('\n\t\t"BtnDropButton"\n\t\t{\n' + kv([
            ("ControlName", "L4D360HybridButton"), ("fieldName", "BtnDropButton"), ("xpos", "0"), ("ypos", "0"),
            ("zpos", "0"), ("wide", "285"), ("wideatopen", "165"), ("tall", "15"), ("autoResize", "1"),
            ("pinCorner", "0"), ("visible", "1"), ("enabled", "1"),
            ("tabPosition", "1" if key == SETTINGS[0][0] else "0"), ("AllCaps", "1"), ("labelText", label),
            ("tooltiptext", tip), ("style", "DropDownButton"), ("command", "Flm_" + key), ("ActivationType", "1"),
            ("OnlyActiveUser", "1")], "\t\t\t") + "\t\t}\n\t}\n")
        out.append('\n\t"Flm_%s"\n\t{\n' % key + kv([
            ("ControlName", "FlyoutMenu"), ("fieldName", "Flm_" + key), ("visible", "0"), ("wide", "0"), ("tall", "0"),
            ("zpos", "4"), ("InitialFocus", "Btn0"), ("ResourceFile", "resource/UI/L4D360UI/InvMod_%s.res" % key),
            ("OnlyActiveUser", "1")], "\t\t") + "\t}\n")
        y += 20
    y = 100 + 20 * rows["left"] + 10
    for name, label, tip, command in (("BtnWindow", "Inventory window", "The mod's own window: the inventory, debugging.",
                                       WINDOW_COMMAND),
                                      ("BtnDone", "#L4D360UI_Done", "", "Back")):
        up, down = nav[name]
        out.append('\n\t"%s"\n\t{\n' % name + kv([
            ("ControlName", "L4D360HybridButton"), ("fieldName", name), ("xpos", "c-300"), ("ypos", str(y)),
            ("zpos", "0"), ("wide", "260"), ("tall", "15"), ("autoResize", "1"), ("pinCorner", "0"), ("visible", "1"),
            ("enabled", "1"), ("tabPosition", "0"), ("wrap", "1"), ("navUp", up), ("navDown", down),
            ("navLeft", across[name]), ("navRight", across[name]), ("AllCaps", "1"),
            ("labelText", label), ("tooltiptext", tip), ("style", "DefaultButton"), ("command", command),
            ("EnabledTextInsetX", "2"), ("DisabledTextInsetX", "2"), ("FocusTextInsetX", "2"), ("OpenTextInsetX", "2")],
            "\t\t") + "\t}\n")
        y += 20
    out.append("}\n")
    return "".join(out)


def main(argv):
    game = GAME
    out = os.path.join(ROOT, "out", "Release", "addons", "inventory_mod_menu.vpk")
    i = 1
    while i < len(argv):
        if argv[i] == "--game":
            game, i = argv[i + 1], i + 2
        elif argv[i] == "--out":
            out, i = argv[i + 1], i + 2
        else:
            print(__doc__)
            return 1
    text = None
    for d in ("update", "left4dead2"):
        pak = vpk.Vpk(os.path.join(game, d, "pak01_dir.vpk"))
        if FLYOUT in pak.entries:
            text = pak.read(FLYOUT).decode("latin-1")
            break
    if text is None:
        raise SystemExit(f"{FLYOUT} is not in the game's VPKs")
    if OPEN_COMMAND in text:
        raise SystemExit("the game's flyout has the button already?")
    # the background one button taller, the last button and the first pointing at the new one
    text = set_in_block(text, "PnlBackground", "tall", "125", "145")
    text = set_in_block(text, "BtnCloud", "navDown", "BtnVideo", "BtnInventoryMod")
    text = set_in_block(text, "BtnVideo", "navUp", "BtnCloud", "BtnInventoryMod")
    last = text.rindex("}")
    text = text[:last] + FLYOUT_BUTTON + text[last:]
    files = {
        FLYOUT: text.encode("latin-1"),
        PAGE: page_res().encode("latin-1"),
        "addoninfo.txt": ('"AddonInfo"\n{\n\taddonSteamAppID 550\n\taddontitle "Inventory mod: menus"\n'
                          '\taddonversion 1\n\taddonauthor "inventory mod"\n'
                          '\taddonDescription "Options > Inventory Mod: the inventory mod\'s settings."\n}\n').encode(),
    }
    for key, _, _, options in SETTINGS:
        files["resource/ui/l4d360ui/invmod_%s.res" % key.lower()] = choices_res(key, options).encode("latin-1")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(make_vpk(files))
    print(f"{out}: the Options flyout with \"Inventory Mod\", its page with {len(SETTINGS)} settings")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
