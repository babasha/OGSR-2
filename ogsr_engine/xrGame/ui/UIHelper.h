////////////////////////////////////////////////////////////////////////////
//	Module 		: UIHelper.h
//	Created 	: 17.01.2008
//	Author		: Evgeniy Sokolov
//	Description : UI Helper class
////////////////////////////////////////////////////////////////////////////

#pragma once

class CUIXml;
class CUIWindow;
class CUIStatic;
class CUIScrollView;
class CUIProgressBar;
class CUIFrameLineWnd;
class CUIFrameWindow;
class CUI3tButton;
class CUICheckButton;
class CUIEditBox;

class UIHelper
{
public:
    UIHelper() = default;
    ~UIHelper() = default;

    static CUIStatic* CreateStatic(CUIXml& xml, LPCSTR ui_path, CUIWindow* parent, bool critical = true);
    static CUIProgressBar* CreateProgressBar(CUIXml& xml, LPCSTR ui_path, CUIWindow* parent, bool critical = true);

};
