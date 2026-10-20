# No More Room in Hell 2: melee mechanic (reverse-engineering notes)

Status: first pass, **data-level only**. Everything below comes from the names, imports and class layout of the shipped
assets. No numeric values have been decoded yet, and the C++ side (`/Script/Kosovo`) has not been disassembled. Items
marked (inferred) are deductions from names and state wiring, not observed behaviour.

## How the assets were read

- NMRiH2 is Unreal Engine 5, shipping build, IoStore containers (`Content/Paks/*.utoc/.ucas`, not encrypted).
- `retoc to-legacy -f <filter> <Paks dir> <out>` (github.com/trumank/retoc, built with
  `cargo install --git https://github.com/trumank/retoc --locked retoc_cli`) converts the zen assets to `.uasset/.uexp`.
- Properties are *unversioned*, so values in `.uexp` need a `.usmap` (not available). Name tables, imports and Blueprint
  bytecode do not need one. The scratch parser reads the name table only.
- The game's own logic is in the native module `/Script/Kosovo` (classes `KCombatState`, `KCombatData`,
  `KPlayerCharacter`, `KExecution_*`), inside `Binaries/Win64/NMRiH2-Win64-Shipping.exe`.

## Architecture (observed)

Melee is two layers: a **combat state machine** (Blueprint subclasses of the native `KCombatState`) and **gameplay
abilities** (`GA_Melee_*`, run through the Gameplay Ability System) that do the hit detection and damage.

### Combat states (`Content/Blueprints/CombatStates/MeleeStates/`)

State tags are `Combat.CombatStates.*`. Transitions are `GotoState`; states react to `EventOnAbilityInputReleased`,
`EventOnSecondaryAbilityInputReleased`, `EventOnCancelInputPressed`, `EventOnQueueAbility`, `EventEndAnimation`.

| State | What it does (names + wiring) |
|---|---|
| Windup | Entered on attack press. Adds loose tag `Status.Attacking`. Variables `EarlyExitToHoldTime`, `HeavyHoldingTime`, `AllowQueue`/`AllowQueueRatio`, `QueuedTime`. Function `TryHeavyAttack`; sends `Event.CombatState.StartHeavyAttack` / `CancelAttack` / `CancelHeavyAttack` / `ReleaseBlock`. Goes to **Holding** if the button is still held, else **Release**. Uses `HasLightAndHeavyAttack` to decide whether a tap is light or the weapon only has one attack. |
| Holding | Charge loop for the heavy attack. `MinHoldTimerHandle` and `MinComboTime` timer. On release goes to Release (`ReleaseAttack`), sends `Event.CombatState.ReleaseHeavyAttack`; `Event.CombatState.Throw` lets the same hold turn into a throw. References `GA_Melee_Light_C` and `GA_Shove_C`. |
| Release | The swing. Plays the release animation, waits for the *combat state anim notify* (`HasCombatStateAnimNotify`), then goes to **ComboWindow** or **Recovery**. `Combat.AbilityTypes.Block` is handled here (`ReleaseBlock`). |
| Thwack | Impact moment (the swing connected). `bAllowedToCombo`, `ComboTime`, `EnableCombo`; `IsAbilityQueued` and `TryCombo` -> **ComboWindow**, otherwise **Idle**. |
| ComboWindow | `MinimumComboTime`, `GetMinimumComboTime`. If another attack is queued (`IsAbilityQueued`) and the item/ability tags do not block it (`BlockQueueTags`, `Item.BlockAbility.Melee`, `Item.BlockAbility.Shove`), fires the queued ability; otherwise -> **Recovery**. Ability types `Shove` / `HeavyShove` can be chained here. |
| Recovery | `MinRecoveryTime`, `GetMinimumRecoveryTime`, takes `FMin` of animation length and the minimum. Accepts a queued ability early (`TryCombo`); then **Idle**. |
| StrongReset | Between strong-attack swings: goes back to **Windup** after the animation. |

Ability types (`CombatStates/AbilityTypes`): `LightAttack`, `StrongAttack`, `HeavyAttack`, `Shove`, `HeavyShove`, `Block`,
plus the non-melee ones (Shoot, Reload, Throw, Sprint, ...). `MeleeAbilityOverrides` is a per-weapon data asset that
overrides the timings above. The queue is the key feel feature: pressing attack during Thwack/ComboWindow/Recovery
buffers the next swing and it fires the moment the minimum time has passed.

### Abilities (`Blueprints/Abilities/Abilities/Items/`)

- `GA_Melee` -> `GA_Melee_Base` -> `GA_Melee_Light`, `GA_Melee_Heavy`; also `GA_Thwack`, `GA_Shove`, `GA_MeleeDeflect`,
  `GA_Melee_Destroy_Item`, `COLDEF_Shove` (the shove's collision definition).
- `GA_Melee_Base` variables: `bHeavyAttack`, `bStrongAttack`, `bPrimaryAbility`, `bSecondaryAbility`, `bEarlyRecovery`,
  `bUseAnimationTiming`, `AnimationStartSection`, `HitCount`, `HitData`, `HitDirection`, `HitIntensity`,
  `DamageSelectionIndex`, `HeadDamage`, `ExtrapolationDuration`.
- Damage multipliers: `CleaveDamageMultiplier`, `CleaveLimbDamageMultiplier`, `CleaveStabilityDamageMultiplier`
  (curves `Data/Curves/Weapons/Melee/FC_{1hBlade,1hBlunt,2hBlade,2hBlunt}_CleaveDamage*.uasset`,
  `FC_2hBladed_LimbCleaveDamage`, `FC_2hBlunt_CleaveStabilityDamage`): damage falls (or rises) with how many enemies one
  swing has already hit (inferred from `HitCount` + "cleave").
- Gameplay tags: `Damage.Melee.{Blade,Blunt,Light,Strong,Heavy}`, direction `Damage.Melee.Direction.{L2R,R2L,Vertical}`,
  combo `Combat.ComboStates.{LeftToRight,RightToLeft}` (swings alternate side), events `Event.Item.MeleeDetection` (the
  animation-notify-driven trace window), `Event.Item.MeleeThwack.{L2R,R2L}.Normal`, `Event.Item.MeleeDeflect`,
  `Event.CombatState.EndAttack`. Impact cues `GameplayCue.Impact.Melee.{L2R,R2L,T2B,Overhead}`.
- Hit detection is driven by animation: the swing animation has a detection window that fires `MeleeDetection`; targets
  come back as `DamageTargetData` / `HitResult`, then `DealDamage` applies `GE_ITEM_MeleeDamage` with a
  `DamageEffectSpecHandle`. `HitReactComponent` stagger/knockdown and `DisposableWeaponComponent` (weapon durability,
  `Event.Item.DestroyWeapon`) hang off it. (Ranges per weapon: `FC_Hatchet_Range`, `FC_TireIron_Range`, ...)

### Stamina (`Blueprints/Abilities/Effects/Items/`)

Gameplay effects: `GE_StaminaCost_LightAttack`, `_QuickAttack_PerHit`, `_StrongAttack` (+`_PerHit`), `_HeavyAttack`
(+`_PerHit`), `_Shove`, `_Heavy_Shove`, `_ExtraHits`; regeneration is blocked after actions by `GE_Stamina_BlockRegen`,
`_PostAttack`, `_PostShove`, `_Post_Hit_Interrupt`; `GE_StaminaStarved` (+`_BlockRegen`) applies when empty. The cost
itself is computed natively: `GE_StaminaCost_LightAttack` just runs `KExecution_LightAttackConsumeStamina`. One float,
`0.01` (0x3C23D70A), is the only literal in that asset. `Data/Curves/Stamina/FC_IdleStaminaRegenRate` is the regen curve.

### Per-weapon data

`Content/Data/Attributes/Items/Approved/Melee/CT_ItemAttributes_<Weapon>.uasset` (CurveTables: Baseball bat, Cleaver,
Crowbar, Fire axe, Hatchet, Machete, Mallet, Pipe, Rebar, Shovel, Sledge, Tire iron, Wrenches, Knife...) and
`ATT_Melee_<Weapon>`; weapon blueprints `Blueprints/Items/Melee/Approved/{Common,Uncommon}/<Weapon>/PM_*.uasset`.
Weapon classes by animation set: `1hBlade`, `1hBlunt`, `2hBlade`, `2hBlunt`, with Light/Heavy attack, `L2R`/`R2L` and
Windup/Loop/Release/Recover animation sections.

## Not yet known (needs the next pass)

1. All numbers: windup/hold/combo/recovery times, stamina costs, damage per weapon, cleave curves, ranges.
2. The native logic in `/Script/Kosovo`: `KCombatState` timers, `KExecution_*ConsumeStamina`, the hit trace shape.
3. Exactly how the heavy attack scales with hold time.

Next pass options: (a) decompile `NMRiH2-Win64-Shipping.exe` (IDA with the MCP server running, or Ghidra) and read the
`KExecution_*`, `KCombatState` and `UKCombatData` functions; (b) get a `.usmap` for the game (e.g. from its reflection
data) so the `.uexp` values and curve tables decode.

## Mapping onto the Source-engine mod (built: `[melee]`)

Built on 2026-10-19 without the numbers above (the timings are the rules file's): see README.md "Melee",
`src/game/melee.cpp`, `src/inv/melee.*` and "Melee" in docs/game-notes.md. L4D2's own swing is NMRiH2's Release; the rest
is around it:

- Windup / Holding: the client holds the attack button back from the game while it is held (the GetButtonBits hook),
  so the game's prediction does not swing early; a ring at the crosshair fills over `heavy_hold`. The Windup / Loop
  animation sections are a procedural pose over L4D2's viewmodel (drawn back, raised, a tremble when a heavy swing is
  ready), the Release a snap forward with a follow-through.
- Damage: light attacks do not kill a zombie by themselves (`light_damage` 40: three body hits for a common), the head
  takes more (`head_damage`), a heavy swing kills.
- Release: let go, the attack is given to the game until the weapon swings. Held past `heavy_hold`: a heavy swing
  (`heavy_damage`, `heavy_recovery`, `heavy_stamina`), checked by the server against the wind-up it saw.
- The queue (EventOnQueueAbility, AllowQueue): built, then taken out (2026-10-19): a swing let go during the
  recovery is dropped; each swing is its own click.
- CancelAttack: a shove while winding up calls it off.
- Holding slows the player: `wind_speed` %, eased in over the charge.
- GE_StaminaStarved: an exhausted survivor only swings light.
- Cleave (CleaveDamageMultiplier by HitCount): each further target of one swing takes `cleave` % of the one before.

Built after (2026-10-19): the weapon classes (`1hBlade`, `1hBlunt`, `2hBlade`, `2hBlunt`: wind-up, damage, stamina,
cleave, durability), durability and breaking (DisposableWeaponComponent), stamina per hit (`_PerHit`) and while winding
up, block and deflect (Block, MeleeDeflect), the heavy shove (HeavyShove), the throw (Throw from Holding), only the
head killing, the hit's impact, a swing into a wall, the heavy swing from above where the weapon's script has one, and
a raise gesture others see.

Not ported: a third-person wind-up pose (others see a gesture, not the hold), new viewmodel animations (the pose is
procedural; the scripts' `strongattacks` are not read by the game).
