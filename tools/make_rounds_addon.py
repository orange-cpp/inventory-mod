"""Builds the addon with the ammo pack of each round type, inventory_mod_rounds.vpk, in the addons folder next to the
mod's DLL.

    python -I tools/make_rounds_addon.py [--game <Left 4 Dead 2 folder>] [--out <file.vpk>] [--install]

For each texture in assets/textures (KINDS below: file -> round type) it makes
    materials/models/inv_mod/rounds_<kind>.vtf / .vmt   the texture (1024x512 DXT5 with mips; the alpha is the
                                                        game's own pack's shine mask) and its material
    models/inv_mod/rounds_<kind>.mdl / .vvd / .dx90.vtx / .phy
                                                        a copy of the game's world model of the incendiary ammo pack
                                                        (w_eq_incendiary_ammopack) whose one texture is that one
The model files and the shine mask are read from the game's own VPKs (they are Valve's, not in this repository).  The
copies are the game's files with two strings changed in place (the model's name and its texture's path, both
shorter than before), so nothing is recompiled and the checksums the .vvd / .vtx carry still match.
It goes to out/Release/addons: the mod mounts every .vpk in the addons folder next to its DLL when it loads (game.cpp,
mount_addons), so the game needs no copy.  --install also copies it into the game's left4dead2/addons (mounted when the game starts).
The mod uses models/inv_mod/rounds_<kind>.mdl for a pile of that type when the game can find it (see game.cpp).
"""
import io
import os
import struct
import sys
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "re"))
import vpk  # noqa: E402
import vtf_to_png  # noqa: E402

ROOT = os.path.dirname(HERE)
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2"
KINDS = {  # assets/textures file -> round type (inv::ammo_index names)
    "rifle_rounds_ammo_pack.png": "assaultrifle",
    "smg_rounds_ammo_pack.png": "smg",
    "shotgun_rounds_ammo_pack.png": "shotgun",
    "auto_shotgun_rounds_ammo_pack.png": "autoshotgun",
    "haunting_riffle_rounds_ammo_pack.png": "huntingrifle",
    "sniper_rounds_ammo_pack.png": "sniperrifle",
    "grenade_rounds_ammo_pack.png": "grenadelauncher",
}
MODEL = "models/w_models/weapons/w_eq_incendiary_ammopack"
TEXTURE = "models/props/terror/incendiary_ammopack"
SIZE = (1024, 512)


# ---- the texture --------------------------------------------------------------------------------------------------

def dxt(image, fmt):
    """Pillow's DDS writer, the block data only (no header)."""
    buf = io.BytesIO()
    image.save(buf, "DDS", pixel_format=fmt)
    data = buf.getvalue()[128:]
    w, h = image.size
    want = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == "DXT1" else 16)
    if len(data) != want:
        raise SystemExit(f"{fmt} {w}x{h}: {len(data)} bytes, {want} expected")
    return data


def make_vtf(rgba):
    """VTF 7.2, DXT5, all mips (largest last), a 16x8 DXT1 thumbnail."""
    w, h = rgba.size
    mips = []
    level = rgba
    while True:
        mips.append(level)
        if level.size == (1, 1):
            break
        level = rgba.resize((max(1, level.size[0] // 2), max(1, level.size[1] // 2)), Image.LANCZOS)
    thumb = rgba.convert("RGB").resize((16, 8), Image.LANCZOS)
    pixels = rgba.convert("RGB").resize((1, 1), Image.BOX).getpixel((0, 0))
    reflectivity = [(c / 255.0) ** 2.2 for c in pixels]
    header = bytearray(80)
    struct.pack_into("<4sIII", header, 0, b"VTF\0", 7, 2, 80)
    struct.pack_into("<HHIHH", header, 16, w, h, 0x2000, 1, 0)  # flags: TEXTUREFLAGS_EIGHTBITALPHA
    struct.pack_into("<3f", header, 32, *reflectivity)
    struct.pack_into("<f", header, 48, 1.0)  # bumpmap scale
    struct.pack_into("<iB", header, 52, 15, len(mips))  # DXT5
    struct.pack_into("<iBBH", header, 57, 13, 16, 8, 1)  # thumbnail DXT1 16x8, depth 1
    out = bytearray(header) + dxt(thumb, "DXT1")
    for level in reversed(mips):
        out += dxt(level, "DXT5")
    return bytes(out)


def make_vmt(kind):
    return ('"VertexlitGeneric"\n{\n'
            f'\t"$basetexture" "models/inv_mod/rounds_{kind}"\n'
            '\t"$envmap" "env_cubemap"\n'
            '\t"$basealphaenvmapmask" "1"\n'
            '\t"$envmapfresnel" "1"\n'
            '\t"$envmapfresnelMinMaxExp" "[0.4 1 3]"\n'
            '}\n').encode()


# ---- the model ----------------------------------------------------------------------------------------------------

def patch_string(data, old, new):
    """`old` (a NUL-terminated string in the file) becomes `new`, NUL-padded: never longer, so no offset moves."""
    old_b, new_b = old.encode() + b"\0", new.encode() + b"\0"
    if len(new_b) > len(old_b):
        raise SystemExit(f"{new} is longer than {old}")
    at = data.find(old_b)
    if at < 0 or data.find(old_b, at + 1) >= 0:
        raise SystemExit(f"{old}: not found once in the model")
    data[at: at + len(old_b)] = new_b + b"\0" * (len(old_b) - len(new_b))


def make_mdl(mdl, kind):
    data = bytearray(mdl)
    name = f"inv_mod\\rounds_{kind}.mdl"
    data[12:12 + 64] = name.encode().ljust(64, b"\0")  # studiohdr_t name[64]
    patch_string(data, TEXTURE, f"models/inv_mod/rounds_{kind}")
    return bytes(data)


# ---- the VPK ------------------------------------------------------------------------------------------------------

def make_vpk(files):
    """VPK version 1, one file (everything in it: archive index 0x7FFF)."""
    tree = {}
    for path in sorted(files):
        directory, name = os.path.split(path)
        stem, ext = name.rsplit(".", 1)
        if stem.endswith(".dx90"):  # (VPK names have one extension: "x.dx90" + "vtx")
            pass
        tree.setdefault(ext, {}).setdefault(directory or " ", []).append((stem, path))
    body = bytearray()
    blob = bytearray()
    for ext in sorted(tree):
        body += ext.encode() + b"\0"
        for directory in sorted(tree[ext]):
            body += directory.encode() + b"\0"
            for stem, path in sorted(tree[ext][directory]):
                data = files[path]
                body += stem.encode() + b"\0"
                body += struct.pack("<IHHIIH", zlib.crc32(data) & 0xFFFFFFFF, 0, 0x7FFF, len(blob), len(data), 0xFFFF)
                blob += data
            body += b"\0"
        body += b"\0"
    body += b"\0"
    return struct.pack("<III", 0x55AA1234, 1, len(body)) + bytes(body) + bytes(blob)


def main(argv):
    game = GAME
    out = os.path.join(ROOT, "out", "Release", "addons", "inventory_mod_rounds.vpk")
    install = False
    i = 1
    while i < len(argv):
        if argv[i] == "--game":
            game, i = argv[i + 1], i + 2
        elif argv[i] == "--out":
            out, i = argv[i + 1], i + 2
        elif argv[i] == "--install":
            install, i = True, i + 1
        else:
            print(__doc__)
            return 1

    paks = [vpk.Vpk(os.path.join(game, d, "pak01_dir.vpk")) for d in ("update", "left4dead2")]

    def game_file(name):
        for p in paks:  # update first: it is what the game loads
            if name.lower() in p.entries:
                return p.read(name)
        raise SystemExit(f"{name} is not in the game's VPKs")

    mdl = game_file(MODEL + ".mdl")
    parts = {ext: game_file(MODEL + ext) for ext in (".vvd", ".dx90.vtx", ".phy")}
    shine = vtf_to_png.load(game_file("materials/" + TEXTURE + ".vtf")).convert("RGBA").getchannel("A").resize(SIZE, Image.LANCZOS)

    files = {}
    made = []
    textures = os.path.join(ROOT, "assets", "textures")
    for file, kind in KINDS.items():
        path = os.path.join(textures, file)
        if not os.path.isfile(path):
            print(f"  (no {file}: no pack for {kind})")
            continue
        rgba = Image.open(path).convert("RGB").resize(SIZE, Image.LANCZOS).convert("RGBA")
        rgba.putalpha(shine)
        files[f"materials/models/inv_mod/rounds_{kind}.vtf"] = make_vtf(rgba)
        files[f"materials/models/inv_mod/rounds_{kind}.vmt"] = make_vmt(kind)
        files[f"models/inv_mod/rounds_{kind}.mdl"] = make_mdl(mdl, kind)
        for ext, data in parts.items():
            files[f"models/inv_mod/rounds_{kind}{ext}"] = data
        made.append(kind)
    files["addoninfo.txt"] = ('"AddonInfo"\n{\n'
                              '\taddonSteamAppID 550\n'
                              '\taddontitle "Inventory mod: ammo packs"\n'
                              '\taddonversion 1\n'
                              '\taddonauthor "inventory mod"\n'
                              '\taddonDescription "The ammo pack of each round type, for the inventory mod\'s dropped rounds."\n'
                              '\taddonContent_Prop 1\n'
                              '}\n').encode()
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = make_vpk(files)
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}: {len(files)} files, {len(data) // 1024} KB, packs for {', '.join(made)}")
    if install:
        target = os.path.join(game, "left4dead2", "addons", os.path.basename(out))
        with open(target, "wb") as f:
            f.write(data)
        print(f"installed: {target} (the game mounts it when it starts)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
