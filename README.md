Modern Weapon Behavior
======================

F4SE plugin of **Modern Weapon Behavior (MWB)**, an all-in-one overhaul of Fallout 4's weapon behaviors for first and third person.

This repository holds the source code of the plugin (`MWB.dll`) as released on Nexus Mods. The behavior files, animations, scripts and interface files ship with the mod itself.

## Features

* Looped reload the way Bethesda does it in Fallout 76 and Starfield: one shell per pass, blended passes, cancel at the end of the pass with the fire or reload button (the end of the clip always plays), and the aim kept from cutting the reload short. Driven by the `MWB_Reload` annotation (`InsertShell` adds one round, on any weapon; `CallLoop` calls the next pass), for NPCs too.
* Tactical reload, sighted reload, sprint transitions and sprinting equips, chosen per weapon through JSON files, by weapon, keyword or form list. Weapons whose own behavior plays a tactical reload on Tactical Reload's events keep it.
* Inspect, ammo check (with an on-screen magazine readout, optionally on every gun), special attack and fire mode switch (optionally an instant switch on every gun, experimental), callable from Papyrus and MCM hotkeys.
* Papyrus functions for attaching, detaching, querying and replacing OMODs on individual inventory stacks, including equipped-item refresh.
* Arm-only animations, one per arm at a time (a new one restarts the arm unless either is locked), with weapon lock (guns and melee weapons), slide lock (with lock and release sounds, nodes hidden while locked, and an option to wait for the last shot clip) and per fire mode weapon bone poses.
* Animation annotations to fire ammo, use items and cast spells from any animation.
* Weapon HUD module (with the names given at the workbench), bullet in the chamber, drop unspent ammo on reload and manual reload, where only the reload button reloads and the trigger on an empty magazine dry fires (the last three with per weapon exclusions).
* Hold to block for melee and fists, and firing while sprinting in first person.
* Tactical sprint (first and third person) and slide (first person), as in Call of Duty, with optional clips per weapon and a slide sound per floor material; jumping while sprinting plays the sprint stop and start clips.
* Last shot clips: a fire animation of its own for the last round in the magazine, from the hip and from the sights.
* Bolt charge while aiming that stays in the sights, always follows the shot and carries on through a sprint.
* See Through Scopes' 3D scopes follow the real aim, so they no longer blink at the start of a sighted bolt charge or reload.
* Aim sway and hold breath, the game's own, for regular scopes, 3D scopes and sights without a scope, each with its own setting.
* Aim down sight in and out animations timed to the weapon's sight speed, and a setting for the sighted fire jiggle.
* Animation clips missing from the AnimTextData still load.
* MWB's animation events and variables are added in memory to every character behavior the game loads, so the mod ships only the behaviors it changes and works under other mods' root behaviors.
* A setting to play the full equip animation instead of the quick one when switching weapons from favorites, guns and melee weapons.
* An exclusion panel inside the MCM: players take any feature away from the weapon in hand, or from every weapon with one of its keywords, and it applies at once.

## Fire mode switch animation

Fire mode switching automatically checks the equipped weapon graph's animation resource directory for `WPNSwitchFireMode.hkx`. When found, MWB follows the regular animation path; when absent, the global MCM option **Allow fire mode switching without an animation** controls first-person behavior. A weapon entry can override detection with `hasSwitchAnimation: true` or `false`; omit the field (or set it to `null`) to use automatic detection. Third-person switching remains instant.

```json
{
  "plugin": "Fallout4.esm",
  "weapon": "0x00004822",
  "fireModeSwitch": {
    "keyID": "ExampleMod_MyRifle_FireMode",
    "hasSwitchAnimation": true,
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
```

Both OMOD references are required and must use the same attach point. The plugin name and plugin-local FormID can refer to any loaded plugin. `hasSwitchAnimation` is optional: omit it to detect the clip automatically, or set it to `true`/`false` to override detection. The animation-optional setting defaults to off, preserving the animation-required behavior when no switch clip is found.

To limit fire-mode switching to weapons fitted with a suitable receiver, add `requiresAnyOMods` with one or more receiver OMOD references. At least one listed OMOD must be installed on the equipped weapon. MWB registers those OMOD attach points on the configured weapon before saves are loaded, so the receiver remains serialized and the requirement can be checked after loading:

```json
"requiresAnyOMods": [
  { "plugin": "ExampleWeapon.esp", "formId": "0x00000820" },
  { "plugin": "ExampleWeapon.esp", "formId": "0x00000821" }
]
```

Omit the field to preserve the previous behavior, where any weapon matching `fireModeSwitch` can switch modes.

Fire-mode configurations that omit either OMOD reference are rejected with a warning; add both references when migrating an existing configuration.

The upstream **Switch fire mode on every gun** option (`FireModeSwitch/bEveryGun`) enables an experimental, animation-free fallback for weapons without a dedicated JSON fire-mode rule. It requires `Modern Weapon Behavior.esp` to provide `ap_MWB_FireMode`, `mod_MWB_FireMode_Auto`, and `mod_MWB_FireMode_Semi`; it is disabled by default. A weapon with a JSON rule continues to use that rule, including its receiver OMOD requirements.

## Generic OMOD switch groups

Shared OMOD switch profiles are declared at the top level of any JSON file in `Data/F4SE/Plugins/MWB`. Weapon entries reference one or more profile IDs using `omodSwitchProfiles`. Each group selects an attach-point keyword and a filter mode:

* `all`: cycle loaded weapon OMODs at that attach point, except records with additional parent or instantiation-filter constraints.
* `include`: cycle only OMODs listed in `omods`.
* `exclude`: cycle OMODs at that attach point except those listed in `omods`, with the same compatibility-constraint exclusions as `all`.

Groups can use one of the built-in hotkey categories (`muzzle`, `sight`, `ammunition`, or `underbarrel`), or use a custom `keyID` without a category. A custom ID selects that exact group, so add-ons can register independent hotkeys without adding a new hard-coded category to MWB. Keep `category` for compatibility with MWB's built-in category hotkeys. `refreshEquipped: true` opts into the native live equipped-weapon refresh used for visual attachment changes; it requires MWB's supported native OMOD runtime path (`1.10.163` or `1.11.240`). Omitted groups preserve the legacy behavior, which defaults this on for muzzle and sight categories and off for other categories.

```json
{
  "omodSwitchProfiles": [
    {
      "id": "example.muzzle",
      "groups": [
        {
          "id": "muzzle",
          "keyID": "ExampleMod_MyRifle_Muzzle",
          "refreshEquipped": true,
          "attachPoint": { "plugin": "ExampleWeapon.esp", "formId": "0x00000800" },
          "filter": "include",
          "omods": [
            { "plugin": "ExampleWeapon.esp", "formId": "0x00000801" },
            { "plugin": "ExampleWeapon.esp", "formId": "0x00000802" }
          ]
        }
      ]
    }
  ],
  "entries": [
    {
      "plugin": "ExampleWeapon.esp",
      "weapon": "0x00000810",
      "omodSwitchProfiles": [ "example.muzzle" ]
    }
  ]
}
```

To register an add-on hotkey, give it a unique ID in that add-on's `MCM/Config/<ModName>/keybinds.json`, expose the same ID as a `hotkey` control in its `config.json`, and call MWB's Papyrus function with the ID as a string parameter:

```json
{
  "id": "ExampleMod_MyRifle_Muzzle",
  "desc": "Switch rifle muzzle",
  "action": {
    "type": "CallGlobalFunction",
    "script": "MWB",
    "function": "SwitchOModGroup",
    "params": [ "ExampleMod_MyRifle_Muzzle" ]
  }
}
```

The hotkey control in `config.json` must use the same ID. The registered keybind ID must exactly match the configured `keyID`; IDs are case-sensitive and should be unique across switch groups, ammo profiles, and fire-mode rules on a weapon. Keep the built-in category keybinds for compatibility, or use unique add-on IDs when a weapon needs multiple independently switchable controls in one category. The generic Papyrus entry routes fire-mode IDs through MWB's existing animation-aware switch event.

The attach point is injected into configured weapon records at data-ready time so the engine can construct the selected OMOD instance. Generic OMOD switching currently requires the equipped weapon instance to be uniquely resolvable; unsupported or ambiguous stacks fail without changing the weapon.

On `1.10.163` and `1.11.240`, muzzle and sight groups share the native inventory and equip refresh used by ammo subtype switching. Switch while the weapon is drawn and idle, with ADS released. These groups preserve the loaded round count and reject changes to the weapon's ammo type or magazine capacity. `10mmMuzzleSwitchTest.json` configures the standard muzzle, suppressor, iron sights and reflex sight for testing.

## Ammo subtype switch profiles

Ammo subtype profiles are declared at the top level of any JSON file in `Data/F4SE/Plugins/MWB`. They are matched by the equipped weapon instance's current ammo form, so weapons that use a configured ammo type do not need individual weapon entries. `baseAmmo` is the first cycle entry; each `ammoTypes` entry pairs an `ammo` form with the OMOD that applies it. `weaponBaseAmmo` optionally scopes a profile to weapons whose default ammo matches that form. Use it when the same ammo form appears in multiple profiles with different OMODs; MWB selects the profile matching the equipped weapon's default ammo. Profiles without this field remain global. `baseOMod` is optional when the base ammo is represented by having no OMOD at the ammo attach point. If provided, it is used when cycling back to the configured base state; a weapon already using that base ammo without the OMOD is also accepted as the initial state. An optional profile `keyID` lets an add-on's MCM hotkey select this ammo profile through `MWB.SwitchOModGroup`, alongside the legacy category hotkey.

```json
{
  "ammoSwitchProfiles": [
    {
      "id": "example.10mm",
      "weaponBaseAmmo": { "plugin": "Fallout4.esm", "formId": "0x0001F276" },
      "baseAmmo": { "plugin": "Fallout4.esm", "formId": "0x0001F276" },
      "baseOMod": { "plugin": "Example.esp", "formId": "0x00000800" },
      "ammoTypes": [
        {
          "ammo": { "plugin": "Example.esp", "formId": "0x00000810" },
          "omod": { "plugin": "Example.esp", "formId": "0x00000811" }
        }
      ]
    }
  ]
}
```

The configured OMODs must target weapons and share one valid attach point. Every alternate ammo type needs an OMOD; the base ammo may omit one. A repeated ammo form is allowed only when its profiles have different `weaponBaseAmmo` selectors; duplicate mappings for the same selector, duplicate OMODs within a profile, and duplicate base-ammo profiles are rejected with a warning. For weapons whose base ammo matches a profile, MWB registers the attach point before loading a save so the stored OMOD can be restored. The **Switch ammunition OMOD** hotkey requests a normal reload after a successful switch. If the game initially refuses it, MWB retries for up to two seconds while the same weapon and ammo are equipped and the magazine is empty. Queued switches are cancelled when a save starts loading. When no ammo profile matches, the hotkey continues to use the generic ammunition OMOD group if configured. A weapon with both kinds of configuration is refused as ambiguous.

The `ExpandingBullets_MR_<caliber>_AmmoSwitch.json` files contain the migrated MSF ammo profiles, split by the original caliber files; the 10mm profile remains in `ExpandingBullets_10mmAmmoSwitch.json`. Together these files contain 61 profiles and 224 alternate ammo entries. Each profile uses `weaponBaseAmmo` to scope it to its source ammo family, allowing a shared ammo form to have different OMOD mappings on different weapon families. Loot generation and crafting data are intentionally omitted. One 5.45mm alternate entry (`MunitionsReloaded.esp|CD808`) is left out because its source maps it to the same OMOD as another alternate in that profile, which cannot represent two distinct ammo states through one installed OMOD.

Ammo subtype profiles use the native inventory and equip refresh on `1.10.163` and `1.11.240`. These runtimes require a matching Address Library and the expected equip call site. Other runtimes use the existing in-place refresh.

## Supported Runtimes

One DLL for every runtime:

* Old-Gen `1.10.163`
* Next-Gen `1.10.980` and `1.10.984`
* Anniversary Edition `1.11.137` to `1.11.240`

## Pre-requisites

To play:

* [Fallout 4 Script Extender (F4SE)](https://f4se.silverlock.org/)
* [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)

To build:

* [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload (MSVC and a Windows 10/11 SDK)
* [XMake](https://xmake.io) 3.0.0 or newer
* [Git](https://git-scm.com/)

## Building

```bat
git clone --recurse-submodules https://github.com/LuBuCake/fallout4-modernweaponbehavior
cd fallout4-modernweaponbehavior
xmake f -y -p windows -a x64 -m releasedbg
xmake build
```

The plugin is written to `build/windows/x64/releasedbg/MWB.dll`, with its `MWB.pdb` next to it.

## Papyrus OMOD API

`MWBOModSwitch.SetItemOMod`, `HasItemOMod`, `GetEquippedStackIndex` and `ReplaceItemOMod` operate on inventory stacks. Equipped weapons are refreshed without a visible unequip/equip cycle when their instance data can be resolved safely. Equipped non-weapons are rejected because their biped refresh path is not implemented. Replacement OMODs must share a valid attach point. See `papyrus/Source/User/MWBOModSwitch.psc` for declarations.

[CommonLibF4](https://github.com/Dear-Modding-FO4/commonlibf4) comes as a git submodule in `lib/commonlibf4`, pinned to the exact commit the release was built with: [`1bd796eb`](https://github.com/Dear-Modding-FO4/commonlibf4/tree/1bd796eb593b32154a0a2bb2e96c3747bae3b0e2). If you cloned without `--recurse-submodules`, run `git submodule update --init --recursive`.

### Visual Studio

```bat
xmake project -k vsxmake
```

Generates a `vsxmake20XX/` solution that stays in sync with `xmake.lua`.

### Installing straight to the game

Set one of these environment variables and run `xmake install`:

* `XSE_FO4_MODS_PATH`: a mod manager's mods folder; the plugin goes to `<path>/MWB/F4SE/Plugins`.
* `XSE_FO4_GAME_PATH`: the game folder; the plugin goes to `<path>/Data/F4SE/Plugins`.

### Troubleshooting

* **`'_Mbstatet': 'struct' type redefinition`**: more than one Windows SDK is installed and the headers got mixed. Pin one: `xmake f -c` and then `xmake f -y -p windows -a x64 -m releasedbg --vs_sdkver=10.0.26100.0`.
* **XMake picks `mingw`**: happens when building from Git Bash. Build from PowerShell or the Developer Command Prompt, or keep `-p windows` in `xmake f`.

## Project Layout

```
src/
  main.cpp     F4SE entry points (OG query and load)
  Actions/     first person only actions and weapon actions allowed per weapon
  Arm/         arm-only animations: weapon lock and arm space
  Config/      the per weapon JSON files and the exclusions
  Core/        runtime check, F4SE messages, the equipped weapon and the gun state
  Fire/        MWB_WeaponFireAmmo annotation
  HUD/         weapon HUD module and ammo check readout
  Behavior/    MWB's event and variable names added to every behavior loaded
  Hooks/       animation events, graph manager events, graph notify, clip generators, graph generate, idle start, input, queued events, subgraph animations, animation update
  Magic/       MWB_UseAlchemyItem and MWB_CastSpell annotations
  Melee/       hold to block
  Papyrus/     native functions of the MWB script
  Reload/      looped, sighted and tactical reload, sighted bolt charge, bullet in the chamber, drop unspent ammo, manual reload, bolt charge resync
  Settings/    MWB.ini, MWB_Custom.ini, the MCM settings and the MCM exclusion panel
  Sprint/      sprint transitions, third person sprint camera, firing while sprinting, tactical sprint and slide
  Weapon/      fire mode switch and pose, slide lock, weapon bones, scope sway, aim down sight, sighted fire jiggle, full equip, last shot clips, aiming right after a save loads and the weapon graph's sights (See Through Scopes)
```

## License

Modern Weapon Behavior codebase is licensed under [GPL-3.0](LICENSE) WITH the [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS), the same terms as CommonLibF4.
Please refer to the LICENSE and EXCEPTIONS files for detailed information.
