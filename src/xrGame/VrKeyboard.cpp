#include "stdafx.h"
#include "VrKeyboard.h"
#include "UIDialogHolder.h"
#include "ui/UIDialogWnd.h"
#include "ui/UICustomEdit.h"
#include "ui_base.h"
#include "../xrEngine/GameFont.h"
#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/UIShader.h"
#include <dinput.h>

namespace
{
struct Key { char label[8]; int dik; float x, y, w; };
// Laid out in UI coordinates across the bottom of the panel: four rows of
// characters and a row of Shift, Space, Back and Done.
constexpr float key_left=112.f, key_top=548.f, key_width=76.f, key_height=40.f, key_gap=4.f;
xr_vector<Key> g_keys;
bool g_shift=false;
// Each hand's pointer on the panel and its trigger.
struct HandPointer { Fvector2 cursor{-1.f,-1.f}; u32 frame=0; bool down=false; };
HandPointer g_hands[2];

int CharDik(char c)
{
    static const char* letters="qwertyuiopasdfghjklzxcvbnm";
    static const int letter_dik[26]={DIK_Q,DIK_W,DIK_E,DIK_R,DIK_T,DIK_Y,DIK_U,DIK_I,DIK_O,DIK_P,
        DIK_A,DIK_S,DIK_D,DIK_F,DIK_G,DIK_H,DIK_J,DIK_K,DIK_L,DIK_Z,DIK_X,DIK_C,DIK_V,DIK_B,DIK_N,DIK_M};
    if (c>='1' && c<='9') return DIK_1+(c-'1');
    if (c=='0') return DIK_0;
    if (c=='-') return DIK_MINUS;
    if (const char* at=strchr(letters,c)) return letter_dik[at-letters];
    return 0;
}

const xr_vector<Key>& Keys()
{
    if (!g_keys.empty()) return g_keys;
    const char* rows[4]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm-"};
    const float indent[4]={0.f,0.f,0.5f,1.f};
    for (int r=0;r<4;++r)
        for (int i=0;rows[r][i];++i)
        {
            Key key{};
            key.label[0]=rows[r][i];
            key.dik=CharDik(rows[r][i]);
            key.x=key_left+(indent[r]+float(i))*(key_width+key_gap);
            key.y=key_top+float(r)*(key_height+key_gap);
            key.w=key_width;
            g_keys.push_back(key);
        }
    const struct { const char* label; int dik; int start, span; } wide[4]={
        {"Shift",DIK_LSHIFT,0,2},{"Space",DIK_SPACE,2,4},{"Back",DIK_BACK,6,2},{"Done",DIK_RETURN,8,2}};
    for (const auto& w:wide)
    {
        Key key{};
        xr_strcpy(key.label,w.label);
        key.dik=w.dik;
        key.x=key_left+float(w.start)*(key_width+key_gap);
        key.y=key_top+4.f*(key_height+key_gap);
        key.w=float(w.span)*key_width+float(w.span-1)*key_gap;
        g_keys.push_back(key);
    }
    return g_keys;
}

Frect Area()
{
    Frect area;
    area.set(key_left-8.f,key_top-8.f,key_left+10.f*(key_width+key_gap)-key_gap+8.f,
        key_top+5.f*(key_height+key_gap)-key_gap+8.f);
    return area;
}

const Key* KeyAt(const Fvector2& position)
{
    for (const Key& key:Keys())
        if (position.x>=key.x && position.x<key.x+key.w && position.y>=key.y && position.y<key.y+key_height)
            return &key;
    return nullptr;
}

CUICustomEdit* FocusedEdit(CUIWindow* window)
{
    if (!window || !window->IsShown()) return nullptr;
    if (auto* edit=smart_cast<CUICustomEdit*>(window))
        if (edit->InputFocused()) return edit;
    for (CUIWindow* child:window->GetChildWndList())
        if (CUICustomEdit* edit=FocusedEdit(child)) return edit;
    return nullptr;
}

CUICustomEdit* FocusedEdit(CDialogHolder& holder)
{
    return g_vr_owns_present ? FocusedEdit(holder.TopInputReceiver()) : nullptr;
}

void Type(CUICustomEdit& edit, const Key& key)
{
    if (key.dik==DIK_LSHIFT) { g_shift=!g_shift; return; }
    // Shift is one-shot: held for the next key only.
    const bool shift=g_shift && key.dik!=DIK_RETURN && key.dik!=DIK_BACK;
    edit.VrTypeKey(key.dik,shift);
    if (shift) g_shift=false;
}
}

bool VrKeyboardPointer(CDialogHolder& holder, const Fvector2& position, int button)
{
    return FocusedEdit(holder) && Area().in(position.x,position.y);
}

bool VrKeyboardHand(CDialogHolder& holder, int hand, const Fvector2& position, bool down)
{
    if (hand<0 || hand>1) return false;
    HandPointer& pointer=g_hands[hand];
    const bool pulled=down && !pointer.down;
    pointer.down=down;
    pointer.cursor=position;
    pointer.frame=Device.dwFrame;
    CUICustomEdit* edit=FocusedEdit(holder);
    if (!edit || !pulled) return false;
    const Key* key=KeyAt(position);
    if (key) Type(*edit,*key);
    return key!=nullptr;
}

namespace
{
// Untextured, coloured by vertex.
void PushRect(float left, float top, float right, float bottom, u32 color)
{
    Fvector2 a, b;
    UI().ClientToScreenScaled(a,left,top);
    UI().ClientToScreenScaled(b,right,bottom);
    const Fvector2 corners[6]={{a.x,a.y},{b.x,a.y},{b.x,b.y},{a.x,a.y},{b.x,b.y},{a.x,b.y}};
    for (const Fvector2& point:corners)
        UIRender->PushPoint(iFloor(point.x)-0.5f,iFloor(point.y)-0.5f,0,color,0.f,0.f);
}
bool Pointing(int hand)
{
    const HandPointer& pointer=g_hands[hand];
    return Device.dwFrame-pointer.frame<3 && pointer.cursor.x>=0.f && pointer.cursor.y>=0.f;
}
}

void VrKeyboardDraw(CDialogHolder& holder)
{
    if (!FocusedEdit(holder)) { g_shift=false; return; }
    // Opaque: dark keys on a darker plate, so the window behind does not show
    // through the labels. Each hand's key is lit in its colour (left blue,
    // right amber), brighter while its trigger is held; Shift on is green.
    static ui_shader shader;
    static bool created=false;
    if (!created) { shader->create("hud\\crosshair"); created=true; }
    const u32 hand_color[2]={0xFF3A6EA8,0xFFB4782A}, pressed_color[2]={0xFF5C9AE0,0xFFE8A040};
    const Key* hovered[2]={nullptr,nullptr};
    for (int h=0;h<2;++h) if (Pointing(h)) hovered[h]=KeyAt(g_hands[h].cursor);
    const Frect area=Area();
    UIRender->StartPrimitive(u32(Keys().size()+1)*6,IUIRender::ptTriList,UI().m_currentPointType);
    PushRect(area.x1,area.y1,area.x2,area.y2,0xFF0E1013);
    for (const Key& key:Keys())
    {
        u32 color=key.dik==DIK_LSHIFT && g_shift ? 0xFF3C8A4C : 0xFF2A2E34;
        for (int h=0;h<2;++h)
            if (&key==hovered[h]) color=g_hands[h].down ? pressed_color[h] : hand_color[h];
        PushRect(key.x,key.y,key.x+key.w,key.y+key_height,color);
    }
    UIRender->SetShader(*shader);
    UIRender->FlushPrimitive();
    CGameFont* font=UI().Font().pFontLetterica25 ? UI().Font().pFontLetterica25 : UI().Font().pFontLetterica18Russian;
    if (font)
    {
        font->SetAligment(CGameFont::alCenter);
        font->SetColor(0xFFFFFFFF);
        const float half=0.5f*font->CurrentHeight_();
        for (const Key& key:Keys())
        {
            Fvector2 point;
            UI().ClientToScreenScaled(point,key.x+0.5f*key.w,key.y+0.5f*key_height);
            string16 label;
            xr_strcpy(label,key.label);
            if (g_shift && !label[1] && label[0]>='a' && label[0]<='z') label[0]=char(label[0]-'a'+'A');
            if (g_shift && !xr_strcmp(label,"-")) xr_strcpy(label,"_");
            font->Out(point.x,point.y-half,"%s",label);
        }
        font->OnRender();
    }
    // Each hand's pointer over the keys: a dot in its colour, outlined.
    if (!Pointing(0) && !Pointing(1)) return;
    UIRender->StartPrimitive(4*6,IUIRender::ptTriList,UI().m_currentPointType);
    for (int h=0;h<2;++h)
    {
        if (!Pointing(h)) continue;
        const Fvector2& c=g_hands[h].cursor;
        PushRect(c.x-7.f,c.y-7.f,c.x+7.f,c.y+7.f,0xFF000000);
        PushRect(c.x-5.f,c.y-5.f,c.x+5.f,c.y+5.f,pressed_color[h]);
    }
    UIRender->SetShader(*shader);
    UIRender->FlushPrimitive();
}
