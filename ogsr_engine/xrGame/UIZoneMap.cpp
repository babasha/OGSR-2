#include "StdAfx.h"
#include "UIZoneMap.h"

#include "HUDManager.h"

#include "InfoPortion.h"
#include "PDA.h"

#include "Grenade.h"
#include "Level.h"
#include "game_cl_base.h"

#include "Actor.h"
#include "ai_space.h"
#include "game_graph.h"

#include "ui/UIMap.h"
#include "ui/UIXmlInit.h"
#include "ui/UIInventoryUtilities.h"

CUIZoneMap::CUIZoneMap()
{
    m_background = xr_new<CUIStatic>();
    m_pointerDistanceText = xr_new<CUIStatic>();
    m_clock_wnd = xr_new<CUIStatic>();
    m_clipFrame = xr_new<CUIStatic>();
    m_center = xr_new<CUIStatic>();
    m_compass = xr_new<CUIStatic>();
    m_activeMap = xr_new<CUIMiniMap>();
}

CUIZoneMap::~CUIZoneMap()
{
    xr_delete(m_center);
    xr_delete(m_clipFrame);
    xr_delete(m_background);
    xr_delete(m_compass);
}

void CUIZoneMap::Init()
{
    CUIXml uiXml;
    const bool xml_result = uiXml.Init(CONFIG_PATH, UI_PATH, "zone_map.xml");
    R_ASSERT(xml_result, "xml file not found", "zone_map.xml");

    // load map background
    CUIXmlInit xml_init;

    xml_init.InitStatic(uiXml, "minimap:background", 0, m_background);
    // m_background->SetAutoDelete(true);

    xml_init.InitStatic(uiXml, "minimap:background:dist_text", 0, m_pointerDistanceText);
    m_background->AttachChild(m_pointerDistanceText);
    m_pointerDistanceText->SetAutoDelete(true);

    if (uiXml.NavigateToNode("minimap:clock_wnd", 0))
    {
        xml_init.InitStatic(uiXml, "minimap:clock_wnd", 0, m_clock_wnd);
        m_background->AttachChild(m_clock_wnd);
        m_clock_wnd->SetAutoDelete(true);
    }

    xml_init.InitStatic(uiXml, "minimap:level_frame", 0, m_clipFrame);
    m_xmlFrame = m_clipFrame->GetWndRect();
    // m_clipFrame->SetAutoDelete(true);

    xml_init.InitStatic(uiXml, "minimap:center", 0, m_center);

    m_rounded = uiXml.ReadAttribInt("minimap", 0, "rounded", 0) == 1;
    m_alpha = uiXml.ReadAttribInt("minimap", 0, "alpha", 127);

    m_clipFrame->AttachChild(m_activeMap);

    m_activeMap->SetAutoDelete(true);
    m_activeMap->EnableHeading(true);
    m_activeMap->SetWindowName("minimap");
    m_activeMap->SetRounded(m_rounded);

    xml_init.InitStatic(uiXml, "minimap:compass", 0, m_compass);
    // m_compass->SetAutoDelete(true);

    m_activeMap->SetTextureColor(color_argb(m_alpha, 255, 255, 255));

    //	m_background.AttachChild(&m_compass);

    m_clipFrame->AttachChild(m_center);
    m_center->SetWndPos(m_clipFrame->GetWidth() / 2, m_clipFrame->GetHeight() / 2);
    // m_center->SetAutoDelete(true);
    m_fScale = 1.f;
}

void CUIZoneMap::Render()
{
    m_clipFrame->Draw();
    m_background->Draw();
    m_compass->Draw();
}

void CUIZoneMap::SetViewRect(const Frect& r)
{
    const Frect& cur = m_clipFrame->GetWndRect();
    if (fsimilar(cur.x1, r.x1, 0.5f) && fsimilar(cur.y1, r.y1, 0.5f) && fsimilar(cur.x2, r.x2, 0.5f) && fsimilar(cur.y2, r.y2, 0.5f))
        return;

    // The same three things SetupCurrentMap derives from the frame: the clip and
    // working area (absolute -- the frame has no parent, so that is its own rect),
    // the centre marker, and the zoom, which is relative to the frame's width.
    m_clipFrame->SetWndRect(r);
    m_center->SetWndPos(r.width() / 2, r.height() / 2);

    Frect abs;
    m_clipFrame->GetAbsoluteRect(abs);
    m_activeMap->SetClipRect(abs);
    m_activeMap->WorkingArea().set(abs);
    ApplyZoom();
}

void CUIZoneMap::SetRoundView(bool round)
{
    // Every frame from CUIMainIngameWnd::Draw: SetupCurrentMap makes a fresh map on
    // each level with zone_map.xml's own rounded flag, and this puts ours back.
    auto* mm = static_cast<CUIMiniMap*>(m_activeMap);
    const bool want = round || m_rounded;
    if (mm->IsRounded() != want)
        mm->SetRounded(want);
    mm->SetRoundFill(round);
    // Under the radar ring the map is a picture of its own, not a tint over the
    // world: denser than zone_map.xml's alpha.
    mm->SetTextureColor(color_argb(round ? _max(m_alpha, 215u) : m_alpha, 255, 255, 255));
}

void CUIZoneMap::SetHeading(float angle) const
{
    m_activeMap->SetHeading(angle);
    m_compass->SetHeading(angle);
};

void CUIZoneMap::UpdateRadar(Fvector pos) const
{
    m_clipFrame->Update();
    m_background->Update();
    m_activeMap->SetActivePoint(pos);

    if (m_activeMap->GetPointerDistance() > 0.5f)
    {
        string64 str;
        sprintf_s(str, "%.1f m.", m_activeMap->GetPointerDistance());
        m_pointerDistanceText->SetText(str);
    }
    else
    {
        m_pointerDistanceText->SetText("");
    }

    if (m_clock_wnd)
    {
        m_clock_wnd->SetText(InventoryUtilities::GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str());
    }
}

bool CUIZoneMap::ZoomIn()
{
    m_fScale = m_fScale + m_fScale * 0.25f;
    clamp(m_fScale, 0.5f, 2.f);
    ApplyZoom();

    return true;
}

bool CUIZoneMap::ZoomOut()
{
    m_fScale = m_fScale - m_fScale * 0.25f;
    clamp(m_fScale, 0.5f, 2.f);
    ApplyZoom();

    return true;
}

void CUIZoneMap::SetupCurrentMap()
{
    CInifile* pLtx = pGameIni;

    R_ASSERT(pLtx->section_exist(Level().name()));

    // dsh: очередной костыль. Если не создавать новый CUIMiniMap, то после
    // перехода с локации, на которой нет текстуры миникарты, на локацию,
    // где эта текстура есть (например, из X-10 на Радар), миникарта
    // перестает показываться.
    m_clipFrame->DetachChild(m_activeMap);
    m_clipFrame->DetachChild(m_center);

    m_activeMap = xr_new<CUIMiniMap>();
    m_clipFrame->AttachChild(m_activeMap);

    m_activeMap->SetAutoDelete(true);
    m_activeMap->EnableHeading(true);
    m_activeMap->SetRounded(m_rounded);

    m_activeMap->Init(Level().name(), *pLtx, "hud\\default");
    m_activeMap->SetWindowName("minimap"); // имя нужно задавать позже чем Init

    m_activeMap->SetTextureColor(color_argb(m_alpha, 255, 255, 255));

    m_clipFrame->AttachChild(m_center);

    Frect r;
    m_clipFrame->GetAbsoluteRect(r);
    m_activeMap->SetClipRect(r);
    m_activeMap->WorkingArea().set(r);

    ApplyZoom();
}

void CUIZoneMap::ApplyZoom() const
{
    Fvector2 wnd_size;
    // ⚠From the frame's HEIGHT, scaled by zone_map.xml's own aspect -- identical
    // to the old width/100 for the XML box. The width is not a measure of size
    // once the RmlUi HUD sets the box: UI units are stretched horizontally on a
    // wide screen, so a square box on screen is narrow in UI units and the map
    // came out zoomed a third further out than the legacy HUD showed it.
    const Frect& f = m_clipFrame->GetWndRect();
    const float xml_aspect = m_xmlFrame.height() > 0.f ? m_xmlFrame.width() / m_xmlFrame.height() : 1.f;
    const float zoom_factor = f.height() * xml_aspect / 100.0f;
    wnd_size.x = m_activeMap->BoundRect().width() * zoom_factor * m_fScale;
    wnd_size.y = m_activeMap->BoundRect().height() * zoom_factor * m_fScale;
    m_activeMap->SetWndSize(wnd_size);
}