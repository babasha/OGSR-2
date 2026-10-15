#include "StdAfx.h"
#include "UIKeyBinding.h"
#include "UIXmlInit.h"
#include "xrUIXmlParser.h"
#include "UIEditKeyBind.h"
#include "UIScrollView.h"
#include "../xr_level_controller.h"
#include "../string_table.h"

CUIKeyBinding::CUIKeyBinding()
{
    for (int i = 0; i < (Core.Features.test(xrCore::Feature::remove_alt_keybinding) ? 2 : 3); i++)
        AttachChild(&m_header[i]);

    AttachChild(&m_frame);
}

void CUIKeyBinding::InitFromXml(CUIXml& xml_doc, LPCSTR path)
{
    CUIXmlInit::InitWindow(xml_doc, path, 0, this);
    string256 buf;
    m_scroll_wnd = xr_new<CUIScrollView>();
    m_scroll_wnd->SetAutoDelete(true);
    AttachChild(m_scroll_wnd);
    CUIXmlInit::InitScrollView(xml_doc, strconcat(sizeof(buf), buf, path, ":scroll_view"), 0, m_scroll_wnd);

    CUIXmlInit::InitFrameWindow(xml_doc, strconcat(sizeof(buf), buf, path, ":frame"), 0, &m_frame);
    CUIXmlInit::InitLabel(xml_doc, strconcat(sizeof(buf), buf, path, ":header_1"), 0, &m_header[0]);
    CUIXmlInit::InitLabel(xml_doc, strconcat(sizeof(buf), buf, path, ":header_2"), 0, &m_header[1]);
    if (!Core.Features.test(xrCore::Feature::remove_alt_keybinding))
        CUIXmlInit::InitLabel(xml_doc, strconcat(sizeof(buf), buf, path, ":header_3"), 0, &m_header[2]);

    FillUpList(xml_doc, path);
}

void CUIKeyBinding::FillUpList(CUIXml& xml_doc_ui, LPCSTR path_ui)
{
    string256 buf;
    CUIXml xml_doc;
    CStringTable st;
    xml_doc.Init(CONFIG_PATH, UI_PATH, "ui_keybinding.xml");

    int groupsCount = xml_doc.GetNodesNum("", 0, "group");

    for (int i = 0; i < groupsCount; i++)
    {
        // add group
        shared_str grp_name = xml_doc.ReadAttrib("group", i, "name");
        R_ASSERT(xr_strlen(grp_name));

        CUIStatic* pItem = xr_new<CUIStatic>();
        CUIXmlInit::InitStatic(xml_doc_ui, strconcat(sizeof(buf), buf, path_ui, ":scroll_view:item_group"), 0, pItem);
        pItem->SetTextST(grp_name.c_str());
        m_scroll_wnd->AddWindow(pItem, true);

        // add group items
        int commandsCount = xml_doc.GetNodesNum("group", i, "command");
        XML_NODE* tab_node = xml_doc.NavigateToNode("group", i);
        xml_doc.SetLocalRoot(tab_node);

        for (int j = 0; j < commandsCount; j++)
        {
            // first field of list item
            shared_str command_id = xml_doc.ReadAttrib("command", j, "id");

            pItem = xr_new<CUIStatic>();
            CUIXmlInit::InitStatic(xml_doc_ui, strconcat(sizeof(buf), buf, path_ui, ":scroll_view:item_key"), 0, pItem);
            pItem->SetTextST(command_id.c_str());
            m_scroll_wnd->AddWindow(pItem, true);

            shared_str exe = xml_doc.ReadAttrib("command", j, "exe");

            float item_width = m_header[1].GetWidth() - 3.0f;
            float item_pos = m_header[1].GetWndPos().x;
            CUIEditKeyBind* pEditKB = xr_new<CUIEditKeyBind>(true);
            pEditKB->SetAutoDelete(true);
            pEditKB->Init(item_pos, 0, item_width, pItem->GetWndSize().y);
            pEditKB->Register(*exe, "key_binding");
            pItem->AttachChild(pEditKB);

            if (!Core.Features.test(xrCore::Feature::remove_alt_keybinding))
            {
                item_width = m_header[2].GetWidth() - 3.0f;
                item_pos = m_header[2].GetWndPos().x;
                pEditKB = xr_new<CUIEditKeyBind>(false);
                pEditKB->SetAutoDelete(true);
                pEditKB->Init(item_pos, 0, item_width, pItem->GetWndSize().y);
                pEditKB->Register(*exe, "key_binding");
                pItem->AttachChild(pEditKB);
            }
        }
        xml_doc.SetLocalRoot(xml_doc.GetRoot());
    }
}

