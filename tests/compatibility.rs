use std::sync::Arc;

use inventory_mod::{
    inventory::{Inventory, Limits, WeaponState},
    items::{AmmoKind, Catalog, UnloadSplit},
    melee::{
        BUTTON_ATTACK, BUTTON_ATTACK2, BUTTON_HEAVY, BUTTON_HEAVY_SHOVE, BUTTON_RELOAD,
        BUTTON_THROW, BUTTON_WINDING, MeleeClass, MeleeFrame, MeleeInput, Shove, Swing,
        WEAR_BROKEN, cleave_scale, melee_overhead_swing, melee_profile, melee_wear_after,
    },
    mutators::{Mutator, pick_mutators},
    remote_state::{NativeWeapon, RemoteState, encode_remote_state, reconcile_native_inventory},
    rules::Rules,
    settings::{Settings, View},
};

#[test]
fn legacy_nameless_melee_equips_a_concrete_script() {
    let mut inventory = Inventory::new(Limits {
        slots: 24,
        ..Limits::default()
    });
    assert!(inventory.load_text(
        "inventory 1\n24 weapon_melee 1 -1 -1 - 0 0 2000\n25 weapon_melee 1 -1 -1 katana 0 0 3500\n"
    ));
    let unnamed = &inventory.slots()[24].weapon;
    let named = &inventory.slots()[25].weapon;
    assert_eq!(unnamed.give_name("weapon_melee"), "fireaxe");
    assert_eq!(named.give_name("weapon_melee"), "katana");
    assert_eq!(named.give_name("weapon_pistol"), "weapon_pistol");
    assert_eq!((unnamed.wear, named.wear), (2000, 3500));
}

#[test]
fn legacy_settings_and_toml_round_trip() {
    let old = "layout=1\nkey_toggle=80\nkey_drop=191\nkey_pickup=71\nreplace_native=1\nslots=6\nweapon_slots=1\nauto_store=1\ninventory_view=2\nhud_scale=4\n";
    let mut settings = Settings::legacy(old);
    assert_eq!(settings.keys.toggle, 80);
    assert_eq!(settings.keys.drop, i32::from(b'G'));
    assert_eq!(settings.keys.pickup, i32::from(b'H'));
    assert!(!settings.hud.replace_native_panel);
    assert_eq!(settings.hud.view, View::Wheel);
    assert_eq!(settings.hud.scale, 3.0);
    assert_eq!(settings.limits.slots, 24);
    assert_eq!(settings.limits.weapon_slots, 8);
    assert!(settings.auto_store);

    settings.infection.screamer_image = String::from("C:\\Game\\photo \"2\".png");
    assert_eq!(Settings::parse(&settings.to_toml()).unwrap(), settings);
}

#[test]
fn catalog_and_ammo_defaults() {
    let catalog = Catalog::default();
    assert_eq!(catalog.items().count(), 36);
    for (id, def) in catalog.items() {
        assert_eq!(catalog.find_item(def.classname), Some(id));
        assert!(def.max_stack >= 1);
        assert!(!def.is_weapon() || def.max_stack == 1);
    }
    assert!(catalog.find_item("weapon_ammo_spawn").is_none());
    assert_eq!(AmmoKind::from_index(3), Some(AmmoKind::AssaultRifle));
    assert_eq!(AmmoKind::from_index(8), None);
    assert_eq!(AmmoKind::AssaultRifle.definition().magazine, 50);
    assert_eq!(catalog.ammo_max(AmmoKind::Smg), 650);
    assert!(!AmmoKind::M60.is_unloadable());
    assert!(!AmmoKind::Chainsaw.is_unloadable());
    assert!(AmmoKind::Shotgun.is_unloadable());

    let mut pool = [0; 32];
    pool[3] = 360;
    pool[7] = 50;
    pool[8] = 100;
    pool[5] = -1;
    assert!((catalog.ammo_weight(&pool) - 5.6).abs() < 1e-5);
    assert_eq!(
        catalog.split_unload(Some(AmmoKind::SniperRifle), 30, 0, Some(0.25)),
        UnloadSplit {
            to_reserve: 12,
            to_ground: 18
        }
    );
    assert_eq!(
        catalog.split_unload(Some(AmmoKind::Shotgun), 8, 0, Some(0.32)),
        UnloadSplit {
            to_reserve: 8,
            to_ground: 0
        }
    );
    assert_eq!(
        catalog.split_unload(Some(AmmoKind::AssaultRifle), 10, 355, None),
        UnloadSplit {
            to_reserve: 5,
            to_ground: 5
        }
    );
}

#[test]
fn items_stack_move_select_and_count_towards_weight() {
    let mut inventory = Inventory::new(Limits {
        slots: 4,
        ..Limits::default()
    });
    let pills = inventory.catalog().find_item("weapon_pain_pills").unwrap();
    let medkit = inventory
        .catalog()
        .find_item("weapon_first_aid_kit")
        .unwrap();
    assert_eq!(inventory.add(pills, 6, 0), 6);
    assert_eq!(inventory.add(medkit, 1, 0), 1);
    let count = |inventory: &Inventory, index| inventory.slot(index).unwrap().count;
    assert_eq!((count(&inventory, 0), count(&inventory, 1)), (4, 2));

    assert!(inventory.move_slot(2, 3));
    assert!(inventory.move_slot(1, 0)); // both full or the destination is full: swap
    assert_eq!((count(&inventory, 0), count(&inventory, 1)), (2, 4));
    assert_eq!(inventory.remove_from_slot(1, 2), 2);
    assert!(inventory.move_slot(1, 0)); // same kind with room: merge
    assert!(inventory.slot(1).unwrap().is_empty());
    assert!(inventory.move_slot(0, 3));
    assert_eq!(inventory.slot(0).unwrap().item, Some(medkit));
    assert_eq!(inventory.slot(3).unwrap().item, Some(pills));
    assert!(!inventory.move_slot(0, 4));
    assert!(!inventory.move_slot(0, 20));
    assert!(!inventory.move_slot(1, 2));
    assert!(!inventory.move_slot(0, 0));
    assert_eq!(inventory.remove_from_slot(90, 1), 0);

    inventory.select(3);
    inventory.select_step(1);
    assert_eq!(inventory.selected(), 0);
    inventory.select_step(-1);
    assert_eq!(inventory.selected(), 3);
    assert_eq!(inventory.remove_item(pills, 9), 4);
    assert_eq!(inventory.used_slots(), 1);

    inventory.set_max_weight(3.0);
    let revision = inventory.revision();
    assert_eq!(inventory.add(pills, 1, 0), 0);
    assert_eq!(inventory.revision(), revision);
    assert_eq!(inventory.remove_from_slot(0, 1), 1);
    assert!(inventory.set_held(0, medkit));
    assert_eq!(inventory.held_slot(medkit), Some(0));
    assert_eq!(inventory.weight(), 3.0);
    assert_eq!(inventory.to_text(), "inventory 1\n");
    assert_eq!(inventory.home_for(medkit), Some(1));
    inventory.release_held(0, 0);
    assert_eq!(inventory.weight(), 3.0);
    assert_eq!(inventory.count_of(medkit), 1);
    inventory.set_held(0, medkit);
    inventory.clear_held(0);
    assert_eq!(inventory.weight(), 3.0);
}

#[test]
fn files_keep_weapon_details_and_reassign_legacy_slots() {
    let mut inventory = Inventory::new(Limits {
        slots: 2,
        weapon_slots: 3,
        max_weight: 6.0,
    });
    let ak = inventory.catalog().find_item("weapon_rifle_ak47").unwrap();
    let melee = inventory.catalog().find_item("weapon_melee").unwrap();
    let state = WeaponState {
        clip: 17,
        ammo: 240,
        upgrades: 5,
        special: 11,
        ..WeaponState::default()
    };
    assert_eq!(inventory.add_weapon(ak, state.clone(), None), Some(2));
    assert_eq!(inventory.add_weapon(ak, WeaponState::default(), None), None);
    inventory.set_max_weight(0.0);
    assert_eq!(inventory.add_weapon(ak, state.clone(), Some(0)), None);
    let axe = WeaponState {
        melee: "fireaxe".into(),
        wear: 4_321,
        ..WeaponState::default()
    };
    assert_eq!(inventory.add_weapon(melee, axe.clone(), Some(4)), Some(4));
    assert!(inventory.move_slot(4, 3));

    let text = inventory.to_text();
    assert_eq!(
        text,
        "inventory 1\n2 weapon_rifle_ak47 1 17 240 - 5 11 0\n3 weapon_melee 1 -1 -1 fireaxe 0 0 4321\n"
    );
    let mut copy = Inventory::new(inventory.limits());
    assert!(copy.load_text(&text));
    assert_eq!(copy.slot(2).unwrap().weapon, state);
    assert_eq!(copy.slot(3).unwrap().weapon, axe);
    assert!(!copy.load_text("inventory 2\n"));
    assert_eq!(copy.to_text(), text);

    let legacy = "\r\n\tinventory 1\r\n0 weapon_shotgun_chrome 1 8 51 -\r\n1 weapon_pain_pills 99\r\n1 weapon_unknown 1\r\n7 weapon_gascan 1\r\nbad line\n";
    assert!(copy.load_text(legacy));
    assert_eq!(
        copy.slot(0).unwrap().item,
        copy.catalog().find_item("weapon_gascan")
    );
    assert_eq!(copy.slot(1).unwrap().count, 4);
    let gun = &copy.slot(2).unwrap().weapon;
    assert_eq!(gun.ammo, 51);
    assert_eq!(gun.upgrades, 0);
    assert_eq!(gun.wear, 0);
}

#[test]
fn gas_can_fuel_survives_transfer_and_save() {
    let (rules, report) = Rules::parse("[chainsaw]\ncan_fuel = 90\n[stack]\ngascan = 3\n").unwrap();
    assert!(report.diagnostics.is_empty());
    let can = rules.catalog.find_item("weapon_gascan").unwrap();
    let mut inventory = Inventory::with_catalog(Limits::default(), Arc::clone(&rules.catalog));
    assert_eq!(inventory.add(can, 2, 0), 2);
    assert_eq!(inventory.slot(0).unwrap().fuel(&rules.catalog), 180);
    assert_eq!(inventory.take_fuel(0, 30), 30);
    assert_eq!(inventory.take_fuel(0, 70), 70);
    assert_eq!(inventory.next_spent(0), 10);
    assert_eq!(inventory.take_fuel(0, 500), 80);
    assert!(inventory.slot(0).unwrap().is_empty());

    assert_eq!(inventory.add(can, 1, 60), 1);
    assert_eq!(inventory.add(can, 1, 60), 1);
    assert_eq!(inventory.next_spent(0), 30);
    assert!(inventory.set_held(1, can));
    inventory.release_held(1, 20);
    assert!(inventory.move_slot(1, 0));
    assert_eq!(inventory.slot(0).unwrap().fuel(&rules.catalog), 130);
    let text = inventory.to_text();
    assert!(inventory.load_text(&text));
    assert_eq!(inventory.slot(0).unwrap().fuel(&rules.catalog), 130);
    assert!(inventory.load_text("inventory 1\n0 weapon_gascan 1 200\n1 weapon_gascan 1\n"));
    assert!(inventory.slot(0).unwrap().is_empty());
    assert_eq!(inventory.slot(1).unwrap().fuel(&rules.catalog), 90);
    let mut defaults = Inventory::default();
    defaults.add(can, 1, 40);
    assert_eq!(defaults.slot(0).unwrap().spent, 0);
}

#[test]
fn parses_legacy_rules_and_preserves_partial_updates() {
    let (mut rules, report) = Rules::parse(
        "[limits]\nslots = 6\nmax_weight = 12\n[weight]\nfirst_aid_kit = 5.5\nweapon_pain_pills = 0.5\n[stack]\nmolotov = 1\n[ammo]\nassault_rifle = 200\n[round_weight]\nassault_rifle = 0.02\n",
    ).unwrap();
    assert!(report.diagnostics.is_empty());
    assert_eq!(report.items_changed, 3);
    assert_eq!(report.ammo_changed, 1);
    assert_eq!(
        rules.limits.apply(Limits::default()),
        Limits {
            slots: 6,
            max_weight: 12.0,
            weapon_slots: 2
        }
    );
    let mut inventory = Inventory::with_catalog(
        rules.limits.apply(Limits::default()),
        Arc::clone(&rules.catalog),
    );
    let kit = inventory
        .catalog()
        .find_item("weapon_first_aid_kit")
        .unwrap();
    let pills = inventory.catalog().find_item("weapon_pain_pills").unwrap();
    assert_eq!(inventory.add(kit, 5, 0), 2);
    assert_eq!(inventory.add(pills, 5, 0), 2);
    assert_eq!(inventory.weight(), 12.0);

    assert!(rules.reload("[limits\nslots = 3").is_err());
    assert_eq!(rules.limits.slots, Some(6));
    assert_eq!(rules.catalog.round_weight(AmmoKind::AssaultRifle), 0.02);
    let report = rules.reload("[limits]\nslots = 0\n[weight]\nno_such_item = 1\n[stack]\nmolotov = \"two\"\n[colour]\n").unwrap();
    assert_eq!(report.diagnostics.len(), 4, "{report:?}");
    assert!(report.diagnostics[0].starts_with("line 2:"));
    assert_eq!(report.items_changed, 0);
    assert_eq!(rules.limits.slots, None);
    assert_eq!(rules.catalog.item(kit).weight, 5.5);
    assert_eq!(rules.catalog.ammo_max(AmmoKind::AssaultRifle), 200);
    assert_eq!(rules.catalog.round_weight(AmmoKind::AssaultRifle), 0.01);
}

#[test]
fn validates_optional_sections_and_the_shipped_rules_file() {
    let (rules, report) = Rules::parse(include_str!("../inventory_mod.example.toml")).unwrap();
    assert!(report.diagnostics.is_empty(), "{report:?}");
    assert!(rules.infection.enabled && rules.stamina.enabled && rules.melee.enabled);
    assert!(rules.ammo_piles.enabled && rules.map_guns.enabled && rules.mutators.enabled);
    assert!(rules.ping.enabled && rules.self_revive.enabled && rules.chainsaw.enabled);
    for invalid in [
        "[infection]\nbots = 1",
        "[stamina]\nrun = true",
        "[stamina]\nswing_speed = 1",
        "[melee]\nheavy_hold = 0",
        "[melee]\nhead_only = 1",
        "[melee.weapons.knife]\nclass = \"sword\"",
        "[ammo_piles]\nmin_magazines = 1.5",
        "[map_guns]\nmin_reserve = 21",
        "[ping]\nper_player = 9",
        "[self_revive]\nitems = [\"chainsaw\"]",
        "[chainsaw]\ncan_fuel = 0",
        "[mutators.fog]\ncolor = \"40 60 400\"",
        "[mutators.hunted]\nclass = \"tank\"",
        "[mutators]\npool = [\"rain\"]",
        "[mutators]\npool = [\"brittle\"]",
    ] {
        let (_, report) = Rules::parse(invalid).unwrap();
        assert_eq!(report.diagnostics.len(), 1, "{invalid}: {report:?}");
    }
    let (rules, report) = Rules::parse(
        "[infection]\nspecials = [\"hunter\", \"charger\", \"tank\"]\n[ammo_piles]\nkinds = [\"smg\", \"hunting_rifle\", \"auto_shotgun\"]\nmin_magazines = 4\n[self_revive]\nitems = [\"adrenaline\", \"pain_pills\", \"weapon_pain_pills\"]",
    ).unwrap();
    assert_eq!(report.diagnostics.len(), 3);
    assert_eq!(rules.infection.specials, [3, 6]);
    assert_eq!(
        rules.ammo_piles.kinds,
        [AmmoKind::Smg, AmmoKind::HuntingRifle]
    );
    assert_eq!(rules.ammo_piles.max_magazines, 4);
    assert_eq!(rules.self_revive.items.len(), 2);
    assert!(rules.revives("weapon_pain_pills"));
    assert!(!rules.revives("weapon_first_aid_kit"));
}

#[test]
fn mutators_roll_in_pool_order_and_apply_chapter_rules() {
    use Mutator::*;
    let (rules, report) = Rules::parse(
        "[mutators]\npool = [\"scarcity\", \"heavy_load\", \"scarcity\", \"contagion\"]\ncount = 2\n[mutators.maps]\nc1m1_hotel = []\nc1m2_streets = [\"fog\"]\n[mutators.scarcity]\nammo = 25\n[infection]\nseconds = 200\n[ammo_piles]\nmax_magazines = 4\n",
    ).unwrap();
    assert!(report.diagnostics.is_empty(), "{report:?}");
    assert_eq!(rules.mutators.pool, [Scarcity, HeavyLoad, Contagion]);
    assert!(pick_mutators(&rules.mutators, "c1m1_hotel", 7).is_empty());
    assert_eq!(pick_mutators(&rules.mutators, "c1m2_streets", 7), [Fog]);
    assert_eq!(
        pick_mutators(&rules.mutators, "c1m3_mall", 7),
        [HeavyLoad, Contagion]
    );

    let chapter = rules.chapter(&[Scarcity, HeavyLoad, Contagion]);
    assert_eq!(chapter.ammo_piles.max_magazines, 1);
    assert_eq!(chapter.map_guns.min_clip, 5);
    assert_eq!(chapter.map_guns.max_clip, 30);
    assert_eq!(chapter.map_guns.max_reserve, 0);
    assert!(chapter.stamina.enabled);
    assert_eq!(chapter.stamina.run, 12.0);
    assert_eq!(chapter.stamina.regen, 20.0);
    assert_eq!(chapter.max_weight(20.0), 12.0);
    assert_eq!(chapter.max_weight(0.0), 15.0);
    assert_eq!(chapter.infection.seconds, 120);
    assert!(!rules.stamina.enabled);
    assert_eq!(rules.chapter(&[]).ammo_piles.max_magazines, 4);
}

#[test]
fn melee_profiles_and_attacks_follow_class_and_stamina() {
    let (rules, report) = Rules::parse(
        "[melee.weapons.katana]\nclass = \"blunt_1h\"\ndurability = 77\nheavy_hold = 1.5\n[melee.weapons.frying_pan]\nstamina = 50\n",
    ).unwrap();
    assert!(report.diagnostics.is_empty());
    let axe = melee_profile("fireaxe", &rules.melee);
    let knife = melee_profile("knife", &rules.melee);
    assert_eq!(axe.class, MeleeClass::Blade2h);
    assert_eq!(knife.class, MeleeClass::Blade1h);
    assert!(axe.heavy_hold > knife.heavy_hold && axe.cleave > knife.cleave);
    assert_eq!(melee_profile("katana", &rules.melee).durability, 77);
    assert_eq!(melee_profile("frying_pan", &rules.melee).stamina, 50.0);
    assert_eq!(melee_overhead_swing(&[2, 7, 4]), Some(1));
    assert_eq!(melee_overhead_swing(&[7, 4, 0]), Some(2));
    assert_eq!(melee_overhead_swing(&[2, 3]), None);
    assert!((cleave_scale(2, 70.0, 30.0) - 0.49).abs() < 1e-5);
    assert_eq!(melee_wear_after(0, 1.0, 200), 50);
    assert_eq!(melee_wear_after(9990, 1.0, 200), WEAR_BROKEN);
    assert_eq!(melee_wear_after(100, 3.0, 0), 100);

    let mut input = MeleeInput::default();
    let mut frame = MeleeFrame {
        melee: true,
        hold: 0.6,
        shove_hold: 0.5,
        game_time: 100.0,
        ..MeleeFrame::default()
    };
    let cmd = |input: &mut MeleeInput, frame: &mut MeleeFrame, buttons, now| {
        *frame = MeleeFrame {
            buttons,
            now,
            ..*frame
        };
        input.command(*frame)
    };
    let attack = BUTTON_ATTACK;
    let shove = BUTTON_ATTACK2;
    assert_eq!(cmd(&mut input, &mut frame, attack, 1.0), BUTTON_WINDING);
    assert_eq!(input.peek(attack | 8), BUTTON_WINDING | 8);
    assert_eq!(cmd(&mut input, &mut frame, 0, 1.7), attack | BUTTON_HEAVY);
    frame.next_attack = 2.0;
    assert_eq!(cmd(&mut input, &mut frame, 0, 1.75), 0);
    assert_eq!(input.swing(), Swing::Idle);

    assert_eq!(cmd(&mut input, &mut frame, attack, 2.0), BUTTON_WINDING);
    frame.can_heavy = false;
    assert_eq!(cmd(&mut input, &mut frame, attack, 2.05), attack);
    assert!(matches!(input.swing(), Swing::Releasing(release) if !release.heavy));
    assert_eq!(cmd(&mut input, &mut frame, 0, 2.2), 0);

    frame.can_heavy = true;
    assert_eq!(cmd(&mut input, &mut frame, shove, 3.0), 0);
    assert_eq!(
        cmd(&mut input, &mut frame, 0, 3.6),
        shove | BUTTON_HEAVY_SHOVE
    );
    frame.next_shove = 4.0;
    assert_eq!(cmd(&mut input, &mut frame, 0, 3.7), 0);
    assert_eq!(input.shove(), Shove::Idle);

    cmd(&mut input, &mut frame, attack, 4.0);
    assert_eq!(
        cmd(&mut input, &mut frame, attack | BUTTON_RELOAD, 4.1),
        BUTTON_THROW
    );
    assert_eq!(input.swing(), Swing::Cancelled);
    assert_eq!(input.thrown_at(), Some(4.1));
    assert_eq!(cmd(&mut input, &mut frame, attack, 4.2), BUTTON_THROW);
    assert_eq!(cmd(&mut input, &mut frame, attack, 4.3), 0);
}

#[test]
fn remote_state_matches_legacy_client_and_keeps_host_stack_sizes() {
    let mut inventory = Inventory::new(Limits {
        slots: 2,
        max_weight: 20.0,
        weapon_slots: 1,
    });
    let pills = inventory.catalog().find_item("weapon_pain_pills").unwrap();
    inventory.add(pills, 1, 0);
    let mut native: [NativeWeapon; 6] = std::array::from_fn(|_| NativeWeapon::default());
    native[1] = NativeWeapon {
        classname: "weapon_melee".into(),
        active: true,
        melee: "fireaxe".into(),
        wear: Some(123),
    };
    native[4].classname = "weapon_pain_pills".into();
    // Reconciliation counts the held pills, stored pills, equipped melee and carried ammunition.
    reconcile_native_inventory(&mut inventory, &native, 0.25);
    assert_eq!(inventory.weight(), 4.25);
    let wire = encode_remote_state(&inventory, &native, "Stored");
    assert_eq!(
        wire,
        "@inv@ 2 20.00 4.25 1|0 weapon_pain_pills 1 1,|-,*weapon_melee:fireaxe:123,-,-,weapon_pain_pills,-|Stored\n"
    );
    let decoded = RemoteState::decode(&wire, inventory.catalog()).unwrap();
    assert_eq!(decoded.native, native);
    assert_eq!(decoded.weight, Some(4.25));
    assert_eq!(decoded.slots[0].as_ref().unwrap().item, pills);
    assert!(decoded.slots[0].as_ref().unwrap().held);

    let old = RemoteState::decode("@inv@ 2 1.50 5.25|0 weapon_gascan 3 0 20 90,|-,*weapon_melee:fireaxe|keeps | separators\r\n", inventory.catalog()).unwrap();
    assert_eq!(old.limits.weapon_slots, 0);
    assert_eq!(old.message, "keeps | separators");
    let gas = old.slots[0].as_ref().unwrap();
    assert_eq!(gas.count, 3);
    assert_eq!(gas.fuel.unwrap().full, 90);
    assert!(RemoteState::decode("@inv@ 65 1.50 5.25|||", inventory.catalog()).is_none());
    assert!(RemoteState::decode("normal console output", inventory.catalog()).is_none());

    reconcile_native_inventory(&mut inventory, &native[..2], 0.25);
    assert_eq!(inventory.held_slot(pills), None);
}
