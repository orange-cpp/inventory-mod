"""Decompiles a simple Source model (MDL 44..49 + VVD + VTX: bones, LOD 0 meshes, no flexes or animations) into a
reference SMD and a QC that recompiles it, for Blender Source Tools (File > Import > Source Engine).

    python -I tools/re/mdl_to_smd.py <model.mdl> <out dir>

Next to the .mdl it reads <name>.vvd and <name>.dx90.vtx.  Writes <out dir>/<name>_ref.smd and <out dir>/<name>.qc.
The collision model (.phy) is not decompiled: the QC builds one from the reference mesh.
"""
import math
import os
import struct
import sys


def cstr(data, at):
    end = data.index(b"\0", at)
    return data[at:end].decode("latin-1")


def read_mdl(path):
    d = open(path, "rb").read()
    ident, version = struct.unpack_from("<4si", d, 0)
    if ident != b"IDST":
        raise SystemExit("not an MDL")
    name = cstr(d, 12)
    numbones, boneindex = struct.unpack_from("<ii", d, 156)
    numtextures, textureindex, numcd, cdindex = struct.unpack_from("<iiii", d, 204)
    numskinref, numfamilies, skinindex, numbodyparts, bodypartindex = struct.unpack_from("<iiiii", d, 220)
    surfaceprop_index = struct.unpack_from("<i", d, 0x134)[0] if len(d) > 0x138 else 0

    bones = []
    for i in range(numbones):
        b = boneindex + i * 216
        nameoff, parent = struct.unpack_from("<ii", d, b)
        pos = struct.unpack_from("<3f", d, b + 32)
        rot = struct.unpack_from("<3f", d, b + 60)
        bones.append((cstr(d, b + nameoff), parent, pos, rot))

    textures = []
    for i in range(numtextures):
        t = textureindex + i * 64
        textures.append(cstr(d, t + struct.unpack_from("<i", d, t)[0]))
    cdmaterials = [cstr(d, struct.unpack_from("<i", d, cdindex + 4 * i)[0]) for i in range(numcd)]

    bodyparts = []
    for i in range(numbodyparts):
        bp = bodypartindex + i * 16
        nameoff, nummodels, base, modelindex = struct.unpack_from("<iiii", d, bp)
        models = []
        for m in range(nummodels):
            mo = bp + modelindex + m * 148
            mname = cstr(d, mo)
            nummeshes, meshindex, numvertices, vertexindex = struct.unpack_from("<iiii", d, mo + 72)
            meshes = []
            for k in range(nummeshes):
                me = mo + meshindex + k * 116
                material, _, mnumverts, vertexoffset = struct.unpack_from("<iiii", d, me)
                meshes.append({"material": material, "numverts": mnumverts, "vertexoffset": vertexoffset})
            models.append({"name": mname, "numvertices": numvertices, "vertexindex": vertexindex, "meshes": meshes})
        bodyparts.append({"name": cstr(d, bp + nameoff), "models": models})
    return {"name": name, "version": version, "bones": bones, "textures": textures, "cdmaterials": cdmaterials,
            "bodyparts": bodyparts}


def read_vvd(path):
    d = open(path, "rb").read()
    ident, version, checksum, numlods = struct.unpack_from("<4siii", d, 0)
    if ident != b"IDSV":
        raise SystemExit("not a VVD")
    lodverts = struct.unpack_from("<8i", d, 16)
    numfixups, fixupstart, vertexstart, tangentstart = struct.unpack_from("<iiii", d, 48)

    def vertex(i):
        o = vertexstart + i * 48
        weights = struct.unpack_from("<3f", d, o)
        bones = struct.unpack_from("<3b", d, o + 12)
        numbones = d[o + 15]
        pos = struct.unpack_from("<3f", d, o + 16)
        normal = struct.unpack_from("<3f", d, o + 28)
        uv = struct.unpack_from("<2f", d, o + 40)
        return {"weights": weights[:numbones], "bones": bones[:numbones], "pos": pos, "normal": normal, "uv": uv}

    # LOD 0's vertices: all of them, or (with fixups) the fixup ranges for LOD 0
    if numfixups == 0:
        ids = list(range(lodverts[0]))
    else:
        ids = []
        for f in range(numfixups):
            lod, source, count = struct.unpack_from("<iii", d, fixupstart + f * 12)
            if lod >= 0:
                ids.extend(range(source, source + count))
    return [vertex(i) for i in ids]


def read_vtx(path, mdl):
    d = open(path, "rb").read()
    version, cache, _, _, _, checksum, numlods, mrlo, numbodyparts, bodypartoffset = struct.unpack_from("<iiHHiiiiii", d, 0)
    # per bodypart / model / mesh: triangles as (origMeshVertID, ...) for LOD 0
    result = []
    for bpi in range(numbodyparts):
        bp = bodypartoffset + bpi * 8
        nummodels, modeloffset = struct.unpack_from("<ii", d, bp)
        models = []
        for mi in range(nummodels):
            mo = bp + modeloffset + mi * 8
            nlods, lodoffset = struct.unpack_from("<ii", d, mo)
            lo = mo + lodoffset  # LOD 0
            nummeshes, meshoffset = struct.unpack_from("<ii", d, lo)
            meshes = []
            for k in range(nummeshes):
                me = lo + meshoffset + k * 9
                numsg, sgoffset = struct.unpack_from("<ii", d, me)
                tris = []
                sg_size = 25 if mdl["version"] < 49 else 33
                for s in range(numsg):
                    sg = me + sgoffset + s * sg_size
                    numverts, vertoffset, numindices, indexoffset = struct.unpack_from("<iiii", d, sg)
                    verts = [struct.unpack_from("<H", d, sg + vertoffset + v * 9 + 4)[0] for v in range(numverts)]
                    idx = struct.unpack_from(f"<{numindices}H", d, sg + indexoffset)
                    for t in range(0, numindices - 2, 3):
                        tris.append((verts[idx[t]], verts[idx[t + 1]], verts[idx[t + 2]]))
                meshes.append(tris)
            models.append(meshes)
        result.append(models)
    return result


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    mdl_path, out = argv[1], argv[2]
    base = mdl_path[:-4]
    mdl = read_mdl(mdl_path)
    verts = read_vvd(base + ".vvd")
    vtx = read_vtx(base + ".dx90.vtx", mdl)
    stem = os.path.basename(base)
    os.makedirs(out, exist_ok=True)

    lines = ["version 1", "nodes"]
    for i, (name, parent, pos, rot) in enumerate(mdl["bones"]):
        lines.append(f'{i} "{name}" {parent}')
    lines += ["end", "skeleton", "time 0"]
    for i, (name, parent, pos, rot) in enumerate(mdl["bones"]):
        lines.append(f"{i} {pos[0]:.6f} {pos[1]:.6f} {pos[2]:.6f} {rot[0]:.6f} {rot[1]:.6f} {rot[2]:.6f}")
    lines += ["end", "triangles"]
    flipped = total = 0
    for bpi, bp in enumerate(mdl["bodyparts"]):
        for mi, model in enumerate(bp["models"]):
            first = model["vertexindex"] // 48
            for k, mesh in enumerate(model["meshes"]):
                material = mdl["textures"][mesh["material"]] if mesh["material"] < len(mdl["textures"]) else "default"
                material = material.replace("\\", "/").split("/")[-1]
                for tri in vtx[bpi][mi][k]:
                    vs = [verts[first + mesh["vertexoffset"] + v] for v in tri]
                    # the winding the vertex normals agree with (SMD: counter-clockwise)
                    face = cross(sub(vs[1]["pos"], vs[0]["pos"]), sub(vs[2]["pos"], vs[0]["pos"]))
                    if dot(face, vs[0]["normal"]) < 0:
                        vs = [vs[0], vs[2], vs[1]]
                        flipped += 1
                    total += 1
                    lines.append(material)
                    for v in vs:
                        links = " ".join(f"{b} {w:.6f}" for b, w in zip(v["bones"], v["weights"]))
                        p, n, uv = v["pos"], v["normal"], v["uv"]
                        lines.append(f"{v['bones'][0] if v['bones'] else 0} {p[0]:.6f} {p[1]:.6f} {p[2]:.6f} "
                                     f"{n[0]:.6f} {n[1]:.6f} {n[2]:.6f} {uv[0]:.6f} {1.0 - uv[1]:.6f} "
                                     f"{len(v['bones'])} {links}")
    lines.append("end")
    open(os.path.join(out, stem + "_ref.smd"), "w", newline="\n").write("\n".join(lines) + "\n")

    cd = mdl["cdmaterials"][0] if mdl["cdmaterials"] else ""
    qc = [f'$modelname "{mdl["name"]}"', f'$cdmaterials "{cd}"', '$surfaceprop "metal"', "$staticprop", "",
          f'$body "body" "{stem}_ref.smd"', "", f'$sequence "idle" "{stem}_ref.smd"', "",
          f'$collisionmodel "{stem}_ref.smd"', "{", "\t$mass 5", "}"]
    open(os.path.join(out, stem + ".qc"), "w", newline="\n").write("\n".join(qc) + "\n")
    print(f"{mdl['name']} (MDL v{mdl['version']}): {len(mdl['bones'])} bones, textures {mdl['textures']}, "
          f"cdmaterials {mdl['cdmaterials']}, {total} triangles ({flipped} turned to counter-clockwise)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
