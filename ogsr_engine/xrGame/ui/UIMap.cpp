#include "StdAfx.h"
#include "../Level.h"
#include "../map_location.h"
#include "../map_manager.h"
#include "../map_spot.h"
#include "UIMap.h"

CUICustomMap::CUICustomMap()
{
    m_BoundRect.set(0, 0, 0, 0);
    SetWindowName("map");
    m_flags.zero();
    SetPointerDistance(0.0f);
}

CUICustomMap::~CUICustomMap() {}

void CUICustomMap::Update()
{
    SetPointerDistance(0.0f);
    if (!Locked())
        UpdateSpots();

    CUIStatic::Update();
}

void CUICustomMap::Init(shared_str name, CInifile& gameLtx, LPCSTR sh_name)
{
    m_name = name;
    LPCSTR tex;
    Fvector4 tmp;
    if (gameLtx.line_exist(m_name, "texture"))
    {
        tex = gameLtx.r_string(m_name, "texture");
        tmp = gameLtx.r_fvector4(m_name, "bound_rect");
    }
    else
    {
        tex = "ui\\ui_nomap2";
        tmp.set(-10000.0f, -10000.0f, 10000.0f, 10000.0f);
    }
    if (!Heading())
    {
        tmp.x *= UI()->get_current_kx();
        tmp.z *= UI()->get_current_kx();
    }
    m_BoundRect.set(tmp.x, tmp.y, tmp.z, tmp.w);
    CUIStatic::InitEx(tex, sh_name, 0, 0, m_BoundRect.width(), m_BoundRect.height());

    SetStretchTexture(true);
    ClipperOn();

    SetWindowName(m_name.c_str());
}

void RotateCoord(float x, float y, const float angle, float& x_, float& y_, float _kx)
{
    const float _sc = _cos(angle);
    const float _sn = _sin(angle);

    x_ = x * _sc + y * _sn;
    y_ = y * _sc - x * _sn;

    x_ *= _kx;
}

Fvector2 CUICustomMap::ConvertLocalToReal(const Fvector2& src) const
{
    Fvector2 res;

    res.x = m_BoundRect.lt.x + src.x / GetCurrentZoom();
    res.y = m_BoundRect.height() + m_BoundRect.lt.y - src.y / GetCurrentZoom();

    return res;
}

Fvector2 CUICustomMap::ConvertRealToLocal(const Fvector2& src, bool for_drawing) // meters->pixels (relatively own left-top pos)
{
    Fvector2 res;
    if (!Heading())
    {
        Frect bound_rect = BoundRect();
        bound_rect.x1 /= UI()->get_current_kx();
        bound_rect.x2 /= UI()->get_current_kx();
        res = ConvertRealToLocalNoTransform(src, bound_rect);
        res.x *= UI()->get_current_kx();
    }
    else
    {
        const Fvector2 heading_pivot = GetStaticItem()->GetHeadingPivot();

        res = ConvertRealToLocalNoTransform(src);
        res.sub(heading_pivot);

        RotateCoord(res.x, res.y, GetHeading(), res.x, res.y, for_drawing ? UI()->get_current_kx() : 1.0f);

        res.add(heading_pivot);
    };
    return res;
}

Fvector2 CUICustomMap::ConvertRealToLocalNoTransform(const Fvector2& src, Frect const& bound_rect) const
// meters->pixels (relatively own left-top pos)
{
    Fvector2 res;
    res.x = (src.x - bound_rect.lt.x) * GetCurrentZoom();
    res.y = (bound_rect.height() - (src.y - bound_rect.lt.y)) * GetCurrentZoom();

    return res;
}

Fvector2 CUICustomMap::ConvertRealToLocalNoTransform(const Fvector2& src) const
// meters->pixels (relatively own left-top pos)
{
    Fvector2 res;
    res.x = (src.x - m_BoundRect.lt.x) * GetCurrentZoom();
    res.y = (m_BoundRect.height() - (src.y - m_BoundRect.lt.y)) * GetCurrentZoom();

    return res;
}

// position and heading for drawing pointer to src pos
bool CUICustomMap::GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading)
{
    const Frect clip_rect_abs = GetClipperRect(); // absolute rect coords
    Frect map_rect_abs;
    GetAbsoluteRect(map_rect_abs);

    Frect rect;
    BOOL res = rect.intersection(clip_rect_abs, map_rect_abs);
    if (!res)
        return false;

    rect = clip_rect_abs;
    rect.sub(map_rect_abs.lt.x, map_rect_abs.lt.y);

    Fbox2 f_clip_rect_local;
    f_clip_rect_local.set(rect.x1, rect.y1, rect.x2, rect.y2);

    Fvector2 f_center;
    f_clip_rect_local.getcenter(f_center);

    Fvector2 f_dir, f_src;

    const auto src_local = ConvertRealToLocal(src, true);

    f_src.set(src_local.x, src_local.y);
    f_dir.sub(f_center, f_src);
    f_dir.normalize_safe();

    Fvector2 f_intersect_point;
    res = f_clip_rect_local.Pick2(f_src, f_dir, f_intersect_point);
    VERIFY(res);

    heading = -f_dir.getH();

    f_intersect_point.mad(f_intersect_point, f_dir, item_radius);

    pos.set(iFloor(f_intersect_point.x), iFloor(f_intersect_point.y));
    return true;
}

void CUICustomMap::FitToWidth(float width)
{
    const float k = m_BoundRect.width() / m_BoundRect.height();
    const float w = width;
    const float h = width / k;
    SetWndRect(0.0f, 0.0f, w, h);
}

void CUICustomMap::FitToHeight(float height)
{
    const float k = m_BoundRect.width() / m_BoundRect.height();
    const float h = height;
    const float w = k * height;
    SetWndRect(0.0f, 0.0f, w, h);
}

void CUICustomMap::OptimalFit(const Frect& r)
{
    if ((m_BoundRect.height() / r.height()) < (m_BoundRect.width() / r.width()))
        FitToHeight(r.height());
    else
        FitToWidth(r.width());
}

// try to positioning clipRect center to vNewPoint
void CUICustomMap::SetActivePoint(const Fvector& vNewPoint)
{
    Fvector2 pos;
    pos.set(vNewPoint.x, vNewPoint.z);
    const Frect bound = BoundRect();
    if (FALSE == bound.in(pos))
        return;

    const Fvector2 pos_on_map = ConvertRealToLocalNoTransform(pos);
    Frect map_abs_rect;
    GetAbsoluteRect(map_abs_rect);
    Fvector2 pos_abs;

    pos_abs.set(map_abs_rect.lt);
    pos_abs.add(pos_on_map);

    const Frect clip_abs_rect = GetClipperRect();
    Fvector2 clip_center;
    clip_abs_rect.getcenter(clip_center);
    clip_center.sub(pos_abs);
    MoveWndDelta(clip_center);
    SetHeadingPivot(pos_on_map, Fvector2().set(0, 0), false);
}

bool CUICustomMap::IsRectVisible(Frect r)
{
    Fvector2 pos;
    GetAbsolutePos(pos);
    r.add(pos.x, pos.y);

    const Frect map_visible_rect = GetClipperRect();
    return !!map_visible_rect.intersected(r);
}

bool CUICustomMap::NeedShowPointer(Frect r)
{
    Fvector2 pos;
    GetAbsolutePos(pos);
    r.add(pos.x, pos.y);

    Frect map_visible_rect = GetClipperRect();
    map_visible_rect.shrink(5, 5);
    return !map_visible_rect.intersected(r);
}

void CUICustomMap::SendMessage(CUIWindow* pWnd, s16 msg, void* pData) { CUIWndCallback::OnEvent(pWnd, msg, pData); }

CUIMiniMap::CUIMiniMap() {}

CUIMiniMap::~CUIMiniMap() {}

void CUIMiniMap::Init(shared_str name, CInifile& gameLtx, LPCSTR sh_name)
{
    inherited::Init(name, gameLtx, sh_name);

    CUIStatic::SetColor(color_argb(127, 255, 255, 255));
}

void CUIMiniMap::UpdateSpots()
{
    DetachAll();

    Locations& ls = Level().MapManager().Locations();
    for (auto& l : ls)
    {
        l.location->UpdateMiniMap(this);
    }

    for (auto& it : m_ChildWndList)
    {
        it->SetCustomDraw(false);
    }
}

void CUIMiniMap::Draw()
{
    if (!IsRounded())
    {
        inherited::Draw();
        return;
    }

    const u32 segments_count = m_round_fill ? 48 : 20; // 20 reads as a polygon at radar size

    UIRender->SetShader(*m_UIStaticItem.GetShader());
    UIRender->StartPrimitive(segments_count * 3, IUIRender::ptTriList, UI()->m_currentPointType);

    const u32 color = m_UIStaticItem.GetTextureColor();
    const float angle = GetHeading();

    const float kx = UI()->get_current_kx();

    // clip poly
    sPoly2D S;
    S.resize(segments_count);
    const float segment_ang = PI_MUL_2 / segments_count;
    const float pt_radius = RoundRadius();
    Fvector2 center;
    WorkingArea().getcenter(center);

    const float tt_radius = pt_radius / GetWidth();
    const float k_tt_height = GetWidth() / GetHeight();

    Fvector2 tt_offset;
    tt_offset.set(m_UIStaticItem.GetHeadingPivot());
    tt_offset.x /= GetWidth();
    tt_offset.y /= GetHeight();

    Fvector2 m_scale_;
    m_scale_.set(float(Device.dwWidth) / UI_BASE_WIDTH, float(Device.dwHeight) / UI_BASE_HEIGHT);

    for (u32 idx = 0; idx < segments_count; ++idx)
    {
        const float cosPT = _cos(segment_ang * idx + angle);
        const float sinPT = _sin(segment_ang * idx + angle);

        const float cosTX = _cos(segment_ang * idx);
        const float sinTX = _sin(segment_ang * idx);

        S[idx].pt.set(pt_radius * cosPT * kx, -pt_radius * sinPT);
        S[idx].uv.set(tt_radius * cosTX, -tt_radius * sinTX * k_tt_height);
        S[idx].uv.add(tt_offset);
        S[idx].pt.add(center);

        S[idx].pt.x *= m_scale_.x;
        S[idx].pt.y *= m_scale_.y;
    }

    for (u32 idx = 0; idx < segments_count - 2; ++idx)
    {
        UIRender->PushPoint(S[0 + 0].pt.x, S[0 + 0].pt.y, 0, color, S[0 + 0].uv.x, S[0 + 0].uv.y);
        UIRender->PushPoint(S[idx + 2].pt.x, S[idx + 2].pt.y, 0, color, S[idx + 2].uv.x, S[idx + 2].uv.y);
        UIRender->PushPoint(S[idx + 1].pt.x, S[idx + 1].pt.y, 0, color, S[idx + 1].uv.x, S[idx + 1].uv.y);
    }

    UIRender->FlushPrimitive();

    //------------
    CUIWindow::Draw(); // draw childs
}

bool CUIMiniMap::GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading)
{
    const auto src_local_calc = ConvertRealToLocal(src, false);

    bool is_visible = false;

    // в ЗП для круглой карты по умолчанию указатель всегда показывает вектор направление
    // этот костыль позволяет его рисовать уже на точке если она видна
    if (IsRounded())
    {
        Frect r;
        r.set(src_local_calc.x, src_local_calc.y, src_local_calc.x + 1, src_local_calc.y + 1);

        is_visible = IsRectVisible(r);
    }

    if (!IsRounded() || is_visible)
    {
        return inherited::GetPointerTo(src, item_radius, pos, heading);
    }

    const Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();
    const float map_radius = RoundRadius();
    Fvector2 direction;

    direction.sub(clip_center, src_local_calc);
    heading = -direction.getH();

    const float kx = UI()->get_current_kx();
    const float cosPT = _cos(heading);
    const float sinPT = _sin(heading);
    pos.set(-map_radius * sinPT * kx, -map_radius * cosPT);
    pos.add(clip_center);

    return true;
}

bool CUIMiniMap::NeedShowPointer(Frect r)
{
    if (!IsRounded())
    {
        return inherited::NeedShowPointer(r);
    }
    const Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();

    Fvector2 spot_pos;
    r.getcenter(spot_pos);

    const float dist = RoundDist(clip_center, spot_pos);
    const float spot_radius = (m_round_fill ? r.height() : r.width()) / 2.0f;

    return dist + spot_radius > RoundRadius();
}

bool CUIMiniMap::IsRectVisible(Frect r)
{
    if (!IsRounded())
    {
        return inherited::IsRectVisible(r);
    }
    const Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();

    Fvector2 rect_center;
    r.getcenter(rect_center);

    const float dist = RoundDist(clip_center, rect_center);
    const float spot_radius = (m_round_fill ? r.height() : r.width()) / 2.0f;

    return dist + spot_radius < RoundRadius(); // assume that all minimap spots are circular
}
float CUIMiniMap::RoundRadius() { return (m_round_fill ? WorkingArea().height() : WorkingArea().width()) / 2.0f; }

// A distance on screen, in UI y units: the x units are kx times narrower on a wide screen.
float CUIMiniMap::RoundDist(const Fvector2& a, const Fvector2& b) const
{
    if (!m_round_fill)
        return a.distance_to(b);
    const float kx = UI()->get_current_kx();
    const float dx = (a.x - b.x) / kx, dy = a.y - b.y;
    return _sqrt(dx * dx + dy * dy);
}
