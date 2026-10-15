#pragma once
#include "UIStatic.h"
#include "UIWndCallback.h"

// 13-10-2026: CUIGlobalMap and CUILevelMap (the legacy PDA map's pictures) went with CUIMapWnd; the PDA map is the
// RmlUi screen's (ui_rml_pda.cpp). What is left is the HUD minimap's.
class CUICustomMap : public CUIStatic, public CUIWndCallback
{
protected:
    shared_str m_name{};
    Frect m_BoundRect{}; // real map size (meters)

    enum EFlags
    {
        eLocked = (1 << 0),
        eRounded = (1 << 1)
    };
    Flags16 m_flags{};

    float m_pointer_dist{};

    Frect m_workingArea{};

public:
    Frect& WorkingArea() { return m_workingArea; }

    CUICustomMap();
    virtual ~CUICustomMap();

    virtual void SetActivePoint(const Fvector& vNewPoint);

    virtual void Init(shared_str name, CInifile& gameLtx, LPCSTR sh_name);

    virtual Fvector2 ConvertRealToLocal(const Fvector2& src, bool for_drawing); // meters->pixels (relatively own left-top pos)
    Fvector2 ConvertLocalToReal(const Fvector2& src) const;
    Fvector2 ConvertRealToLocalNoTransform(const Fvector2& src) const; // meters->pixels (relatively own left-top pos)
    Fvector2 ConvertRealToLocalNoTransform(const Fvector2& src, Frect const& bound_rect) const;

    virtual bool GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading); // position and heading for drawing pointer to src pos

    void FitToWidth(float width);
    void FitToHeight(float height);

    float GetCurrentZoom() const { return GetWndRect().width() / m_BoundRect.width(); }
    const Frect& BoundRect() const { return m_BoundRect; };
    virtual void OptimalFit(const Frect& r);

    shared_str MapName() { return m_name; }

    virtual void Update();
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData);

    virtual bool IsRectVisible(Frect r);
    virtual bool NeedShowPointer(Frect r);

    bool Locked() const { return !!m_flags.test(eLocked); }
    void SetLocked(bool b) { m_flags.set(eLocked, b); }

    bool IsRounded() const { return m_flags.test(eRounded); }
    void SetRounded(bool b) { m_flags.set(eRounded, b); }

    void SetPointerDistance(float d) { m_pointer_dist = d; };
    float GetPointerDistance() const { return m_pointer_dist; };

protected:
    virtual void UpdateSpots(){};
};

class CUIMiniMap : public CUICustomMap
{
    typedef CUICustomMap inherited;

public:
    CUIMiniMap();
    virtual ~CUIMiniMap();

    virtual void Init(shared_str name, CInifile& gameLtx, LPCSTR sh_name);

    virtual void Draw();
    virtual bool GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading); // position and heading for drawing pointer to src pos
    virtual bool NeedShowPointer(Frect r);
    virtual bool IsRectVisible(Frect r);

    // ⭐06-10, the RmlUi HUD's round radar: the circle fills the box on any aspect.
    // The ZP code takes the radius from the box WIDTH in UI units, which a wide
    // screen squeezes (1024 across any width), so on 16:9 the circle came out at 75%
    // of a square box; and it measured spot distances in those squeezed units too.
    // With this on, the radius is half the box HEIGHT and distances are corrected
    // by kx. Off (zone_map.xml rounded="1" of other mods): the ZP behaviour as it was.
    void SetRoundFill(bool b) { m_round_fill = b; }

protected:
    virtual void UpdateSpots();

private:
    bool m_round_fill{};
    float RoundRadius();
    float RoundDist(const Fvector2& a, const Fvector2& b) const;
};