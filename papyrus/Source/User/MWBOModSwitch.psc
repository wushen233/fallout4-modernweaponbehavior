; This file is part of Modern Weapon Behavior and is licensed under GPL-3.0-or-later.
; It is additionally covered by the Modding Exception and GPL-3.0 Linking Exception
; with Corresponding Source described in the repository's EXCEPTIONS file.
Scriptname MWBOModSwitch Native Hidden

; Applies or removes one OMOD on the indexed stack of an inventory item.
; stackIndex is the zero-based stack position for akItem in akOwner's inventory.
; Equipped weapons refresh in place; equipped non-weapons are rejected until their refresh path is supported.
Bool Function SetItemOMod(ObjectReference akOwner, Form akItem, Int stackIndex, ObjectMod akOMod, Bool attach) Global Native

; Returns whether the indexed stack has the OMOD.
Bool Function HasItemOMod(ObjectReference akOwner, Form akItem, Int stackIndex, ObjectMod akOMod) Global Native

; Returns the unique equipped stack index for this base item, or -1 if none or ambiguous.
Int Function GetEquippedStackIndex(ObjectReference akOwner, Form akItem) Global Native

; Replaces one OMOD with another on a single stack and refreshes equipped state once.
; The source and destination OMODs must share a valid attach point.
Bool Function ReplaceItemOMod(ObjectReference akOwner, Form akItem, Int stackIndex, ObjectMod akFromOMod, ObjectMod akToOMod) Global Native
