#pragma once

class CActor;

// Status display on the back of the off-hand wrist (vr_wrist_display):
// health, stamina, and radiation, psy, bleeding and hunger when they matter,
// as bars. It shows while the player looks at the wrist with the forearm
// across the view and the back of the wrist turned up, as when reading a
// watch. Drawn in the VR world-overlay pass.
void RenderVrWristDisplay(CActor& actor);
