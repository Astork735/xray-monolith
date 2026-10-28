#pragma once
#include "VrBodySlotPolicy.h"

class CActor;

// Body slots (vr_body_slots): a squeeze at a place on the body takes the item
// kept there (see VrBodySlotPolicy.h for the layout). Knife, grenade, bolt and
// binoculars are drawn, or put away when already in use; the detector is
// switched on or off; the PDA on the chest opens the PDA and reaching over
// the off-hand shoulder opens the inventory. A chest pocket puts its quick-use item
// in the hand; held to the mouth (or with the trigger pulled) it is used.

// World position of a body slot.
bool VrBodySlotPoint(CActor& actor, int kind, Fvector& point);
// The nearest body slot this hand can use now, or -1.
int VrBodySlotCandidate(CActor& actor, int hand, const Fvector& position, float& distance);
// Takes, puts away, toggles or opens what the slot holds. True when it
// changed the active inventory slot.
bool VrBodySlotTake(CActor& actor, int kind, int hand);
// A light pulse when a hand enters a slot's reach.
void VrBodySlotHover(CActor& actor, const bool* tracked, const Fmatrix* hands);
// Holster point for a knife or binoculars held as a physical weapon.
bool VrBodySlotHolster(CActor& actor, u16 inventory_slot, Fvector& point, float& range);
// The hand holding the switched-on detector, or -1.
int VrDetectorHand(CActor& actor);
// Where the detector or flashlight is drawn in that hand (its first-person
// model's transform).
bool VrDevicePose(CActor& actor, Fmatrix& pose);
// Where the knife (its first-person model) is held in its hand.
bool VrKnifePose(CActor& actor, Fmatrix& pose);
// Places the detector or flashlight and the knife in their hands as the game
// holds them in first person, and makes that the device's first-person
// transform. Right after the first-person HUD's own update, each frame.
void VrUpdateHudItems(CActor& actor);
// Draws its screen (artefact detectors' displays) on it: per eye, after
// vr_world_overlay.
void RenderVrDeviceUI(CActor& actor);
// Where an item without a VR weapon profile (knife, unsupported weapon,
// grenade, bolt) is drawn in a hand: its world model at the grip.
bool VrHeldItemPose(int hand, Fmatrix& pose);
// Binoculars held up to the eyes: the magnified view is asked for each
// update and drawn on two lenses just ahead of the eyes (per eye, after
// vr_world_overlay); the binoculars' own model is not drawn meanwhile.
bool VrBinocularsAtEyes(CActor& actor);
// A free hand at the side or top of the head (the helmet): its trigger
// switches the headlamp and its upper face button (B/Y) night vision, as
// the desktop keys do. Once per frame.
void VrHeadGestureUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands);
void VrBinocularsUpdate(CActor& actor);
void RenderVrBinoculars(CActor& actor);
// Items held from the belt are let go with the grip: the knife and
// binoculars go back on the belt and the detector or flashlight is put away.
// A swing of the knife hits what its blade passes; the flashlight's trigger
// switches its light. Once per frame after the body-slot grabs.
void VrHandItemsUpdate(CActor& actor, const bool* tracked, const bool* grip);
// The hand holding the active grenade or bolt, or -1.
int VrMissileHand(CActor& actor);
// Grenades and bolts thrown by hand: closing the hand readies one; letting
// go while the hand moves faster than 1.5 m/s throws it with the hand's
// velocity (times vr_throw_scale); opening it gently keeps a grenade
// unreadied and puts a bolt back in its pouch. After a throw the next bolt
// waits in the pouch for the hand to take it. Once per frame after the
// body-slot grabs.
void VrMissileUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands, const bool* grip);
// Items held from chest pockets: used at the mouth or by the trigger, put
// back when the grip opens. Once per frame after the body-slot grabs.
void VrPocketUpdate(CActor& actor, const bool* tracked, const Fmatrix* hands, const bool* grip);
bool VrPocketHeld(CActor& actor, int hand);
// The hand is at the equipment (a body slot, holster or ammunition pouch),
// where a squeeze takes from it rather than grabbing what the hand points at.
bool VrHandAtEquipment(CActor& actor, const Fvector& position);
// While the PDA window is open, a squeeze at its place on the chest closes it.
void VrPdaUpdate(CActor& actor);
// Frees the bolt pouch model.
void ReleaseVrBodySlots(CActor& actor);
// Draws the items carried in body slots on the belt (bolts as a pouch), the PDA on the chest,
// the detector in the off hand, the pocket item a hand is reaching for and
// the one it holds (world scene graph).
void RenderVrBodySlots(CActor& actor);
