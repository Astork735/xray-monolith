#pragma once

#include "ui_base.h"
class CUIStatic;

class CUICursor : public pureRender,
                  public pureScreenResolutionChanged
{
	bool bVisible;
	Fvector2 vPos;
	Fvector2 vPrevPos;
	bool m_b_use_win_cursor;
	CUIStatic* m_static;
	bool m_vr_pressed=false;
	void InitInternal();
public:
	CUICursor();
	virtual ~CUICursor();
	virtual void OnRender();

	Fvector2 GetCursorPositionDelta();

	Fvector2 GetCursorPosition();
	void SetUICursorPosition(Fvector2 pos);
    void SetVrCursorPosition(Fvector2 pos);
    // The VR pointer's trigger is held on a window (a drag, as the left mouse
    // button held: the PDA map pans with it). Never set outside VR.
    void SetVrPressed(bool pressed) { m_vr_pressed=pressed; }
    bool VrPressed() const { return m_vr_pressed; }
    // The left mouse button is held, or the VR pointer's trigger is: for a
    // drag (the map, a slider, a list's pad, an item).
    bool PrimaryHeld() const;
	void UpdateCursorPosition(int _dx, int _dy);
	virtual void OnScreenResolutionChanged();

	bool IsVisible() { return bVisible; }
	void Show() { bVisible = true; }
	void Hide() { bVisible = false; }
};
