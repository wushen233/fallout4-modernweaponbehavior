Scriptname MWB Native Hidden
{Modern Weapon Behavior: first person weapon actions for weapon mods to call.
Each one plays a clip from the weapon's first person animation folder, when the
behavior allows it, and returns true if it started, false if it was refused
(firing, reloading, aiming, sprinting, equipping, third person...). They are
first person only: in third person they always return False. The weapon needs
the clip in its animation folder; AnimTextData generated with Modern Weapon
Behavior's GunBehavior active is recommended, not required.}

; WPNInspect.hkx. Only from the hip idle; anything the player does cuts it short.
Bool Function Inspect() global native

; WPNAmmoCheck.hkx. Same rules as Inspect. With "Ammo check on every gun" on (MCM),
; a gun without "ammoCheck" shows only the ammo check display, without an animation.
Bool Function AmmoCheck() global native

; WPNSpecialAttack.hkx, played as an attack: refused while firing, reloading or
; in melee until the clip playing allows a new action; starts out of a sprint,
; as melee does. Annotations: weaponFire fires the weapon; weaponSwing then
; hitFrame strikes, as in melee; initiateStart lets fire and reload cut it
; short from there on (without it, it plays to the end); AttackEnd ends it.
; The game keeps one attack state: the clip starts in the firing one, and
; weaponSwing / hitFrame leave it, so a weaponFire after them does nothing.
; Fire first, or put attackState.Enter between the hitFrame and the weaponFire.
Bool Function SpecialAttack() global native

; Arm animations need no function: PlayIdle with an IDLE record whose graph is
; Actors\Character\_1stPerson\Behaviors\RootBehavior.hkx, whose event is
; dyn_MWB_LeftArm, dyn_MWB_LeftArmLocked, dyn_MWB_RightArm or dyn_MWB_RightArmLocked
; (or the WithCamera ones, dyn_MWB_LeftArmWithCamera[Locked] and
; dyn_MWB_RightArmWithCamera[Locked], which move the view with the animation's
; camera too), and whose animation file is the .hkx to play on that arm (any
; folder, no AnimTextData). The Locked ones hold the weapon still while they play. One at a
; time: the graph refuses another until the arm is back on the weapon.
; StopArmAnimation blends a playing one back to the weapon right away.
Bool Function StopArmAnimation() global native

; WPNSwitchFireMode.hkx, then the weapon in the player's hands switches between
; automatic and semi-automatic, and keeps the choice in the save (Modern Weapon
; Behavior.esp). The switch comes on the clip's MWB_SwitchFireMode annotation
; or, without one, at its end; cut short, it does not switch. Same rules as
; Inspect, and only for weapons with "fireModeSwitch" in their Modern Weapon
; Behavior .json entry.
Bool Function SwitchFireMode() global native

; Cycles the active weapon's configured OMOD switch group for each category.
Bool Function SwitchMuzzleOMod() global native
Bool Function SwitchSightOMod() global native
Bool Function SwitchAmmunitionOMod() global native
Bool Function SwitchUnderbarrelOMod() global native
