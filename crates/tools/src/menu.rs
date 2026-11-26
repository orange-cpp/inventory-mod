use std::{collections::BTreeMap, fmt::Write, io};

pub const FLYOUT: &str = "resource/ui/l4d360ui/optionsflyout.res";

struct Setting {
    key: &'static str,
    label: &'static str,
    tip: &'static str,
    choices: &'static [(&'static str, &'static str)],
    right: bool,
}

const ON_OFF: &[(&str, &str)] = &[("1", "On"), ("0", "Off")];
const KEYS: &[(&str, &str)] = &[
    ("0", "None"),
    ("66", "B"),
    ("67", "C"),
    ("70", "F"),
    ("71", "G"),
    ("72", "H"),
    ("73", "I"),
    ("74", "J"),
    ("75", "K"),
    ("76", "L"),
    ("77", "M"),
    ("78", "N"),
    ("79", "O"),
    ("80", "P"),
    ("84", "T"),
    ("85", "U"),
    ("86", "V"),
    ("88", "X"),
    ("90", "Z"),
    ("219", "["),
    ("221", "]"),
    ("220", "Backslash"),
    ("186", ";"),
    ("222", "'"),
    ("188", ","),
    ("190", "."),
    ("191", "/"),
    ("45", "Insert"),
    ("46", "Delete"),
    ("36", "Home"),
    ("33", "Page Up"),
    ("34", "Page Down"),
];

const SETTINGS: &[Setting] = &[
    Setting {
        key: "show_hotbar",
        label: "Show the item boxes",
        tip: "The mod's item boxes beside the game's weapon panel (or its hotbar).",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "inventory_view",
        label: "Inventory view",
        tip: "Slots: the boxes. Wheel: hold the middle mouse button.",
        choices: &[
            ("0", "Slots and wheel"),
            ("1", "Slots only"),
            ("2", "Wheel only"),
        ],
        right: false,
    },
    Setting {
        key: "auto_store",
        label: "Store items on use",
        tip: "E on an item stores it even when you could carry it.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "replace_native",
        label: "Weapon panel",
        tip: "Beside: the game's panel stays, the boxes under it. Replaced: the mod's hotbar.",
        choices: &[("0", "Beside the game's"), ("1", "Replaced")],
        right: false,
    },
    Setting {
        key: "hud_scale",
        label: "HUD scale",
        tip: "The size of the mod's boxes, wheel and window.",
        choices: &[
            ("0.75", "75%"),
            ("1", "100%"),
            ("1.25", "125%"),
            ("1.5", "150%"),
            ("2", "200%"),
            ("2.5", "250%"),
            ("3", "300%"),
        ],
        right: false,
    },
    Setting {
        key: "infection_effect",
        label: "Infection: screen effect",
        tip: "The picture throbs, sways and drains while infected.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "infection_cover_game_hud",
        label: "Infection: over the game's HUD",
        tip: "Off: the effect stays under the game's HUD and the mod's boxes.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "infection_cover_mod_hud",
        label: "Infection: over the mod's window",
        tip: "Off: the inventory window stays clear.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "infection_sound",
        label: "Infection: sound",
        tip: "The sound muffled and warped, a heartbeat, ringing.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "infection_screamer",
        label: "Infection: screamer",
        tip: "A face and a scream when the clock runs out.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "screamer_chance",
        label: "Infection: screamer chance",
        tip: "How often the screamer comes when the clock runs out.",
        choices: &[
            ("0", "Never"),
            ("0.1", "10%"),
            ("0.25", "25%"),
            ("0.5", "50%"),
            ("0.75", "75%"),
            ("1", "Always"),
        ],
        right: false,
    },
    Setting {
        key: "stamina_bar",
        label: "Stamina: bar",
        tip: "A thin bar under the crosshair while stamina is not full.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "stamina_vignette",
        label: "Stamina: vignette",
        tip: "The screen's edges darken as stamina runs low.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "stamina_cover_game_hud",
        label: "Stamina: over the game's HUD",
        tip: "Off: the vignette stays under the game's HUD and the mod's boxes.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "stamina_cover_mod_hud",
        label: "Stamina: over the mod's window",
        tip: "Off: the inventory window stays clear.",
        choices: ON_OFF,
        right: false,
    },
    Setting {
        key: "key_toggle",
        label: "Key: inventory window",
        tip: "Opens and closes the mod's inventory window.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_prev",
        label: "Key: previous item",
        tip: "Selects the previous box.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_next",
        label: "Key: next item",
        tip: "Selects the next box.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_use",
        label: "Key: use item",
        tip: "Takes the selected item in your hands.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_drop",
        label: "Key: drop",
        tip: "Drops what is in your hands (or the selected box's item).",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_store",
        label: "Key: store",
        tip: "Stores the item or weapon in your hands.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_pickup",
        label: "Key: pick up",
        tip: "Takes the item or weapon in front of you into the inventory.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "key_unload",
        label: "Key: unload gun",
        tip: "Empties the gun in your hands (or the one you look at) into your reserve.",
        choices: KEYS,
        right: true,
    },
    Setting {
        key: "slots",
        label: "Item boxes",
        tip: "The most item boxes. (A host's inventory_mod.toml wins over this; it applies after a restart.)",
        choices: &[
            ("4", "4"),
            ("6", "6"),
            ("8", "8"),
            ("10", "10"),
            ("12", "12"),
            ("16", "16"),
            ("20", "20"),
            ("24", "24"),
        ],
        right: true,
    },
    Setting {
        key: "weapon_slots",
        label: "Weapon boxes",
        tip: "The most boxes for stored guns and melee weapons. (A host's inventory_mod.toml wins over this; it applies after a restart.)",
        choices: &[
            ("0", "0"),
            ("1", "1"),
            ("2", "2"),
            ("3", "3"),
            ("4", "4"),
            ("6", "6"),
            ("8", "8"),
        ],
        right: true,
    },
    Setting {
        key: "max_weight",
        label: "Weight limit",
        tip: "What you can carry. (A host's inventory_mod.toml wins over this; it applies after a restart.)",
        choices: &[
            ("0", "None"),
            ("10", "10"),
            ("15", "15"),
            ("20", "20"),
            ("25", "25"),
            ("30", "30"),
            ("40", "40"),
            ("50", "50"),
        ],
        right: true,
    },
    Setting {
        key: "melee_ring",
        label: "Melee: wind-up ring",
        tip: "A ring at the crosshair fills while a melee swing winds up.",
        choices: ON_OFF,
        right: true,
    },
];

const FLYOUT_BUTTON: &str = r#"
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
		"command"				"InventoryModSettings"
	}
"#;

const ADDON_INFO: &str = r#""AddonInfo"
{
	addonSteamAppID 550
	addontitle "Inventory mod: menus"
	addonversion 1
	addonauthor "inventory mod"
	addonDescription "Options > Inventory Mod: the inventory mod's settings."
}
"#;

pub fn files(stock_flyout: &[u8]) -> io::Result<BTreeMap<String, Vec<u8>>> {
    // Valve's resource files can contain characters encoded as Latin-1.
    let mut flyout: String = stock_flyout.iter().copied().map(char::from).collect();
    if flyout.contains("InventoryModSettings") {
        return Err(invalid("the options flyout already contains Inventory Mod"));
    }
    replace_property(&mut flyout, "PnlBackground", "tall", "125", "145")?;
    replace_property(
        &mut flyout,
        "BtnCloud",
        "navDown",
        "BtnVideo",
        "BtnInventoryMod",
    )?;
    replace_property(
        &mut flyout,
        "BtnVideo",
        "navUp",
        "BtnCloud",
        "BtnInventoryMod",
    )?;
    let end = flyout
        .rfind('}')
        .ok_or_else(|| invalid("unterminated options flyout"))?;
    flyout.insert_str(end, FLYOUT_BUTTON);
    let mut files = BTreeMap::from([
        (
            FLYOUT.to_owned(),
            flyout.chars().map(|ch| ch as u8).collect(),
        ),
        (
            "resource/ui/l4d360ui/audiovideo.res".to_owned(),
            page().into_bytes(),
        ),
        ("addoninfo.txt".to_owned(), ADDON_INFO.as_bytes().to_vec()),
    ]);
    for setting in SETTINGS {
        files.insert(
            format!("resource/ui/l4d360ui/invmod_{}.res", setting.key),
            choices(setting).into_bytes(),
        );
    }
    Ok(files)
}

fn invalid(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

fn replace_property(
    text: &mut String,
    name: &str,
    key: &str,
    old: &str,
    value: &str,
) -> io::Result<()> {
    let marker = format!("\"{name}\"");
    let start = text
        .match_indices(&marker)
        .find_map(|(index, _)| {
            let line = text[..index].rfind('\n').map_or(0, |i| i + 1);
            if !text[line..index].trim().is_empty() {
                return None;
            }
            let mut rest = text[index + marker.len()..].trim_start();
            if let Some(condition) = rest.strip_prefix('[') {
                rest = condition.split_once(']')?.1.trim_start();
            }
            rest.starts_with('{').then_some(text.len() - rest.len())
        })
        .ok_or_else(|| invalid(format!("missing resource control {name}")))?;

    let mut depth = 0;
    let mut quoted = false;
    let mut escape = false;
    let size = text[start..]
        .char_indices()
        .find_map(|(index, ch)| {
            if std::mem::take(&mut escape) {
                return None;
            }
            match ch {
                '\\' if quoted => escape = true,
                '"' => quoted = !quoted,
                '{' if !quoted => depth += 1,
                '}' if !quoted => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(index);
                    }
                }
                _ => {}
            }
            None
        })
        .ok_or_else(|| invalid(format!("unterminated resource control {name}")))?;

    let tag = format!("\"{key}\"");
    let expected = format!("\"{old}\"");
    for (index, _) in text[start..start + size].match_indices(&tag) {
        let prefix = start + index + tag.len();
        let next = text[prefix..start + size].trim_start();
        if next.starts_with(&expected) {
            let at = start + size - next.len();
            text.replace_range(at..at + expected.len(), &format!("\"{value}\""));
            return Ok(());
        }
    }
    Err(invalid(format!("{name} is missing {key} = {old}")))
}

fn field(out: &mut String, indent: &str, key: &str, value: &str) {
    let align = 24usize.saturating_sub(key.len()).max(1);
    writeln!(out, "{indent}\"{key}\"{:align$}\"{value}\"", "").unwrap();
}

fn begin(out: &mut String, indent: &str, name: &str, fields: &[(&str, &str)]) {
    writeln!(out, "{indent}\"{name}\"\n{indent}{{").unwrap();
    let inside = format!("{indent}\t");
    for &(key, value) in fields {
        field(out, &inside, key, value);
    }
}

fn end(out: &mut String, indent: &str) {
    writeln!(out, "{indent}}}").unwrap();
}

fn control(out: &mut String, indent: &str, name: &str, fields: &[(&str, &str)]) {
    begin(out, indent, name, fields);
    end(out, indent);
}

fn choices(setting: &Setting) -> String {
    let count = setting.choices.len();
    let columns = if setting.key.starts_with("key_") {
        3
    } else {
        1
    };
    let rows = count.div_ceil(columns);
    let width = if columns == 1 { 150 } else { 70 };
    let mut out = format!("\"Resource/UI/InvMod_{}.res\"\n{{\n", setting.key);
    control(
        &mut out,
        "\t",
        "PnlBackground",
        &[
            ("ControlName", "Panel"),
            ("fieldName", "PnlBackground"),
            ("xpos", "0"),
            ("ypos", "0"),
            ("zpos", "-1"),
            ("wide", &(columns * width + 6).to_string()),
            ("tall", &(rows * 20 + 5).to_string()),
            ("visible", "1"),
            ("enabled", "1"),
            ("paintbackground", "1"),
            ("paintborder", "1"),
        ],
    );
    for (index, &(value, label)) in setting.choices.iter().enumerate() {
        let name = format!("Btn{index}");
        let button = |offset: isize| {
            format!(
                "Btn{}",
                (index as isize + offset).rem_euclid(count as isize)
            )
        };
        out.push('\n');
        control(
            &mut out,
            "\t",
            &name,
            &[
                ("ControlName", "L4D360HybridButton"),
                ("fieldName", &name),
                ("xpos", &(index / rows * width).to_string()),
                ("ypos", &(index % rows * 20).to_string()),
                ("wide", &width.to_string()),
                ("tall", "20"),
                ("autoResize", "1"),
                ("pinCorner", "0"),
                ("visible", "1"),
                ("enabled", "1"),
                ("tabPosition", "0"),
                ("wrap", "1"),
                ("navUp", &button(-1)),
                ("navDown", &button(1)),
                ("navLeft", &button(-(rows as isize))),
                ("navRight", &button(rows as isize)),
                ("labelText", label),
                ("style", "FlyoutMenuButton"),
                ("command", &format!("InvMod {} {value}", setting.key)),
                ("OnlyActiveUser", "1"),
            ],
        );
    }
    end(&mut out, "");
    out
}

fn page() -> String {
    let mut left = Vec::new();
    let mut right = Vec::new();
    for setting in SETTINGS {
        let column = if setting.right { &mut right } else { &mut left };
        column.push(format!("Drp_{}", setting.key));
    }
    left.extend(["BtnWindow".into(), "BtnDone".into()]);
    let navigation = |column: &[String], other: &[String], row: usize| -> [String; 3] {
        [
            column[(row + column.len() - 1) % column.len()].clone(),
            column[(row + 1) % column.len()].clone(),
            other[row.min(other.len() - 1)].clone(),
        ]
    };
    let mut out = String::from("\"Resource/UI/AudioVideo.res\"\n{\n");
    control(
        &mut out,
        "\t",
        "AudioVideo",
        &[
            ("ControlName", "Frame"),
            ("fieldName", "AudioVideo"),
            ("xpos", "0"),
            ("ypos", "0"),
            ("wide", "f0"),
            ("tall", "448"),
            ("autoResize", "0"),
            ("pinCorner", "0"),
            ("visible", "1"),
            ("enabled", "1"),
            ("tabPosition", "0"),
        ],
    );
    out.push('\n');
    control(
        &mut out,
        "\t",
        "ImgBackground",
        &[
            ("ControlName", "L4DMenuBackground"),
            ("fieldName", "ImgBackground"),
            ("xpos", "0"),
            ("ypos", "79"),
            ("zpos", "-1"),
            ("wide", "f0"),
            ("tall", "350"),
            ("autoResize", "0"),
            ("pinCorner", "0"),
            ("visible", "1"),
            ("enabled", "1"),
            ("tabPosition", "0"),
            ("fillColor", "0 0 0 0"),
        ],
    );
    let mut left_row = 0;
    let mut right_row = 0;
    for (index, setting) in SETTINGS.iter().enumerate() {
        let key = setting.key;
        let (column, other, row, x) = if setting.right {
            (&right, &left, &mut right_row, "c+15")
        } else {
            (&left, &right, &mut left_row, "c-300")
        };
        let name = &column[*row];
        let [up, down, across] = navigation(column, other, *row);
        let y = (100 + 20 * *row).to_string();
        *row += 1;
        out.push('\n');
        begin(
            &mut out,
            "\t",
            name,
            &[
                ("ControlName", "DropDownMenu"),
                ("fieldName", name),
                ("xpos", x),
                ("ypos", &y),
                ("zpos", "3"),
                ("wide", "285"),
                ("tall", "15"),
                ("visible", "1"),
                ("enabled", "1"),
                ("usetitlesafe", "0"),
                ("tabPosition", "0"),
                ("navUp", &up),
                ("navDown", &down),
                ("navLeft", &across),
                ("navRight", &across),
            ],
        );
        out.push('\n');
        let flyout = format!("Flm_{key}");
        control(
            &mut out,
            "\t\t",
            "BtnDropButton",
            &[
                ("ControlName", "L4D360HybridButton"),
                ("fieldName", "BtnDropButton"),
                ("xpos", "0"),
                ("ypos", "0"),
                ("zpos", "0"),
                ("wide", "285"),
                ("wideatopen", "165"),
                ("tall", "15"),
                ("autoResize", "1"),
                ("pinCorner", "0"),
                ("visible", "1"),
                ("enabled", "1"),
                ("tabPosition", if index == 0 { "1" } else { "0" }),
                ("AllCaps", "1"),
                ("labelText", setting.label),
                ("tooltiptext", setting.tip),
                ("style", "DropDownButton"),
                ("command", &flyout),
                ("ActivationType", "1"),
                ("OnlyActiveUser", "1"),
            ],
        );
        end(&mut out, "\t");
        out.push('\n');
        control(
            &mut out,
            "\t",
            &flyout,
            &[
                ("ControlName", "FlyoutMenu"),
                ("fieldName", &flyout),
                ("visible", "0"),
                ("wide", "0"),
                ("tall", "0"),
                ("zpos", "4"),
                ("InitialFocus", "Btn0"),
                (
                    "ResourceFile",
                    &format!("resource/UI/L4D360UI/InvMod_{key}.res"),
                ),
                ("OnlyActiveUser", "1"),
            ],
        );
    }
    for (index, (name, label, tip, command)) in [
        (
            "BtnWindow",
            "Inventory window",
            "The mod's own window: the inventory, debugging.",
            "InventoryModWindow",
        ),
        ("BtnDone", "#L4D360UI_Done", "", "Back"),
    ]
    .into_iter()
    .enumerate()
    {
        let row = left_row + index;
        let [up, down, across] = navigation(&left, &right, row);
        out.push('\n');
        control(
            &mut out,
            "\t",
            name,
            &[
                ("ControlName", "L4D360HybridButton"),
                ("fieldName", name),
                ("xpos", "c-300"),
                ("ypos", &(110 + 20 * row).to_string()),
                ("zpos", "0"),
                ("wide", "260"),
                ("tall", "15"),
                ("autoResize", "1"),
                ("pinCorner", "0"),
                ("visible", "1"),
                ("enabled", "1"),
                ("tabPosition", "0"),
                ("wrap", "1"),
                ("navUp", &up),
                ("navDown", &down),
                ("navLeft", &across),
                ("navRight", &across),
                ("AllCaps", "1"),
                ("labelText", label),
                ("tooltiptext", tip),
                ("style", "DefaultButton"),
                ("command", command),
                ("EnabledTextInsetX", "2"),
                ("DisabledTextInsetX", "2"),
                ("FocusTextInsetX", "2"),
                ("OpenTextInsetX", "2"),
            ],
        );
    }
    end(&mut out, "");
    out
}
