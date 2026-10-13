#pragma once

#include "ui/UIStatic.h"

class CActor;
class CUICustomMap;
//////////////////////////////////////////////////////////////////////////

class CUIZoneMap
{
    CUICustomMap* m_activeMap;
    float m_fScale;

    CUIStatic* m_background{};
    CUIStatic* m_center{};
    CUIStatic* m_compass{};
    CUIStatic* m_clipFrame{};
    CUIStatic* m_pointerDistanceText{};
    CUIStatic* m_clock_wnd{};

    bool m_rounded{};
    u32 m_alpha{};
    Frect m_xmlFrame{}; // the level frame as zone_map.xml placed it

public:
    CUIZoneMap();
    virtual ~CUIZoneMap();

    void SetHeading(float angle) const;
    void Init();

    void Render();
    void UpdateRadar(Fvector pos) const;

    // ⭐For the RmlUi HUD (ui_rml_hud.cpp), which draws the frame, the compass and
    // the distance itself: only the map picture, into a box the document chose
    // (UI units, 1024x768). Moving the box re-clips and re-zooms the map.
    void SetViewRect(const Frect& r);
    // Back to zone_map.xml's box, for the legacy HUD after `ui_new_hud 0`.
    void RestoreViewRect()
    {
        SetRoundView(false);
        SetViewRect(m_xmlFrame);
    }
    // The document's box wants a round radar that fills it (hud.rml data-round, 06-10).
    void SetRoundView(bool round);
    void RenderMapOnly() { m_clipFrame->Draw(); }
    LPCSTR DistanceText() const { return m_pointerDistanceText->GetText(); }

    void SetScale(float s) { m_fScale = s; }
    float GetScale() const { return m_fScale; }

    bool ZoomIn();
    bool ZoomOut();

    void ApplyZoom() const;

    CUIStatic* Background() const { return m_background; };
    CUIStatic* ClipFrame() const { return m_clipFrame; }; // alpet: для экспорта в скрипты
    CUIStatic* Compass() const { return m_compass; }; // alpet: для экспорта в скрипты

    void SetupCurrentMap();
};