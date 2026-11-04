// Vector controller glyphs: independent of fonts, textures and platform assets.
#pragma once
#include "imgui.h"
namespace SettingsIcons {
enum { Confirm, Back, Save, Undo, Left, Right, Dpad };
inline void draw(ImDrawList *d, ImVec2 p, float size, int layout, int action, int physical) {
    const ImU32 ink = IM_COL32(242,246,255,255), edge = IM_COL32(160,184,211,255);
    ImVec2 c(p.x + size*.5f, p.y + size*.5f);
    if (action == Dpad) {
        d->AddRectFilled(ImVec2(c.x-size*.38f,c.y-size*.12f),ImVec2(c.x+size*.38f,c.y+size*.12f),ink,1);
        d->AddRectFilled(ImVec2(c.x-size*.12f,c.y-size*.38f),ImVec2(c.x+size*.12f,c.y+size*.38f),ink,1);
        return;
    }
    if (action == Left || action == Right) {
        const char *name = layout == 0 ? (action == Left ? "LB" : "RB") :
                           layout == 1 ? (action == Left ? "L" : "R") : (action == Left ? "L1" : "R1");
        d->AddRect(p, ImVec2(p.x+size*1.5f,p.y+size), edge, size*.2f, 1.5f, 0);
        ImVec2 t=ImGui::CalcTextSize(name);
        d->AddText(ImVec2(p.x+(size*1.5f-t.x)*.5f,c.y-t.y*.5f), ink, name);
        return;
    }
    d->AddCircleFilled(c, size*.47f, IM_COL32(23,34,48,255));
    d->AddCircle(c, size*.47f, edge, 0, 1.5f);
    if (layout != 2) {
        const char *namesXbox[] = {"A","B","X","Y"}, *namesSwitch[] = {"B","A","Y","X"};
        const char *name = layout == 1 ? namesSwitch[physical] : namesXbox[physical];
        ImVec2 t=ImGui::CalcTextSize(name);
        d->AddText(ImVec2(c.x-t.x*.5f,c.y-t.y*.5f), ink, name);
    } else {
        float r=size*.23f;
        if (physical == 0) {
            d->AddLine(ImVec2(c.x-r,c.y-r),ImVec2(c.x+r,c.y+r),IM_COL32(125,198,255,255),2);
            d->AddLine(ImVec2(c.x-r,c.y+r),ImVec2(c.x+r,c.y-r),IM_COL32(125,198,255,255),2);
        } else if (physical == 1) d->AddCircle(c,r,IM_COL32(255,157,158,255),0,2);
        else if (physical == 2) d->AddRect(ImVec2(c.x-r,c.y-r),ImVec2(c.x+r,c.y+r),IM_COL32(244,166,230,255),0.0f,2.0f,0);
        else d->AddTriangle(ImVec2(c.x,c.y-r),ImVec2(c.x+r,c.y+r),ImVec2(c.x-r,c.y+r),IM_COL32(139,235,202,255),2);
    }
}
}
