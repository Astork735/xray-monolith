#pragma once

class CDialogHolder;

// A keyboard on the VR UI panel, shown below the window while one of its
// text boxes has focus (naming a saved game, a search box). The pointer types
// on it; Done is Enter. Nothing outside VR.

// The window's pointer at a position on the panel (UI coordinates) with its
// button edge (1 pressed, -1 released, 0 neither). True when it is on the
// keyboard: the window gets no click there, which would take the focus from
// the box. It does not type; the hands do (VrKeyboardHand).
bool VrKeyboardPointer(CDialogHolder& holder, const Fvector2& position, int button);
// One hand's own ray on the panel (negative: off it) and its trigger. Each
// hand has its pointer on the keyboard and types the key under it as its
// trigger is pulled, so both hands type. True when it typed a key.
bool VrKeyboardHand(CDialogHolder& holder, int hand, const Fvector2& position, bool down);
// Draws the keyboard over the holder's windows (inside UI().pp_start).
void VrKeyboardDraw(CDialogHolder& holder);
