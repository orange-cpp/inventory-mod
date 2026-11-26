use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    ffi::{CStr, CString, c_char, c_int, c_void},
    fs,
    panic::{AssertUnwindSafe, catch_unwind},
    path::PathBuf,
    ptr::{self, NonNull},
    sync::Arc,
    time::{Duration, Instant, SystemTime},
};

use inventory_mod::{
    inventory::{Inventory, Limits},
    items::AmmoKind,
    mutators::{Mutator, pick_mutators},
    remote_state::{encode_remote_state, reconcile_native_inventory},
    rules::{ChapterRules, Rules},
    settings::Settings,
};

use crate::{
    game::{
        ActionContext, Actions, AttackContext, DamageInput, Exposure, Game, Marker, PickupCall,
        PickupContext, PingContext, Pings, Reserve, Vector,
    },
    source::{self, Clients, Command, ConVar, Edict, Entity, Factory, Source},
};

const CONTINUE: c_int = 0;
const STOP: c_int = 2;

struct Player {
    inventory: Inventory,
    file: Option<PathBuf>,
    chapter_start: String,
    last_saved: u32,
    edict: Option<NonNull<Edict>>,
    transition_reserve: Option<Reserve>,
    condition: Vec<inventory_mod::remote_state::CarriedCondition>,
    death_position: Option<(u32, Vector)>,
    next_mutators: Option<Instant>,
    pending_notice: String,
    actions: Actions,
    last_ping: Option<Instant>,
}

impl Player {
    fn new(
        rules: &Rules,
        limits: Limits,
        file: Option<PathBuf>,
        campaign_start: Option<SystemTime>,
    ) -> Self {
        let mut inventory = Inventory::with_catalog(limits, Arc::clone(&rules.catalog));
        if let Some(file) = &file {
            let previous_campaign = campaign_start.is_some_and(|started| {
                fs::metadata(file)
                    .and_then(|meta| meta.modified())
                    .is_ok_and(|saved| saved < started)
            });
            if !previous_campaign && let Ok(contents) = fs::read_to_string(file) {
                inventory.load_text(&contents);
            }
        }
        Self {
            chapter_start: inventory.to_text(),
            last_saved: inventory.revision(),
            inventory,
            file,
            edict: None,
            transition_reserve: None,
            condition: Vec::new(),
            death_position: None,
            next_mutators: None,
            pending_notice: String::new(),
            actions: Actions::default(),
            last_ping: None,
        }
    }

    fn restore_equipment(&mut self, game: &mut Game, entity: NonNull<Entity>) {
        if let Some(reserve) = self.transition_reserve.take() {
            game.restore_reserve(entity, &reserve);
            game.restore_condition(entity, &self.condition);
            self.condition.clear();
        }
    }

    fn save(&mut self) {
        let Some(file) = self
            .file
            .as_ref()
            .filter(|_| self.inventory.revision() != self.last_saved)
        else {
            return;
        };
        let temporary = file.with_extension("inventory.tmp");
        match fs::write(&temporary, self.inventory.to_text())
            .and_then(|()| fs::rename(&temporary, file))
        {
            Ok(()) => self.last_saved = self.inventory.revision(),
            Err(error) => {
                let _ = fs::remove_file(temporary);
                eprintln!("inventory_mod: could not save {} ({error})", file.display());
            }
        }
    }
}

struct Server {
    source: Source,
    game: Game,
    rules: Rules,
    chapter: ChapterRules,
    active_mutators: Vec<Mutator>,
    mutator_generation: u32,
    hunted_class: i32,
    saved_cvars: Vec<(ConVar, f32)>,
    horde_at: Option<f32>,
    place_piles_at: Option<Instant>,
    clients: Option<Clients>,
    pings: Pings,
    next_pickup_scan: Option<Instant>,
    limits: Limits,
    directory: PathBuf,
    players: HashMap<i32, Player>,
    away: HashMap<PathBuf, Player>,
    map: String,
    round_map: String,
    transitioned_players: HashSet<i32>,
    transition: bool,
    campaign_start: Option<SystemTime>,
    paused: bool,
    paused_level: Option<i32>,
}

impl Server {
    unsafe fn load(engine: Factory, game: Factory) -> Option<Self> {
        let source = unsafe { Source::bind(engine, game) }?;
        let Some(mut game) = Game::bind() else {
            eprintln!(
                "inventory_mod: unsupported server_srv.so build or unavailable game functions"
            );
            return None;
        };
        let directory = source.directory()?.join("addons/inventory_mod");
        if let Err(error) = fs::create_dir_all(&directory) {
            eprintln!(
                "inventory_mod: could not open {} ({error})",
                directory.display()
            );
            return None;
        }
        let file = directory.join("inventory_mod.toml");
        let mut rules = Rules::default();
        match fs::read_to_string(&file) {
            Ok(text) => match rules.reload(&text) {
                Ok(report) => {
                    for warning in report.diagnostics {
                        eprintln!("inventory_mod: {warning}");
                    }
                }
                Err(error) => eprintln!("inventory_mod: {error}"),
            },
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => eprintln!("inventory_mod: could not read {} ({error})", file.display()),
        }
        let limits = rules.limits.apply(Limits {
            slots: 24,
            max_weight: 0.0,
            weapon_slots: 8,
        });
        let chapter = rules.chapter(&[]);
        let mutator_generation = game.round_seed();
        if !game.install_ammo() {
            eprintln!("inventory_mod: ammo-pile handling could not be installed");
        }
        let auto_store = fs::read_to_string(directory.join(Settings::NAME))
            .ok()
            .and_then(|text| {
                Settings::parse(&text)
                    .map_err(|error| eprintln!("inventory_mod: invalid settings file ({error})"))
                    .ok()
            })
            .is_some_and(|settings| settings.auto_store);
        if !game.install_pickups(auto_store) {
            eprintln!("inventory_mod: automatic pickups could not be installed");
        }
        if !game.install_actions(&rules) {
            eprintln!("inventory_mod: chainsaw refuelling could not be installed");
        }
        if !game.install_infection(&rules) {
            eprintln!("inventory_mod: infection handling could not be installed");
        }
        if !game.install_melee(&rules.melee) {
            eprintln!("inventory_mod: expanded melee could not be installed");
        }
        let mut server = Self {
            source,
            game,
            rules,
            chapter,
            limits,
            directory,
            active_mutators: Vec::new(),
            mutator_generation,
            hunted_class: 0,
            saved_cvars: Vec::new(),
            horde_at: None,
            place_piles_at: None,
            clients: None,
            pings: Pings::default(),
            next_pickup_scan: None,
            players: HashMap::new(),
            away: HashMap::new(),
            map: String::new(),
            round_map: String::new(),
            transitioned_players: HashSet::new(),
            transition: false,
            campaign_start: None,
            paused: false,
            paused_level: None,
        };
        server.listen();
        Some(server)
    }

    fn listen(&mut self) {
        for name in [
            c"map_transition",
            c"player_transitioned",
            c"round_start",
            c"player_death",
            c"player_spawn",
            c"player_left_start_area",
            c"round_end",
            c"mission_lost",
            c"finale_vehicle_leaving",
            c"finale_win",
            c"tongue_grab",
            c"lunge_pounce",
            c"jockey_ride",
            c"charger_carry_start",
            c"charger_pummel_start",
            c"player_now_it",
            c"player_incapacitated",
            c"pills_used",
        ] {
            self.source.add_listener(listener(), name);
        }
    }

    fn pause(&mut self) {
        if self.paused {
            return;
        }
        self.paused = true;
        self.paused_level = self.source.current_level().map(|(number, _, _)| number);
        self.source.remove_listener(listener());
        self.pings.clear(&self.game, true);
        self.game.clear_melee(true);
        self.game
            .clear_stamina(&self.source, true, |id| self.players.contains_key(&id));
        self.game
            .clear_infection(&self.source, true, |id| self.players.contains_key(&id));
        self.restore_cvars();
        self.horde_at = None;
        self.mutator_generation = self.mutator_generation.wrapping_add(1);
        let clear = CString::new(format!("@mut@ {} 0|\n", self.mutator_generation)).unwrap();
        for (id, mut player) in std::mem::take(&mut self.players) {
            let edict = player
                .edict
                .take()
                .filter(|&e| self.source.user_id(e) == Some(id));
            self.game.cancel_actions(
                edict.and_then(|e| self.source.entity(e)),
                &mut player.actions,
            );
            if let Some(edict) = edict {
                self.source.client_print(edict, c"@mel@ 0 0 0\n");
                self.source.client_print(edict, &clear);
            }
            player.pending_notice.clear();
            player.next_mutators = None;
            player.death_position = None;
            player.last_ping = None;
            player.save();
            if let Some(file) = &player.file {
                self.away.insert(file.clone(), player);
            } else {
                // LAN-only clients may keep their existing connection and user ID.
                self.players.insert(id, player);
            }
        }
        self.clients = None;
    }

    fn unpause(&mut self) {
        if !self.paused {
            return;
        }
        self.paused = false;
        self.listen();
        let before = self.paused_level.take();
        if let Some((number, map, clients)) = self.source.current_level() {
            self.clients = Some(clients);
            self.map = map;
            if before != Some(number) {
                // The plugin manager skips its level callbacks while disabled.
                self.game.reset();
                self.game.clear_piles(&self.source, false);
                self.transition = false;
                self.next_pickup_scan = None;
                self.start_round();
            } else {
                self.apply_cvars();
                self.mutator_generation = self.mutator_generation.wrapping_add(1);
            }
        }
    }

    fn shutdown_level(&mut self) {
        self.game.reset();
        self.game.clear_melee(false);
        self.game.clear_stamina(&self.source, false, |_| false);
        self.game.clear_infection(&self.source, true, |_| false);
        self.game.clear_piles(&self.source, false);
        self.pings.clear(&self.game, false);
        self.place_piles_at = None;
        self.clients = None;
        self.next_pickup_scan = None;
        for player in self.players.values_mut() {
            player.save();
            player.edict = None;
            player.death_position = None;
            player.next_mutators = None;
            player.actions = Actions::default();
            player.last_ping = None;
        }
    }

    fn start_round(&mut self) {
        if !self.transition {
            self.transitioned_players.clear();
        }
        let restart = !self.transition && !self.map.is_empty() && self.round_map == self.map;
        let new_campaign = !restart && !self.transition && !self.round_map.is_empty();
        self.game
            .clear_infection(&self.source, true, |user| self.players.contains_key(&user));
        if !restart {
            self.active_mutators =
                pick_mutators(&self.rules.mutators, &self.map, self.game.round_seed());
            self.mutator_generation = self.mutator_generation.wrapping_add(1);
            self.hunted_class = if self.active_mutators.contains(&Mutator::Hunted) {
                self.rules
                    .mutators
                    .hunted
                    .class
                    .unwrap_or_else(|| (self.game.round_seed() % 6) as i32 + 1)
            } else {
                0
            };
            self.apply_cvars();
        }
        self.horde_at = None;
        self.game.clear_melee(true);
        self.game
            .clear_stamina(&self.source, true, |id| self.players.contains_key(&id));
        self.chapter = self.rules.chapter(&self.active_mutators);
        self.pings.clear(&self.game, true);
        self.game.reset();
        self.game.clear_piles(&self.source, true);
        self.place_piles_at = self
            .chapter
            .ammo_piles
            .enabled
            .then(|| Instant::now() + Duration::from_millis(1500));
        if new_campaign {
            self.campaign_start = Some(SystemTime::now());
        }

        for (id, player) in self
            .players
            .iter_mut()
            .map(|(&id, p)| (Some(id), p))
            .chain(self.away.values_mut().map(|p| (None, p)))
        {
            player.death_position = None;
            let entity = player
                .edict
                .filter(|&edict| id.is_some() && self.source.user_id(edict) == id)
                .and_then(|edict| self.source.entity(edict));
            self.game.cancel_actions(entity, &mut player.actions);
            if restart {
                player.inventory.load_text(&player.chapter_start);
            } else if new_campaign {
                player.inventory.clear();
            }
            player
                .inventory
                .set_max_weight(self.chapter.max_weight(self.limits.max_weight));
            player.next_mutators = None;
            if !self.transition {
                player.transition_reserve = None;
                player.condition.clear();
            }
            player.chapter_start = player.inventory.to_text();
            player.save();
        }
        self.round_map.clone_from(&self.map);
        self.transition = false;
    }

    fn event(&mut self, event: Event) {
        match event {
            Event::RoundStart => self.start_round(),
            Event::RoundEnd => {
                self.game
                    .clear_infection(&self.source, false, |user| self.players.contains_key(&user));
            }
            Event::Exposure(exposure) => {
                self.game.exposure(
                    &self.source,
                    self.clients.as_ref(),
                    exposure,
                    &self.chapter.infection,
                    |user| self.players.contains_key(&user),
                );
            }
            Event::LeftStartArea => {
                if self.horde_at.is_none() && self.active_mutators.contains(&Mutator::HordeTide) {
                    self.horde_at = self
                        .source
                        .current_time()
                        .map(|now| now + self.rules.mutators.horde_tide.first as f32);
                }
            }
            Event::PlayerTransitioned(user) => {
                self.transitioned_players.insert(user);
            }
            Event::Transition => {
                self.transitioned_players.clear();
                self.game
                    .clear_infection(&self.source, true, |user| self.players.contains_key(&user));
                self.transition = true;
                for player in self.players.values_mut() {
                    if let Some(edict) = player.edict
                        && self
                            .source
                            .player_info(edict)
                            .is_some_and(|info| info.team == 2)
                        && let Some(entity) = self.source.entity(edict)
                    {
                        player.transition_reserve = Some(self.game.reserve(entity));
                        player.condition = self.game.carried_condition(entity);
                    }
                }
            }
            Event::Death(user) => {
                self.game
                    .forget_infection(&self.source, user, self.players.contains_key(&user));
                if let Some(player) = self.players.get_mut(&user)
                    && let Some(edict) = player.edict
                    && self
                        .source
                        .player_info(edict)
                        .is_some_and(|info| info.team == 2)
                {
                    player.death_position = self.game.death_position(&self.source, edict);
                }
            }
            Event::Spawn(user) => {
                if let Some(player) = self.players.get_mut(&user) {
                    player.death_position = None;
                }
            }
        }
    }

    fn game_frame(&mut self) {
        if self.paused {
            return;
        }
        let now = Instant::now();
        self.game
            .melee_frame(&self.source, self.clients.as_ref(), &self.chapter, |user| {
                self.players.contains_key(&user)
            });
        for (user, message) in self.game.melee_notices() {
            if let Some(player) = self.players.get_mut(&user) {
                player.pending_notice = message;
            }
        }
        for marker in self.pings.advance(&self.game, now) {
            self.send_ping(&marker);
        }
        self.game
            .infection_frame(&self.source, &self.chapter.infection, |user| {
                self.players.contains_key(&user)
            });
        self.game.stamina_frame(
            &self.source,
            self.clients.as_ref(),
            &self.chapter.stamina,
            |id| self.players.contains_key(&id),
        );
        if self.next_pickup_scan.is_none_or(|at| now >= at) {
            self.game.scan_pickups(&self.source);
            self.next_pickup_scan = Some(now + Duration::from_millis(350));
        }
        if self.place_piles_at.is_some_and(|at| now >= at) {
            self.place_piles_at = None;
            self.game
                .place_map_piles(&self.source, &self.chapter.ammo_piles);
        }
        if let Some(game_time) = self.source.current_time()
            && self.horde_at.is_some_and(|at| game_time >= at)
        {
            self.horde_at = Some(game_time + self.rules.mutators.horde_tide.every as f32);
            self.game.trigger_panic();
        }
        if let Some(clients) = &self.clients {
            for edict in clients.live() {
                if self
                    .source
                    .player_info(edict)
                    .is_some_and(|info| info.connected && info.team == 2 && !info.dead)
                    && let Some(entity) = self.source.entity(edict)
                {
                    self.game.normalize_ammo(entity, &self.rules.catalog);
                }
            }
        }
        let msg = CString::new(self.mutator_line()).ok();
        for (&id, player) in &mut self.players {
            let Some(edict) = player.edict else {
                continue;
            };
            if self.source.user_id(edict) != Some(id) {
                player.edict = None;
                player.death_position = None;
                player.actions = Actions::default();
                continue;
            }
            let Some(info) = self.source.player_info(edict).filter(|info| info.connected) else {
                continue;
            };
            if let Some(entity) = self.source.entity(edict) {
                if info.team == 2 && !info.dead {
                    if !self.transition && self.transitioned_players.remove(&id) {
                        player.restore_equipment(&mut self.game, entity);
                    }
                    let mut context = ActionContext {
                        source: &self.source,
                        rules: &self.rules,
                        inventory: &mut player.inventory,
                        actions: &mut player.actions,
                    };
                    if let Some(notice) = self.game.action_frame(entity, &mut context) {
                        player.pending_notice = notice;
                        player.save();
                    }
                } else {
                    self.game.cancel_actions(Some(entity), &mut player.actions);
                }
            }
            if info.dead
                && let Some((handle, position)) = player.death_position.take()
                && info.team == 2
                && let Some(entity) = self
                    .source
                    .entity(edict)
                    .filter(|&actor| self.game.handle(actor) == handle)
            {
                let offset = self
                    .game
                    .drop_on_death(&self.source, position, &mut player.inventory);
                self.game
                    .drop_reserve(&self.source, entity, position, offset);
                player.save();
            }
            if let Some(msg) = &msg
                && player.next_mutators.is_none_or(|next| now >= next)
            {
                self.source.client_print(edict, msg);
                player.next_mutators = Some(now + Duration::from_secs(2));
            }
        }
    }

    fn client_command(&mut self, edict: NonNull<Edict>, command: &Command) -> c_int {
        let name = command.arg(0);
        if self.paused || !inventory_mod::protocol::is_inventory_command(name) {
            return CONTINUE;
        }
        let Some(info) = self
            .source
            .player_info(edict)
            .filter(|info| info.connected && !info.fake)
        else {
            return STOP;
        };
        let Some(user) = self.source.user_id(edict) else {
            return STOP;
        };
        let network_id = self.source.network_id(edict);
        if network_id == "STEAM_ID_PENDING" {
            return STOP;
        }
        let player = self.players.entry(user).or_insert_with(|| {
            let limits = Limits {
                max_weight: self.chapter.max_weight(self.limits.max_weight),
                ..self.limits
            };
            let file = inventory_mod::inventory::player_file(&self.directory, network_id);
            file.as_ref()
                .and_then(|path| self.away.remove(path))
                .unwrap_or_else(|| Player::new(&self.rules, limits, file, self.campaign_start))
        });
        player.edict = Some(edict);
        let entity = self.source.entity(edict);
        let survivor = entity.filter(|_| info.team == 2 && !info.dead);
        if let Some(entity) = survivor {
            if !self.transition && self.transitioned_players.remove(&user) {
                player.restore_equipment(&mut self.game, entity);
            }
            self.game.normalize_ammo(entity, &self.rules.catalog);
        }
        let mut native = entity
            .map(|entity| self.game.native_weapons(entity))
            .unwrap_or_default();
        let rounds_weight = entity.map_or(0.0, |entity| {
            self.rules.catalog.ammo_weight(&self.game.reserve(entity))
        });
        let inventory = &mut player.inventory;
        reconcile_native_inventory(inventory, &native, rounds_weight);

        let slot = |index| command.arg(index).parse::<usize>().ok();
        let class = command.arg(1);
        let mut message = std::mem::take(&mut player.pending_notice);
        let mut marker = None;
        if name == "inv_move" {
            if let (Some(from), Some(to)) = (slot(1), slot(2)) {
                inventory.move_slot(from, to);
            }
        } else if name == "inv_ping" {
            if !self.rules.ping.enabled {
                message = "Pings are off on this server".into();
            } else if !info.dead && (2..=3).contains(&info.team) {
                let now = Instant::now();
                let cooldown = Duration::from_secs_f32(self.rules.ping.cooldown);
                if player
                    .last_ping
                    .is_none_or(|previous| now - previous >= cooldown)
                {
                    marker = self.pings.mark(
                        &self.game,
                        PingContext {
                            source: &self.source,
                            catalog: &self.rules.catalog,
                            rules: &self.rules.ping,
                            edict,
                            owner: user,
                            team: info.team,
                            now,
                        },
                    );
                    if marker.is_some() {
                        player.last_ping = Some(now);
                    }
                }
            }
        } else if let Some(entity) = survivor.filter(|_| name != "inv_sync") {
            message = match name {
                "inv_select" => {
                    let revive = inventory.catalog().find_item(class).and_then(|item| {
                        self.game.start_revive(
                            entity,
                            item,
                            None,
                            &mut ActionContext {
                                source: &self.source,
                                rules: &self.rules,
                                inventory,
                                actions: &mut player.actions,
                            },
                        )
                    });
                    revive.unwrap_or_else(|| {
                        self.game.select(entity, class, inventory);
                        String::new()
                    })
                }
                "inv_pickup" => {
                    self.game
                        .pickup(&self.source, entity, inventory, &self.chapter.map_guns)
                }
                "inv_store" => self.game.store(entity, inventory),
                "inv_unload" => {
                    self.game
                        .unload_gun(&self.source, entity, inventory, &self.chapter.map_guns)
                }
                "inv_dropammo" => AmmoKind::from_name(class)
                    .map(|kind| {
                        self.game.drop_ammo(
                            &self.source,
                            entity,
                            kind,
                            command.arg(2).parse().unwrap_or(0),
                        )
                    })
                    .unwrap_or_default(),
                "inv_use" => slot(1)
                    .map(|slot| {
                        let revive =
                            inventory
                                .slot(slot)
                                .and_then(|stack| stack.item)
                                .and_then(|item| {
                                    self.game.start_revive(
                                        entity,
                                        item,
                                        Some(slot),
                                        &mut ActionContext {
                                            source: &self.source,
                                            rules: &self.rules,
                                            inventory,
                                            actions: &mut player.actions,
                                        },
                                    )
                                });
                        revive.unwrap_or_else(|| self.game.take(entity, inventory, slot))
                    })
                    .unwrap_or_default(),
                "inv_drop" => slot(1)
                    .map(|slot| {
                        let count = command.arg(2).parse().unwrap_or(1);
                        self.game
                            .drop_items(&self.source, entity, inventory, slot, count)
                    })
                    .unwrap_or_default(),
                "inv_dropweapon" => self.game.drop_weapon(entity, class, inventory.catalog()),
                "inv_spawn" if !self.rules.debug_spawn => {
                    "Item spawning is disabled on this server".into()
                }
                "inv_spawn" => {
                    self.game
                        .spawn_item(&self.source, entity, inventory.catalog(), class)
                }
                "inv_infect" => {
                    self.game
                        .infect(&self.source, edict, &self.chapter.infection, true);
                    String::new()
                }
                _ => String::new(),
            };
            native = self.game.native_weapons(entity);
            let rounds_weight = self.rules.catalog.ammo_weight(&self.game.reserve(entity));
            reconcile_native_inventory(inventory, &native, rounds_weight);
        }
        let reply = encode_remote_state(inventory, &native, &message);
        player.save();
        if let Some(marker) = marker {
            self.send_ping(&marker);
        }
        if let Ok(reply) = CString::new(reply) {
            self.source.client_print(edict, &reply);
        }
        STOP
    }

    fn send_ping(&self, marker: &Marker) {
        let now = self.source.current_time().unwrap_or(0.0);
        for (&user, player) in &self.players {
            if let Some(edict) = player.edict
                && self.source.user_id(edict) == Some(user)
                && self
                    .source
                    .player_info(edict)
                    .is_some_and(|info| info.connected && info.team == marker.team())
                && let Ok(line) = CString::new(marker.encode(user, now))
            {
                self.source.client_print(edict, &line);
            }
        }
    }

    fn mutator_line(&self) -> String {
        use std::fmt::Write;

        let mut line = format!(
            "@mut@ {} {}|",
            self.mutator_generation,
            u8::from(self.rules.mutators.announce)
        );
        for (index, &mutator) in self.active_mutators.iter().enumerate() {
            if index != 0 {
                line.push(';');
            }
            line.push_str(mutator.id());
            match mutator {
                Mutator::Fog => {
                    let fog = &self.rules.mutators.fog;
                    let density = (fog.density * 1000.0).round() as i32;
                    let color = if fog.color.is_empty() {
                        "-"
                    } else {
                        &fog.color
                    };
                    write!(line, " {density} {color}").unwrap();
                }
                Mutator::Hunted => write!(line, " {}", self.hunted_class).unwrap(),
                _ => {}
            }
        }
        line.push('\n');
        line
    }

    fn restore_cvars(&mut self) {
        for (var, original) in self.saved_cvars.drain(..) {
            var.set(original, false);
        }
    }

    fn apply_cvars(&mut self) {
        self.restore_cvars();
        let mut set = |name: &CStr, value: i32, integer: bool, absolute: bool| {
            if let Some(var) = self.source.find_cvar(name) {
                let original = var.value();
                var.set(
                    if absolute {
                        value as f32
                    } else {
                        original * value as f32 / 100.0
                    },
                    integer,
                );
                self.saved_cvars.push((var, original));
            } else {
                eprintln!(
                    "inventory_mod: mutator setting {} is unavailable",
                    name.to_string_lossy()
                );
            }
        };
        let rules = &self.rules.mutators;
        for &mutator in &self.active_mutators {
            match mutator {
                Mutator::Hunted => {
                    let limits = [
                        c"z_smoker_limit",
                        c"z_boomer_limit",
                        c"z_hunter_limit",
                        c"z_spitter_limit",
                        c"z_jockey_limit",
                        c"z_charger_limit",
                    ];
                    if let Some(name) = limits.get(self.hunted_class.saturating_sub(1) as usize) {
                        set(name, rules.hunted.limit, true, true);
                    }
                }
                Mutator::Crowd => {
                    set(c"z_common_limit", rules.crowd.commons, true, false);
                    for name in [
                        c"z_mob_spawn_min_size",
                        c"z_mob_spawn_max_size",
                        c"z_mega_mob_size",
                    ] {
                        set(name, rules.crowd.mobs, true, false);
                    }
                }
                Mutator::Witch => {
                    set(c"z_witch_health", rules.witch.health, true, false);
                    set(c"z_witch_anger_rate", rules.witch.anger, false, false);
                    set(c"z_witch_personal_space", rules.witch.space, false, false);
                    set(c"z_witch_damage", rules.witch.damage, false, false);
                }
                Mutator::Tank => {
                    set(c"z_tank_health", rules.tank.health, true, false);
                    set(c"z_tank_speed", rules.tank.speed, false, false);
                }
                _ => {}
            }
        }
    }
}

#[derive(Clone, Copy)]
enum Event {
    Transition,
    PlayerTransitioned(i32),
    RoundStart,
    Death(i32),
    Spawn(i32),
    LeftStartArea,
    RoundEnd,
    Exposure(Exposure),
}

// Source dispatches these callbacks on the game thread. Entity operations can
// synchronously fire another event, so queue those events until the operation ends.
thread_local! {
    static SERVER: RefCell<Option<Server>> = const { RefCell::new(None) };
    static EVENTS: RefCell<Vec<Event>> = const { RefCell::new(Vec::new()) };
}

fn with_gameplay<T: Copy>(fallback: T, f: impl FnOnce(&mut Server) -> T) -> T {
    with_server(
        fallback,
        |server| if server.paused { fallback } else { f(server) },
    )
}

fn with_server<T: Copy>(fallback: T, f: impl FnOnce(&mut Server) -> T) -> T {
    ffi(fallback, || {
        SERVER.with(|server| {
            let Ok(mut guard) = server.try_borrow_mut() else {
                return fallback;
            };
            let Some(server) = guard.as_mut() else {
                return fallback;
            };
            let result = f(server);
            loop {
                let events = EVENTS.with(|events| events.take());
                if server.paused || events.is_empty() {
                    break;
                }
                for event in events {
                    server.event(event);
                }
            }
            result
        })
    })
}

pub fn ammo_use(pile: *mut Entity, player: *mut Entity) -> bool {
    with_gameplay(false, |server| {
        let (Some(pile), Some(player)) = (NonNull::new(pile), NonNull::new(player)) else {
            return false;
        };
        let inventory = server
            .source
            .as_edict(player)
            .and_then(|edict| server.source.user_id(edict))
            .and_then(|id| server.players.get_mut(&id))
            .map(|player| &mut player.inventory);
        server.game.use_pile(
            &server.source,
            pile,
            player,
            &server.chapter.ammo_piles,
            &server.rules.catalog,
            inventory,
        )
    })
}

pub fn pickup(player: *mut Entity, item: *mut Entity, call: PickupCall) -> Option<bool> {
    let (player, item) = (NonNull::new(player)?, NonNull::new(item)?);
    with_gameplay(None, |server| {
        let player_state = server
            .source
            .as_edict(player)
            .and_then(|edict| server.source.user_id(edict))
            .and_then(|id| server.players.get_mut(&id));
        let mut player_state = player_state;
        let mut context = PickupContext {
            source: &server.source,
            catalog: &server.rules.catalog,
            rules: &server.chapter.map_guns,
            inventory: player_state.as_mut().map(|player| &mut player.inventory),
            notice: None,
        };
        let result = server.game.native_pickup(player, item, call, &mut context);
        if let Some(notice) = context.notice
            && let Some(player) = player_state
        {
            player.pending_notice = notice;
            player.save();
        }
        Some(result)
    })
}

pub fn block_reload(weapon: NonNull<Entity>) -> bool {
    with_gameplay(false, |server| server.game.block_reload(weapon))
}

pub fn keep_empty_chainsaw() -> bool {
    with_gameplay(false, |_| true)
}

pub fn reload_on_empty(weapon: NonNull<Entity>) {
    with_gameplay((), |server| server.game.reload_on_empty(weapon));
}

pub fn combat_damage(victim: NonNull<Entity>, damage: DamageInput, original: f32) -> f32 {
    with_gameplay(original, |server| {
        server
            .game
            .combat_damage(&server.source, victim, damage, &server.chapter, |user| {
                server.players.contains_key(&user)
            })
    })
}

pub fn prepare_attack(weapon: NonNull<Entity>, shove: bool) -> Option<AttackContext> {
    with_gameplay(None, |server| {
        server
            .game
            .prepare_attack(&server.source, weapon, shove, &server.chapter.melee)
    })
}

pub fn finish_attack(attack: AttackContext) {
    with_gameplay((), |server| {
        server
            .game
            .finish_attack(&server.source, attack, &server.chapter)
    });
}

pub fn wall_hit(weapon: NonNull<Entity>) {
    with_gameplay((), |server| {
        server
            .game
            .wall_hit(&server.source, weapon, &server.chapter.melee, |user| {
                server.players.contains_key(&user)
            })
    });
}

pub fn can_rescue(player: NonNull<Entity>) -> bool {
    with_gameplay(true, |server| {
        server.game.can_rescue(&server.source, player)
    })
}

fn ffi<T: Copy>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|_| {
        eprintln!("inventory_mod: Rust panic in Source callback");
        fallback
    })
}

// The Itanium C++ ABI's vptr points at the first function, with offset-to-top
// and type information before it. Source calls through the interface directly.
#[repr(C)]
struct Vtable<T> {
    offset_to_top: isize,
    type_info: usize,
    methods: T,
}

#[repr(C)]
struct Object<T: 'static> {
    vtable: &'static T,
}

#[repr(C)]
struct PluginMethods {
    load: extern "C" fn(*mut c_void, Option<Factory>, Option<Factory>) -> bool,
    unload: Void,
    pause: Void,
    unpause: Void,
    description: extern "C" fn(*mut c_void) -> *const c_char,
    level_init: extern "C" fn(*mut c_void, *const c_char),
    server_activate: extern "C" fn(*mut c_void, *mut Edict, c_int, c_int),
    game_frame: extern "C" fn(*mut c_void, bool),
    level_shutdown: Void,
    client_active: extern "C" fn(*mut c_void, *mut Edict),
    client_disconnect: extern "C" fn(*mut c_void, *mut Edict),
    client_put_in_server: extern "C" fn(*mut c_void, *mut Edict, *const c_char),
    set_command_client: extern "C" fn(*mut c_void, c_int),
    client_settings_changed: extern "C" fn(*mut c_void, *mut Edict),
    client_connect: extern "C" fn(
        *mut c_void,
        *mut bool,
        *mut Edict,
        *const c_char,
        *const c_char,
        *mut c_char,
        c_int,
    ) -> c_int,
    client_command: extern "C" fn(*mut c_void, *mut Edict, *const Command) -> c_int,
    network_id_validated: extern "C" fn(*mut c_void, *const c_char, *const c_char) -> c_int,
    query_finished:
        extern "C" fn(*mut c_void, c_int, *mut Edict, c_int, *const c_char, *const c_char),
}

type Void = extern "C" fn(*mut c_void);

#[repr(C)]
struct ListenerMethods {
    destructor: Void,
    delete_destructor: Void,
    fire_game_event: extern "C" fn(*mut c_void, *mut c_void),
    debug_id: extern "C" fn(*mut c_void) -> c_int,
}

static CALLBACKS: Vtable<PluginMethods> = Vtable {
    offset_to_top: 0,
    type_info: 0,
    methods: PluginMethods {
        load,
        unload,
        pause,
        unpause,
        description,
        level_init,
        server_activate: activate,
        game_frame,
        level_shutdown,
        client_active: ignore_client,
        client_disconnect,
        client_put_in_server: ignore_client_name,
        set_command_client: ignore_command_client,
        client_settings_changed: ignore_client,
        client_connect,
        client_command,
        network_id_validated,
        query_finished,
    },
};

static EVENT_METHODS: Vtable<ListenerMethods> = Vtable {
    offset_to_top: 0,
    type_info: 0,
    methods: ListenerMethods {
        destructor: ignore,
        delete_destructor: ignore,
        fire_game_event,
        debug_id,
    },
};

static PLUGIN: Object<PluginMethods> = Object {
    vtable: &CALLBACKS.methods,
};
static LISTENER: Object<ListenerMethods> = Object {
    vtable: &EVENT_METHODS.methods,
};

fn listener() -> NonNull<c_void> {
    NonNull::from(&LISTENER).cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn CreateInterface(name: *const c_char, result: *mut c_int) -> *mut c_void {
    ffi(ptr::null_mut(), || {
        // SAFETY: The engine's factory passes a C string and an optional result pointer.
        let supported =
            !name.is_null() && unsafe { CStr::from_ptr(name) } == c"ISERVERPLUGINCALLBACKS002";
        if !result.is_null() {
            unsafe { result.write(c_int::from(!supported)) };
        }
        if supported {
            NonNull::from(&PLUGIN).cast().as_ptr()
        } else {
            ptr::null_mut()
        }
    })
}

extern "C" fn load(_: *mut c_void, engine: Option<Factory>, game: Option<Factory>) -> bool {
    ffi(false, || {
        let (Some(engine), Some(game)) = (engine, game) else {
            return false;
        };
        SERVER.with(|server| {
            let Ok(mut current) = server.try_borrow_mut() else {
                return false;
            };
            if current.is_some() {
                return false;
            }
            *current = unsafe { Server::load(engine, game) };
            current.is_some()
        })
    })
}

extern "C" fn unload(_: *mut c_void) {
    ffi((), || {
        if let Some(mut server) = SERVER.with(RefCell::take) {
            server.source.remove_listener(listener());
            server.pings.clear(&server.game, true);
            server.game.uninstall_melee();
            server
                .game
                .uninstall_infection(&server.source, |user| server.players.contains_key(&user));
            server.game.uninstall_actions();
            server
                .game
                .uninstall_stamina(&server.source, |id| server.players.contains_key(&id));
            server.game.uninstall_pickups();
            server.game.uninstall_ammo(&server.source);
            server.restore_cvars();
            server.active_mutators.clear();
            server.mutator_generation = server.mutator_generation.wrapping_add(1);
            if let Ok(message) = CString::new(server.mutator_line()) {
                for (&id, player) in &mut server.players {
                    if let Some(edict) = player.edict
                        && server.source.user_id(edict) == Some(id)
                    {
                        server
                            .game
                            .cancel_actions(server.source.entity(edict), &mut player.actions);
                        server.source.client_print(edict, &message);
                    }
                }
            }
            server.shutdown_level();
        }
        EVENTS.with(|events| events.borrow_mut().clear());
    });
}

extern "C" fn pause(_: *mut c_void) {
    with_server((), Server::pause);
}

extern "C" fn unpause(_: *mut c_void) {
    with_server((), Server::unpause);
}

extern "C" fn description(_: *mut c_void) -> *const c_char {
    c"Inventory Mod (Rust Linux dedicated server)".as_ptr()
}

extern "C" fn level_init(_: *mut c_void, map: *const c_char) {
    with_server((), |server| {
        server.map = unsafe { source::text(map) }.unwrap_or("").to_owned();
        server.listen();
    });
}

extern "C" fn activate(_: *mut c_void, edicts: *mut Edict, count: c_int, clients: c_int) {
    with_server((), |server| {
        server.clients = unsafe { Clients::new(edicts, count, clients) }
    });
}

extern "C" fn game_frame(_: *mut c_void, simulating: bool) {
    if simulating {
        with_server((), Server::game_frame);
    }
}

extern "C" fn level_shutdown(_: *mut c_void) {
    with_server((), Server::shutdown_level);
}

extern "C" fn client_disconnect(_: *mut c_void, edict: *mut Edict) {
    with_server((), |server| {
        if let Some(user) = NonNull::new(edict).and_then(|edict| server.source.user_id(edict))
            && let Some(mut player) = server.players.remove(&user)
        {
            server.pings.remove(&server.game, user);
            server.game.forget_infection(&server.source, user, false);
            server.game.cancel_actions(
                player.edict.and_then(|e| server.source.entity(e)),
                &mut player.actions,
            );
            player.edict = None;
            player.death_position = None;
            player.pending_notice.clear();
            player.last_ping = None;
            player.next_mutators = None;
            player.save();
            if let Some(file) = &player.file {
                server.away.insert(file.clone(), player);
            }
        }
    });
}

extern "C" fn client_command(_: *mut c_void, edict: *mut Edict, command: *const Command) -> c_int {
    with_server(CONTINUE, |server| {
        let (Some(edict), Some(command)) = (NonNull::new(edict), unsafe { command.as_ref() })
        else {
            return CONTINUE;
        };
        server.client_command(edict, command)
    })
}

extern "C" fn fire_game_event(_: *mut c_void, event: *mut c_void) {
    ffi((), || {
        let Some(event) = NonNull::new(event) else {
            return;
        };
        let kind = match unsafe { source::event_name(event) } {
            Some("round_start") => Event::RoundStart,
            Some("player_left_start_area") => Event::LeftStartArea,
            Some("map_transition") => Event::Transition,
            Some("player_transitioned") => {
                Event::PlayerTransitioned(unsafe { source::event_int(event, c"userid") })
            }
            Some("player_death") => Event::Death(unsafe { source::event_int(event, c"userid") }),
            Some("player_spawn") => Event::Spawn(unsafe { source::event_int(event, c"userid") }),
            Some("round_end" | "mission_lost" | "finale_vehicle_leaving" | "finale_win") => {
                Event::RoundEnd
            }
            Some(
                "tongue_grab"
                | "lunge_pounce"
                | "jockey_ride"
                | "charger_carry_start"
                | "charger_pummel_start",
            ) => Event::Exposure(Exposure::Grab(unsafe {
                source::event_int(event, c"victim")
            })),
            Some("player_now_it")
                if unsafe {
                    source::event_bool(event, c"by_boomer")
                        || source::event_bool(event, c"exploded")
                } =>
            {
                Event::Exposure(Exposure::Grab(unsafe {
                    source::event_int(event, c"userid")
                }))
            }
            Some("player_incapacitated") => Event::Exposure(Exposure::KnockedDown {
                user: unsafe { source::event_int(event, c"userid") },
                attacker: unsafe { source::event_int(event, c"attacker") },
                entity_index: unsafe { source::event_int(event, c"attackerentid") },
            }),
            Some("pills_used") => {
                let subject = unsafe { source::event_int(event, c"subject") };
                Event::Exposure(Exposure::Pills(if subject > 0 {
                    subject
                } else {
                    unsafe { source::event_int(event, c"userid") }
                }))
            }
            _ => return,
        };
        EVENTS.with(|events| events.borrow_mut().push(kind));
        with_server((), |_| {});
    });
}

extern "C" fn debug_id(_: *mut c_void) -> c_int {
    42
}
extern "C" fn ignore(_: *mut c_void) {}
extern "C" fn ignore_client(_: *mut c_void, _: *mut Edict) {}
extern "C" fn ignore_client_name(_: *mut c_void, _: *mut Edict, _: *const c_char) {}
extern "C" fn ignore_command_client(_: *mut c_void, _: c_int) {}
extern "C" fn client_connect(
    _: *mut c_void,
    _: *mut bool,
    _: *mut Edict,
    _: *const c_char,
    _: *const c_char,
    _: *mut c_char,
    _: c_int,
) -> c_int {
    CONTINUE
}
extern "C" fn network_id_validated(_: *mut c_void, _: *const c_char, _: *const c_char) -> c_int {
    CONTINUE
}
extern "C" fn query_finished(
    _: *mut c_void,
    _: c_int,
    _: *mut Edict,
    _: c_int,
    _: *const c_char,
    _: *const c_char,
) {
}
