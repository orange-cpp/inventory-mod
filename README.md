# inventory-mod

An inventory system for Left 4 Dead 2, as a DLL loaded into `left4dead2.exe`. The HUD (boxes, ammo counter, stamina
bar, melee ring, radial wheel) is drawn by the game itself, with its VGUI surface, fonts and icons; the inventory window
(I) uses the [imgui-nullcore](third_party/imgui-nullcore/UPSTREAM.md) look, drawn over the game by hooking `IDirect3DDevice9::Present`. The inventory
talks to the game's own functions (found by reverse engineering, see [docs/game-notes.md](docs/game-notes.md)).

L4D2 is a **32-bit** process, so everything here is built for x86.
The Windows client can also connect to the [Linux dedicated server plugin](#linux-dedicated-server).

> Run the game with `-insecure`. A foreign DLL in a VAC-secured session can get the account banned. Use this in
> single player, listen servers and private servers only.

## What it does

- A **hotbar** (always on) and an **inventory window** (grid, tooltips, right-click menu, drag and drop).
- **Store**: takes the item or weapon in your hands into the inventory.
- **Pick up nearby**: takes the item or weapon lying in front of you into the inventory.
- **Unload** (`U`): empties the gun in your hands, or the gun on the ground you look straight at, completely: its
  magazine goes into your reserve of its round type, as many rounds as fit (the type's most, the weight limit); the
  rest is dropped in front of you as a pack of those rounds. Incendiary / explosive rounds come out as plain rounds (the
  laser sight stays). Not the pistols, the M60 or the chainsaw. A gun on the ground stays there, empty; a map's gun
  spawner gives up one gun, which lies there empty (the spawner goes when it had one left). A gun unloaded in your hands
  stays empty until you reload or fire. The rounds taken make the game's pick-up sound.
- **Pistol ammo**: the pistols no longer have an endless reserve: the pistol reloads from SMG rounds, the magnum from
  sniper rounds (the same reserve the SMGs and sniper rifles use).
- **Use**: puts the selected item in your hands, if the player's slot for it is free.
- **Drop**: puts the selected item on the ground in front of you, like `ent_create` does.
- **Auto-store**: pressing +use (E) on a supported item while your slot for it is full puts it into the inventory
  instead of leaving it on the floor. With `auto_store = true` (`[play]` in `inventory_mod_settings.toml`) (or "Store items on use" in Settings)
  every such +use stores the item, even when you could carry it.
- Stacks, a slot count and an optional weight limit. The contents are saved in `inventory_mod.inventory` (a
  dedicated server keeps one file per player, `inventory_<name>.inventory`).
- **The game's rules for what you carry**: the inventory goes on to the next chapter with you, and so does the
  reserve of the guns in it (the game itself carries over only the reserve of the guns in your hands); a lost round or a
  restarted chapter gives back what it held when the chapter began, as the game does with your weapons; a new campaign
  (another map without a saferoom transition) starts with it empty. A survivor who dies drops it: the items fall where
  they died and each round type lies there as an ammo pile, so the others can take them.
- **Multiplayer**: the inventory lives on the server, so whoever hosts runs the mod (a listen server, or a dedicated
  server with the mod injected into `srcds.exe`). Every player who also runs the mod gets an inventory of their own:
  their HUD sends its requests as console commands and the server answers with their inventory. A player without the
  mod has no inventory, but the host's rules (infection, stamina, the finite ammo piles and map guns) apply to them
  too.
- **Rules file** `inventory_mod.toml` next to the DLL (start from [inventory_mod.example.toml](inventory_mod.example.toml)):
  the boxes per player, the carry limit (`max_weight`), each item's weight and stack size, and each gun's and melee
  weapon's weight. On a dedicated server
  it is the rules for every player: the server refuses what is too heavy ("Too heavy: 18.0 / 20.0, ..."), and the
  clients show its numbers. With a limit set, a bar under the grid shows the weight, red when nothing more fits. The
  weight counts everything you carry: what is stored, the throwable, kit, pills or carried item in your game slots,
  and your gun and melee weapon / pistol. Taking an item in hand or putting it back changes nothing; picking an item
  or a weapon up in the game (E) is refused when it would be too heavy (a weapon that replaces yours counts only the
  difference).
- **Beside the game's weapon panel** (`replace_native=0`, the default): the game's own panel on the right stays as it
  is for the gun and the melee / pistol (ammo, slot keys `1`, `2`); its three item boxes are hidden and the inventory
  is a grid under them, three boxes per row, drawn the same way. Boxes appear as they are needed (the ones in use and
  one free box; up to `slots`, 24 by default), so the weight limit is what limits. The throwable, kit and pills you carry live in their
  own inventory box (counted in its number), green while in your hands, so every item has one place. The wheel (and
  `[` / `]`) steps through everything: gun, melee / pistol, then the grid row by row, left to right. `3`..`9`, `0` take inventory boxes 1-8 in hand, and
  the drop key drops what is in your hands (gun or item), like the game's own.
- **Replaces the game's weapon panel** (`replace_native=1`): the native panel is hidden (hook on
  `CHudWeaponSelection::ShouldDraw`) and the mod's bottom hotbar takes its place, with its slot keys and wheel. `Q`
  (last weapon) still works to get back to the gun.
- The column or hotbar shows only while you play a living survivor (not in menus, as a spectator or as infected).
- Hotkeys are ignored while the console, the chat line or a menu has the keyboard.
- **Inventory view** (Settings, `view` under `[hud]` in `inventory_mod_settings.toml`): slots and wheel ("both", the default), slots only ("slots": the
  middle mouse button stays the game's), or wheel only ("wheel": no grid or weapon boxes, and the game's own throwable / kit /
  pills boxes stay hidden too, the wheel shows everything; the number keys and the mouse wheel are the game's again).
- A carried item (gas can, propane / oxygen tank, fireworks, gnome, cola) is put back into its box when you switch
  with the mod (wheel, keys, boxes) instead of being dropped, as the game would; with no room it is dropped as before.
  The game's own keys `1` / `2` still drop it.

Supported items: first aid kit, defibrillator, pain pills, adrenaline, pipe bomb, molotov, bile jar, the two ammo
upgrade packs, gas can, propane / oxygen tank, fireworks, gnome, cola bottles, and every gun and melee weapon (pistol,
magnum, melee weapons, chainsaw, SMGs, shotguns, rifles, sniper rifles, M60, grenade launcher).

**Weapons** go into weapon boxes of their own (up to `weapon_slots`, 8 by default; a box appears only while it holds a weapon), drawn like the game's gun and melee
boxes, between the game's melee box and the item grid. Each keeps what is in it: a gun its magazine, a melee weapon
which one it is ("fireaxe", "katana" ...). The reserve is yours and shared, as in the game, by every gun of the same
ammo type (M16, AK-47, SCAR and SG552 share rifle rounds; all four shotguns, pump and auto, share one kind of shell;
the SMGs and the sniper rifles each share theirs): storing a gun leaves it with you, a gun taken out uses it (the game does not hand out a
fresh reserve), and a gun picked up off the ground into a box adds the rounds it carried, up to the most you can
carry of that type. That maximum is the rules file's `[ammo]` table (`assault_rifle = 360`, `smg`, `shotgun`,
`hunting_rifle`, `sniper_rifle`, `grenade_launcher`, `m60`, `chainsaw`); the mod also sets the game's
own `ammo_<type>_max` to it, so ammo piles stop there too. A stored gun shows its magazine and the reserve it would use
(your reserve of its type, e.g. "50 / 290"). The store key stores the gun or melee weapon in your
hands, the pick-up key takes one off the ground, and taking one out (the wheel, double click in the window) swaps it
with the one in that game slot, which goes into the same box. The wheel shows the weapons, then the items, each
keeping the place it first had (a swap does not move them). E on a weapon puts it in your hands when that game slot is empty; when you already have one there, it goes into
a free weapon box with its rounds; with no free box (or too heavy for one) it is the game's own swap, if that is not
too heavy.

The weight is checked on every pickup and store. It can still end up over the limit without one (a teammate hands you
pills, or the rules file is changed to heavier weights): the bar then turns red and nothing more can be picked up or
stored until you drop something.

## The infection

With `[infection]` in the host's `inventory_mod.toml` (on in the example): a survivor knocked down by the infected,
grabbed by a smoker, hunter, jockey or charger, or vomited on by a boomer is infected and a clock starts (`seconds`,
120). Each new hit takes `hit_seconds` (20) off it; pain pills put it back to the start, they do not cure it
(`no_cure = false`: they do). When it runs out the survivor dies and a random
special infected (`specials`) rises where they stood, and that survivor cannot come back before the next chapter: their
body is removed (no defibrillator) and no rescue closet calls for them. There is no clock on the screen: the player
sees it. A shader over the game's picture (and the mod's own boxes, wheel and window) throbs with a heartbeat that speeds up as time runs out; the world sways,
the edges blur and split into red and blue, the colour drains to a sick yellow green, dark veins creep in from the
edges, the vignette closes in, then double vision and dimming near the end and a red beat in the last ten seconds.
(It needs Windows' `d3dcompiler_47.dll`; without it a plain green tint and vignette.) `bots = true` infects the
survivor bots too. With `cough = true` (the default) an infected survivor coughs now and then with their own voice
(the coughs they use in smoke), heard by everyone around: about every 26 s at first, every 4 s near the end. With
`insane = true` (the default), from `insane_at` (70) % of the clock gone they go insane: they laugh for no reason or
scream (their own death screams, and "get it off!" at a special that is not there), every 18 s or so at first and
every 5 s at the end, screaming more as it gets worse. The window's Debug tab has "Infect me" to try it and a preview slider for the effect alone. (In co-op the round is lost when every human player is dead, so playing alone, turning ends the round.)

The sound goes too: the game's own mix gets more muffled and warped (its pitch bends up and down), a heartbeat thumps
under it in time with the picture, the ears ring past a third of the way and the sound crushes near the end. When the
clock runs out a screamer jumps at the player: a face over the whole screen, lunging and shaking, with a scream. Each
player chooses for themselves in the Settings tab ("Infection") or under `[infection]` in `inventory_mod_settings.toml`:
`effect`, `sound`, `screamer` (true / false), `cover_game_hud` and `cover_mod_hud` (whether the effect also covers
the game's HUD with the mod's boxes and wheel, which the game draws with its own, and the mod's inventory window; off,
they stay clear over it: "Effect covers the game's HUD" / "... the mod's window"; the window is covered only together
with the HUD), `screamer_chance` (how likely the screamer is each time the clock runs
out: 0 never, 1 always, 0.3 about one time in three), and `screamer_image` (next to the DLL or a full path; PNG, JPEG, BMP;
`inventory_mod_screamer.jpg` by default, shipped with the mod) and `screamer_sound` (a 16-bit PCM .wav under the game's
`left4dead2/sound`, next to the DLL or a full path; the witch's scream by default). The Debug tab has "Screamer (test)".
The screamer and the screen effects are drawn into the game's own frame (after the HUD), so a stream or a recording
(OBS, Discord, Steam) shows them whichever hooked the game first; only the inventory window is drawn at `Present`.

## Rounds

The reserve rounds you carry weigh, by `[round_weight]` in the host's `inventory_mod.toml` (one round: rifle 0.01, so
360 weigh 3.6; SMG 0.006; shotgun shell 0.04; sniper and hunting rifle 0.02; grenade 0.15; 0 makes a type weightless).
The window's Items tab lists each type you carry with what it weighs, and drops it a magazine's worth at a time
("Drop 50") or all of it: the rounds land as an ammo pile that holds exactly those rounds, which anyone (not the bots)
takes back with E, as many as fit (with the game's pick-up sound); the rest stays in the pile; rounds dropped next to a pile of their type join it. Each
round type is its own ammo pack (the textures in `assets/textures`) when the addon `inventory_mod_rounds.vpk` is in the
`addons` folder next to the DLL: the mod mounts every `.vpk` there when it loads, so nothing is copied into the game
(in `left4dead2/addons` it works too). Without it, and for M60 rounds and chainsaw fuel, the pile is the incendiary
ammo pack tinted the type's colour. Every player needs the addon to see the packs (whoever lacks it sees the ERROR
model). An ammo pile of the map, or a gun picked up, fills you only up to the weight limit. The rounds are yours, not
the gun's: swapping your gun for another on the ground keeps your rounds of its type (the game would leave them in the
dropped gun), and a gun someone dropped adds its rounds to yours (the game would replace yours with them).

With `[ammo_piles]` in the host's `inventory_mod.toml` (on in the example) the map's ammo piles are not endless: a pile
holds `min_magazines` to `max_magazines` magazines (1 to 3) of a random ammo type of `kinds`, rolled the first time
someone uses it (it then takes that type's colour). Anyone takes as many as they can carry, the rest stays in the
pile, and an empty pile is gone. A bot takes only rounds for a gun it has. With `replace_map_piles` (on) each of the
map's piles becomes an ammo pack of its rolled type when the round starts, so what it holds shows from afar, and
`extra` (4) more packs are put beside the map's item spots (pills, throwables, guns; not near the start).

With `[map_guns]` (on in the example) a gun nobody has held yet, lying in the map or from a weapon spawner, has only
`min_clip` to `max_clip` % of its magazine (10 to 60) when it is first picked up, and it adds only `min_reserve` to
`max_reserve` magazines (0 to 1) to your reserve, where the game would fill it. A gun someone has held, or one
dropped from a box, keeps what it had. With `spawners_once` (on) a weapon spawner of the map, for guns or melee
weapons, gives one and is gone; item spawners (pills, kits) are the game's.

## Pings

With `[ping]` in the host's `inventory_mod.toml` (on in the example) the ping key (`V`) marks what you look at, as in
Apex Legends, for every player of your team who runs the mod: a diamond in the world with the distance in metres and, for
what it is, a name, shown through walls. It marks an enemy (an infected, a witch, a special: red) or an item or weapon
lying there (gold) when you point at one, and otherwise the spot your view meets (blue). Off the screen the marker
sticks to the edge nearest the way it lies, with an arrow. It pops in with the game's menu tick and fades out after
`lifetime` seconds (8); you keep `per_player` pings (1: a new one replaces yours), at most one every `cooldown` seconds
(0.4), up to `range` units away (6000). The server finds the target (a trace along your view, then the entities near
the ray) and tells every teammate's mod, so it works as host, guest and on a dedicated server; players without the
mod see nothing. When you ping an item or an enemy your survivor says it, with the game's own look-at line
("Pills here", "Ammo here"; `vocalize smartlook auto`, the Z menu's Look). A ping stays where it was made: it does not follow a moving enemy.

## Chapter mutators

With `[mutators]` in the host's `inventory_mod.toml` a chapter can get mutators. When a chapter starts (the next one, or
a new campaign; a lost round keeps the chapter's) the host rolls: with `chance` % (30 in the example) the chapter gets
`count` different ones from `pool`. A map in `[mutators.maps]` gets its own list instead (`[]`: never any there). Every
player with the mod is told, and with `announce` the HUD says "Mutator: Fog"; the scoreboard (Tab) shows the
chapter's under its title, and so does the Debug tab.

- **Fog** (`[mutators.fog]`): the game's own distance fog, thick and near (`density` 0 .. 1, 0.6), in the map's fog
  colour or a `color` of its own (`"40 60 40"`). Each player's game draws it (client.dll's fog ConVars), so it works
  as host, guest and on a dedicated server, but players without the mod don't see it. It goes when the chapter ends,
  and the next map's own fog comes back first.

The gameplay mutators change the host's rules for the chapter, on the server, so they apply to every player, with
the mod or not. Each switches on, with its defaults, the feature it builds on:

- **Scarcity** (`[mutators.scarcity]`): ammo piles hold `ammo` % of their magazines (at least 1), map guns are found
  with `clip` % of theirs and add `ammo` % of their reserve, and `extra` packs lie by the item spots
  (`[ammo_piles]`, `[map_guns]`).
- **Heavy load** (`[mutators.heavy_load]`): the carry limit is `weight` % of `max_weight` (or `limit` when there is
  none), and running, jumping, swings and shoves use `stamina` % (`[stamina]`).
- **Contagion** (`[mutators.contagion]`): the infection is on, with `clock` % of its clock (`[infection]`).
- **Brittle** (`[mutators.brittle]`): melee weapons wear `wear` % per hit. It needs `[melee]`; the rules refuse it in
  the pool without it.
- **Horde tide** (`[mutators.horde_tide]`): the director's panic event (`director_force_panic_event`) `first` seconds
  after the survivors leave the start area (60), then every `every` seconds (180).
- **Hunted** (`[mutators.hunted]`): up to `limit` (3) of one special infected at once, a `class` of its own or a
  random one each chapter (the game's `z_<class>_limit`, put back at the next chapter). A finale's or a mutation's own
  director script still sets its own limits, and the director's cap on all specials together still holds.
- **Crowd** (`[mutators.crowd]`): `commons` % (200) of the common infected about at once (`z_common_limit`) and `mobs`
  % (150) of a mob's size (`z_mob_spawn_min_size`, `_max_size`, `z_mega_mob_size`).
- **Witch** (`[mutators.witch]`): an angrier, tougher Witch who hits less hard: `health` % (150) of her health, `anger`
  % (300) of how fast she gets angry, `space` % (300) of how near is too near, and `damage` % (45) of her hit on a
  standing survivor, so one blow no longer incaps (`z_witch_health`, `_anger_rate`, `_personal_space`, `_damage`).
- **Tank** (`[mutators.tank]`): `health` % (150) of its health and `speed` % (110) of its speed (`z_tank_health`,
  `z_tank_speed`; the difficulty still scales the health).

Hunted, crowd, witch and tank set the game's own ConVars for the chapter, as % of what they were (hunted: a number);
the next chapter puts them back. A finale's or a mutation's director script may still set its own.

## Self-revive

With `[self_revive]` in the host's `inventory_mod.toml` (on in the example) a survivor who is down, or hanging from a
ledge (`ledge`), gets up by themselves with an item: take one of `items` (adrenaline in the example; also pain pills,
first aid kit) from an inventory box (the hotbar, the wheel, the window), or pick the one in your pills slot. The game's
own progress bar fills for `seconds` (3), then you are up as if a friend had revived you: it counts as an incap, and the
last one leaves you black and white. The item is used up only then; being pinned by a special, revived by a friend
first or dying calls it off, and the item stays. Adrenaline also gives its rush (`rush`). It is the server's, so it
works as host, guest and on a dedicated server (a player without the mod has no inventory, but sees the progress bar of
anything the server does).

## Chainsaw fuel

With `[chainsaw]` in the host's `inventory_mod.toml` (on in the example) the chainsaw runs on the fuel the survivors
carry. The game throws an empty chainsaw away and hands out a pistol; here it stays in your hands, dead (no motor, no
swing, no shove), until it is refuelled. Press R with the chainsaw in hand: the game's progress bar fills for `seconds`
(2), and the tank (30) is filled from the gas cans in your inventory, half-empty ones first. A gas can holds `can_fuel`
(90, three tanks); one poured dry is gone. A can keeps what it holds when it is dropped, taken in hand, picked up again
or dropped where you died; a thin amber bar under its box shows the open one, and the wheel and the window's tooltip
the fuel. A can poured into a generator or a scavenge goal counts whole, whatever it held. Switching away, going down or
being pinned calls a refuel off.

## Stamina

With `[stamina]` in the host's `inventory_mod.toml` (on in the example): running uses stamina (`run` a second), and so
do jumps (`jump`), melee swings (`swing`) and shoves (`shove`), out of 100. `regen_delay` seconds after the last use it
comes back (`regen` a second; half that while an exhausted survivor keeps running). A survivor who runs out is
exhausted until it is back to `recover`: they move at `slow` % of their speed (on the ground; jumps and falls are not
slowed), wait longer between swings and shoves (`swing_speed` %), and their melee hits do `damage` % (at 50, a common
infected takes two hits). While it is on, the game's own shove fatigue (each shove in a row waiting longer) is off:
stamina is the limit. `bots = true` gives the survivor bots stamina too. An adrenaline shot gives endless stamina for
as long as it works: the bar fills, an exhausted survivor gets their breath back, and nothing uses it.

Each player chooses how it shows, in the Settings tab ("Stamina") or under `[stamina]` in `inventory_mod_settings.toml`:
`bar` (a thin bar under the crosshair while it is not full; orange when low, red and pulsing when exhausted) and
`vignette` (the screen's edges darken and lose their colour from half down; exhausted, they close in and pulse with
each gasp), and, as for the infection, `cover_game_hud` and `cover_mod_hud` (whether the vignette also covers the game's
HUD with the mod's boxes and wheel, and the mod's inventory window: "Vignette covers the game's HUD" / "... the mod's
window"). The Debug tab has
a preview of both.

## Melee

With `[melee]` in the host's `inventory_mod.toml` (on in the example) melee weapons fight as in No More Room in Hell 2.

| With a melee weapon | |
|---|---|
| tap the attack | a light swing |
| hold the attack, let go | a heavy swing once the ring at the crosshair is orange (`heavy_hold`, 0.6 s, times the weapon class's) |
| shove while holding | calls the swing off |
| the reload key while holding | throws the weapon |
| hold the reload key | block |
| hold the shove, let go | a heavy shove (`heavy_shove_hold`, 0.5 s) |

There is no queue: a swing let go while the weapon is still recovering is dropped, each swing is its own click once
the weapon is ready.

Each feature can be switched off on its own under `[melee]` (all on when left out): `heavy` (heavy swings and the
wind-up; off, every swing is light and there is no throw), `wind_slowdown`, `blocking`, `deflecting`, `heavy_shove`,
`throwing`, `breaking` (wear), `hit_stagger` (a heavy hit staggers), `view_kick`, `wall_bounce` (a swing into a wall
stops short), `head_only`, `overhead` and `gesture`. The numbers below tune
what stays on.

**Damage.** The game's swing kills a common outright; here a swing does a share of a full hit (a common's health when
new, 50; anything else the game's own damage): `light_damage` % (40) for a light one, `heavy_damage` % (200) for a
heavy one, `head_damage` % (200) more to the head. With `head_only` (on) the body alone never kills a common: it is left
at 1 health, and the head finishes it. So a common takes three light hits, two to the head; a heavy hit to the head
kills, and a heavy hit staggers what it does not kill. One swing into a crowd does less to each further one (`cleave`,
`cleave_min`). Teammates take the game's own friendly fire.

**Weapon classes.** Each melee weapon has NMRiH2's class: blade one-handed (machete, knife), blunt one-handed
(crowbar, frying pan, tonfa, riot shield), blade two-handed (fireaxe, katana, pitchfork), blunt two-handed (bats,
guitar, golf club, shovel); a custom one is blunt one-handed. The class scales the wind-up, the damage, the stamina
and the cleave, and sets the durability: two-handers wind up slower and hit harder; blades cut through a crowd and
wear out sooner, blunt weapons hit one target hard and last. `[melee.weapons.<name>]` sets one weapon's own (`class`,
`heavy_hold`, `light_damage`, `heavy_damage`, `cleave`, `stamina` %, `durability` in hits).

**Stamina** (with `[stamina]`). A heavy swing costs `heavy_stamina` (25), each target hit `hit_stamina` (3), winding up
`wind_stamina` (4) a second, each times the class's; run out while winding up and the swing goes off by itself, a light
one. Exhausted, only light swings and no block. Winding up slows you to `wind_speed` % (65) as it charges, blocking to
`block_speed` % (60).

**Durability.** Each target hit wears the weapon (a heavy hit 1.5, a swing into a wall 3, a thrown hit 2); at its
durability (`durability` %, 100, of the class's: 150 to 300 hits) it breaks, with a notice ("about to break" at 80 %).
A stored weapon keeps its wear in the inventory (a bar on its box); the one in your hands shows a bar under the
crosshair once half worn. A weapon carried in the hands across a chapter change comes back new (the game gives it
again).

**Block.** Holding the reload key with a melee weapon blocks a common's hit from the front: you take `block` % (25)
of it, for `block_stamina` (6); raised just in time (`deflect`, 0.25 s before the hit) you take nothing and the common
staggers. Specials and the witch are not blocked.

**Heavy shove.** Held, the shove costs `heavy_shove_stamina` (20), recovers slower and knocks back every infected in
front within reach.

**Throw.** The reload key while winding up throws the weapon along your aim (`throw_speed`, 900): the first infected it
passes takes `throw_damage` % (250) of a full hit and staggers; the weapon falls there to be picked up again.

**A wall.** A swing that goes into a wall (`wall_bounce`) before it hits anything stops short: your view kicks and the
weapon wears more (you do not stagger). The floor and props do not count.

**How it looks.** The weapon in your hands is drawn back, raised and turned as the swing winds up, and trembles once a
heavy swing is ready; let go, it snaps forward into the game's swing with a follow-through, a deeper lunge for a heavy
one or a throw. Blocking holds it across the body; a heavy shove draws the arms in first. A hit jolts it and kicks your
view (`view_punch`, 3 degrees for a heavy hit). A heavy swing comes down from above where the weapon has such a swing
(`overhead`: the bats, the frying pan, the machete; the fireaxe and the katana only swing sideways). Others see you
raise the weapon when you start to wind up (`gesture`). (L4D2 has no wind-up animations of its own: the pose is laid
over the viewmodel.)

Winding up, the heavy shove and the throw need the mod on the player's own game (it holds the buttons back from the
game, so the game's own prediction does not swing early); the server checks how long they wound up. A player without
the mod, and the bots, swing as the game does, always light, with the damage, the cleave and the wear. A ring at the
crosshair fills while a swing winds up and turns orange when it will be heavy (grey while exhausted); `ring` under
`[melee]` in `inventory_mod_settings.toml` ("Melee: wind-up ring" on the menu page) turns it off.

## Settings in the game's menu

Options > Inventory Mod (main menu and the in-game menu) is a page of the game's own menu, in its own style, with all
of the mod's settings as drop-downs in two columns: the HUD, the infection and stamina on the left; the keys (from a
list: free letters, `[ ] \ ; ' , . /`, Insert, Delete, Home, Page Up / Down, None), the limits and the melee's ring on the right;
then "Inventory window" (the mod's window) and Done. The window has no Settings tab while the page is there. A choice is saved to `inventory_mod_settings.toml` at once. It
comes with the addon `inventory_mod_menu.vpk` in the `addons` folder next to the DLL (built by
`python -I tools/make_menu_addon.py`): the Options flyout with the button, the page's layout and each drop-down's
choices. The page is the Xbox build's Audio / Video page, which the PC menu never opens, with its own code switched
off (see docs/game-notes.md); without the addon, or on another game build, the button is not there and the window's
Settings tab is back (it also rebinds a key to any key at all).

## Keys (all can be rebound on Options > Inventory Mod)

| Key | Action |
|---|---|
| `I` | open / close the inventory window |
| middle mouse (hold) | the radial wheel, as in No More Room in Hell: everything you carry around the middle of the screen (gun, melee, stored weapons, items, then your reserve rounds in their colours), each segment as large as its share of the weight, with the weight as a ring that turns from green to red as the load nears the limit. It is drawn by the game itself (its VGUI surface, in the HUD's own Trade Gothic / Stubble fonts and icons); it opens with a quick grow, the segment under the cursor slides out and lights up with the game's menu tick (`wheel_sounds`), and the pick follows the cursor at once. Point at one and let go (or left click) to take it in hand, right click drops it (rounds: a magazine's worth, or all of that type with the button held half a second), Escape closes. The view stops turning while it is open (and does not jump when it closes); the keyboard stays the game's, so you keep walking |
| `1`..`5` | the game's own slots (beside the game's panel) |
| `3`..`9`, `0` | beside the game's panel: take inventory box 1..8 in your hands (an item whose game slot is taken swaps with it; that one goes back into its own box). `1` / `2` stay the game's gun and melee keys |
| `1`..`9`, `0` | with `replace_native=1`: select hotbar entry 1..10: first the game's two gun slots (primary, secondary; always shown, empty or not), then the inventory boxes. A throwable, kit, pills or carried item in your hands is shown in its own inventory box (green bar under it), so items never change boxes. Like the game's own panel, selecting any of them takes it in your hands; an inventory item whose game slot is occupied swaps with what is there (that one goes back into its own box) |
| mouse wheel | previous / next entry through all of them: beside the game's panel gun, melee / pistol, then the grid row by row; with `replace_native=1` the hotbar's entries (the game's wheel switching is swallowed either way) |
| `[` / `]` | the same as the wheel |
| `\` | use the selected item |
| `G` | beside the game's panel: drop what is in your hands (stored items are dropped from the window). With `replace_native=1`: drop the selected entry, an inventory item or a weapon / item you carry in the game's own slots |
| `;` | store the item in your hands |
| `H` | pick up the item in front of you |
| `V` | ping: mark the spot, item or enemy you look at for your team (`[ping]`) |
| `U` | unload the gun in your hands (or the one on the ground you look at) into your reserve. U is the game's team chat key by default: while you play, the mod's key wins; rebind one of them if you need both |
| `R` | with the chainsaw in hand: refuel it from the gas cans in your inventory (`[chainsaw]`; the game's reload key, read by the server) |
| `Ctrl+End` | unload the DLL |

## Build

```
build.bat Release            # configure + build everything into out\Release
build.bat Release inventory_mod
```

`build.bat` uses the Visual Studio 18 toolchain (vcvarsall x86, its bundled CMake, Ninja and vcpkg). Libraries come
from vcpkg in manifest mode ([vcpkg.json](vcpkg.json), triplet `x86-windows-static`, pinned by `builtin-baseline`):
MinHook and toml++. `VCPKG_ROOT` picks another vcpkg. CMake fetches ImGui 1.92.8 and builds it with the pinned
C++ snapshot in `third_party/imgui-nullcore`. Set `INVENTORY_NULLCORE_DIR` in CMake to use a different checkout.
If an existing CMake cache still points at the old sibling checkout, clear that cache entry to use the bundled copy.

Outputs in `out\Release`: `inventory_mod.dll`, `injector.exe`, `inventory_tests.exe` (the inventory model's tests).

The ammo packs' addon is built apart, by `python -I tools/make_rounds_addon.py` (Python 3 with Pillow, and the game
installed: it reads the game's own ammo pack model from its VPKs) into `out\Release\addons\inventory_mod_rounds.vpk`.
The addon in use by a running game cannot be replaced; the game keeps it open until it closes.

### Linux dedicated server

Build the 32-bit server plugin with an x86 C++ runtime and development toolchain, CMake, Ninja, and the L4D2 Source
SDK headers. The commands below fetch the tested SDK revision.

```sh
git init ../hl2sdk-l4d2
git -C ../hl2sdk-l4d2 fetch --depth 1 https://github.com/alliedmodders/hl2sdk.git ee978cde412bb8720453f1f89e5748ec1449a4b3
git -C ../hl2sdk-l4d2 checkout --detach FETCH_HEAD
# Fix the SDK's copy constructor declaration for GCC 11 in C++20 mode.
sed -i 's/CAutoLockT<MUTEX_TYPE>( const CAutoLockT/CAutoLockT( const CAutoLockT/' ../hl2sdk-l4d2/public/tier0/threadtools.h
cmake -S . -B build/linux-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 \
  -DINVENTORY_LINUX_SERVER=ON -DINVENTORY_L4D2_SDK_DIR="$PWD/../hl2sdk-l4d2"
cmake --build build/linux-x86
ctest --test-dir build/linux-x86 --output-on-failure
```

Copy `build/linux-x86/inventory_mod_server.so` into the dedicated server's `left4dead2/addons/` folder and start
`srcds_run` with `-game left4dead2 -insecure +plugin_load addons/inventory_mod_server`. The plugin checks the Linux
`server_srv.so` build before loading. It targets Left 4 Dead 2 version **2.2.4.3**. Put `inventory_mod.toml` in
`left4dead2/addons/inventory_mod/`, where the plugin
also saves inventories for clients with a Steam ID. The Windows client uses the same console commands and state replies
as on a Windows server.

The Linux server supports native slot sync and selection, inventory drag and drop, and the
explicit pickup, store, use, and drop commands for items and weapons. A stored gun retains its magazine and upgrades,
and the player keeps their reserve when they store or swap a gun. Unloading shares the same ammo limits as dropped
piles. Stored items and reserve ammo drop on death. With `[ammo_piles]` enabled, the server can make the map's
piles finite and add extra packs. The Linux plugin tints the map's existing pile models and uses a built-in model for
new packs. The server stores a pickup when its native slot is occupied and the inventory has room. To also store
stackable items when their native slot is free, set `auto_store = true` under `[play]` in
`left4dead2/addons/inventory_mod/inventory_mod_settings.toml`. The server also supports `[map_guns]`, `[stamina]`,
chainsaw fuel, self-revive, and team pings with moving enemy markers and temporary glow. Chapter mutators use the
existing Windows HUD announcement and fog settings. Infection, including Contagion, uses the Windows client's
existing countdown and effects. Expanded melee rules and Brittle use the existing wind-up controls, hit feedback,
and durability display.

## Release

The **CI** workflow builds, tests and uploads a Windows x86 ZIP and a Linux x86 dedicated server tarball for pull
requests into `main` and pushes to `main`. The Windows CI package contains the binaries, rules and player guide.

The **Release** workflow also generates both Windows add-ons using the dedicated server's SteamCMD download.
Packaging changes trigger this check on pull requests. Run it manually from the Actions tab for an on-demand build.
Push a version tag such as `v0.1.0` to publish both archives and `SHA256SUMS.txt` after both builds and tests pass.
Tags with a suffix such as `v0.1.0-rc.1` publish as prereleases. The Linux tarball can be unpacked at the dedicated
server root and loaded as described in [Linux dedicated server](#linux-dedicated-server).

For a local Windows release, run the following commands.

```
make_dist.bat                # build, test, then dist\inventory-mod and dist\inventory-mod-<date>.zip
make_dist.bat -SkipBuild     # the DLL already built (e.g. while the game has it loaded)
make_dist.bat -SkipAddons    # package the binaries without needing the game installed
```

[tools/make_dist.ps1](tools/make_dist.ps1) runs `build.bat Release` and the tests. Unload the DLL with `Ctrl+End`
before rebuilding if the game is running. The script packages the DLL, injector, launcher, player guide, sample
rules, screamer image and both add-ons. Pass `-GameDir` with a game or dedicated server installation path if Steam
installed it outside the default location. Packages are written to `dist`.

## Run

1. Start the game with `-insecure` (add `-windowed -w 1280 -h 720` for development) and load a map.
2. `out\Release\injector.exe` loads `inventory_mod.dll` into the game (or use your own injector). With
   `out\Release\addons\inventory_mod_rounds.vpk` built, dropped rounds are their type's pack.
3. `inventory_mod.log`, `inventory_mod_settings.toml` (settings and keys; an old `inventory_mod.ini` is moved into it once) and `inventory_mod.inventory` are written next to the DLL; `inventory_mod.toml` (the rules) is only read.

The game access only turns on for game build 2.2.4.3 (`server.dll` timestamp `0x6A440E53`). On any other build the log
says why and the inventory only edits its own list.

## Layout

```
src/inv/       the model, no Windows or ImGui (tested on its own); rules.cpp reads inventory_mod.toml, melee.cpp
               is the melee's wind-up as user-command buttons
src/core/      log, config, MinHook wrapper
src/ui/        overlay.cpp (D3D9 hooks, WndProc, cursor), hud.cpp (boxes, wheel, window), icons.cpp (the game's
               own weapon and item icons, read from its VTF sheets), postfx.cpp (the infection and stamina shaders),
               audio.cpp (the infection's sound), vgui.cpp (the game's VGUI surface and scheme fonts:
               the whole HUD but the window is drawn with them)
src/game/      backend.hpp (the seam), game.cpp (the real backend: the request queue and init),
               one module per topic (entities, weapons, pickup, rounds, players, infection,
               stamina, melee, revive, fuel, lifecycle, client_remote ...), sdk.hpp (the game's addresses, offsets
               and signatures), mem.cpp (guarded reads and calls)
tests/         inventory_test.cpp
assets/        the screamer picture, textures/ (each round type's ammo pack)
packaging/     what the release adds: HOW-TO.txt (for players), load-mod.bat
tools/         injector.cpp, game_test.ps1 (drives a test session, a dedicated server too), make_dist.ps1 (the
               release), make_rounds_addon.py (the ammo packs' addon), mdmp_summary.py (reads a crash dump)
tools/re/      pe_tools.py (static analysis), vpk.py (reads the game's VPKs), mdl_to_smd.py (a model to SMD + QC for
               Blender Source Tools), vtf_to_png.py (a texture to PNG)
docs/          game-notes.md: every game address / slot / offset used, and how each was found
```

`tools/game_test.ps1` launches the game, injects, posts keys and clicks to the game window and captures only that
window to `%TEMP%`, so the real mouse and keyboard are left alone. `Reload-Mod` unloads the DLL (a posted message),
rebuilds it and loads it again without restarting the game.

## How it works

- The HUD is painted by the game: vgui2.dll's `IPanel::PaintTraverse` is hooked, and once the game's last panel
  (`FocusOverlayPanel`) has painted, the mod draws its boxes, counters, rings and wheel with the game's `ISurface`, in
  the client scheme's fonts and the HUD's own icon sheets, on the main thread, under the game's menus.
- The overlay hooks `Present` / `Reset` through the vtable of a throwaway device, draws the inventory window with ImGui
  between a captured and re-applied state block, and turns sRGB writes off (the game leaves them on, which washes out
  the colours). It also draws the rest of the HUD, the same shapes, should the game's VGUI not be there.
- While the window is open the game gets none of the mouse or keyboard messages, `SetCursorPos` is ignored for the game
  and `GetCursorPos` returns the window's centre to it, so the view does not turn and the cursor is free.
- Game calls (`GiveNamedItem`, `UTIL_Remove`, spawning) are not thread safe and rendering is on another thread, so the
  UI only queues requests; they run in the window procedure, which is the game's main thread.

## Not done

- Only the +use pickup is intercepted (not items given by the game, e.g. at a map start or from a bot).
- A player without the mod on a server that runs it has no inventory and sees the ammo packs as ERROR models.
- The finite ammo piles, map guns and ammo packs have not been tried on the dedicated server yet.
- The bots: whether they take ammo from the map's piles through the same +use is not known (they may refill without
  limit).
