use std::{
    collections::{BTreeMap, BTreeSet},
    path::Path,
};

use image::RgbaImage;

use crate::{vpk::Vpk, vtf};

const SEARCH_PATH: &[&str] = &[
    "update",
    "left4dead2_dlc3",
    "left4dead2_dlc2",
    "left4dead2_dlc1",
    "left4dead2",
];

#[derive(Default)]
pub struct Icons {
    sprites: BTreeMap<String, Sprite>,
    sheets: BTreeMap<String, RgbaImage>,
}

pub struct Sprite {
    pub material: String,
    pub region: [u32; 4],
    pub atlas: (u32, u32),
}

impl Sprite {
    pub fn uv(&self) -> [f32; 4] {
        let [left, top, right, bottom] = self.region;
        let (width, height) = self.atlas;
        [
            left as f32 / width as f32,
            top as f32 / height as f32,
            right as f32 / width as f32,
            bottom as f32 / height as f32,
        ]
    }

    pub fn aspect(&self) -> f32 {
        let [left, top, right, bottom] = self.region;
        (right - left) as f32 / (bottom - top) as f32
    }
}

impl Icons {
    pub fn load(game: &Path) -> Self {
        let archives: Vec<_> = SEARCH_PATH
            .iter()
            .filter_map(|folder| {
                Vpk::open_filtered(&game.join(folder).join("pak01_dir.vpk"), |name| {
                    name.starts_with("scripts/weapon_")
                        || name.starts_with("scripts/melee/")
                        || name.starts_with("materials/vgui/hud/")
                })
                .ok()
            })
            .collect();
        let mut seen = BTreeSet::new();
        let mut failed = BTreeSet::new();
        let mut icons = Self::default();
        for archive in &archives {
            for (name, melee, path) in archive.names().filter_map(script_name) {
                if !seen.insert(name) {
                    continue;
                }
                let Ok(bytes) = archive.read(path) else {
                    continue;
                };
                let Some((sheet, region)) = script_sprite(&String::from_utf8_lossy(&bytes), melee)
                else {
                    continue;
                };
                if failed.contains(&sheet) {
                    continue;
                }
                if !icons.sheets.contains_key(&sheet) {
                    let asset = format!("materials/{sheet}.vtf");
                    let image = archives
                        .iter()
                        .find(|vpk| vpk.contains(&asset))
                        .and_then(|vpk| vpk.read(&asset).ok())
                        .and_then(|bytes| vtf::decode(&bytes).ok());
                    let Some(image) = image else {
                        failed.insert(sheet);
                        continue;
                    };
                    icons.sheets.insert(sheet.clone(), image);
                }
                let image = &icons.sheets[&sheet];
                if let Some(region) = trim(image, region) {
                    icons.sprites.insert(
                        name.to_owned(),
                        Sprite {
                            material: sheet,
                            region,
                            atlas: image.dimensions(),
                        },
                    );
                }
            }
        }
        icons
    }

    pub fn get(&self, name: &str) -> Option<&Sprite> {
        self.sprites.get(name)
    }

    pub fn sheet(&self, material: &str) -> Option<&RgbaImage> {
        self.sheets.get(material)
    }

    pub fn len(&self) -> usize {
        self.sprites.len()
    }

    pub fn is_empty(&self) -> bool {
        self.sprites.is_empty()
    }
}

fn script_name(path: &str) -> Option<(&str, bool, &str)> {
    let base = path.strip_suffix(".txt")?.strip_prefix("scripts/")?;
    if let Some(name) = base.strip_prefix("melee/") {
        Some((name, true, path))
    } else {
        base.starts_with("weapon_").then_some((base, false, path))
    }
}

fn tokens(mut rest: &str) -> impl Iterator<Item = &str> {
    std::iter::from_fn(move || {
        loop {
            rest = rest.trim_start();
            if rest.starts_with("//") {
                rest = rest.find('\n').map_or("", |end| &rest[end + 1..]);
                continue;
            }
            let first = *rest.as_bytes().first()?;
            let (value, tail) = match first {
                b'"' => {
                    let quoted = &rest[1..];
                    quoted
                        .find('"')
                        .map_or((quoted, ""), |end| (&quoted[..end], &quoted[end + 1..]))
                }
                b'{' | b'}' => rest.split_at(1),
                _ => rest.split_at(
                    rest.find(|c: char| c.is_whitespace() || matches!(c, '{' | '}' | '"'))
                        .unwrap_or(rest.len()),
                ),
            };
            rest = tail;
            return Some(value);
        }
    })
}

fn block<'a>(tokens: &'a [&'a str], key: &str) -> Option<&'a [&'a str]> {
    let start = tokens
        .windows(2)
        .position(|pair| pair[0].eq_ignore_ascii_case(key) && pair[1] == "{")?
        + 2;
    let mut depth = 1;
    for (index, &word) in tokens.iter().enumerate().skip(start) {
        match word {
            "{" => depth += 1,
            "}" => {
                depth -= 1;
                if depth == 0 {
                    return Some(&tokens[start..index]);
                }
            }
            _ => {}
        }
    }
    None
}

fn script_sprite(text: &str, melee: bool) -> Option<(String, Option<[u32; 4]>)> {
    let words: Vec<_> = tokens(text).collect();
    let kind = if melee { "sprite_active" } else { "weapon" };
    let texture = block(&words, "TextureData").and_then(|words| block(words, kind));
    if let Some(texture) = texture {
        let value = |key: &str| {
            texture
                .as_chunks::<2>()
                .0
                .iter()
                .find(|pair| pair[0].eq_ignore_ascii_case(key))
                .map(|pair| pair[1])
        };
        let sheet = value("file")?.to_ascii_lowercase();
        let number = |key| value(key)?.parse::<u32>().ok();
        let x = number("x").unwrap_or_default();
        let y = number("y").unwrap_or_default();
        let right = x.checked_add(number("width")?)?;
        let bottom = y.checked_add(number("height")?)?;
        Some((sheet, Some([x, y, right, bottom])))
    } else if melee {
        let icon = words.windows(2).find_map(|pair| {
            (pair[0].eq_ignore_ascii_case(kind) && pair[1] != "{").then_some(pair[1])
        })?;
        Some((format!("vgui/hud/{}", icon.to_ascii_lowercase()), None))
    } else {
        None
    }
}

fn trim(image: &RgbaImage, region: Option<[u32; 4]>) -> Option<[u32; 4]> {
    let (width, height) = image.dimensions();
    let [left, top, right, bottom] = region.unwrap_or([0, 0, width, height]);
    let right = width.min(right);
    let bottom = height.min(bottom);
    if left >= right || top >= bottom {
        return None;
    }
    let mut bounds = [right, bottom, left, top];
    for y in top..bottom {
        for x in left..right {
            if image.get_pixel(x, y).0[3] > 24 {
                bounds[0] = bounds[0].min(x);
                bounds[1] = bounds[1].min(y);
                bounds[2] = bounds[2].max(x + 1);
                bounds[3] = bounds[3].max(y + 1);
            }
        }
    }
    (bounds[0] < bounds[2] && bounds[1] < bounds[3]).then_some(bounds)
}

#[cfg(test)]
mod tests {
    use super::script_sprite;

    #[test]
    fn standalone_weapon_texture_defaults_to_its_top_left() {
        let script = r#"WeaponData
        {
            TextureData { weapon {
                "file" "vgui/hud/icon_pistol"
                "x" "64"
                //"y" "320"
                "width" "64"
                "height" "64"
            } }
        }"#;
        assert_eq!(
            script_sprite(script, false),
            Some(("vgui/hud/icon_pistol".to_owned(), Some([64, 0, 128, 64])))
        );
    }

    #[test]
    fn older_melee_script_uses_the_whole_icon() {
        assert_eq!(
            script_sprite(r#"MeleeData { "sprite_active" "icon_fireaxe" }"#, true),
            Some(("vgui/hud/icon_fireaxe".to_owned(), None))
        );
    }
}
