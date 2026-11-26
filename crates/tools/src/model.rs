use std::{fmt::Write, fs, io, path::Path};

use crate::binary::{Bytes, invalid};

type Point = [f32; 3];

struct Bone {
    name: String,
    parent: i32,
    position: Point,
    rotation: Point,
}

struct Mesh {
    material: usize,
    first: usize,
}

struct Model {
    first: usize,
    meshes: Vec<Mesh>,
}

struct Mdl {
    name: String,
    version: u32,
    bones: Vec<Bone>,
    textures: Vec<String>,
    directories: Vec<String>,
    bodies: Vec<Vec<Model>>,
}

struct Vertex {
    weights: [f32; 3],
    bones: [i8; 3],
    count: usize,
    position: Point,
    normal: Point,
    uv: [f32; 2],
}

fn point(bytes: Bytes<'_>, offset: usize) -> io::Result<Point> {
    Ok([
        bytes.f32(offset)?,
        bytes.f32(offset + 4)?,
        bytes.f32(offset + 8)?,
    ])
}

fn table(
    bytes: Bytes<'_>,
    base: usize,
    fields: usize,
    stride: usize,
) -> io::Result<impl Iterator<Item = usize>> {
    let count = bytes.u32(fields)? as usize;
    let index = bytes.u32(fields + 4)? as usize;
    let start = base
        .checked_add(index)
        .ok_or_else(|| invalid("invalid model data offset"))?;
    bytes.at(
        start,
        count
            .checked_mul(stride)
            .ok_or_else(|| invalid("invalid model table size"))?,
    )?;
    Ok((0..count).map(move |index| start + index * stride))
}

fn read_mdl(data: &[u8]) -> io::Result<Mdl> {
    let data = Bytes(data);
    if data.at(0, 4)? != b"IDST" {
        return Err(invalid("not a Source MDL"));
    }
    let version = data.u32(4)?;
    if !(44..=49).contains(&version) {
        return Err(invalid("expected a Source MDL from versions 44 through 49"));
    }
    let bones = table(data, 0, 156, 216)?
        .map(|at| {
            Ok(Bone {
                name: data.string(at + data.u32(at)? as usize)?,
                parent: data.i32(at + 4)?,
                position: point(data, at + 32)?,
                rotation: point(data, at + 60)?,
            })
        })
        .collect::<io::Result<_>>()?;
    let textures = table(data, 0, 204, 64)?
        .map(|at| data.string(at + data.u32(at)? as usize))
        .collect::<io::Result<_>>()?;
    let directories = table(data, 0, 212, 4)?
        .map(|at| data.string(data.u32(at)? as usize))
        .collect::<io::Result<_>>()?;
    let bodies = table(data, 0, 232, 16)?
        .map(|body| {
            let model_count = data.u32(body + 4)? as usize;
            let start = body + data.u32(body + 12)? as usize;
            data.at(
                start,
                model_count
                    .checked_mul(148)
                    .ok_or_else(|| invalid("invalid model count"))?,
            )?;
            let models = (0..model_count)
                .map(|index| {
                    let at = start + index * 148;
                    let meshes = table(data, at, at + 72, 116)?
                        .map(|mesh| {
                            Ok(Mesh {
                                material: data.u32(mesh)? as usize,
                                first: data.u32(mesh + 12)? as usize,
                            })
                        })
                        .collect::<io::Result<_>>()?;
                    Ok(Model {
                        first: data.u32(at + 84)? as usize / 48,
                        meshes,
                    })
                })
                .collect::<io::Result<_>>()?;
            Ok(models)
        })
        .collect::<io::Result<_>>()?;
    Ok(Mdl {
        name: data.string(12)?,
        version,
        bones,
        textures,
        directories,
        bodies,
    })
}

fn read_vvd(data: &[u8]) -> io::Result<Vec<Vertex>> {
    let data = Bytes(data);
    if data.at(0, 4)? != b"IDSV" {
        return Err(invalid("not a Source VVD"));
    }
    let count = data.u32(16)? as usize;
    let start = data.u32(56)? as usize;
    let max_vertices = data.0.len().saturating_sub(start) / 48;
    let mut vertices = Vec::new();
    let mut read = |range: std::ops::Range<usize>| -> io::Result<()> {
        if range.end > max_vertices {
            return Err(invalid("vertex index outside the VVD"));
        }
        for index in range {
            let at = start + 48 * index;
            let packed = data.at(at + 12, 4)?;
            let count = usize::from(packed[3]);
            if count > 3 {
                return Err(invalid("vertex has more than three bone weights"));
            }
            vertices.push(Vertex {
                weights: point(data, at)?,
                bones: [packed[0] as i8, packed[1] as i8, packed[2] as i8],
                count,
                position: point(data, at + 16)?,
                normal: point(data, at + 28)?,
                uv: [data.f32(at + 40)?, data.f32(at + 44)?],
            });
        }
        Ok(())
    };
    if data.u32(48)? == 0 {
        read(0..count)?;
    } else {
        for fixup in table(data, 0, 48, 12)? {
            if data.i32(fixup)? >= 0 {
                let index = data.u32(fixup + 4)? as usize;
                let length = data.u32(fixup + 8)? as usize;
                let end = index
                    .checked_add(length)
                    .ok_or_else(|| invalid("invalid vertex fixup"))?;
                read(index..end)?;
            }
        }
    }
    Ok(vertices)
}

fn triangles(data: Bytes<'_>, mesh: usize, version: u32) -> io::Result<Vec<[usize; 3]>> {
    let group_size = if version < 49 { 25 } else { 33 };
    let mut triangles = Vec::new();
    for group in table(data, mesh, mesh, group_size)? {
        let verts = table(data, group, group, 9)?
            .map(|at| data.u16(at + 4).map(usize::from))
            .collect::<io::Result<Vec<_>>>()?;
        let index = table(data, group, group + 8, 2)?
            .map(|at| {
                verts
                    .get(data.u16(at)? as usize)
                    .copied()
                    .ok_or_else(|| invalid("VTX vertex index outside its strip group"))
            })
            .collect::<io::Result<Vec<_>>>()?;
        triangles.extend_from_slice(index.as_chunks::<3>().0);
    }
    Ok(triangles)
}

fn point_text(position: &Point) -> String {
    let [x, y, z] = position;
    format!("{x:.6} {y:.6} {z:.6}")
}

fn winding(vertices: [&Vertex; 3]) -> bool {
    let [a, b, c] = vertices;
    let u: Point = std::array::from_fn(|i| b.position[i] - a.position[i]);
    let v: Point = std::array::from_fn(|i| c.position[i] - a.position[i]);
    let normal = [
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    ];
    normal
        .into_iter()
        .zip(a.normal)
        .map(|(a, b)| a * b)
        .sum::<f32>()
        < 0.0
}

pub fn export(model: &Path, out: &Path) -> io::Result<()> {
    let filename = model
        .file_stem()
        .and_then(|stem| stem.to_str())
        .ok_or_else(|| invalid("invalid model filename"))?;
    let data = read_mdl(&fs::read(model)?)?;
    let vertices = read_vvd(&fs::read(model.with_extension("vvd"))?)?;
    let vtx = fs::read(model.with_extension("dx90.vtx"))?;
    let vtx = Bytes(&vtx);
    let mut output = String::from("version 1\nnodes\n");
    for (index, bone) in data.bones.iter().enumerate() {
        writeln!(output, "{index} \"{}\" {}", bone.name, bone.parent).unwrap();
    }
    output.push_str("end\nskeleton\ntime 0\n");
    for (index, bone) in data.bones.iter().enumerate() {
        writeln!(
            output,
            "{index} {} {}",
            point_text(&bone.position),
            point_text(&bone.rotation)
        )
        .unwrap();
    }
    output.push_str("end\ntriangles\n");

    let mut total = 0;
    let mut flipped = 0;
    for (body_index, body) in table(vtx, 0, 28, 8)?.enumerate() {
        let body_data = data
            .bodies
            .get(body_index)
            .ok_or_else(|| invalid("VTX and MDL body parts differ"))?;
        for (model_index, model) in table(vtx, body, body, 8)?.enumerate() {
            let source = body_data
                .get(model_index)
                .ok_or_else(|| invalid("VTX and MDL model lists differ"))?;
            let lod = table(vtx, model, model, 12)?
                .next()
                .ok_or_else(|| invalid("VTX model has no reference LOD"))?;
            for (mesh_index, mesh) in table(vtx, lod, lod, 9)?.enumerate() {
                let mesh_data = source
                    .meshes
                    .get(mesh_index)
                    .ok_or_else(|| invalid("VTX and MDL mesh lists differ"))?;
                let material = data
                    .textures
                    .get(mesh_data.material)
                    .and_then(|path| path.rsplit(['/', '\\']).next())
                    .unwrap_or("default");
                let first = source.first + mesh_data.first;
                for indices in triangles(vtx, mesh, data.version)? {
                    let [Some(a), Some(b), Some(c)] = indices
                        .map(|index| first.checked_add(index).and_then(|at| vertices.get(at)))
                    else {
                        return Err(invalid("VTX triangle points outside the VVD"));
                    };
                    let mut points = [a, b, c];
                    if winding(points) {
                        points.swap(1, 2);
                        flipped += 1;
                    }
                    writeln!(output, "{material}").unwrap();
                    for vertex in points {
                        let parent = if vertex.count > 0 { vertex.bones[0] } else { 0 };
                        write!(
                            output,
                            "{parent} {} {} {:.6} {:.6} {}",
                            point_text(&vertex.position),
                            point_text(&vertex.normal),
                            vertex.uv[0],
                            1.0 - f64::from(vertex.uv[1]),
                            vertex.count
                        )
                        .unwrap();
                        for index in 0..vertex.count {
                            write!(
                                output,
                                " {} {:.6}",
                                vertex.bones[index], vertex.weights[index]
                            )
                            .unwrap();
                        }
                        output.push('\n');
                    }
                    total += 1;
                }
            }
        }
    }
    output.push_str("end\n");
    fs::create_dir_all(out)?;
    let reference = format!("{filename}_ref.smd");
    fs::write(out.join(&reference), output)?;
    let material = data.directories.first().map_or("", String::as_str);
    let qc = format!(
        "$modelname \"{}\"\n$cdmaterials \"{material}\"\n$surfaceprop \"metal\"\n$staticprop\n\n$body \"body\" \"{reference}\"\n\n$sequence \"idle\" \"{reference}\"\n\n$collisionmodel \"{reference}\"\n{{\n\t$mass 5\n}}\n",
        data.name
    );
    fs::write(out.join(format!("{filename}.qc")), qc)?;
    println!(
        "{} (MDL v{}): {} bones, textures {:?}, cdmaterials {:?}, {total} triangles ({flipped} turned to counter-clockwise)",
        data.name,
        data.version,
        data.bones.len(),
        data.textures,
        data.directories
    );
    Ok(())
}
