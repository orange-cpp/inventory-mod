use std::{collections::BTreeMap, io, path::Path};

use image::{
    ImageReader,
    imageops::{self, FilterType},
};

use crate::vtf;

const MODEL: &str = "models/w_models/weapons/w_eq_incendiary_ammopack";
const TEXTURE: &str = "models/props/terror/incendiary_ammopack";
const SIZE: (u32, u32) = (1024, 512);
const KINDS: [(&str, &str); 6] = [
    ("rifle_rounds_ammo_pack.png", "assaultrifle"),
    ("smg_rounds_ammo_pack.png", "smg"),
    ("shotgun_rounds_ammo_pack.png", "shotgun"),
    ("haunting_riffle_rounds_ammo_pack.png", "huntingrifle"),
    ("sniper_rounds_ammo_pack.png", "sniperrifle"),
    ("grenade_rounds_ammo_pack.png", "grenadelauncher"),
];

const ADDON_INFO: &str = r#""AddonInfo"
{
	addonSteamAppID 550
	addontitle "Inventory mod: ammo packs"
	addonversion 1
	addonauthor "inventory mod"
	addonDescription "The ammo pack of each round type, for the inventory mod's dropped rounds."
	addonContent_Prop 1
}
"#;

pub fn files(
    textures: &Path,
    read_game: impl Fn(&str) -> io::Result<Vec<u8>>,
) -> Result<BTreeMap<String, Vec<u8>>, Box<dyn std::error::Error>> {
    let model = read_game(&format!("{MODEL}.mdl"))?;
    let parts: Vec<_> = [".vvd", ".dx90.vtx", ".phy"]
        .into_iter()
        .map(|ext| read_game(&format!("{MODEL}{ext}")).map(|bytes| (ext, bytes)))
        .collect::<io::Result<_>>()?;
    let mask = vtf::decode(&read_game(&format!("materials/{TEXTURE}.vtf"))?)?;
    let alpha = image::GrayImage::from_fn(mask.width(), mask.height(), |x, y| {
        image::Luma([mask.get_pixel(x, y)[3]])
    });
    let alpha = imageops::resize(&alpha, SIZE.0, SIZE.1, FilterType::Lanczos3);
    let mut files = BTreeMap::new();
    for (file, kind) in KINDS {
        let path = textures.join(file);
        let image = ImageReader::open(&path)
            .map_err(|error| io::Error::new(error.kind(), format!("{}: {error}", path.display())))?
            .decode()?
            .into_rgb8();
        let image = imageops::resize(&image, SIZE.0, SIZE.1, FilterType::Lanczos3);
        let rgba = image::RgbaImage::from_fn(SIZE.0, SIZE.1, |x, y| {
            let [r, g, b] = image.get_pixel(x, y).0;
            image::Rgba([r, g, b, alpha.get_pixel(x, y)[0]])
        });
        files.insert(
            format!("materials/models/inv_mod/rounds_{kind}.vtf"),
            vtf::encode(&rgba)?,
        );
        files.insert(
            format!("materials/models/inv_mod/rounds_{kind}.vmt"),
            material(kind).into_bytes(),
        );
        files.insert(
            format!("models/inv_mod/rounds_{kind}.mdl"),
            patch_model(&model, kind)?,
        );
        for (extension, data) in &parts {
            files.insert(
                format!("models/inv_mod/rounds_{kind}{extension}"),
                data.clone(),
            );
        }
    }
    files.insert("addoninfo.txt".to_owned(), ADDON_INFO.as_bytes().to_vec());
    Ok(files)
}

fn material(kind: &str) -> String {
    format!(
        r#""VertexlitGeneric"
{{
	"$basetexture" "models/inv_mod/rounds_{kind}"
	"$envmap" "env_cubemap"
	"$basealphaenvmapmask" "1"
	"$envmapfresnel" "1"
	"$envmapfresnelMinMaxExp" "[0.4 1 3]"
}}
"#
    )
}

fn patch_model(model: &[u8], kind: &str) -> io::Result<Vec<u8>> {
    let invalid = || {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "unexpected incendiary ammo pack model",
        )
    };
    if !model.starts_with(b"IDST") {
        return Err(invalid());
    }
    let mut data = model.to_owned();
    let name = format!("inv_mod\\rounds_{kind}.mdl");
    let model_name = data.get_mut(12..76).ok_or_else(invalid)?;
    model_name.fill(0);
    model_name
        .get_mut(..name.len())
        .ok_or_else(invalid)?
        .copy_from_slice(name.as_bytes());

    let old = format!("{TEXTURE}\0");
    let new = format!("models/inv_mod/rounds_{kind}\0");
    let mut offsets = data
        .windows(old.len())
        .enumerate()
        .filter_map(|(at, window)| (window == old.as_bytes()).then_some(at));
    let at = offsets.next().ok_or_else(invalid)?;
    if offsets.next().is_some() || new.len() > old.len() {
        return Err(invalid());
    }
    data[at..at + old.len()].fill(0);
    data[at..at + new.len()].copy_from_slice(new.as_bytes());
    Ok(data)
}
