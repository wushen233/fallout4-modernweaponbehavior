# Vanilla Fire Mode Switch Design

## Goal

Provide the built-in switch-fire hotkey only to vanilla weapons whose model and
installed modification can plausibly support more than one fire mode.

The configuration should represent this rule:

> The weapon gains select fire only while it has a compatible automatic-capable
> receiver, barrel, or equivalent fire-control component installed.

The base weapon alone must not grant the feature.

## Eligibility Rules

An entry is accepted only when all of these conditions are met:

1. The weapon is a gun and uses a normal magazine or cell feed.
2. The weapon has an installed OMOD that plausibly contains the fire-control
   mechanism for automatic fire.
3. The weapon is not driven by charging, spin-up, manual cycling, multiple
   barrels, or a continuous-fuel mechanism.
4. At least one installed OMOD in `requiresAnyOMods` is present.
5. The weapon can represent automatic fire without changing the base weapon
   form or forcing a visible unequip/equip cycle.
6. Existing MWB exclusions and the global MCM feature toggle still apply.

`requiresAnyOMods` should contain the automatic-capable attachments, not every
attachment at that slot. A semi-automatic receiver on the same weapon must make
the hotkey unavailable.

For receiver-driven weapons, the accepted OMODs are automatic or select-fire
receivers. For barrel-driven energy weapons, they are the automatic barrels.
The OMOD set must be generated from the loaded plugin records and checked for
the `Automatic` weapon flag or equivalent fire-mode property. Editor ID text
alone is not sufficient evidence.

## Vanilla Baseline

### Default Support

These weapons are in the verified vanilla baseline. Each entry accepts every
automatic receiver variant for its base weapon and rejects semi-only receivers:

| Weapon | Required component | Reason |
| --- | --- | --- |
| 10mm Pistol | Automatic receiver | Automatic receiver already provides a full-auto fire-control configuration. |
| Combat Rifle | Automatic receiver | The receiver family supports semi and automatic fire. |
| Assault Rifle | Automatic receiver | The receiver family supports semi and automatic fire. |
| Combat Shotgun | Automatic receiver | The automatic receiver is a coherent rapid-fire conversion. |
| Radium Rifle | Automatic receiver | The receiver family supports semi and automatic fire. |
| Handmade Rifle | Automatic receiver | The receiver family supports semi and automatic fire. |
| Pipe Gun | Automatic receiver | The game has a complete automatic receiver conversion. |

Unique weapons based on these base forms inherit the same rule. For example,
Overseer's Guardian inherits the Combat Rifle rule.

### Conditional Support

| Weapon | Recommendation | Condition |
| --- | --- | --- |
| Submachine Gun | Include as a deliberate exception | The platform already represents a mature automatic fire-control group; allowing a semi mode is mechanically plausible, even though vanilla ships it as automatic only. Require one of its valid receiver OMODs. |
| Laser Gun | Investigate separately | Its automatic barrel carries the spinning-barrel animation and model. Test whether an instant semi/automatic switch remains coherent before enabling. |
| Institute Laser | Investigate separately | Its automatic barrel is also a spinning-barrel configuration. Do not enable it with the ordinary receiver weapons. |
| Plasma Gun | Investigate separately | Its automatic barrels include spinning and flamethrower variants. Exclude the flamethrower and test the remaining configurations individually. |
| Tesla Rifle | Defer | It has no ordinary receiver or barrel family in the loaded records; its arc firing mechanism needs dedicated handling. |

The pipe gun is acceptable because the game already models a complete automatic
receiver conversion. The lore problem is solved by requiring that conversion;
the base improvised weapon does not gain a selector switch for free.

The submachine gun is the one worthwhile vanilla exception. Its risk is animation
or audio identity rather than mechanical plausibility, so it should be tested
separately and can be removed from the baseline without affecting other weapons.
Spray n' Pray inherits this rule because it uses the Submachine Gun base form.

### Do Not Enable

| Category | Vanilla examples | Reason |
| --- | --- | --- |
| Manual repeaters | Pipe Bolt-Action, Pipe Revolver, Lever-Action Rifle, Railway Rifle | There is no automatic fire-control group to switch between. |
| Revolvers and conventional pistols without automatic receivers | .44 Pistol, Western Revolver, Alien Blaster | No compatible automatic conversion. |
| Charging weapons | Gauss Rifle, Laser Musket, Cryolator | Hold/release or charge behavior conflicts with instant fire-mode changes. |
| Spin-up or fixed automatic weapons | Minigun, Gatling Laser | Their rate-of-fire and animation state are intrinsic to the firing cycle. |
| Continuous or exotic feed weapons | Flamer, Junk Jet, Syringer, Gamma Gun | Automatic/semi-automatic classification does not describe the operating mechanism. |
| Launchers and single-shot heavy weapons | Fat Man, Missile Launcher, Broadsider, Harpoon Gun | No select-fire mechanism is represented. |
| Melee and thrown weapons | All melee, grenades, mines | They are not guns. |
| Salvaged Assaultron Head | Vanilla weapon | Its discharge behavior is not a conventional receiver mode. |

The existing runtime already refuses weapons with the charging flags. The JSON
baseline should still exclude them so the MCM panel does not imply support.

## Animation Policy

Vanilla weapons do not ship with MWB's dedicated fire-mode switch clip. The
configuration should therefore not force `hasSwitchAnimation` unless a specific
weapon graph is proven to produce a false automatic-detection result.

- Prefer automatic detection so animation packs still work.
- Set `hasSwitchAnimation: false` only for a known broken graph.
- With animation-free switching disabled, the vanilla selection remains
  unavailable until a valid switch animation is found.
- With animation-free switching enabled, vanilla switching is instant.

## Configuration Shape

The vanilla baseline ships one JSON file per base weapon. This keeps each
weapon's receiver list isolated and makes third-party overrides or removals
straightforward. A representative entry looks like this:

```json
{
  "entries": [
    {
      "plugin": "Fallout4.esm",
      "weapon": "0xWEAPON_FORM_ID",
      "fireModeSwitch": {
        "requiresAnyOMods": [
          {
            "plugin": "Fallout4.esm",
            "formId": "0xAUTOMATIC_OMOD_FORM_ID"
          }
        ],
        "automaticOMod": {
          "plugin": "Modern Weapon Behavior.esp",
          "formId": "0x00000801"
        },
        "semiAutomaticOMod": {
          "plugin": "Modern Weapon Behavior.esp",
          "formId": "0x00000802"
        }
      }
    }
  ]
}
```

Do not set `keyID` for the vanilla baseline. MWB's built-in fire-mode hotkey
calls `MWB.SwitchFireMode()`, which does not require a hotkey ID. An ID is only
needed for a separate directed binding that calls
`MWB.SwitchOModGroup(keyID)`. Keeping IDs out of these files avoids turning the
same rule into a second independently bound action.

Do not use a shared FormList or broad weapon keyword for vanilla coverage: the
required automatic OMODs differ by weapon. Unique weapons still inherit the
rule through their base weapon form.

The shipped file set is:

- `Vanilla_10mm_FireMode.json`
- `Vanilla_CombatRifle_FireMode.json`
- `Vanilla_AssaultRifle_FireMode.json`
- `Vanilla_CombatShotgun_FireMode.json`
- `Vanilla_RadiumRifle_FireMode.json`
- `Vanilla_HandmadeRifle_FireMode.json`
- `Vanilla_PipeGun_FireMode.json`
- `Vanilla_SubmachineGun_FireMode.json`

## Runtime Behavior To Verify

For each supported weapon, test in both first and third person:

1. Automatic component installed: the hotkey switches between automatic and semi.
2. Semi-only component installed: the hotkey is refused and leaves the weapon unchanged.
3. No receiver or barrel installed: the hotkey is refused without forcing a re-equip.
4. Switching changes rate, ammo consumption, HUD label, sound set, and firing audio.
5. Switching preserves the visual model and does not trigger an equip animation.
6. Reloading, firing, aiming, slam-fire, and a shared inventory stack are rejected safely.
7. Save and load retain the installed component and current fire mode.
8. An exclusion made through MWB takes priority over the JSON rule.

Test one representative from each OMOD family first:

- 10mm Pistol
- Combat Rifle
- Assault Rifle
- Combat Shotgun
- Laser Gun
- Plasma Gun
- Pipe Gun
- Submachine Gun

Only after these pass should the remaining receiver and barrel families be
generated and added.

## Implementation Order

1. Enumerate vanilla receiver and barrel OMODs from `Fallout4.esm`.
2. Keep only OMODs that produce the automatic weapon flag or an equivalent
   automatic fire-mode property.
3. Record the base weapon, OMOD attach point, EditorID, and local FormID in an
   intermediate manifest.
4. Generate the weapon entries from that manifest rather than hand-copying IDs.
5. Validate every cited OMOD against its base weapon and attach point.
6. The first configuration ships 10mm, Combat Rifle, Assault Rifle, Combat
   Shotgun, Radium Rifle, Handmade Rifle, Pipe Gun, and Submachine Gun.
7. Test the spinning-barrel Laser Gun, Institute Laser, and Plasma Gun variants
   separately before enabling them.
8. Keep third-party weapons and receiver conversions in separate add-on files.
