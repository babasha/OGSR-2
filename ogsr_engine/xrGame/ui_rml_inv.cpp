#include "StdAfx.h"
#include "ui_rml.h"

// The inventory on RmlUi: a port of CUIInventoryWnd (ui/UIInventoryWnd.cpp,
// uiinventorywnd2.cpp, UIInventoryWnd3.cpp) and the cell machinery under it
// (UIDragDropListEx, UICellItem, UICellCustomItems) onto HTML and CSS.
//
// ⭐WHY A THIRD FILE. ui_rml.cpp is the core (interfaces, context, cursor, the
// dev pickers) and ui_rml_menu.cpp is the SECOND HOST -- the menu, which runs
// when no level does. The inventory is neither: it hangs off the level like the
// pickers do (Update from Level.cpp, Render from CHUDManager::RenderUI, input
// from Level_input.cpp), but it is a game screen with real consequences rather
// than a dev tool, and it is as big as the menu was. Its own file.
//
// ⚠⚠THE UNIT HERE IS THE LEGACY ONE, NOT THE IN-GAME ONE. Every other in-game
// document on this track was authored against a 1080-tall reference, so the core
// puts the context in Space::InGame (dp = height/1080). inventory_new.xml is a
// legacy screen: its coordinates are X-Ray's own 1024x768 UI space, exactly like
// the menu's were. So while this document is up the context runs in Space::Menu
// (dp = height/768) and every number copied out of the XML lands where it did.
// ⚠One context, one ratio: a picker opened on top of the inventory would be laid
// out in the inventory's space. They are dev tools and this is a game screen;
// the trade is deliberate and shows only if someone opens both.
//
// ⭐WHAT LIVES WHERE. The screen -- which panels exist, where they sit, how big a
// cell is, how many cells a list holds -- is CONTENT: gamedata\config\ui_rml\
// inventory.rml/.rcss, as it was content in inventory_new.xml before. This file
// supplies only what markup cannot know: what the actor is actually carrying,
// which items fold into one cell, and what the engine must be told when
// something moves.
//
// ⭐THE LEGACY WINDOWS ARE GONE (03-10). CUIInventoryWnd, CUICarBodyWnd,
// CUITradeWnd and CUITalkWnd/CUITalkDialogWnd were removed once all four screens
// here had been played through; there is no gate and no fallback. Scripts reach
// these screens through the `rml` module (level_script.cpp) -- see "the script
// bridge" below.
//
// ⭐A PORT, NOT A VIEW (the owner's call, 03-10). Unlike the HUD, nothing here
// drove CUIInventoryWnd: every action -- to slot, to belt, to bag, drop, eat,
// attach and detach addons, unload -- is the legacy code carried over line for
// line (ui/uiinventorywnd2.cpp, ui/UIInventoryWnd3.cpp), working on items rather
// than on cells. What the legacy cells did for it is done here explicitly:
//   - the selected item is an object ID, resolved fresh every time it is used;
//   - a stack is a list of items, so "drop all" and "unload" reach every one;
//   - eCellItemSelect / eUIGroupItems are raised exactly where the cells raised
//     them, and "ui_inventory" / "ui_inventory_hide" are given to the actor --
//     this mod's scripts (bind_stalker, ogse_car_control, xr_conditions) wait
//     for those infos.
// ⚠The belt is trimmed to a new suit's artefact count by the inventory itself
// (CInventory::TrimBeltToOutfit, which was CUIInventoryWnd::UpdateOutfit); the
// one flag the legacy ToSlot set around a suit swap is set here
// (CInventory::KeepBeltOnce).
//
// ⛔Not here: a gamepad scheme for the Switch (next step, by the owner's order --
// the console drives this with the right-stick cursor for now), and the
// highlighting of related items (ColorizeItems).
#include "xr_platform_keys.h"

#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"

extern u32 g_actor_test_move; // Actor.cpp: mc* flags held by `ui_test move`
void EditorSetOpenForTest(bool open); // embedded_editor_main.cpp: `ui_test editor`
#include "entity_alive.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryOwner.h"
#include "Weapon.h"
#include "CustomOutfit.h"
#include "string_table.h"
#include "game_object_space.h" // eUIGroupItems -- the mod's veto on stacking
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "ui/UIIconParams.h"
#include "ui/UIInventoryUtilities.h"
#include "ui/UIWindow.h"
#include "WeaponAmmo.h"
#include "WeaponMagazined.h"
#include "WeaponMagazinedWGrenade.h"
#include "eatable_item.h"
#include "BottleItem.h"
#include "medkit.h"
#include "antirad.h"
#include "Artifact.h"
#include "Missile.h"
#include "CustomDetector.h"
#include "player_hud.h"
#include "xr_level_controller.h"
#include "../xr_3da/xr_input.h"
#include "InventoryBox.h"
#include "PDA.h"
#include "alife_registry_wrappers.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "HUDManager.h"
#include "UIGameSP.h"
#include "UI.h"
#include "UIGameCustom.h"
#include "trade.h"
#include "trade_parameters.h"
#include "ui/UIXmlInit.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIDiaryWnd.h"
#include "ui/UITextureMaster.h"
#include "ui/UIMessages.h"
#include "PhraseDialog.h"
#include "PhraseDialogManager.h"
#include "character_info.h"
#include "game_news.h"
#include "ai_space.h"
#include "script_engine.h"
#include "../xr_3da/CameraBase.h"
#include "../xr_3da/XR_IOConsole.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "game_graph.h"
#include "ai_object_location.h"
#include "ai/trader/ai_trader.h"
#include "../Include/xrRender/Kinematics.h"
#include <span>

// ui/UICharacterInfo.cpp: the ALife record of a character, for its portrait.
CSE_ALifeTraderAbstract* ch_info_get_from_id(u16 id);

// One item from one owner to another, as two network events (was in
// ui/UICarBodyWnd.cpp).
static void move_item(u16 from_id, u16 to_id, u16 what_id)
{
    NET_Packet P;
    CGameObject::u_EventGen(P, GE_TRANSFER_REJECT, from_id);
    P.w_u16(what_id);
    CGameObject::u_EventSend(P);

    // the other inventory takes it
    CGameObject::u_EventGen(P, GE_TRANSFER_TAKE, to_id);
    P.w_u16(what_id);
    CGameObject::u_EventSend(P);
}

// ⚠windowsx.h (through the precompiled header) defines these as one-argument
// macros over GetWindow(); Rml::Element declares methods with the same names and
// the macro turns the declaration into a comma expression. Same dance as the
// other two files.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>

namespace
{
using namespace UIRmlInternal;

Rml::ElementDocument* s_doc = nullptr;

// ⭐The unit of the open document. The legacy screens (search, trade, talk) are
// ports of 1024x768 layouts and run in Space::Menu; the inventory, restyled after
// True Stalker, asks for 2560x1440 with <body data-space="screen1440">. Read once
// per build, after the document is loaded.
Space s_doc_space = Space::Menu;

// ⭐THE GAMEPAD (Switch; the arrow keys and Enter do the same on a PC). The
// pointer is not replaced: a D-pad press SNAPS it onto the next cell, so hover,
// the description popup and the comparison all work exactly as with a mouse.
// While the pointer was put there by the pad (s_pad_mode) the arrow is not drawn
// -- the lit cell under it is the focus -- and a real mouse movement ends the
// mode. s_snapping marks our own CursorTo so it does not end it at once.
bool s_pad_mode = false;
bool s_snapping = false;
// What A opened: the item's action list, or (its "to a quick slot..." row) the
// choice of a quick slot.
enum class PadMenu
{
    None,
    List,
    Quick,
};
PadMenu s_pad_menu = PadMenu::None;

// ⚠Rebuilding is a FRAME's job, never a handler's. Everything that moves an item
// deletes the element the click is still being dispatched on -- the menu learned
// the general form of this and the picker paid a vanished panel for the narrow
// one. Handlers set this; the next frame does the work.
bool s_rebuild = false;

// The kind of list a cell sits in -- the same three the legacy window sorts by
// (EListType in UIDragDropListEx.h). What may go where is decided by kind, not
// by which element the pointer happened to hit.
enum class Kind
{
    Slot,
    Belt,
    Bag,
    Others, // the search screen: what the body or the box holds; trade: the partner's bag
    OurOffer, // trade: what the actor puts on the table
    TheirOffer, // trade: what the partner puts on the table
};

// One drawn cell: an item, whatever stacked into it, and where it sits.
struct Cell
{
    PIItem item{};
    xr_vector<PIItem> more; // the items folded in under it (the legacy cell's children)
    Ivector2 grid{}; // size in cells, from the item's inv_grid_width/height
    Ivector2 at{}; // top-left cell inside its list
};

// A list on screen and the state needed to fill it. Capacity and cell size come
// off the markup (see fill_list) because both are geometry.
struct List
{
    const char* id{}; // the element in inventory.rml
    Kind kind{Kind::Slot};
    u8 slot{NO_ACTIVE_SLOT}; // for Kind::Slot -- which one
    Ivector2 capacity{1, 1}; // in cells
    Ivector2 cell{INV_GRID_WIDTH, INV_GRID_HEIGHT}; // in legacy pixels
    bool group{}; // fold equal items into one cell (flGroupSimilar)
    bool fit{}; // one item filling the whole list (the legacy virtual_cells slots)
    bool unlimited{}; // grows downwards as far as it needs (the bag)
    bool one_cell{}; // every item takes ONE cell however big its grid is (True Stalker's bag)
    bool bg{}; // draw the empty cells too, behind the items
    float pad{}; // picture inset inside its cell, in the document's dp
    float gap{}; // space between neighbouring cells: each box shrinks by it on every side
    u32 available{u32(-1)}; // cells that exist at all -- the belt has as many as the suit allows
    xr_vector<Cell> cells;
};

// ⭐The lists, in the order the legacy window creates them. The slot ones are
// exactly CUIInventoryWnd::m_slots_array -- the map from slot id to a place on
// screen -- and the two containers are the belt and the bag.
//
// ⚠A missing element is not an error: a screen may legitimately leave a slot out
// (the mod's own XML does, for the ones behind ogse_new_slots). A list whose id
// is absent from the document is skipped.
List s_lists[] = {
    {"dd-pistol", Kind::Slot, FIRST_WEAPON_SLOT},
    {"dd-automatic", Kind::Slot, SECOND_WEAPON_SLOT},
    {"dd-knife", Kind::Slot, KNIFE_SLOT},
    {"dd-binocular", Kind::Slot, APPARATUS_SLOT},
    {"dd-detector", Kind::Slot, DETECTOR_SLOT},
    {"dd-torch", Kind::Slot, TORCH_SLOT},
    {"dd-helmet", Kind::Slot, HELMET_SLOT},
    {"dd-nv", Kind::Slot, NIGHT_VISION_SLOT},
    {"dd-biodet", Kind::Slot, BIODETECTOR_SLOT},
    {"dd-outfit", Kind::Slot, OUTFIT_SLOT},
    {"dd-belt", Kind::Belt, NO_ACTIVE_SLOT},
    {"dd-bag", Kind::Bag, NO_ACTIVE_SLOT},
};

// ⭐THE SEARCH SCREEN (CUICarBodyWnd) is the same machinery with two lists: what
// the actor carries and what the body or the box holds. Cells, stacks, the menu,
// dragging -- all shared; only the lists and the actions differ.
List s_cb_lists[] = {
    {"dd-our", Kind::Bag, NO_ACTIVE_SLOT},
    {"dd-other", Kind::Others, NO_ACTIVE_SLOT},
};

// ⭐THE TRADE SCREEN (CUITradeWnd): the two bags and the two "on the table"
// lists. Nothing changes hands until the deal is struck -- the table is only a
// selection here (s_tr_*_offer), exactly as the legacy lists were only cells.
List s_tr_lists[] = {
    {"dd-our-bag", Kind::Bag, NO_ACTIVE_SLOT},
    {"dd-our-offer", Kind::OurOffer, NO_ACTIVE_SLOT},
    {"dd-their-offer", Kind::TheirOffer, NO_ACTIVE_SLOT},
    {"dd-their-bag", Kind::Others, NO_ACTIVE_SLOT},
};

int s_pda_open_tab = -1; // the tab PdaOpen asked for, handed to PdaBuilt
bool s_pda_blocked = false; // PdaOpen blocked the slots (more_hide_weapon): the close unblocks

// Which screen the document is. One document at a time.
enum class Screen
{
    None,
    Inventory,
    CarBody,
    Trade,
    Talk,
    Pda, // the content is ui_rml_pda.cpp's
    Dialog, // a script's own window (rml.open): its markup, its data-onclick hooks
};
Screen s_screen = Screen::None;

std::span<List> lists()
{
    switch (s_screen)
    {
    case Screen::Inventory: return s_lists;
    case Screen::CarBody: return s_cb_lists;
    case Screen::Trade: return s_tr_lists;
    default: return {};
    }
}

// The trade: who, through which CTrade objects, and what lies on the table.
CInventoryOwner* s_tr_our = nullptr;
CInventoryOwner* s_tr_others = nullptr;
CInventory* s_tr_others_inv = nullptr;
CTrade* s_tr_trade = nullptr; // ours
CTrade* s_tr_others_trade = nullptr; // the partner's -- the one prices and deals go through
xr_set<u16> s_tr_our_offer, s_tr_their_offer;
u32 s_tr_our_stamp = 0, s_tr_other_stamp = 0;
u32 s_tr_our_price = 0, s_tr_their_price = 0;
SDrawStaticStruct* s_tr_deal_msg = nullptr; // "not enough money", for 2 s
// [trade] others_zero_trade in system.ltx -- the partner pays at most what it has.
bool s_tr_zero_trade = false;

// The trade actions live further down, after the shared machinery.
bool tr_can_move(PIItem it, bool ours);
void tr_update_prices();
float stolen_hours(PIItem it);
bool stolen_trader_takes(PIItem it);

// The search screen's other side: a body (an inventory owner) or a box.
CInventoryOwner* s_cb_others = nullptr;
IInventoryBox* s_cb_box = nullptr;
// Inventory change stamps seen at the last rebuild: items move by network
// events, so the lists change a frame or two AFTER the action that moved them.
u32 s_cb_our_stamp = 0, s_cb_other_stamp = 0;

CInventory* actor_inventory()
{
    auto owner = smart_cast<CInventoryOwner*>(Level().CurrentEntity());
    return owner ? &owner->inventory() : nullptr;
}

// ------------------------------------------------------- the item sprites ----
//
// ⭐⭐THE SAME BRIDGE AS THE LEGACY ATLASES, ONE LEVEL DOWN. AttachSprites
// publishes ui\textures_descr\*.xml -- the table the old screens are drawn from
// -- as RCSS sprites. Item icons are not in that table: an item's picture is
// four numbers in its own .ltx section (inv_grid_x/y/width/height), counted in
// 50px cells of ui\ui_icon_equipment[_N]. That is what CIconParams reads and
// what every cell of the legacy inventory draws with.
//
// RmlUi cannot say "this rectangle of that file" outside a @spritesheet, so the
// same trick applies: generate one from pSettings. Built once -- sections do not
// change while the game runs.
//
// ⚠Named after the SECTION, because that is what a cell knows. Characters that
// are not RCSS identifiers are mangled rather than skipped: an item with a dot
// in its section name still needs a picture.
xr_string sprite_name(const char* section)
{
    xr_string out = "inv_";
    for (const char* c = section; c && *c; ++c)
        out += (isalnum(u8(*c)) || *c == '_') ? *c : '_';
    return out;
}

// An item with icon_name has a file of its own; everything else is a rectangle
// in the equipment atlas, published as a sprite by attach_item_sprites below.
xr_string item_decorator(const char* section, const char* icon_file)
{
    if (icon_file && icon_file[0])
    {
        xr_string file = icon_file;
        for (char& ch : file)
            if (ch == '\\')
                ch = '/';
        return xr_string("image(") + file + ")";
    }
    return xr_string("image(") + sprite_name(section) + ")";
}

void attach_item_sprites(Rml::ElementDocument* doc)
{
    if (!doc)
        return;

    static Rml::String s_rcss;
    static bool s_built = false;
    if (!s_built)
    {
        s_built = true;

        // icon_group -> the sprite lines for that atlas. Group 0 is
        // ui\ui_icon_equipment, group N is ..._N, which is the name
        // InventoryUtilities::GetEquipmentIconsShader builds.
        xr_map<u32, xr_string> by_group;
        u32 icons = 0;

        for (const auto& kv : pSettings->sections())
        {
            const shared_str& name = kv.first;
            CInifile::Sect* s = kv.second;
            if (!s)
                continue;

            // ⚠icon_section points one item's picture at another section's
            // numbers, and the engine honours it (CIconParams::Load). Resolved
            // here by hand rather than through CIconParams, because that reads
            // with r_float -- which is fatal, not false, when a section names a
            // redirect that carries no grid.
            const shared_str from = READ_IF_EXISTS(pSettings, r_string, name, "icon_section", name.c_str());
            if (!pSettings->line_exist(from, "inv_grid_x") || !pSettings->line_exist(from, "inv_grid_width"))
                continue;

            // An item with icon_name draws from a file of its own; there is no
            // rectangle to publish and the markup reaches it by name.
            if (pSettings->line_exist(from, "icon_name"))
                continue;

            const float x = INV_GRID_WIDTH * pSettings->r_float(from, "inv_grid_x");
            const float y = INV_GRID_HEIGHT * pSettings->r_float(from, "inv_grid_y");
            const float w = INV_GRID_WIDTH * pSettings->r_float(from, "inv_grid_width");
            const float h = INV_GRID_HEIGHT * pSettings->r_float(from, "inv_grid_height");
            if (w <= 0.f || h <= 0.f)
                continue;

            const u32 group = READ_IF_EXISTS(pSettings, r_u32, from, "icon_group", 0);

            string512 line;
            xr_sprintf(line, sizeof(line), "\t%s: %.0fpx %.0fpx %.0fpx %.0fpx;\n", sprite_name(name.c_str()).c_str(), x, y, w, h);
            by_group[group] += line;
            ++icons;
        }

        for (const auto& kv : by_group)
        {
            string_path file;
            // ⚠Forward slashes: a backslash is an escape in RCSS, and LoadTexture
            // turns '/' back into '\' on the way to the archives (which are
            // indexed with backslashes). Getting this wrong makes every icon
            // "not load", with nothing in the log to say why.
            if (kv.first)
                xr_sprintf(file, sizeof(file), "ui/ui_icon_equipment_%u", kv.first);
            else
                xr_strcpy(file, sizeof(file), "ui/ui_icon_equipment");

            s_rcss += "@spritesheet xr_items_";
            s_rcss += std::to_string(kv.first).c_str();
            s_rcss += " {\n\tsrc: ";
            s_rcss += file;
            s_rcss += ";\n";
            s_rcss += kv.second.c_str();
            s_rcss += "}\n";
        }
        Msg("~ [rml] item icons: %u sprite(s) in %u atlas(es)", icons, u32(by_group.size()));

        // ⭐Load the atlases NOW, while the HUD is being built under the
        // loading screen. RmlUi asks for a texture only when a decorator first
        // draws -- 📏04-10-2026 Switch v153: the first inventory open spent
        // 166 ms of ui_post reading ui_icon_equipment_2 off the card. The
        // renderer keeps UI textures by name for the life of the device, so
        // RmlUi's own LoadTexture later finds them already there.
        for (const auto& kv : by_group)
            InventoryUtilities::GetEquipmentIconsShader(kv.first);
    }

    // Same order as AttachSprites: generated first, the document's own rules
    // second, so a screen can always override what this produced.
    CombineGeneratedSheet(doc, s_rcss, "item sprite");
}

// ------------------------------------------------------------- the stacks ----
//
// ⭐A PORTED RULE, NOT AN INVENTED ONE. CUIInventoryCellItem::EqualTo decides
// what folds into one cell: same section, condition within 1%, neither side
// marked ungroupable. A weapon additionally demands the same addons and the same
// kind of place, which is why two identical rifles stop stacking the moment one
// gets a scope.
//
// ⚠⚠The legacy rule also RUNS A SCRIPT CALLBACK -- eUIGroupItems -- which lets
// the mod veto one comparison by setting FIUngroupable on either item. Dropping
// it would be silent: stacks the mod means to keep apart would quietly merge,
// and the only sign would be a wrong count on a cell. Called here in the same
// shape, flags cleared afterwards exactly as the original does.
bool same_stack(PIItem a, PIItem b)
{
    if (!a || !b)
        return false;
    if (a->m_always_ungroupable || b->m_always_ungroupable)
        return false;
    if (a->m_flags.test(CInventoryItem::FIUngroupable) || b->m_flags.test(CInventoryItem::FIUngroupable))
        return false;

    if (g_actor)
        g_actor->callback(GameObject::eUIGroupItems)(a->object().lua_game_object(), b->object().lua_game_object());

    const Flags16 fl_a = a->m_flags;
    const Flags16 fl_b = b->m_flags;
    a->m_flags.set(CInventoryItem::FIUngroupable, false);
    b->m_flags.set(CInventoryItem::FIUngroupable, false);

    if (fl_a.test(CInventoryItem::FIUngroupable) || fl_b.test(CInventoryItem::FIUngroupable))
        return false;

    if (a->object().cNameSect() != b->object().cNameSect())
        return false;
    if (!fsimilar(a->GetCondition(), b->GetCondition(), 0.01f))
        return false;

    auto wa = smart_cast<CWeapon*>(a);
    auto wb = smart_cast<CWeapon*>(b);
    if (wa || wb)
        return wa && wb && wa->GetAddonsState() == wb->GetAddonsState() && wa->m_eItemPlace == wb->m_eItemPlace;

    return true;
}

// ------------------------------------------------------------ the filter -----
//
// ⭐THE BAG'S SORT BUTTONS (True Stalker: all / weapons / ammo / armour / drugs /
// food / artefacts / other). It only decides WHICH of the ruck's items the bag
// list shows; the items themselves, their order and the drag all stay as they
// were. "Equipped" is not here: what is worn already has its own lists.
enum class Filter : int
{
    All,
    Weapons,
    Ammo,
    Armor,
    Drugs,
    Food,
    Artefacts,
    Other,
};
constexpr const char* s_filter_names[] = {"all", "weapons", "ammo", "armor", "drugs", "food", "artefacts", "other"};
Filter s_filter = Filter::All;
// The trade screen sorts both bags, each with its own row of buttons: data-side="their" on the partner's.
Filter s_filter_their = Filter::All;

Filter& filter_of(Rml::Element* button)
{
    return button->GetAttribute<Rml::String>("data-side", "") == "their" ? s_filter_their : s_filter;
}

// A sort button pressed: which category its bag shows.
void press_filter(Rml::Element* button)
{
    const Rml::String want = button->GetAttribute<Rml::String>("data-filter", "all");
    for (int i = 0; i < int(std::size(s_filter_names)); ++i)
        if (want == s_filter_names[i])
            filter_of(button) = Filter(i);
}

Filter classify(PIItem it)
{
    if (smart_cast<CArtefact*>(it))
        return Filter::Artefacts;
    if (smart_cast<CWeaponAmmo*>(it))
        return Filter::Ammo;
    if (smart_cast<CWeapon*>(it) || smart_cast<CMissile*>(it)) // guns, knife, binoculars, grenades
        return Filter::Weapons;
    if (smart_cast<CCustomOutfit*>(it))
        return Filter::Armor;
    for (const u8 s : it->GetSlots()) // helmets are plain items that live in the helmet slot
        if (s == HELMET_SLOT || s == OUTFIT_SLOT)
            return Filter::Armor;
    if (smart_cast<CMedkit*>(it) || smart_cast<CAntirad*>(it))
        return Filter::Drugs;
    if (smart_cast<CEatableItem*>(it)) // food, drink, and whatever else is eaten
        return Filter::Food;
    return Filter::Other;
}

// ------------------------------------------------------------ the placing ----
//
// Row-major first fit, which is what CUICellContainer::FindFreeCell does. The
// legacy container can also grow, compact and place at the cursor; none of the
// three belongs to a rebuild from scratch, and all three arrive with dragging.
bool room_free(const List& l, const xr_vector<bool>& busy, Ivector2 at, Ivector2 size)
{
    if (at.x + size.x > l.capacity.x || at.y + size.y > l.capacity.y)
        return false;
    for (int y = 0; y < size.y; ++y)
        for (int x = 0; x < size.x; ++x)
            if (busy[size_t(at.y + y) * size_t(l.capacity.x) + size_t(at.x + x)])
                return false;
    return true;
}

bool find_free_cell(const List& l, const xr_vector<bool>& busy, Ivector2 size, Ivector2& out)
{
    for (out.y = 0; out.y + size.y <= l.capacity.y; ++out.y)
        for (out.x = 0; out.x + size.x <= l.capacity.x; ++out.x)
            if (room_free(l, busy, out, size))
                return true;
    return false;
}

void place(List& l, const xr_vector<PIItem>& items)
{
    l.cells.clear();
    if (l.capacity.x <= 0 || l.capacity.y <= 0)
        return;

    // ⭐The bag has no bottom (unlimited="1" in the XML): it is laid out as tall
    // as the items could possibly need and its frame scrolls. Summing every
    // item's height is a bound no first fit can exceed.
    const Ivector2 declared = l.capacity;
    if (l.unlimited)
    {
        int rows = 0;
        for (PIItem it : items)
            if (it)
                rows += l.one_cell ? 1 : std::max(1, int(it->GetGridHeight()));
        l.capacity.y = std::max(l.capacity.y, rows);
    }

    xr_vector<bool> busy;
    busy.assign(size_t(l.capacity.x) * size_t(l.capacity.y), false);

    // The belt has as many cells as the suit gives (CInventory::BeltSlotsCount);
    // the rest of the row exists on screen but takes nothing.
    for (size_t i = l.available; i < busy.size(); ++i)
        busy[i] = true;

    for (PIItem it : items)
    {
        if (!it)
            continue;

        // ⚠An item can ask not to be shown at all (FIHiddenForInventory) -- quest
        // bookkeeping and the like. The legacy window checks this for the ruck
        // and for the grenade slot it folds into the ruck; checking it in one
        // place is the same behaviour with one fewer special case.
        if (it->m_flags.test(CInventoryItem::FIHiddenForInventory))
            continue;

        if (l.group)
        {
            bool folded = false;
            for (Cell& c : l.cells)
                if (same_stack(c.item, it))
                {
                    c.more.push_back(it);
                    folded = true;
                    break;
                }
            if (folded)
                continue;
        }

        Cell c;
        c.item = it;
        c.grid.set(it->GetGridWidth(), it->GetGridHeight());
        // ⚠A list that shows one item over its whole box (fit) takes any item,
        // whatever its grid: the box IS the cell. A 1x1 slot list must not look
        // for room for a 5x2 rifle.
        if (c.grid.x <= 0 || c.grid.y <= 0 || l.one_cell || l.fit)
            c.grid.set(1, 1);

        if (!find_free_cell(l, busy, c.grid, c.at))
        {
            // ⚠Said out loud rather than dropped quietly: an item the screen
            // cannot place is still in the inventory, and a cell that is simply
            // absent reads as a lost item.
            Msg("! [rml] inventory: no room in '%s' for %s (%dx%d in %dx%d)", l.id, it->object().cNameSect().c_str(), c.grid.x, c.grid.y, l.capacity.x, l.capacity.y);
            continue;
        }

        for (int y = 0; y < c.grid.y; ++y)
            for (int x = 0; x < c.grid.x; ++x)
                busy[size_t(c.at.y + y) * size_t(l.capacity.x) + size_t(c.at.x + x)] = true;

        l.cells.push_back(c);
    }

    // Back to the height the screen shows at least; fill_list grows the element
    // to whatever the items took.
    if (l.unlimited)
    {
        int used = declared.y;
        for (const Cell& c : l.cells)
            used = std::max(used, c.at.y + c.grid.y);
        l.capacity.y = used;
    }
}

// ------------------------------------------------------------- the screen ----

Rml::Element* by_id(const char* id) { return s_doc ? s_doc->GetElementById(id) : nullptr; }

void set_text(const char* id, const char* text)
{
    if (auto e = by_id(id))
        e->SetInnerRML(ToUtf8(text).c_str());
}

void set_pct(const char* id, float v01)
{
    if (auto e = by_id(id))
        e->SetProperty("width", Rml::String(std::to_string(int(clampr(v01, 0.f, 1.f) * 100.f)) + "%"));
}

Rml::String dp(float v)
{
    char buf[32];
    xr_sprintf(buf, sizeof(buf), "%.2fdp", v);
    return buf;
}

// The selected item, as an object ID -- see the note at the top. u16(-1) is none.
u16 s_sel = u16(-1);

// The cell of an item, wherever it is drawn: the list it sits in and the cell
// itself. An item folded into a stack is found through the stack's head.
struct Located
{
    List* list{};
    Cell* cell{};
};

Located locate(u16 id)
{
    for (List& l : lists())
        for (Cell& c : l.cells)
        {
            if (c.item && c.item->object().ID() == id)
                return {&l, &c};
            for (PIItem m : c.more)
                if (m && m->object().ID() == id)
                    return {&l, &c};
        }
    return {};
}

List* list_by_id(const Rml::String& id)
{
    for (List& l : lists())
        if (id == l.id)
            return &l;
    return nullptr;
}

// The list a drop landed on. ⭐Not only the list's own box: a panel that holds
// exactly ONE list (the bag's frame, the box's panel with its caption) counts as
// that list, so a drop on the frame's edge or caption is not silently lost --
// the log caught one 1dp left of the grid, on the panel.
List* list_by_element(Rml::Element* e)
{
    for (; e; e = e->GetParentNode())
    {
        if (e->IsClassSet("dd"))
            return list_by_id(e->GetId());
        Rml::ElementList inside;
        e->GetElementsByClassName(inside, "dd");
        if (inside.size() == 1)
            return list_by_id(inside.front()->GetId());
        if (inside.size() > 1)
            return nullptr; // the whole stage: no one list is meant
    }
    return nullptr;
}

Rml::Element* up_to_class(Rml::Element* e, const char* cls)
{
    for (; e; e = e->GetParentNode())
        if (e->IsClassSet(cls))
            return e;
    return nullptr;
}

Rml::Element* up_to_id(Rml::Element* e, const char* id)
{
    for (; e; e = e->GetParentNode())
        if (e->GetId() == id)
            return e;
    return nullptr;
}

bool shift_held() { return !!pInput->iGetAsyncKeyState(DIK_LSHIFT) || !!pInput->iGetAsyncKeyState(DIK_RSHIFT); }

u16 id_of(Rml::Element* cell) { return cell ? u16(cell->GetAttribute<int>("data-id", 0xffff)) : u16(-1); }

// ⭐Fitting one icon into a box the way the cells look: scaled uniformly, never
// distorted, centred. The atlas icon is grid*50 legacy pixels; a list's cells can
// be anything (the bag's are 26x33), so a stretch would squash every picture.
struct Fit
{
    float k{1.f}, ox{}, oy{};
};

Fit fit_into(float box_w, float box_h, float pic_w, float pic_h)
{
    Fit f;
    if (pic_w <= 0.f || pic_h <= 0.f)
        return f;
    f.k = std::min(box_w / pic_w, box_h / pic_h);
    f.ox = (box_w - pic_w * f.k) * 0.5f;
    f.oy = (box_h - pic_h * f.k) * 0.5f;
    return f;
}

void add_picture(Rml::Element* parent, const char* cls, const char* decorator, float x, float y, float w, float h)
{
    Rml::ElementPtr e = s_doc->CreateElement("div");
    e->SetClass(cls, true);
    e->SetProperty("left", dp(x));
    e->SetProperty("top", dp(y));
    e->SetProperty("width", dp(w));
    e->SetProperty("height", dp(h));
    e->SetProperty("decorator", decorator);
    parent->AppendChild(std::move(e));
}

void fill_list(List& l)
{
    Rml::Element* host = by_id(l.id);
    if (!host)
        return;

    host->SetInnerRML("");

    // The bag grows with what it holds; the frame around it scrolls.
    if (l.unlimited)
        host->SetProperty("height", dp(float(l.capacity.y * l.cell.y)));

    // ⭐The empty cells, first so the items lie over them (data-bg: the True
    // Stalker bag and belt show their whole grid, filled or not). A list that
    // shows one item over its whole box (fit) is its own background.
    if (l.bg && !l.fit)
        for (int i = 0; i < l.capacity.x * l.capacity.y; ++i)
        {
            if (l.available != u32(-1) && i >= int(l.available))
                break; // the ones past it are `na`, below
            Rml::ElementPtr bg = s_doc->CreateElement("div");
            bg->SetClass("bgc", true);
            bg->SetProperty("left", dp(float((i % l.capacity.x) * l.cell.x) + l.gap));
            bg->SetProperty("top", dp(float((i / l.capacity.x) * l.cell.y) + l.gap));
            bg->SetProperty("width", dp(float(l.cell.x) - 2.f * l.gap));
            bg->SetProperty("height", dp(float(l.cell.y) - 2.f * l.gap));
            host->AppendChild(std::move(bg));
        }

    // Cells that do not exist (the belt past what the suit gives) are drawn as
    // such, so a full belt reads as full rather than as broken.
    if (l.available != u32(-1))
        for (int i = int(l.available); i < l.capacity.x * l.capacity.y; ++i)
        {
            Rml::ElementPtr na = s_doc->CreateElement("div");
            na->SetClass("na", true);
            na->SetProperty("left", dp(float((i % l.capacity.x) * l.cell.x) + l.gap));
            na->SetProperty("top", dp(float((i / l.capacity.x) * l.cell.y) + l.gap));
            na->SetProperty("width", dp(float(l.cell.x) - 2.f * l.gap));
            na->SetProperty("height", dp(float(l.cell.y) - 2.f * l.gap));
            host->AppendChild(std::move(na));
        }

    for (const Cell& c : l.cells)
    {
        Rml::ElementPtr e = s_doc->CreateElement("div");
        e->SetClass("ci", true);
        if (c.item->object().ID() == s_sel)
            e->SetClass("sel", true);
        // The search screen marks what the actor has on (m_highlight_equipped).
        if (c.item->m_highlight_equipped)
            e->SetClass("eq", true);
        // A quest item gets True Stalker's violet "unique" frame.
        if (c.item->IsQuestItem())
            e->SetClass("quest", true);
        // Trade: what cannot go on the table is greyed (CUITradeWnd::ColorizeItem).
        if (s_screen == Screen::Trade && (l.kind == Kind::Bag || l.kind == Kind::Others) && !tr_can_move(c.item, l.kind == Kind::Bag))
            e->SetClass("untr", true);
        // Stolen goods carry their mark until it fades (ogsr_stolen.script). A
        // stack is marked when any of it is stolen: that is what a search finds.
        bool stolen = stolen_hours(c.item) >= 0.f;
        for (PIItem m : c.more)
            stolen = stolen || stolen_hours(m) >= 0.f;
        if (stolen)
            e->SetClass("stolen", true);

        // What a handler needs to find its way back to the item. ⚠The object ID,
        // not the pointer: a rebuild can happen between the click and the action,
        // and an id that no longer resolves is a caught mistake where a stale
        // pointer is a crash.
        e->SetAttribute("data-id", int(c.item->object().ID()));
        e->SetAttribute("data-sect", c.item->object().cNameSect().c_str());

        // A slot list shows its one item over the whole list (the legacy
        // virtual cells); everything else sits on the grid.
        float box_x = float(c.at.x * l.cell.x), box_y = float(c.at.y * l.cell.y);
        float box_w = float(c.grid.x * l.cell.x), box_h = float(c.grid.y * l.cell.y);
        if (l.fit)
        {
            box_x = box_y = 0.f;
            box_w = float(l.capacity.x * l.cell.x);
            box_h = float(l.capacity.y * l.cell.y);
        }
        box_x += l.gap;
        box_y += l.gap;
        box_w -= 2.f * l.gap;
        box_h -= 2.f * l.gap;
        e->SetProperty("left", dp(box_x));
        e->SetProperty("top", dp(box_y));
        e->SetProperty("width", dp(box_w));
        e->SetProperty("height", dp(box_h));

        // The picture, and the addons a weapon carries drawn over it where the
        // inventory cell puts them (CUIWeaponCellItem's offsets, in icon pixels).
        const CIconParams& p = c.item->m_icon_params;
        const Frect& r = p.original_rect();
        // data-pad keeps the picture off the cell's frame.
        Fit f = fit_into(std::max(1.f, box_w - 2.f * l.pad), std::max(1.f, box_h - 2.f * l.pad), r.width(), r.height());
        f.ox += l.pad;
        f.oy += l.pad;
        add_picture(e.get(), "pic", item_decorator(c.item->object().cNameSect().c_str(), p.name.c_str()).c_str(), f.ox, f.oy, r.width() * f.k, r.height() * f.k);

        if (auto wpn = smart_cast<CWeapon*>(c.item))
        {
            const bool sil = wpn->SilencerAttachable() && wpn->IsSilencerAttached();
            const bool scope = wpn->ScopeAttachable() && wpn->IsScopeAttached();
            const bool gl = wpn->GrenadeLauncherAttachable() && wpn->IsGrenadeLauncherAttached();
            if (sil || scope || gl)
            {
                // An addon sits where the weapon section says (scope_x/scope_y,
                // silencer_x/..., grenade_launcher_x/...), in the icon's pixels.
                auto addon = [&](const shared_str& sect, float ox, float oy) {
                    CIconParams ap(sect);
                    const Frect& ar = ap.original_rect();
                    add_picture(e.get(), "pic", item_decorator(sect.c_str(), ap.name.c_str()).c_str(), f.ox + ox * f.k, f.oy + oy * f.k, ar.width() * f.k, ar.height() * f.k);
                };
                if (sil)
                    addon(wpn->GetSilencerName(), float(wpn->GetSilencerX()), float(wpn->GetSilencerY()));
                if (scope)
                    addon(wpn->GetScopeName(), float(wpn->GetScopeX()), float(wpn->GetScopeY()));
                if (gl)
                    addon(wpn->GetGrenadeLauncherName(), float(wpn->GetGrenadeLauncherX()), float(wpn->GetGrenadeLauncherY()));
            }
        }

        // The wear bar, on the items that show one -- the same three cases the
        // legacy cell asks about (a weapon, an outfit, or inv_show_condition).
        // The mod's look: a red track with the green remainder over it.
        if (smart_cast<CWeapon*>(c.item) || smart_cast<CCustomOutfit*>(c.item) || c.item->GetInvShowCondition())
        {
            Rml::ElementPtr track = s_doc->CreateElement("div");
            track->SetClass("cond", true);
            Rml::ElementPtr bar = s_doc->CreateElement("div");
            bar->SetClass("cond-fill", true);
            bar->SetProperty("width", Rml::String(std::to_string(int(clampr(c.item->GetConditionToShow(), 0.f, 1.f) * 100.f)) + "%"));
            track->AppendChild(std::move(bar));
            e->AppendChild(std::move(track));
        }

        // The count: rounds for ammo (CUIAmmoCellItem::UpdateItemText adds up
        // every box in the stack), "xN" for any other stack.
        xr_string count;
        if (auto ammo = smart_cast<CWeaponAmmo*>(c.item))
        {
            u32 total = ammo->m_boxCurr;
            for (PIItem m : c.more)
                if (auto a = smart_cast<CWeaponAmmo*>(m))
                    total += a->m_boxCurr;
            count = std::to_string(total).c_str();
        }
        else if (!c.more.empty())
            count = ("x" + std::to_string(c.more.size() + 1)).c_str();
        if (!count.empty())
        {
            Rml::ElementPtr t = s_doc->CreateElement("div");
            t->SetClass("cnt", true);
            t->SetInnerRML(count.c_str());
            e->AppendChild(std::move(t));
        }

        // The red hand in the corner (styled by .stolen-mark in the screen's rcss).
        if (stolen)
        {
            Rml::ElementPtr m = s_doc->CreateElement("div");
            m->SetClass("stolen-mark", true);
            e->AppendChild(std::move(m));
        }

        host->AppendChild(std::move(e));
    }

    // ⭐A slot's PANEL (the element holding the list, class slot-panel) says what
    // is in it: the item's name in its .sl-name and `empty` on the panel itself,
    // which the stylesheet turns into the "empty" look. A list outside such a
    // panel is left alone.
    if (l.kind == Kind::Slot)
        if (Rml::Element* panel = host->GetParentNode(); panel && panel->IsClassSet("slot-panel"))
        {
            const bool empty = l.cells.empty();
            panel->SetClass("empty", empty);
            Rml::ElementList names;
            panel->GetElementsByClassName(names, "sl-name");
            for (Rml::Element* n : names)
                n->SetInnerRML(empty ? "" : ToUtf8(l.cells.front().item->NameShort()).c_str());

            // What the weapon in it carries: the panel wears has-scope /
            // has-sil / has-gl and its chips (.ad-*) turn on.
            bool scope = false, sil = false, gl = false;
            if (!empty)
                if (auto wpn = smart_cast<CWeapon*>(l.cells.front().item))
                {
                    scope = wpn->ScopeAttachable() && wpn->IsScopeAttached();
                    sil = wpn->SilencerAttachable() && wpn->IsSilencerAttached();
                    gl = wpn->GrenadeLauncherAttachable() && wpn->IsGrenadeLauncherAttached();
                }
            panel->SetClass("has-scope", scope);
            panel->SetClass("has-sil", sil);
            panel->SetClass("has-gl", gl);
        }
}

// ------------------------------------------------------------- the sounds ----
//
// The legacy window reads them from <action_sounds> in its XML; here they are
// attributes on the document's body (data-snd-*), same files.
enum Snd
{
    SndOpen,
    SndClose,
    SndToSlot,
    SndToBelt,
    SndToRuck,
    SndProperties,
    SndDrop,
    SndAttach,
    SndDetach,
    SndUse,
    SndUnload,
    SndMove, // the search and trade screens: an item to the other side / the table
    SndTrade, // a deal struck
    SndTradeBlock, // a deal refused
    SndCount
};
constexpr const char* s_snd_attr[SndCount] = {"data-snd-open",   "data-snd-close",  "data-snd-to-slot", "data-snd-to-belt", "data-snd-to-ruck",
                                              "data-snd-properties", "data-snd-drop", "data-snd-attach",  "data-snd-detach",  "data-snd-use",
                                              "data-snd-unload", "data-snd-move",   "data-snd-trade",   "data-snd-trade-blocked"};
// ⚠⚠On the heap and never freed. As a static array the ref_sounds were destroyed
// by the atexit chain AFTER the sound system was gone, and ~ref_sound_data read
// freed memory: every quit to desktop ended in an access violation. The process
// is ending at that point; there is nothing to give back.
ref_sound* const s_snd = new ref_sound[SndCount];
bool s_snd_loaded = false;

// Per document: the inventory and the search screen name different sets.
void load_sounds()
{
    if (!s_doc)
        return;
    s_snd_loaded = true;
    for (int i = 0; i < SndCount; ++i)
    {
        const Rml::String file = s_doc->GetAttribute<Rml::String>(s_snd_attr[i], "");
        // CUIWindow::create_ui_snd, which is protected.
        s_snd[i].destroy();
        if (!file.empty())
            ::Sound->create(s_snd[i], file.c_str(), st_Effect, sg_SourceType);
    }
}

void play(Snd s)
{
    if (s_snd[s]._handle())
        s_snd[s].play(nullptr, sm_2D);
}

// ------------------------------------------------------------ the actions ----
//
// ⭐Line for line the legacy ones (ui/uiinventorywnd2.cpp, UIInventoryWnd3.cpp),
// minus the cell bookkeeping: the screen is rebuilt from the inventory after
// every change instead of being edited in step with it.

void send_item_event(PIItem it, u16 type)
{
    NET_Packet P;
    it->object().u_EventGen(P, type, it->object().H_Parent()->ID());
    P.w_u16(it->object().ID());
    it->object().u_EventSend(P);
}

void send_activate_slot(PIItem it)
{
    NET_Packet P;
    it->object().u_EventGen(P, GEG_PLAYER_ACTIVATE_SLOT, it->object().H_Parent()->ID());
    P.w_u32(it->GetSlot());
    it->object().u_EventSend(P);
}

bool to_bag(PIItem it)
{
    CInventory* inv = actor_inventory();
    if (!inv || !inv->CanPutInRuck(it))
        return false;
    inv->Ruck(it);
    send_item_event(it, GEG_PLAYER_ITEM2RUCK);
    play(SndToRuck);
    s_rebuild = true;
    return true;
}

bool to_belt(PIItem it)
{
    CInventory* inv = actor_inventory();
    if (!inv || !inv->CanPutInBelt(it))
        return false;
    inv->Belt(it);
    send_item_event(it, GEG_PLAYER_ITEM2BELT);
    play(SndToBelt);
    s_rebuild = true;
    return true;
}

// Is there a list on screen for this slot? The legacy GetSlotList: none for a
// persistent slot, and none for the grenade slot, which is drawn in the bag.
bool slot_has_list(u8 slot)
{
    CInventory* inv = actor_inventory();
    if (!inv || slot == NO_ACTIVE_SLOT || slot >= inv->m_slots.size() || inv->m_slots[slot].m_bPersistent)
        return false;
    for (const List& l : lists())
        if (l.kind == Kind::Slot && l.slot == slot && by_id(l.id))
            return true;
    return false;
}

bool to_slot(PIItem it, bool force_place)
{
    CInventory* inv = actor_inventory();
    if (!inv)
        return false;
    const u8 slot = u8(it->GetSlot());
    bool result = false;

    if (inv->CanPutInSlot(it))
    {
        if (slot == GRENADE_SLOT && !slot_has_list(slot))
            return true; // fake, sorry ((( -- the legacy comment, and the legacy behaviour

        result = inv->Slot(it);
        VERIFY(result);
        send_item_event(it, GEG_PLAYER_ITEM2SLOT);
        play(SndToSlot);
        send_activate_slot(it);
        s_rebuild = true;
    }
    else
    {
        // The slot is taken: with force, whatever is in it goes to the bag first.
        if (!force_place || slot == NO_ACTIVE_SLOT || slot >= inv->m_slots.size() || inv->m_slots[slot].m_bPersistent)
            return false;
        PIItem occupant = inv->m_slots[slot].m_pIItem;
        if (!occupant)
            return false;

        // ⚠Taking a suit off and putting the next on must not trim the belt in
        // between -- see CInventory::KeepBeltOnce.
        if (slot == OUTFIT_SLOT)
            inv->KeepBeltOnce();

        to_bag(occupant);
        return to_slot(it, false);
    }

    if (result && slot == DETECTOR_SLOT)
        if (auto det = smart_cast<CCustomDetector*>(it))
            det->ToggleDetector(g_player_hud->attached_item(0) != nullptr);

    return result;
}

void drop(PIItem it)
{
    it->SetDropManual(TRUE);
    play(SndDrop);
    s_rebuild = true;
}

void eat(PIItem it)
{
    if (!it->Useful())
        return;
    R_ASSERT(it->m_pCurrentInventory == actor_inventory());
    send_item_event(it, GEG_PLAYER_ITEM_EAT);
    play(SndUse);
    s_rebuild = true;
}

bool try_use(PIItem it)
{
    if (smart_cast<CMedkit*>(it) || smart_cast<CAntirad*>(it) || smart_cast<CEatableItem*>(it) || smart_cast<CBottleItem*>(it))
    {
        s_sel = u16(-1);
        eat(it);
        return true;
    }
    return false;
}

// The legacy DropItem(itm, list): dropping onto the suit means "use it", and
// dropping onto a slot whose item takes this as an addon attaches it.
bool use_or_attach(PIItem it, const List* to)
{
    if (!to)
        return false;
    if (to->kind == Kind::Slot && to->slot == OUTFIT_SLOT)
        return try_use(it);
    if (to->kind != Kind::Slot)
        return false;
    CInventory* inv = actor_inventory();
    PIItem target = (inv && to->slot < inv->m_slots.size()) ? inv->m_slots[to->slot].m_pIItem : nullptr;
    if (!target || !target->CanAttach(it))
        return false;
    play(SndAttach);
    target->Attach(it, true);
    s_sel = u16(-1);
    s_rebuild = true;
    return true;
}

// Double click: use what can be used (shift skips that), otherwise send it
// "the other way" -- a slot or the belt to the bag, the bag to a slot or belt.
void double_click(u16 id)
{
    const Located at = locate(id);
    if (!at.cell)
        return;
    PIItem it = at.cell->item;
    Msg("~ [rml] inventory: double click on %s", it->object().cNameSect().c_str());

    const bool shift = !!pInput->iGetAsyncKeyState(DIK_LSHIFT) || !!pInput->iGetAsyncKeyState(DIK_RSHIFT);
    if (!shift && try_use(it))
        return;

    switch (at.list->kind)
    {
    case Kind::Slot:
    case Kind::Belt: to_bag(it); break;
    case Kind::Bag: {
        // A free allowed slot first; failing that the belt; failing that the
        // first allowed slot by force.
        auto slots = it->GetSlots();
        for (u8 i = 0; i < u8(slots.size()); ++i)
        {
            it->SetSlot(slots[i]);
            if (to_slot(it, false))
                return;
        }
        it->SetSlot(slots.size() ? slots[0] : NO_ACTIVE_SLOT);
        if (!to_slot(it, false))
            if (!to_belt(it))
                to_slot(it, true);
    }
    break;
    }
}

// Dropped from one list onto another (the legacy OnItemDrop).
void drop_on(u16 id, List* to)
{
    const Located from = locate(id);
    if (!from.cell || !to || from.list == to)
        return;
    PIItem it = from.cell->item;
    Msg("~ [rml] inventory: drag %s from '%s' to '%s'", it->object().cNameSect().c_str(), from.list->id, to->id);

    if (to->kind == from.list->kind && to->kind != Kind::Slot)
        return;

    switch (to->kind)
    {
    case Kind::Slot: {
        // ⚠The legacy size check compares the item's grid (in cells) with the
        // list's CellSize() (in PIXELS), so it never refuses anything; not carried.
        const u8 slot = to->slot;
        if (it->IsPlaceable(slot, slot))
            it->SetSlot(slot);
        else
        {
            if (!use_or_attach(it, to) && it->GetSlotsCount() > 0)
                Msg("! cannot put item %s into slot %d, allowed slots {%s}", it->object().Name_script(), slot, it->GetSlotsSect());
            return;
        }
        if (it->GetSlot() == slot)
            to_slot(it, true);
    }
    break;
    case Kind::Bag: to_bag(it); break;
    case Kind::Belt: to_belt(it); break;
    }

    use_or_attach(it, to);
}

void drop_selected(bool all)
{
    const Located at = locate(s_sel);
    if (!at.cell || at.cell->item->IsQuestItem())
    {
        Msg("~ [rml] inventory: nothing to drop (selected %u%s)", s_sel, at.cell ? ", a quest item" : "");
        return;
    }
    Msg("~ [rml] inventory: drop %s%s", at.cell->item->object().cNameSect().c_str(), all ? " and its whole stack" : "");
    if (all)
        for (PIItem m : at.cell->more)
            drop(m);
    drop(at.cell->item);
    s_sel = u16(-1);
}

void unload(PIItem it)
{
    auto wm = smart_cast<CWeaponMagazined*>(it);
    if (!wm)
        return;
    wm->UnloadMagazine();
    if (auto wgl = smart_cast<CWeaponMagazinedWGrenade*>(wm))
        if (wgl->IsGrenadeLauncherAttached())
        {
            wgl->PerformSwitchGL();
            wgl->UnloadMagazine();
            wgl->PerformSwitchGL();
        }
    // So the weapon goes straight to idle_empty.
    if (wm->GetState() == CHUDState::eIdle && wm->HudItemData())
        wm->SwitchState(CHUDState::eIdle);
}

// --------------------------------------------------- the right-click menu ----
//
// ⭐The legacy ActivatePropertiesBox, item for item and in the same order: what
// is offered depends on what the item is and where it is, and the mod's
// localisation provides every caption (st_move_to_slot, st_drop, ...).
enum class Act
{
    ToSlot,
    ToBelt,
    ToBag,
    Drop,
    DropAll,
    Eat,
    Attach,
    DetachScope,
    DetachSilencer,
    DetachGL,
    Unload,
    Move, // the search screen: to the other side
    MoveAll,
};
struct MenuEntry
{
    xr_string text; // cp1251, translated
    Act act;
    u32 data{}; // a slot for ToSlot, a target object id for Attach
};
xr_vector<MenuEntry> s_menu;

// The search screen's side of the menu, further down.
void build_cb_menu(const Located& at);
void cb_move(u16 id, bool all);
void cb_drop(bool all);
void cb_eat();
void cb_take_all();
void build_tr_menu(const Located& at);
// Everything on the table back into its owner's bag (the "cancel" button, Y on the pad).
void tr_clear_table();
void tr_move(u16 id, bool all);
void tr_perform();

bool can_unload(PIItem it)
{
    auto wm = smart_cast<CWeaponMagazined*>(it);
    if (!wm)
        return false;
    auto wgl = smart_cast<CWeaponMagazinedWGrenade*>(wm);
    return wm->GetAmmoElapsed() > 0 || (wgl && !wgl->m_magazine2.empty());
}

void build_menu(const Located& at)
{
    if (s_screen == Screen::CarBody)
    {
        build_cb_menu(at);
        return;
    }
    if (s_screen == Screen::Trade)
    {
        build_tr_menu(at);
        return;
    }
    s_menu.clear();
    CInventory* inv = actor_inventory();
    PIItem it = at.cell->item;
    if (!inv || !it)
        return;

    auto add = [](LPCSTR st, Act act, u32 data = 0) { s_menu.push_back({CStringTable().translate(st).c_str(), act, data}); };

    auto outfit = smart_cast<CCustomOutfit*>(it);
    auto wpn = smart_cast<CWeapon*>(it);
    bool already_dressed = false;

    if (!outfit && it->GetSlot() != NO_ACTIVE_SLOT)
    {
        auto slots = it->GetSlots();
        const bool multi = slots.size() > 1;
        for (u8 i = 0; i < u8(slots.size()); ++i)
        {
            const u8 slot = slots[i];
            if (slot == NO_ACTIVE_SLOT || slot == GRENADE_SLOT)
                continue;
            if (!inv->m_slots[slot].m_pIItem || inv->m_slots[slot].m_pIItem != it)
            {
                if (multi && Core.Features.test(xrCore::Feature::slots_extend_menu))
                {
                    string128 st;
                    strconcat(sizeof(st), st, "st_move_to_slot_", std::to_string(slot).c_str());
                    add(st, Act::ToSlot, slot);
                }
                else
                {
                    add("st_move_to_slot", Act::ToSlot, u32(-1));
                    break;
                }
            }
        }
    }

    if (it->Belt() && inv->CanPutInBelt(it))
        add("st_move_on_belt", Act::ToBelt);

    if (it->Ruck() && inv->CanPutInRuck(it) && (it->GetSlot() == NO_ACTIVE_SLOT || !inv->m_slots[it->GetSlot()].m_bPersistent))
    {
        add(outfit ? "st_undress_outfit" : "st_move_to_bag", Act::ToBag);
        already_dressed = true;
    }
    if (outfit && !already_dressed)
        add("st_dress_outfit", Act::ToSlot, u32(-1));

    if (wpn)
    {
        if (wpn->GrenadeLauncherAttachable() && wpn->IsGrenadeLauncherAttached())
            add("st_detach_gl", Act::DetachGL);
        if (wpn->ScopeAttachable() && wpn->IsScopeAttached())
            add("st_detach_scope", Act::DetachScope);
        if (wpn->SilencerAttachable() && wpn->IsSilencerAttached())
            add("st_detach_silencer", Act::DetachSilencer);

        bool loaded = can_unload(it);
        for (PIItem m : at.cell->more)
            loaded = loaded || can_unload(m);
        if (loaded && smart_cast<CWeaponMagazined*>(wpn))
            add("st_unload_magazine", Act::Unload);
    }

    // Attaching this item to a weapon in a slot -- the caption names both.
    for (u32 i = 0; i < SLOTS_TOTAL; ++i)
    {
        PIItem tgt = inv->m_slots[i].m_pIItem;
        if (tgt && tgt->CanAttach(it))
        {
            const std::string key = "st_attach_addon_to_wpn_in_slot_" + std::to_string(i);
            string512 str{};
            std::snprintf(str, sizeof str, CStringTable().translate(key.c_str()).c_str(), it->m_nameShort.c_str(), tgt->m_nameShort.c_str());
            s_menu.push_back({str, Act::Attach, u32(tgt->object().ID())});
        }
    }

    if (smart_cast<CMedkit*>(it) || smart_cast<CAntirad*>(it))
        add("st_use", Act::Eat);
    else if (smart_cast<CEatableItem*>(it))
        add(smart_cast<CBottleItem*>(it) ? "st_drink" : "st_eat", Act::Eat);

    const bool no_drop = (outfit && already_dressed) || it->IsQuestItem();
    if (!no_drop)
    {
        add("st_drop", Act::Drop);
        if (!at.cell->more.empty())
            add("st_drop_all", Act::DropAll);
    }
}

void hide_menu()
{
    if (auto m = by_id("ctx"))
        m->SetProperty("display", "none");
    // The pad's list goes with it: any click, drag or rebuild closes both.
    if (auto p = by_id("padmenu"))
        p->SetProperty("display", "none");
    if (s_pad_menu != PadMenu::None)
        for (const char* id : {"descr", "descr2"}) // the list hid them (pad_descr_visible)
            if (Rml::Element* d = by_id(id))
                d->SetProperty("visibility", "visible");
    s_pad_menu = PadMenu::None;
}

// At the pointer, kept inside the stage.
void show_menu(u16 id, float mouse_x, float mouse_y)
{
    const Located at = locate(id);
    Rml::Element* m = by_id("ctx");
    Rml::Element* stage = by_id("stage");
    if (!at.cell || !m || !stage)
        return;

    build_menu(at);
    if (s_menu.empty())
    {
        hide_menu();
        return;
    }

    xr_string rml;
    for (size_t i = 0; i < s_menu.size(); ++i)
    {
        xr_string text = ToUtf8(s_menu[i].text.c_str()).c_str();
        xr_string safe;
        for (char ch : text)
            safe += (ch == '<') ? xr_string("&lt;") : (ch == '&') ? xr_string("&amp;") : xr_string(1, ch);
        rml += "<div class=\"act\" data-act=\"" + xr_string(std::to_string(i).c_str()) + "\">" + safe + "</div>";
    }
    m->SetInnerRML(rml.c_str());
    m->SetProperty("display", "block");

    // Place it now that it has a size.
    s_doc->UpdateDocument();
    const Rml::Vector2f origin = stage->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f stage_sz = stage->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2f sz = m->GetBox().GetSize(Rml::BoxArea::Border);
    float x = mouse_x - origin.x, y = mouse_y - origin.y;
    x = std::min(x, stage_sz.x - sz.x);
    y = std::min(y, stage_sz.y - sz.y);
    m->SetProperty("left", Rml::String(std::to_string(int(std::max(0.f, x))) + "px"));
    m->SetProperty("top", Rml::String(std::to_string(int(std::max(0.f, y))) + "px"));
    play(SndProperties);
}

void run_menu(size_t index)
{
    if (index >= s_menu.size())
        return;
    const MenuEntry e = s_menu[index];
    hide_menu();

    const Located at = locate(s_sel);
    if (!at.cell)
    {
        Msg("! [rml] inventory: menu '%s' -- the selected item %u is not on screen", e.text.c_str(), s_sel);
        return;
    }
    PIItem it = at.cell->item;
    auto wpn = smart_cast<CWeapon*>(it);
    Msg("~ [rml] inventory: menu '%s' on %s", e.text.c_str(), it->object().cNameSect().c_str());

    switch (e.act)
    {
    case Act::ToSlot: {
        // A slot named in the menu first; then a free allowed one; then the
        // first allowed one by force.
        if (e.data != u32(-1))
        {
            it->SetSlot(u8(e.data));
            if (to_slot(it, true))
                return;
        }
        auto slots = it->GetSlots();
        for (u8 i = 0; i < u8(slots.size()); ++i)
        {
            it->SetSlot(slots[i]);
            if (to_slot(it, false))
                return;
        }
        it->SetSlot(slots.size() ? slots[0] : NO_ACTIVE_SLOT);
        to_slot(it, true);
    }
    break;
    case Act::ToBelt: to_belt(it); break;
    case Act::ToBag: to_bag(it); break;
    case Act::Drop:
    case Act::DropAll:
        if (s_screen == Screen::CarBody)
            cb_drop(e.act == Act::DropAll);
        else
            drop_selected(e.act == Act::DropAll);
        break;
    case Act::Eat:
        if (s_screen == Screen::CarBody)
            cb_eat();
        else
        {
            s_sel = u16(-1);
            eat(it);
        }
        break;
    case Act::Move:
    case Act::MoveAll:
        if (s_screen == Screen::Trade)
            tr_move(s_sel, e.act == Act::MoveAll);
        else
            cb_move(s_sel, e.act == Act::MoveAll);
        break;
    case Act::Attach:
        if (auto tgt = smart_cast<PIItem>(Level().Objects.net_Find(u16(e.data))))
        {
            play(SndAttach);
            tgt->Attach(it, true);
            s_sel = u16(-1);
            s_rebuild = true;
        }
        break;
    case Act::DetachScope:
    case Act::DetachSilencer:
    case Act::DetachGL:
        if (wpn)
        {
            play(SndDetach);
            const shared_str& name = e.act == Act::DetachScope ? wpn->GetScopeName() : e.act == Act::DetachSilencer ? wpn->GetSilencerName() : wpn->GetGrenadeLauncherName();
            it->Detach(name.c_str(), true);
            s_rebuild = true;
        }
        break;
    case Act::Unload:
        unload(it);
        for (PIItem m : at.cell->more)
            unload(m);
        play(SndUnload);
        s_rebuild = true;
        break;
    }
}

// ------------------------------------------------------ the search screen ----
//
// ⭐CUICarBodyWnd's actions, carried over the same way. Everything moves by
// network events (GE_TRANSFER_REJECT / GE_TRANSFER_TAKE through move_item), so
// nothing here edits a list: the inventories change a frame later and InvFrame
// notices their ModifyFrame stamps.

u16 cb_our_id() { return Actor() ? Actor()->ID() : u16(-1); }

u16 cb_other_id()
{
    if (s_cb_box)
        return s_cb_box->object().ID();
    if (auto go = smart_cast<CGameObject*>(s_cb_others))
        return go->ID();
    return u16(-1);
}

// The legacy TransferItem: to a monster never; to a living owner only within
// what it can carry when asked to check.
bool cb_transfer(PIItem it, CInventoryOwner* from, CInventoryOwner* to, bool check)
{
    auto go_from = smart_cast<CGameObject*>(from);
    auto go_to = smart_cast<CGameObject*>(to);
    if (!go_from || !go_to || smart_cast<CBaseMonster*>(go_to))
        return false;
    if (check)
    {
        const float inv_w = to->inventory().CalcTotalWeight();
        const float max_w = to->inventory().GetMaxWeight();
        if (inv_w + it->Weight() >= max_w)
            return false;
    }
    move_item(go_from->ID(), go_to->ID(), it->object().ID());
    return true;
}

// One item from wherever it is to the other side.
bool cb_move_one(PIItem it, bool from_others)
{
    if (s_cb_others)
    {
        CInventoryOwner* us = smart_cast<CInventoryOwner*>(Actor());
        return from_others ? cb_transfer(it, s_cb_others, us, false) : cb_transfer(it, us, s_cb_others, true);
    }
    if (s_cb_box)
    {
        move_item(from_others ? cb_other_id() : cb_our_id(), from_others ? cb_our_id() : cb_other_id(), it->object().ID());
        return true;
    }
    return false;
}

// MoveItem / MoveItems: the selected cell, or its whole stack.
void cb_move(u16 id, bool all)
{
    const Located at = locate(id);
    if (!at.cell)
        return;
    const bool from_others = at.list->kind == Kind::Others;
    Msg("~ [rml] carbody: move %s%s %s", at.cell->item->object().cNameSect().c_str(), all ? " (whole stack)" : "", from_others ? "to us" : "away");
    if (all)
        for (PIItem m : at.cell->more)
            cb_move_one(m, from_others);
    cb_move_one(at.cell->item, from_others);
    s_sel = u16(-1);
    play(SndMove);
    s_rebuild = true;
}

void cb_take_all()
{
    for (List& l : lists())
        if (l.kind == Kind::Others)
            for (const Cell& c : l.cells)
            {
                for (PIItem m : c.more)
                    cb_move_one(m, true);
                cb_move_one(c.item, true);
            }
    Msg("~ [rml] carbody: take all");
    play(SndMove);
    s_rebuild = true;
}

// DropItemsfromCell: thrown on the ground from whichever side it is on.
void cb_drop(bool all)
{
    const Located at = locate(s_sel);
    if (!at.cell)
        return;
    const u16 from_id = at.list->kind == Kind::Others ? cb_other_id() : cb_our_id();
    auto drop_one = [from_id](PIItem it) {
        it->SetDropManual(TRUE);
        NET_Packet P;
        it->object().u_EventGen(P, GE_OWNERSHIP_REJECT, from_id);
        P.w_u16(it->object().ID());
        it->object().u_EventSend(P);
    };
    Msg("~ [rml] carbody: drop %s%s", at.cell->item->object().cNameSect().c_str(), all ? " (whole stack)" : "");
    if (all)
        for (PIItem m : at.cell->more)
            drop_one(m);
    drop_one(at.cell->item);
    s_sel = u16(-1);
    play(SndDrop);
    s_rebuild = true;
}

// Eaten from the other side: taken first, then eaten (the legacy EatItem).
void cb_eat()
{
    const Located at = locate(s_sel);
    if (!at.cell || !Actor())
        return;
    PIItem it = at.cell->item;
    if (at.list->kind == Kind::Others)
        move_item(cb_other_id(), Actor()->ID(), it->object().ID());
    NET_Packet P;
    CGameObject::u_EventGen(P, GEG_PLAYER_ITEM_EAT, Actor()->ID());
    P.w_u16(it->object().ID());
    CGameObject::u_EventSend(P);
    s_sel = u16(-1);
    play(SndUse);
    s_rebuild = true;
}

// The legacy ActivatePropertiesBox of the search screen, in its order.
void build_cb_menu(const Located& at)
{
    s_menu.clear();
    PIItem it = at.cell->item;
    auto add = [](LPCSTR st, Act act, u32 data = 0) { s_menu.push_back({CStringTable().translate(st).c_str(), act, data}); };

    if (auto wpn = smart_cast<CWeapon*>(it))
    {
        if (wpn->GrenadeLauncherAttachable() && wpn->IsGrenadeLauncherAttached())
            add("st_detach_gl", Act::DetachGL);
        if (wpn->ScopeAttachable() && wpn->IsScopeAttached())
            add("st_detach_scope", Act::DetachScope);
        if (wpn->SilencerAttachable() && wpn->IsSilencerAttached())
            add("st_detach_silencer", Act::DetachSilencer);
        bool loaded = can_unload(it);
        for (PIItem m : at.cell->more)
            loaded = loaded || can_unload(m);
        if (loaded && smart_cast<CWeaponMagazined*>(wpn))
            add("st_unload_magazine", Act::Unload);
    }

    if (smart_cast<CMedkit*>(it) || smart_cast<CAntirad*>(it))
        add("st_use", Act::Eat);
    else if (smart_cast<CEatableItem*>(it))
        add(smart_cast<CBottleItem*>(it) ? "st_drink" : "st_eat", Act::Eat);

    const bool many = !at.cell->more.empty();
    add("st_move", Act::Move);
    if (many)
        add("st_move_all", Act::MoveAll);
    add("st_drop", Act::Drop);
    if (many)
        add("st_drop_all", Act::DropAll);
}

// ------------------------------------------------------- the trade screen ----
//
// ⭐CUITradeWnd, carried over. The table is a selection, not a place: an item
// "on the table" is still in its owner's inventory until PerformTrade, which
// hands every one of them to CTrade::TransferItem -- the call that pays, moves
// and raises the mod's trade_sell_buy_item callback.

PIItem item_by_id(u16 id) { return smart_cast<PIItem>(Level().Objects.net_Find(id)); }

float tr_offer_weight(const xr_set<u16>& offer)
{
    float w = 0.f;
    for (u16 id : offer)
        if (PIItem it = item_by_id(id))
            w += it->Weight();
    return w;
}

// CanMoveToOther: never what is marked untradable; what the actor offers must be
// something the partner buys at all, in good enough condition, and must fit in
// what the partner can carry after the deal.
// ⭐Stolen goods (loose gamedata\scripts\ogsr_stolen.script): items the player
// took off the body of a man he murdered carry a mark that fades with game time.
// Presence is checked once -- asking the script engine about a missing file
// would try to load it for every cell -- and the functor is looked up per call,
// because a load recreates Lua. The script must always return a number/boolean:
// a nil cannot be converted and luabind throws.
bool stolen_script()
{
    static const bool present = FS.exist("$game_scripts$", "ogsr_stolen.script") != nullptr;
    return present;
}

// Game hours until the item's mark fades; negative for a clean item.
float stolen_hours(PIItem it)
{
    if (!it || !stolen_script())
        return -1.f;
    luabind::functor<float> f;
    if (!ai().script_engine().functor("ogsr_stolen.hours_left", f))
        return -1.f;
    return f(int(it->object().ID()));
}

// Will the trader on the other side take this item off the player? A faction
// does not buy back what was taken from its own dead.
bool stolen_trader_takes(PIItem it)
{
    if (!it || !s_tr_others || !stolen_script())
        return true;
    auto go = smart_cast<CGameObject*>(s_tr_others);
    luabind::functor<bool> f;
    if (!go || !ai().script_engine().functor("ogsr_stolen.trader_takes", f))
        return true;
    return f(go->lua_game_object(), int(it->object().ID()));
}

bool tr_can_move(PIItem it, bool ours)
{
    if (it->m_flags.test(CInventoryItem::FIAlwaysUntradable))
        return false;
    if (!ours)
        return true;
    if (!s_tr_others || !s_tr_others_inv)
        return false;
    if (!stolen_trader_takes(it))
        return false;

    const shared_str& sect = it->object().cNameSect();
    if (!s_tr_others->trade_parameters().enabled(CTradeParameters::action_buy(0), sect))
        return false;
    if (it->GetCondition() < s_tr_others->trade_parameters().factors(CTradeParameters::action_buy(0), sect).min_condition())
        return false;

    const float r1 = tr_offer_weight(s_tr_our_offer), r2 = tr_offer_weight(s_tr_their_offer);
    if (s_tr_others_inv->CalcTotalWeight() - r2 + r1 + it->Weight() > s_tr_others_inv->GetMaxWeight())
        return false;
    return true;
}

u32 tr_price(const xr_set<u16>& offer, bool buying)
{
    u32 sum = 0;
    if (s_tr_others_trade)
        for (u16 id : offer)
            if (PIItem it = item_by_id(id))
                sum += s_tr_others_trade->GetItemPrice(it, buying);
    return sum;
}

// UpdatePrices: both totals, both purses.
void tr_update_prices()
{
    if (!s_tr_our || !s_tr_others)
        return;
    s_tr_our_price = tr_price(s_tr_our_offer, true);
    s_tr_their_price = tr_price(s_tr_their_offer, false);
    if (!s_tr_others->InfinitiveMoney() && s_tr_zero_trade)
        s_tr_our_price = std::min(s_tr_our_price, s_tr_others->get_money());

    const xr_string money = CStringTable().translate("ui_st_money_descr").c_str();
    auto amount = [&money](u32 v) { return xr_string(std::to_string(v).c_str()) + " " + money; };
    set_text("our-price", amount(s_tr_our_price).c_str());
    set_text("their-price", amount(s_tr_their_price).c_str());
    set_text("our-money", amount(s_tr_our->get_money()).c_str());
    set_text("their-money", s_tr_others->InfinitiveMoney() ? "---" : amount(s_tr_others->get_money()).c_str());

    // What the deal does to the actor's purse (trade_money of True Stalker): plus = paid to us, minus = we pay.
    if (Rml::Element* sum = by_id("trade-sum"))
    {
        const s64 net = s64(s_tr_our_price) - s64(s_tr_their_price);
        const xr_string text = (net > 0 ? "+" : net < 0 ? "-" : "") + amount(u32(net < 0 ? -net : net));
        set_text("trade-sum", text.c_str());
        sum->SetClass("give", net > 0);
        sum->SetClass("lose", net < 0);
    }
}

// MoveItem / MoveItems: a bag item onto its owner's side of the table (if it may
// go), a table item back into its owner's bag.
void tr_move(u16 id, bool all)
{
    const Located at = locate(id);
    if (!at.cell)
        return;
    xr_vector<PIItem> what{at.cell->item};
    if (all)
        what.insert(what.end(), at.cell->more.begin(), at.cell->more.end());

    switch (at.list->kind)
    {
    case Kind::Bag:
    case Kind::Others: {
        const bool ours = at.list->kind == Kind::Bag;
        // The check is on the cell's own item, as in the legacy MoveItems.
        if (!tr_can_move(at.cell->item, ours))
        {
            Msg("~ [rml] trade: %s cannot go on the table", at.cell->item->object().cNameSect().c_str());
            return;
        }
        for (PIItem it : what)
            (ours ? s_tr_our_offer : s_tr_their_offer).insert(it->object().ID());
    }
    break;
    case Kind::OurOffer:
    case Kind::TheirOffer:
        for (PIItem it : what)
            (at.list->kind == Kind::OurOffer ? s_tr_our_offer : s_tr_their_offer).erase(it->object().ID());
        break;
    default: return;
    }
    Msg("~ [rml] trade: %s%s from '%s'", at.cell->item->object().cNameSect().c_str(), all ? " (whole stack)" : "", at.list->id);
    s_sel = u16(-1);
    play(SndMove);
    s_rebuild = true;
}

// TransferItems: everything on one side of the table changes hands.
void tr_transfer(xr_set<u16>& offer, bool buying)
{
    for (u16 id : offer)
        if (PIItem it = item_by_id(id))
        {
            it->m_highlight_equipped = false; // no longer "worn" once sold
            s_tr_others_trade->TransferItem(it, buying, !s_tr_zero_trade);
        }
    offer.clear();
    if (!s_tr_zero_trade)
    {
        s_tr_others_trade->pThis.inv_owner->set_money(s_tr_others_trade->pThis.inv_owner->get_money(), true);
        s_tr_others_trade->pPartner.inv_owner->set_money(s_tr_others_trade->pPartner.inv_owner->get_money(), true);
    }
}

// PerformTrade.
void tr_perform()
{
    if (!s_tr_our || !s_tr_others || !s_tr_others_trade)
        return;
    if (s_tr_our_offer.empty() && s_tr_their_offer.empty())
        return;
    tr_update_prices();

    int our_money = int(s_tr_our->get_money());
    int others_money = int(s_tr_others->get_money());
    const int delta = int(s_tr_our_price) - int(s_tr_their_price);
    our_money += delta;
    others_money -= s_tr_others->InfinitiveMoney() ? 0 : delta;

    if (our_money >= 0 && others_money >= 0)
    {
        Msg("~ [rml] trade: deal -- we give %u, they give %u", s_tr_our_price, s_tr_their_price);
        s_tr_others_trade->OnPerformTrade(s_tr_their_price, s_tr_our_price);
        tr_transfer(s_tr_our_offer, true);
        tr_transfer(s_tr_their_offer, false);
        if (s_tr_zero_trade)
        {
            s_tr_others_trade->pThis.inv_owner->set_money(others_money, true);
            s_tr_others_trade->pPartner.inv_owner->set_money(our_money, true);
        }
        play(SndTrade);
    }
    else
    {
        // The legacy message, through the mod's own statics; the game removes it
        // when its two seconds are up.
        Msg("~ [rml] trade: refused -- not enough money (%s)", others_money < 0 ? "theirs" : "ours");
        if (g_hud)
            if (CUI* ui = g_hud->GetUI())
                if (CUIGameCustom* game = ui->UIGame())
                    if (SDrawStaticStruct* msg = game->AddCustomStatic(others_money < 0 ? "not_enough_money_other" : "not_enough_money_mine", true))
                        msg->m_endTime = Device.fTimeGlobal + 2.f;
        play(SndTradeBlock);
    }
    s_sel = u16(-1);
    s_rebuild = true;
}

void tr_clear_table()
{
    s_tr_our_offer.clear();
    s_tr_their_offer.clear();
    hide_menu();
    s_rebuild = true;
}

void build_tr_menu(const Located& at)
{
    s_menu.clear();
    s_menu.push_back({CStringTable().translate("st_move").c_str(), Act::Move});
    if (!at.cell->more.empty())
        s_menu.push_back({CStringTable().translate("st_move_all").c_str(), Act::MoveAll});
}

// -------------------------------------------------------------- the talk ----
//
// ⭐THE TALK, WHOLE. CUITalkWnd drove the dialog and CUITalkDialogWnd drew it;
// both are gone (03-10), and their dialog driving is here line for line: the
// two CPhraseDialogManager halves, topics and phrases, the partner's voice, the
// PDA log entry for every line, the end of the talk when the partner walks off.
// The phrase managers and CPhraseDialog are the dialog ENGINE and stay as they
// are -- only who asks them and who shows the answers changed.

struct TalkLine
{
    xr_string name, text; // cp1251, translated
    bool actor{};
    xr_string icon; // iconed answers (AddIconedMessage): a texture and a rectangle in it
    Frect rect{};
};
struct TalkQuestion
{
    xr_string text, value;
    int number{}; // 1-based; keys 1..0 answer the first ten
};
xr_vector<TalkLine> s_talk_log;
xr_vector<TalkQuestion> s_talk_q;
xr_string s_talk_our, s_talk_other;
bool s_talk_osoznanie = false;
// The answer the pad points at (class `cur` on its .q), -1 = none. Back to the top on every rebuild.
int s_talk_cur = -1;
// ⚠Clicks are acted on in the next frame, never inside the event dispatch: a
// phrase can end the talk or start the trade, and either closes this document.
xr_string s_talk_pending_q;
bool s_talk_pending_q_set = false, s_talk_pending_trade = false;

// The dialog (CUITalkWnd's members).
bool s_tk_active = false; // a talk is running -- also while its trade screen is up
CActor* s_tk_actor = nullptr;
CInventoryOwner* s_tk_our = nullptr;
CInventoryOwner* s_tk_other = nullptr;
CPhraseDialogManager* s_tk_our_mgr = nullptr;
CPhraseDialogManager* s_tk_other_mgr = nullptr;
DIALOG_SHARED_PTR s_tk_dialog; // null: topic mode
bool s_tk_need_questions = false;
// ⚠On the heap and never freed, for the same reason as the screen sounds.
ref_sound* const s_tk_voice = new ref_sound();

// An engine colour code -- a name from color_defs.xml or "a,r,g,b" / "r,g,b".
bool legacy_colour(const xr_string& code, u32& argb)
{
    if (const CUIXmlInit::ColorDefs* defs = CUIXmlInit::GetColorDefs())
    {
        const auto it = defs->find(shared_str(code.c_str()));
        if (it != defs->end())
        {
            argb = it->second;
            return true;
        }
    }
    int v[4]{};
    const int n = sscanf(code.c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
    if (n == 4)
        argb = color_argb(v[0], v[1], v[2], v[3]);
    else if (n == 3)
        argb = color_argb(255, v[0], v[1], v[2]);
    else
        return false;
    return true;
}

// ⭐Legacy "complex mode" text to RML: the dialogs colour words with %c[...]
// (CUILines::GetColorFromText) and break lines with a literal \n. Escaped first,
// so a phrase with '<' or '&' is text, not markup.
xr_string legacy_text_to_rml(LPCSTR cp1251)
{
    const xr_string s = ToUtf8(cp1251 ? cp1251 : "").c_str();
    xr_string out;
    bool open = false;
    for (size_t i = 0; i < s.size();)
    {
        if (s.compare(i, 3, "%c[") == 0)
        {
            const size_t end = s.find(']', i + 3);
            if (end != xr_string::npos)
            {
                const xr_string code = s.substr(i + 3, end - i - 3);
                if (open)
                {
                    out += "</span>";
                    open = false;
                }
                u32 argb;
                if (code != "default" && legacy_colour(code, argb))
                {
                    char buf[64];
                    xr_sprintf(buf, sizeof(buf), "<span style=\"color: #%02x%02x%02x%02x;\">", color_get_R(argb), color_get_G(argb), color_get_B(argb), color_get_A(argb));
                    out += buf;
                    open = true;
                }
                i = end + 1;
                continue;
            }
        }
        if (s.compare(i, 2, "\\n") == 0)
        {
            out += "<br/>";
            i += 2;
            continue;
        }
        const char c = s[i++];
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    if (open)
        out += "</span>";
    return out;
}

void talk_changed()
{
    if (s_screen == Screen::Talk)
        s_rebuild = true;
}

void talk_rebuild()
{
    set_text("talk-our", s_talk_our.c_str());
    set_text("talk-other", s_talk_other.c_str());
    if (Rml::Element* body = s_doc ? s_doc->GetElementById("talk") : nullptr)
    {
        body->SetClass("osoznanie", s_talk_osoznanie);
#ifdef __SWITCH__
        body->SetClass("nx", true); // the pad hints
#endif
    }

    if (Rml::Element* log = by_id("answers"))
    {
        xr_string rml;
        for (const TalkLine& l : s_talk_log)
        {
            rml += l.actor ? "<div class=\"ans actor\">" : "<div class=\"ans other\">";
            if (!l.icon.empty())
            {
                xr_string src = l.icon;
                for (char& ch : src)
                    if (ch == '\\')
                        ch = '/';
                char rect[96];
                // ⚠The rectangle's x2/y2 are a WIDTH and a HEIGHT here -- the
                // legacy SetOriginalRect(x, y, w, h) it was handed to.
                xr_sprintf(rect, sizeof(rect), "%.0f %.0f %.0f %.0f", l.rect.x1, l.rect.y1, l.rect.x2, l.rect.y2);
                rml += xr_string("<img class=\"ico\" src=\"") + src + "\" rect=\"" + rect + "\"/>";
            }
            else
                rml += "<div class=\"who\">" + legacy_text_to_rml(l.name.c_str()) + "</div>";
            rml += "<div class=\"txt\">" + legacy_text_to_rml(l.text.c_str()) + "</div></div>";
        }
        log->SetInnerRML(rml.c_str());
    }

#ifdef __SWITCH__
    s_talk_cur = s_talk_q.empty() ? -1 : 0; // the pad always points at one
#else
    s_talk_cur = -1; // until an arrow key is pressed
#endif
    if (Rml::Element* list = by_id("questions"))
    {
        xr_string rml;
        for (size_t i = 0; i < s_talk_q.size(); ++i)
        {
            // The number is the key that picks it (1..9, then 0), as True Stalker prints it.
            const int key = s_talk_q[i].number;
            const bool numbered = Core.Features.test(xrCore::Feature::show_dialog_numbers);
            const xr_string num = (numbered && key >= 1 && key <= 10) ? xr_string(std::to_string(key % 10).c_str()) + "." : xr_string();
            rml += xr_string(int(i) == s_talk_cur ? "<div class=\"q cur\" data-q=\"" : "<div class=\"q\" data-q=\"") + std::to_string(i).c_str() + "\"><span class=\"qn\">" + num + "</span><span class=\"qt\">" +
                legacy_text_to_rml(s_talk_q[i].text.c_str()) + "</span></div>";
        }
        list->SetInnerRML(rml.c_str());
    }

    // The newest line in view, as the legacy list's ScrollToEnd.
    s_doc->UpdateDocument();
    if (Rml::Element* log = by_id("answers"))
        log->SetScrollTop(log->GetScrollHeight());
}

// --- the partner's voice (CUITalkWnd::PlaySnd / StopSnd) ---
void tk_stop_voice()
{
    if (s_tk_actor && s_tk_actor->OnDialogSoundHandlerStop(s_tk_other))
        return;
    if (s_tk_voice->_feedback())
        s_tk_voice->stop();
}

void tk_play_voice(LPCSTR text)
{
    if (!text || !xr_strlen(text))
        return;
    tk_stop_voice();
    string_path fn;
    strconcat(sizeof(fn), fn, "characters_voice\\dialogs\\", text, ".ogg");
    if (FS.exist("$game_sounds$", fn) && s_tk_actor && !s_tk_actor->OnDialogSoundHandlerStart(s_tk_other, fn))
    {
        s_tk_voice->create(fn, st_Effect, sg_SourceType);
        s_tk_voice->play(nullptr, sm_2D);
    }
}

// --- the log ---
void tk_log_line(LPCSTR speaker, LPCSTR text, bool actor)
{
    TalkLine l;
    l.name = speaker ? speaker : "";
    l.text = text ? text : "";
    l.actor = actor;
    s_talk_log.push_back(std::move(l));
    talk_changed();

    // CUITalkDialogWnd::AddAnswer: every line also goes to the PDA log, with the
    // speaker's portrait.
    CActor* actor_obj = Actor();
    CInventoryOwner* who = actor ? s_tk_our : s_tk_other;
    auto who_go = smart_cast<CGameObject*>(who);
    if (!actor_obj || !who_go)
        return;
    GAME_NEWS_DATA news;
    news.m_type = GAME_NEWS_DATA::eTalk;
    xr_string line = speaker ? speaker : "";
    line += " >> ";
    line += text ? text : "";
    news.news_text = line.c_str();
    if (CSE_ALifeTraderAbstract* T = ch_info_get_from_id(who_go->ID()))
    {
        CCharacterInfo info;
        info.Init(T);
        news.texture_name = info.IconName();
        Frect r = CUITextureMaster::GetTextureRect(info.IconName().c_str());
        r.x2 = r.width();
        r.y2 = r.height();
        news.tex_rect = r;
    }
    actor_obj->PushNewsData(news);
    if (auto sp = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame()))
        sp->PdaMenu->UIDiaryWnd->AddNews();
}

// CUITalkWnd::AddAnswer: a phrase id, voiced, translated, logged.
void tk_add_answer(const shared_str& text, LPCSTR speaker)
{
    if (!text.size())
        return; // an empty phrase shows nothing at all
    tk_play_voice(text.c_str());
    const bool i_am = s_tk_our && 0 == xr_strcmp(speaker, s_tk_our->Name());
    tk_log_line(speaker, CStringTable().translate(text).c_str(), i_am);
}

// CUITalkWnd::AddQuestion + CUITalkDialogWnd::AddQuestion: translated, numbered
// when the feature asks for it.
void tk_add_question(const shared_str& text, const shared_str& value, int& number)
{
    if (!text.size())
        return;
    ++number;
    // The number (show_dialog_numbers) is NOT written into the text: talk_rebuild prints it in its own column (.qn).
    s_talk_q.push_back({CStringTable().translate(text).c_str(), value.c_str(), number});
    talk_changed();
}

void tk_to_topic() { s_tk_dialog = DIALOG_SHARED_PTR((CPhraseDialog*)nullptr); }

void tk_say(const shared_str& phrase_id)
{
    tk_add_answer(s_tk_dialog->GetPhraseText(phrase_id), s_tk_our->Name());
    s_tk_our_mgr->SayPhrase(s_tk_dialog, phrase_id);
    if (s_tk_dialog && s_tk_dialog->IsFinished())
        tk_to_topic();
}

void tk_update_questions()
{
    s_talk_q.clear();
    talk_changed();

    if (!s_tk_dialog)
    {
        s_tk_our_mgr->UpdateAvailableDialogs(s_tk_other_mgr);
        int number = 0;
        for (const DIALOG_SHARED_PTR& d : s_tk_our_mgr->AvailableDialogs())
            tk_add_question(d->DialogCaption(), d->GetDialogID(), number);
    }
    else if (s_tk_dialog->IsWeSpeaking(s_tk_our_mgr))
    {
        // One dummy phrase only: just say it, the player does nothing.
        if (!s_tk_dialog->PhraseList().empty() && s_tk_dialog->allIsDummy())
        {
            CPhrase* phrase = s_tk_dialog->PhraseList()[Random.randI(s_tk_dialog->PhraseList().size())];
            tk_say(phrase->GetID());
        }
        if (s_tk_dialog && !s_tk_dialog->allIsDummy())
        {
            int number = 0;
            for (CPhrase* phrase : s_tk_dialog->PhraseList())
                tk_add_question(phrase->GetText(), phrase->GetID(), number);
        }
        else
            tk_update_questions();
    }
    s_tk_need_questions = false;
}

// CUITalkWnd::AskQuestion.
void tk_ask(const shared_str& id)
{
    if (s_tk_need_questions)
        return; // quick double click
    shared_str phrase_id;
    if (!s_tk_dialog)
    {
        if (!id.size() || !s_tk_our_mgr->HaveAvailableDialog(id))
        {
            Msg("! [rml] talk: '%s' is not an available dialog", id.c_str());
            return;
        }
        s_tk_dialog = s_tk_our_mgr->GetDialogByID(id);
        s_tk_our_mgr->InitDialog(s_tk_other_mgr, s_tk_dialog);
        phrase_id = "0";
    }
    else
        phrase_id = id;
    tk_say(phrase_id);
    s_tk_need_questions = true;
}

// CUITalkWnd::InitOthersStartDialog: the partner opens if it has something to say.
void tk_others_start()
{
    s_tk_other_mgr->UpdateAvailableDialogs(s_tk_our_mgr);
    if (s_tk_other_mgr->AvailableDialogs().empty())
        return;
    s_tk_dialog = s_tk_other_mgr->AvailableDialogs().front();
    s_tk_other_mgr->InitDialog(s_tk_our_mgr, s_tk_dialog);
    tk_add_answer(s_tk_dialog->GetPhraseText("0"), s_tk_other->Name());
    s_tk_other_mgr->SayPhrase(s_tk_dialog, "0");
    if (!s_tk_dialog || s_tk_dialog->IsFinished())
        tk_to_topic();
}

// May the talk be broken off now? (CUITalkWnd::IR_OnKeyboardPress / OnKeyboard)
bool tk_may_break()
{
    if (Core.Features.test(xrCore::Feature::disable_dialog_break) && s_tk_dialog)
        return false;
    return !(s_tk_other && s_tk_other->NeedOsoznanieMode());
}

// The camera turns to the partner's head (UITalkWnd.cpp's UpdateCameraDirection).
void tk_face_partner()
{
    auto go = smart_cast<CGameObject*>(s_tk_other);
    auto actor = Actor();
    if (!go || !actor || !go->Visual())
        return;
    auto k = smart_cast<IKinematics*>(go->Visual());
    if (!k)
        return;
    const u16 bone = k->LL_BoneID("bip01_head");
    if (bone == BI_NONE)
        return;
    Fmatrix m;
    m.mul_43(go->XFORM(), k->LL_GetBoneInstance(bone).mTransform);
    CCameraBase* cam = actor->cam_Active();
    Fvector dir;
    dir.sub(m.c, cam->vPosition);
    float p, h;
    dir.getHP(h, p);
    if (angle_difference(cam->yaw, -h) > 0.2)
        cam->yaw = angle_inertion_var(cam->yaw, -h, 0.15f, 0.2f, PI_DIV_6, Device.fTimeDelta);
    if (angle_difference(cam->pitch, -p) > 0.2)
        cam->pitch = angle_inertion_var(cam->pitch, -p, 0.15f, 0.2f, PI_DIV_6, Device.fTimeDelta);
}

// The talk's keys: the number keys answer, F and Esc try to end the talk, X trades.
bool talk_key(int dik)
{
    if (dik >= DIK_1 && dik <= DIK_0)
    {
        const int number = dik - DIK_1 + 1; // DIK_0 follows DIK_9: number 10
        for (const TalkQuestion& q : s_talk_q)
            if (q.number == number)
            {
                s_talk_pending_q = q.value;
                s_talk_pending_q_set = true;
                break;
            }
        return true;
    }
    if (dik == DIK_ESCAPE || is_binded(kUSE, dik))
    {
        if (tk_may_break())
            UIRml::InvClose(); // ends the talk
        return true;
    }
    // The trade button's accelerator in the mod's talk.xml (accel="kX"); there is
    // no trade button in the "osoznanie" mode.
    if (dik == DIK_X)
    {
        if (!s_talk_osoznanie)
            s_talk_pending_trade = true;
        return true;
    }
    return is_binded(kINVENTORY, dik); // swallowed, as the legacy talk does
}

// ------------------------------------------------------------ selection ------

void select(u16 id)
{
    if (id == s_sel)
        return;
    s_sel = id;

    // The highlight moves without a rebuild: only the class changes.
    Rml::ElementList cells;
    if (s_doc)
        s_doc->GetElementsByClassName(cells, "ci");
    for (Rml::Element* c : cells)
        c->SetClass("sel", id_of(c) == id);

    const Located at = locate(id);
    if (at.cell && g_actor)
        g_actor->callback(GameObject::eCellItemSelect)(at.cell->item->object().lua_game_object());
}

// While an item is dragged, the lists that would take it light up (the legacy
// ShowSlotsHighlight).
void highlight_for(PIItem it, bool on)
{
    CInventory* inv = actor_inventory();
    for (List& l : lists())
    {
        Rml::Element* host = by_id(l.id);
        if (!host)
            continue;
        bool lit = false;
        if (on && inv && it)
        {
            if (l.kind == Kind::Belt)
                lit = it->m_flags.test(CInventoryItem::Fbelt) && !inv->InBelt(it);
            else if (l.kind == Kind::Slot)
                for (const u8 s : it->GetSlots())
                    if (s == l.slot && (!inv->InSlot(it) || it->GetSlot() != s))
                        lit = true;
        }
        host->SetClass("hl", lit);
        // A slot's whole panel lights, not only the box the picture sits in.
        if (Rml::Element* panel = host->GetParentNode(); panel && panel->IsClassSet("slot-panel"))
            panel->SetClass("hl", lit);
    }
}

// --------------------------------------------------------- the suit panel ----
//
// ⭐CUIOutfitInfo::Update: what the worn suit and the artefacts on the belt do
// to the actor. Rows are declared in the markup (data-param = the legacy
// static_<name>), so a mod that leaves a row out of its screen loses it here too.
constexpr const char* s_param_names[] = {
    "health_restore_speed", "radiation_restore_speed", "satiety_restore_speed", "thirst_restore_speed", "power_restore_speed", "bleeding_restore_speed", "psy_health_restore_speed",
    "burn_immunity",        "shock_immunity",          "strike_immunity",       "wound_immunity",       "radiation_immunity",  "telepatic_immunity",     "chemical_burn_immunity",
    "explosion_immunity",   "fire_wound_immunity",
};
constexpr const char* s_param_captions[] = {
    "ui_inv_health",
    "ui_inv_radiation",
    "ui_inv_satiety",
    "ui_inv_thirst",
    "ui_inv_power",
    "ui_inv_bleeding",
    "ui_inv_psy_health",
    "ui_inv_outfit_burn_protection",
    "ui_inv_outfit_shock_protection",
    "ui_inv_outfit_strike_protection",
    "ui_inv_outfit_wound_protection",
    "ui_inv_outfit_radiation_protection",
    "ui_inv_outfit_telepatic_protection",
    "ui_inv_outfit_chemical_burn_protection",
    "ui_inv_outfit_explosion_protection",
    "ui_inv_outfit_fire_wound_protection",
};
constexpr const char* s_actor_params[] = {"satiety_health_v", "radiation_v", "satiety_v", "thirst_v", "satiety_power_v", "wound_incarnation_v", "psy_health_v"};
constexpr u32 RESTORE_COUNT = 7;

float restore_param(const ActorRestoreParams& p, u32 i)
{
    switch (i)
    {
    case 0: return p.HealthRestoreSpeed;
    case 1: return p.RadiationRestoreSpeed;
    case 2: return p.SatietyRestoreSpeed;
    case 3: return p.ThirstRestoreSpeed;
    case 4: return p.PowerRestoreSpeed;
    case 5: return p.BleedingRestoreSpeed;
    case 6: return p.PsyHealthRestoreSpeed;
    }
    return 0.f;
}

void refresh_outfit_info()
{
    CActor* actor = Actor();
    Rml::Element* panel = by_id("outfit-info");
    if (!actor || !panel)
        return;
    auto outfit = smart_cast<CCustomOutfit*>(actor->inventory().m_slots[OUTFIT_SLOT].m_pIItem);
    const auto af = actor->ActiveArtefactsOnBelt();

    Rml::ElementList rows;
    panel->GetElementsByClassName(rows, "oi");
    for (Rml::Element* row : rows)
    {
        const Rml::String name = row->GetAttribute<Rml::String>("data-param", "");
        u32 i = 0;
        while (i < std::size(s_param_names) && name != s_param_names[i])
            ++i;
        if (i >= std::size(s_param_names))
        {
            row->SetProperty("display", "none");
            continue;
        }

        float v_outfit = 0.f, v_af = 0.f;
        if (i < RESTORE_COUNT)
        {
            const float base = READ_IF_EXISTS(pSettings, r_float, "actor_condition", s_actor_params[i], 0.f);
            v_outfit = base != 0.f ? restore_param(af, i) / base : 0.f;
        }
        else
        {
            const auto hit = ALife::EHitType(i - RESTORE_COUNT);
            v_outfit = 1.f - (outfit ? outfit->GetDefHitTypeProtection(hit) : 1.f);
            v_af = 1.f - actor->HitArtefactsOnBelt(1.f, hit);
        }

        if (fsimilar(v_outfit, 0.f) && fsimilar(v_af, 0.f))
        {
            row->SetProperty("display", "none");
            continue;
        }

        const char* unit = "";
        if (i != 1 /*radiation restore*/ && i != 4 /*power restore*/)
        {
            v_outfit *= 100.f;
            v_af *= 100.f;
            unit = "%";
        }
        if (i == 5 /*bleeding*/)
            v_outfit = -v_outfit;

        // Green is good -- except for bleeding and radiation, where more is worse.
        bool good = v_outfit > 0.f;
        if (i == 5 || i == 1)
            good = !good;

        char val[64];
        xr_sprintf(val, sizeof(val), "%+3.0f%s", v_outfit, unit);
        xr_string rml = ToUtf8(CStringTable().translate(s_param_captions[i]).c_str()).c_str();
        rml += xr_string(" <span class=\"") + (good ? "good" : "bad") + "\">" + val + "</span>";
        if (!fsimilar(v_af, 0.f))
        {
            xr_sprintf(val, sizeof(val), "%+3.0f%%", v_af);
            rml += xr_string(" <span class=\"") + (v_af > 0.f ? "good" : "bad") + "\">" + val + "</span>";
        }
        if (Rml::Element* t = row->GetElementById(Rml::String("oi-") + name))
            t->SetInnerRML(rml.c_str());
        else if (row->GetNumChildren() > 1)
            row->GetChild(1)->SetInnerRML(rml.c_str());
        row->SetProperty("display", "block");
    }
}

// The numbers around the edges: weight, money, the clock. All of them can change
// while the screen is open (food is eaten, time passes), so they are refreshed
// every frame rather than at build time.
void refresh_stats()
{
    auto entity = smart_cast<CEntityAlive*>(Level().CurrentEntity());
    if (!entity)
        return;

    set_pct("bar-health", entity->conditions().GetHealth());
    set_pct("bar-psy", entity->conditions().GetPsyHealth());
    set_pct("bar-power", entity->conditions().GetPower());
    set_pct("bar-radiation", entity->conditions().GetRadiation());

    auto owner = smart_cast<CInventoryOwner*>(entity);
    if (!owner)
        return;

    string256 buf;
    xr_sprintf(buf, sizeof(buf), "%d %s", owner->get_money(), CStringTable().translate("ui_st_money_descr").c_str());
    set_text("money", buf);

    // ⭐The same numbers InventoryUtilities::UpdateWeight prints (with its
    // "ui_inv_weight" prefix), with the colour as a class instead of %c[red].
    const float total = owner->inventory().CalcTotalWeight();
    const float max = owner->MaxCarryWeight();
    if (auto e = by_id("weight"))
    {
        xr_sprintf(buf, sizeof(buf), "%3.1f/%5.1f", total, max);
        xr_string rml = ToUtf8(CStringTable().translate("ui_inv_weight").c_str()).c_str();
        rml += xr_string(" <span class=\"w\">") + buf + "</span>";
        e->SetInnerRML(rml.c_str());
        e->SetClass("over", total > max);
    }

    set_text("clock", InventoryUtilities::GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str());
}

u16 s_focus = u16(-1); // the cell the scripts were last told about
float s_mouse_x = 0.f, s_mouse_y = 0.f; // the pointer, in context pixels
u16 s_focus_want = u16(-1); // the cell under the pointer now

void rebuild()
{
    CInventory* inv = actor_inventory();
    if (!inv || !s_doc)
        return;

    if (s_screen == Screen::Talk)
    {
        talk_rebuild();
        s_rebuild = false;
        return;
    }
    if (s_screen == Screen::Pda || s_screen == Screen::Dialog)
    {
        s_rebuild = false; // the PDA rebuilds its own panes (ui_rml_pda.cpp), a script dialog its own markup
        return;
    }

    // A selection that no longer resolves (eaten, dropped) is no selection. On the
    // search screen it may legitimately belong to the other side.
    if (s_sel != u16(-1))
    {
        auto sel = smart_cast<PIItem>(Level().Objects.net_Find(s_sel));
        if (!sel || (s_screen == Screen::Inventory && sel->m_pCurrentInventory != inv))
            s_sel = u16(-1);
    }

    if (s_screen == Screen::CarBody)
    {
        s_cb_our_stamp = inv->ModifyFrame();
        s_cb_other_stamp = s_cb_others ? s_cb_others->inventory().ModifyFrame() : 0;
    }
    if (s_screen == Screen::Trade)
    {
        s_tr_our_stamp = inv->ModifyFrame();
        s_tr_other_stamp = s_tr_others_inv ? s_tr_others_inv->ModifyFrame() : 0;
        // What left either inventory is off the table too.
        auto prune = [](xr_set<u16>& offer, CInventory* owner) {
            for (auto it = offer.begin(); it != offer.end();)
            {
                PIItem item = item_by_id(*it);
                if (!item || item->m_pCurrentInventory != owner)
                    it = offer.erase(it);
                else
                    ++it;
            }
        };
        prune(s_tr_our_offer, inv);
        prune(s_tr_their_offer, s_tr_others_inv);
    }

    for (List& l : lists())
    {
        Rml::Element* host = by_id(l.id);
        if (!host)
            continue;

        // ⭐Geometry is the screen's business, not this file's: how many cells a
        // list holds and how big one is belong in the markup next to where the
        // list sits. Absent, a list is one cell of the atlas grid -- which is
        // exactly what every equipment slot is.
        l.capacity.x = std::max(1, host->GetAttribute<int>("data-cells-x", 1));
        l.capacity.y = std::max(1, host->GetAttribute<int>("data-cells-y", 1));
        l.cell.x = std::max(1, host->GetAttribute<int>("data-cell-w", INV_GRID_WIDTH));
        l.cell.y = std::max(1, host->GetAttribute<int>("data-cell-h", INV_GRID_HEIGHT));
        l.group = host->GetAttribute<int>("data-group", l.kind != Kind::Slot ? 1 : 0) != 0;
        l.fit = host->GetAttribute<int>("data-fit", l.kind == Kind::Slot ? 1 : 0) != 0;
        l.unlimited = host->GetAttribute<int>("data-unlimited", 0) != 0;
        l.one_cell = host->GetAttribute<int>("data-one-cell", 0) != 0;
        l.bg = host->GetAttribute<int>("data-bg", 0) != 0;
        l.pad = host->GetAttribute<float>("data-pad", 0.f);
        l.gap = host->GetAttribute<float>("data-gap", 0.f);
        l.available = l.kind == Kind::Belt ? std::min(u32(l.capacity.x * l.capacity.y), inv->BeltSlotsCount()) : u32(-1);

        xr_vector<PIItem> items;
        if (s_screen == Screen::Trade)
        {
            // CUITradeWnd::UpdateLists: each side's tradeable items, split by
            // whether they lie on the table.
            const bool ours = (l.kind == Kind::Bag || l.kind == Kind::OurOffer);
            const bool table = (l.kind == Kind::OurOffer || l.kind == Kind::TheirOffer);
            const xr_set<u16>& offer = ours ? s_tr_our_offer : s_tr_their_offer;
            TIItemContainer list;
            if (CInventory* src = ours ? inv : s_tr_others_inv)
                src->AddAvailableItems(list, true);
            for (PIItem it : list)
                if ((offer.count(it->object().ID()) != 0) == table)
                    items.push_back(it);
            // The sort buttons over each bag (the table always shows everything on it).
            const Filter f = table ? Filter::All : (ours ? s_filter : s_filter_their);
            if (f != Filter::All)
                items.erase(std::remove_if(items.begin(), items.end(), [f](PIItem it) { return !it || classify(it) != f; }), items.end());
            std::sort(items.begin(), items.end(), InventoryUtilities::GreaterRoomInRuck);
            place(l, items);
            fill_list(l);
            continue;
        }
        if (s_screen == Screen::CarBody)
        {
            // CUICarBodyWnd::UpdateLists: the actor's tradeable items (bag, belt
            // and slots), the other side's everything; both sorted as the bag is.
            TIItemContainer list;
            if (l.kind == Kind::Others)
            {
                if (s_cb_others)
                    s_cb_others->inventory().AddAvailableItems(list, false);
                else if (s_cb_box && s_cb_box->m_in_use)
                    s_cb_box->AddAvailableItems(list);
            }
            else
                inv->AddAvailableItems(list, true);
            items.assign(list.begin(), list.end());
            std::sort(items.begin(), items.end(), InventoryUtilities::GreaterRoomInRuck);
            place(l, items);
            fill_list(l);
            continue;
        }
        switch (l.kind)
        {
        case Kind::Slot:
            if (l.slot < inv->m_slots.size())
                if (PIItem it = inv->m_slots[l.slot].m_pIItem)
                    items.push_back(it);
            break;

        case Kind::Belt: items.assign(inv->m_belt.begin(), inv->m_belt.end()); break;

        case Kind::Bag:
            items.assign(inv->m_ruck.begin(), inv->m_ruck.end());
            // ⭐Sorted the way the legacy window sorts it -- the ones needing most
            // room first, so the first fit above does not leave a hole no rifle
            // can use. Without this the same inventory lays out differently here
            // and there, and the two screens stop being comparable at a glance.
            std::sort(items.begin(), items.end(), InventoryUtilities::GreaterRoomInRuck);
            // The grenade slot is drawn in the bag -- "fake, sorry (((" is the
            // legacy comment. It is a slot with no place of its own on screen.
            if (GRENADE_SLOT < inv->m_slots.size())
                if (PIItem g = inv->m_slots[GRENADE_SLOT].m_pIItem)
                    items.push_back(g);
            // The sort buttons: only what the chosen category holds.
            if (s_filter != Filter::All)
                items.erase(std::remove_if(items.begin(), items.end(), [](PIItem it) { return !it || classify(it) != s_filter; }), items.end());
            break;

        case Kind::Others:
        case Kind::OurOffer:
        case Kind::TheirOffer: break; // the search and trade screens, handled above
        }

        place(l, items);
        fill_list(l);
    }

    // The pressed sort button wears `on`.
    if (s_screen == Screen::Inventory || s_screen == Screen::Trade)
    {
        Rml::ElementList buttons;
        s_doc->GetElementsByClassName(buttons, "flt");
        for (Rml::Element* b : buttons)
            b->SetClass("on", b->GetAttribute<Rml::String>("data-filter", "") == s_filter_names[int(filter_of(b))]);
    }

    refresh_stats();
    refresh_outfit_info();
    if (s_screen == Screen::Trade)
        tr_update_prices();
    s_rebuild = false;

    // The item under the pointer may have just moved away (taken, put on the table, "take all"): its old cell went
    // with no mouseout, so the scripts were never told and its description stayed up over an empty cell. If its
    // cell is no longer under the pointer, losing the focus here makes the sync raise eCellItemFocusLost.
    if (s_focus_want != u16(-1))
    {
        s_doc->UpdateDocument(); // the new cells' boxes
        bool under = false;
        Rml::ElementList cells;
        s_doc->GetElementsByClassName(cells, "ci");
        for (Rml::Element* c : cells)
            if (id_of(c) == s_focus_want)
            {
                const Rml::Vector2f o = c->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f s = c->GetBox().GetSize(Rml::BoxArea::Border);
                under = under || (s_mouse_x >= o.x && s_mouse_x <= o.x + s.x && s_mouse_y >= o.y && s_mouse_y <= o.y + s.y);
            }
        if (!under)
            s_focus_want = u16(-1);
    }
}

// ------------------------------------------------------- the script bridge ----
//
// ⭐WHAT THE LEGACY CELLS TOLD THE SCRIPTS. Every cell raised eCellItemFocus /
// eCellItemFocusLost when the pointer entered and left it (also mid-drag) and
// eOnCellItemMouse(item, x, y, DRAG_DROP_ITEM_DROP) when it was dropped; this
// mod's dsh_drag_drop turns that into on_drag_drop(item, onto) -- combining one
// item with another -- and ogsr_quick_slots and ui_inv_descr live off the focus.
// Raised here the same way, from the hover state of the cells.
//
// ⭐AND WHAT THE SCREENS OFFER SCRIPTS NOW. Any element may carry
// data-onclick="module.function" or data-ondrop="module.function": a click calls
// function(element_id), a drop calls function(element_id, item). That is how a
// script hangs a button or a drop target on a screen (the quick slots, the exo
// button) without a window of its own; the markup decides where it sits.
//
// ⚠⚠NEVER INTO LUA FROM INSIDE THE EVENT DISPATCH. A script can close the screen
// (ogsr_items_anims does, to play a use animation) and the element being
// dispatched on would be freed under the dispatch. Everything is queued and run at
// the top of the next frame.
struct LuaCall
{
    xr_string func, id;
    u16 item{u16(-1)};
};
xr_vector<LuaCall> s_lua_calls;
// s_focus, s_focus_want: above rebuild(), which drops a focus whose item is gone
u16 s_dropped = u16(-1); // a cell just dropped: DRAG_DROP_ITEM_DROP is due
// s_mouse_x, s_mouse_y: above rebuild() too
u32 s_inv_stamp = 0; // the inventory screen's change stamp

void raise_focus(u16 id, bool on)
{
    PIItem it = item_by_id(id);
    if (!it || !g_actor || !it->object().m_spawned)
        return;
    g_actor->callback(on ? GameObject::eCellItemFocus : GameObject::eCellItemFocusLost)(it->object().lua_game_object());
}

// A C++ dialog's hooks (UIRml::DialogOpenNative): data-onclick="native:<act>".
std::function<void(LPCSTR)> s_dialog_native_act;
std::function<void()> s_dialog_native_close;

void call_lua(const LuaCall& c)
{
    if (c.func.compare(0, 7, "native:") == 0)
    {
        // a copy: the act may close the dialog, which drops the stored function while it runs
        if (const std::function<void(LPCSTR)> act = s_dialog_native_act)
            act(c.func.c_str() + 7);
        return;
    }
    luabind::functor<void> f;
    if (!ai().script_engine().functor(c.func.c_str(), f))
    {
        Msg("! [rml] a script function '%s' (element '%s') does not exist", c.func.c_str(), c.id.c_str());
        return;
    }
    if (c.item != u16(-1))
        if (PIItem it = item_by_id(c.item))
        {
            f(c.id.c_str(), it->object().lua_game_object());
            return;
        }
    f(c.id.c_str());
}

// Run what the last frame's events asked for. Order matters: the focus first, so
// a drop finds the cell it landed on already focused, as with the legacy cells.
void run_script_queue()
{
    if (s_focus_want != s_focus)
    {
        const u16 was = s_focus;
        s_focus = s_focus_want;
        if (was != u16(-1))
            raise_focus(was, false);
        if (s_focus != u16(-1))
            raise_focus(s_focus, true);
    }
    if (s_dropped != u16(-1))
    {
        const u16 id = s_dropped;
        s_dropped = u16(-1);
        if (PIItem it = item_by_id(id); it && g_actor)
            g_actor->callback(GameObject::eOnCellItemMouse)(it->object().lua_game_object(), 0.f, 0.f, int(DRAG_DROP_ITEM_DROP));
    }
    if (!s_lua_calls.empty())
    {
        const xr_vector<LuaCall> calls = std::move(s_lua_calls);
        s_lua_calls.clear();
        for (const LuaCall& c : calls)
        {
            call_lua(c);
            if (!UIRmlInternal::InvVisible())
                break; // the script closed the screen; the rest were for it
        }
    }
}

// The element (or an ancestor) carrying a script hook.
Rml::Element* up_to_attr(Rml::Element* e, const char* attr)
{
    for (; e; e = e->GetParentNode())
        if (e->HasAttribute(attr))
            return e;
    return nullptr;
}

void queue_lua(Rml::Element* hook, const char* attr, u16 item)
{
    LuaCall c;
    c.func = hook->GetAttribute<Rml::String>(attr, "").c_str();
    c.id = hook->GetId().c_str();
    c.item = item;
    if (!c.func.empty())
        s_lua_calls.push_back(std::move(c));
}

// ------------------------------------------------------------- the events ----
//
// One listener on the document, for everything: events bubble up to it, and the
// element under the pointer says what was hit -- a cell (.ci), a list (.dd), a
// menu entry (.act). ⚠Handlers never rebuild (they set s_rebuild): the element
// an event is being dispatched on would be deleted under the dispatch.
struct InvListener final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override
    {
        Rml::Element* t = ev.GetTargetElement();
        if (s_screen == Screen::Pda && ev.GetId() != Rml::EventId::Mousemove)
            return; // ui_rml_pda.cpp listens to its document itself
        switch (ev.GetId())
        {
        case Rml::EventId::Mousemove:
            s_mouse_x = ev.GetParameter<float>("mouse_x", s_mouse_x);
            s_mouse_y = ev.GetParameter<float>("mouse_y", s_mouse_y);
            if (!s_snapping)
                s_pad_mode = false; // a hand on the mouse again
            break;

        // ⚠Leaving a cell's picture for the cell itself is an out and an over in
        // the same frame; only the state at the frame's end is reported.
        case Rml::EventId::Mouseover:
        case Rml::EventId::Dragover:
            if (Rml::Element* cell = up_to_class(t, "ci"))
                s_focus_want = id_of(cell);
            break;

        case Rml::EventId::Mouseout:
        case Rml::EventId::Dragout:
            if (Rml::Element* cell = up_to_class(t, "ci"); cell && id_of(cell) == s_focus_want)
                s_focus_want = u16(-1);
            break;

        case Rml::EventId::Mousedown: {
            const int button = ev.GetParameter<int>("button", 0);
            Rml::Element* cell = up_to_class(t, "ci");
            if (button == 1)
            {
                if (cell)
                {
                    select(id_of(cell));
                    show_menu(id_of(cell), ev.GetParameter<float>("mouse_x", 0.f), ev.GetParameter<float>("mouse_y", 0.f));
                }
                else
                    hide_menu();
            }
            // ⚠The menu is #ctx -- an ID, not a class. Testing for a class here
            // hid the menu on the press of every one of its own entries, and the
            // release then found nothing to click: every menu action was dead.
            else if (button == 0 && !up_to_id(t, "ctx"))
                hide_menu();
        }
        break;

        case Rml::EventId::Click:
            if (Rml::Element* hook = up_to_attr(t, "data-onclick"))
                queue_lua(hook, "data-onclick", u16(-1));
            else if (Rml::Element* act = up_to_class(t, "act"))
                run_menu(size_t(act->GetAttribute<int>("data-act", -1)));
            else if (up_to_id(t, "take-all"))
                cb_take_all();
            else if (Rml::Element* q = up_to_class(t, "q"))
            {
                const int i = q->GetAttribute<int>("data-q", -1);
                if (i >= 0 && size_t(i) < s_talk_q.size())
                {
                    s_talk_pending_q = s_talk_q[i].value;
                    s_talk_pending_q_set = true;
                }
            }
            else if (up_to_id(t, "talk-trade"))
                s_talk_pending_trade = true;
            else if (up_to_id(t, "trade-do"))
                tr_perform();
            else if (up_to_id(t, "trade-cancel"))
                tr_clear_table();
            else if (up_to_id(t, "cb-leave"))
                UIRml::InvClose();
            else if (up_to_id(t, "trade-leave"))
                UIRml::InvClose(); // back to the talk
            else if (Rml::Element* flt = up_to_class(t, "flt"))
            {
                press_filter(flt);
                hide_menu();
                s_rebuild = true;
            }
            else if (Rml::Element* cell = up_to_class(t, "ci"))
                select(id_of(cell));
            break;

        case Rml::EventId::Dblclick:
            if (Rml::Element* cell = up_to_class(t, "ci"))
            {
                hide_menu();
                // The search screen: to the other side, the whole stack with shift.
                if (s_screen == Screen::CarBody)
                    cb_move(id_of(cell), shift_held());
                else if (s_screen == Screen::Trade)
                    tr_move(id_of(cell), shift_held());
                else
                    double_click(id_of(cell));
            }
            break;

        case Rml::EventId::Dragstart:
            if (Rml::Element* cell = up_to_class(t, "ci"))
            {
                hide_menu();
                select(id_of(cell));
                const Located at = locate(id_of(cell));
                highlight_for(at.cell ? at.cell->item : nullptr, true);
            }
            break;

        case Rml::EventId::Dragend:
            highlight_for(nullptr, false);
            if (Rml::Element* cell = up_to_class(t, "ci"))
                s_dropped = id_of(cell); // the legacy cell's DRAG_DROP_ITEM_DROP
            break;

        case Rml::EventId::Dragdrop: {
            auto dragged = static_cast<Rml::Element*>(ev.GetParameter<void*>("drag_element", nullptr));
            if (Rml::Element* cell = up_to_class(dragged, "ci"))
            {
                // A script's drop target takes the drop whole.
                if (Rml::Element* hook = up_to_attr(t, "data-ondrop"))
                    queue_lua(hook, "data-ondrop", id_of(cell));
                else
                {
                    List* to = list_by_element(t);
                    if (s_screen == Screen::CarBody || s_screen == Screen::Trade)
                    {
                        // OnItemDrop: only onto ANOTHER list counts, and then the
                        // item goes "the other way" whatever list it landed on --
                        // the legacy MoveItem of both screens.
                        const Located from = locate(id_of(cell));
                        if (from.list && to && from.list != to)
                        {
                            if (s_screen == Screen::Trade)
                                tr_move(id_of(cell), shift_held());
                            else
                                cb_move(id_of(cell), shift_held());
                        }
                    }
                    else
                        drop_on(id_of(cell), to);
                }
            }
            highlight_for(nullptr, false);
        }
        break;

        default: break;
        }
    }
} s_listener;

// ⭐The character block of the inventory (True Stalker draws the 3D actor there;
// this engine has no model-in-UI pass, so it is the character's portrait): the
// icon as a one-sprite sheet, and name / rank / reputation / community as text.
// ⚠The sheet lives in a static string because CombineGeneratedSheet caches the
// parsed result by that string's ADDRESS -- built once, the actor's icon does
// not change within a run.
void attach_portrait(Rml::ElementDocument* doc)
{
    CActor* actor = Actor();
    if (!doc || !actor)
        return;
    CSE_ALifeTraderAbstract* T = ch_info_get_from_id(actor->ID());
    if (!T)
        return;

    CCharacterInfo info;
    info.Init(T);
    CStringTable st;
    set_text("pt-name", T->name_translated().c_str());
    set_text("pt-rank", st.translate(InventoryUtilities::GetRankAsText(info.Rank().value())).c_str());
    set_text("pt-rep", st.translate(InventoryUtilities::GetReputationAsText(info.Reputation().value())).c_str());
    set_text("pt-comm", st.translate(info.Community().id()).c_str());

    static Rml::String s_rcss;
    static float s_aspect = 0.f; // width / height of the portrait
    if (s_rcss.empty() && info.IconName().size())
    {
        const char* icon = info.IconName().c_str();
        LPCSTR file = CUITextureMaster::GetTextureFileName(icon);
        const Frect r = CUITextureMaster::GetTextureRect(icon);
        if (file && file[0] && r.width() > 0.f && r.height() > 0.f)
        {
            xr_string f = file; // ⚠forward slashes: a backslash is an escape in RCSS
            for (char& ch : f)
                if (ch == '\\')
                    ch = '/';
            string512 buf;
            xr_sprintf(buf, sizeof(buf), "@spritesheet actor_pt {\n\tsrc: %s;\n\tactor_pt: %.0fpx %.0fpx %.0fpx %.0fpx;\n}\n", f.c_str(), r.x1, r.y1, r.width(), r.height());
            s_rcss = buf;
            s_aspect = r.width() / r.height();
        }
    }
    if (s_rcss.empty())
        return;
    CombineGeneratedSheet(doc, s_rcss, "portrait");
    // The box's height is the markup's (data-h, dp); the width follows the
    // picture, so a portrait is never squeezed.
    if (Rml::Element* pic = by_id("pt-pic"))
    {
        const float h = pic->GetAttribute<float>("data-h", 440.f);
        pic->SetProperty("height", dp(h));
        pic->SetProperty("width", dp(h * s_aspect));
        pic->SetProperty("decorator", "image(actor_pt)");
    }
}

bool build(const char* leaf)
{
    if (s_doc)
    {
        s_doc->Close();
        s_doc = nullptr;
    }

    // 10-10: where an open's time goes (the Switch inventory was 54 ms with the sheets already cached)
    const u64 freq = CPU::QPCFreq();
    auto ms = [freq](u64 a, u64 b) { return 1000.0 * double(b - a) / double(freq); };
    const u64 t0 = CPU::QPC();
    s_doc = BuildDocument(leaf);
    if (!s_doc)
        return false;
    const u64 t1 = CPU::QPC();

    {
        const Rml::String want = s_doc->GetAttribute<Rml::String>("data-space", "");
        s_doc_space = (want == "screen1440") ? Space::Screen1440 : Space::Menu;
        UseSpace(s_doc_space);
    }

#ifdef __SWITCH__
    s_doc->SetClass("nx", true); // the documents show the pad's buttons only on the console
#endif
    attach_item_sprites(s_doc);
    const u64 t2 = CPU::QPC();
    if (s_screen == Screen::Inventory)
        attach_portrait(s_doc);
    const u64 t3 = CPU::QPC();
    load_sounds();
    const u64 t4 = CPU::QPC();
    for (Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Dragstart, Rml::EventId::Dragend, Rml::EventId::Dragdrop,
                            Rml::EventId::Mousemove, Rml::EventId::Mouseover, Rml::EventId::Mouseout, Rml::EventId::Dragover, Rml::EventId::Dragout})
        s_doc->AddEventListener(id, &s_listener, false);
    hide_menu();
    rebuild();
    if (s_screen == Screen::Pda)
        PdaBuilt(s_doc, s_pda_open_tab);
    const u64 t5 = CPU::QPC();
    Msg("~ [rml] build '%s': document %.1f | sprites %.1f | portrait %.1f | sounds %.1f | listeners+rebuild %.1f ms", leaf, ms(t0, t1), ms(t1, t2), ms(t2, t3),
        ms(t3, t4), ms(t4, t5));
    return true;
}

// The document alone goes; what the screen meant to the world is the caller's.
void close_doc()
{
    if (!s_doc)
        return;
    // The scripts were told a cell had the pointer; it no longer exists.
    if (s_focus != u16(-1))
        raise_focus(s_focus, false);
    s_focus = s_focus_want = s_dropped = u16(-1);
    s_lua_calls.clear();
    s_doc->Close();
    s_doc = nullptr;
    s_menu.clear();
    s_pad_menu = PadMenu::None;
    s_pad_mode = false;
    s_screen = Screen::None;
    UseSpace(Space::InGame);
}

// Common to opening every screen: the space and the document. ⚠The caller has
// closed whatever screen was up -- closing one can mean ending a talk.
bool open_screen(Screen s, const char* leaf)
{
    if (!UIRml::Ready() && !UIRml::Init())
        return false;
    if (!actor_inventory())
        return false;
    close_doc();

    // ⚠The space switches with the SCREEN, not with the host: every document here
    // is a port of a 1024x768 legacy layout and every number in their .rcss is a
    // legacy coordinate. See the note at the top of this file.
    UseSpace(Space::Menu);

    s_screen = s;
    s_sel = u16(-1);
    s_filter = s_filter_their = Filter::All; // a screen opens showing everything
    if (!build(leaf))
    {
        s_screen = Screen::None;
        UseSpace(Space::InGame);
        return false;
    }
    return true;
}

// ------------------------------------------------------- the talk, driven ----

void tk_open_view()
{
    s_talk_pending_q_set = s_talk_pending_trade = false;
    if (!open_screen(Screen::Talk, "talk.rml"))
    {
        Msg("! [rml] talk: the screen did not open");
        return;
    }
    InventoryUtilities::SendInfoToActor("ui_talk"); // CUITalkDialogWnd::Show
}

// The trade's end -- back to the talk, or not when the talk itself is ending.
void tr_close(bool back_to_talk)
{
    close_doc();
    // CUITradeWnd::Hide (and StopTrade).
    InventoryUtilities::SendInfoToActor("ui_trade_hide"); // bind_stalker: on_trade_hide
    if (s_tr_trade)
        s_tr_trade->TradeCB(false);
    if (s_tr_others_trade)
        s_tr_others_trade->TradeCB(false);
    if (g_hud)
        if (CUI* ui = g_hud->GetUI())
            if (CUIGameCustom* game = ui->UIGame())
            {
                game->RemoveCustomStatic("not_enough_money_mine");
                game->RemoveCustomStatic("not_enough_money_other");
            }
    s_tr_our_offer.clear();
    s_tr_their_offer.clear();
    s_tr_our = s_tr_others = nullptr;
    s_tr_others_inv = nullptr;
    s_tr_trade = s_tr_others_trade = nullptr;
    Msg("~ [rml] trade: close");
    if (back_to_talk && s_tk_active)
        tk_open_view();
}

bool tr_open()
{
    CInventoryOwner* our = s_tk_our;
    CInventoryOwner* others = s_tk_other;
    if (!our || !others || !our->GetTrade())
        return false;

    s_tr_our = our;
    s_tr_others = others;
    s_tr_others_inv = our->GetTrade()->GetPartnerInventory();
    s_tr_trade = our->GetTrade();
    s_tr_others_trade = our->GetTrade()->GetPartnerTrade();
    s_tr_our_offer.clear();
    s_tr_their_offer.clear();
    s_tr_zero_trade = !!READ_IF_EXISTS(pSettings, r_bool, "trade", "others_zero_trade", false);

    if (!open_screen(Screen::Trade, "trade.rml"))
    {
        s_tr_our = s_tr_others = nullptr;
        s_tr_others_inv = nullptr;
        s_tr_trade = s_tr_others_trade = nullptr;
        return false;
    }

    // Who trades with whom, and the captions of the two tables.
    set_text("our-name", our->Name());
    set_text("their-name", others->Name());
    set_text("their-cap", CStringTable().translate("ui_st_opponent_items").c_str());

    // CUITradeWnd::Show, then StartTrade.
    InventoryUtilities::SendInfoToActor("ui_trade"); // bind_stalker: on_trade_open
    if (Core.Features.test(xrCore::Feature::engine_ammo_repacker) && !Core.Features.test(xrCore::Feature::hard_ammo_reload))
        if (auto actor = Actor())
            actor->RepackAmmo();
    if (s_tr_trade)
        s_tr_trade->TradeCB(true);
    if (s_tr_others_trade)
        s_tr_others_trade->TradeCB(true);
    s_rebuild = true; // the prices need the lists

    Msg("~ [rml] trade: open with %s", others->Name());
    return true;
}

// CUITalkWnd::Hide: the talk is over, whatever screen of it is up.
void tk_end()
{
    if (!s_tk_active)
        return;
    s_tk_active = false; // first: closing the trade must not reopen the talk

    if (s_screen == Screen::Trade)
        tr_close(false);
    else if (s_screen == Screen::Talk)
        close_doc();
    InventoryUtilities::SendInfoToActor("ui_talk_hide");
    tk_stop_voice();
    tk_to_topic();

    if (s_tk_actor)
    {
        if (s_tk_actor->IsTalking())
            s_tk_actor->StopTalk();
        if (Core.Features.test(xrCore::Feature::more_hide_weapon))
            s_tk_actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, false);
    }
    s_tk_actor = nullptr;
    s_tk_our = s_tk_other = nullptr;
    s_tk_our_mgr = s_tk_other_mgr = nullptr;
    s_tk_need_questions = false;
    Msg("~ [rml] talk: end");
}

// CUITalkWnd::Update, every frame of the talk -- also while its trade is up.
void tk_frame()
{
    CActor* actor = Actor();
    if (!actor || !s_tk_actor || !s_tk_actor->IsTalking())
    {
        tk_end();
        return;
    }
    auto our = smart_cast<CGameObject*>(s_tk_our);
    auto other = smart_cast<CGameObject*>(s_tk_other);
    if (!our || !other ||
        (our->Position().distance_to(other->Position()) - other->Radius() - our->Radius() > s_tk_our->inventory().GetTakeDist() + 0.5f && !s_tk_other->NeedOsoznanieMode()))
    {
        tk_end();
        return;
    }

    if (s_screen == Screen::Talk)
    {
        // The clicks of the last frame, acted on outside the event dispatch.
        if (s_talk_pending_trade)
        {
            s_talk_pending_trade = false;
            UIRml::TalkSwitchToTrade();
            if (!s_tk_active || s_screen != Screen::Talk)
                return;
        }
        if (s_talk_pending_q_set)
        {
            s_talk_pending_q_set = false;
            Msg("~ [rml] talk: answer '%s'", s_talk_pending_q.c_str());
            tk_ask(s_talk_pending_q.c_str()); // may end the dialog, never the talk
        }
    }

    if (s_tk_need_questions)
        tk_update_questions();
    tk_face_partner();
}
} // namespace

// ================================================================= the API ====

bool UIRmlInternal::InvVisible() { return s_doc && s_doc->IsVisible(); }

UIRmlInternal::Space UIRmlInternal::InvSpace() { return s_doc_space; }

void UIRmlInternal::InvFrame()
{
    if (!InvVisible())
        return;

    // The actor died or is gone: no screen survives that.
    CActor* actor = Actor();
    if (!actor || !actor->g_Alive())
    {
        if (s_tk_active)
            tk_end();
        UIRml::InvClose();
        return;
    }

    run_script_queue();
    if (!InvVisible())
        return;

    if (s_screen == Screen::Pda)
    {
        PdaFrame();
        return;
    }
    if (s_screen == Screen::Dialog)
        return; // all a script dialog does, it does through its hooks (run above)

    if (s_tk_active)
    {
        tk_frame();
        if (!InvVisible())
            return;
    }

    if (s_screen == Screen::Inventory)
    {
        // Items land by network events; a script may hand the actor one too.
        if (actor->inventory().ModifyFrame() != s_inv_stamp)
        {
            s_inv_stamp = actor->inventory().ModifyFrame();
            s_rebuild = true;
        }
    }

    if (s_screen == Screen::Trade)
    {
        // A deal moves items by events too, and the partner's stock can change
        // under the screen (CUITradeWnd::Update watched the same two stamps).
        if (actor->inventory().ModifyFrame() != s_tr_our_stamp || (s_tr_others_inv && s_tr_others_inv->ModifyFrame() != s_tr_other_stamp))
            s_rebuild = true;
    }

    if (s_screen == Screen::CarBody)
    {
        // Moved items land a frame or two after the action -- watch the stamps.
        if (actor->inventory().ModifyFrame() != s_cb_our_stamp || (s_cb_others && s_cb_others->inventory().ModifyFrame() != s_cb_other_stamp))
            s_rebuild = true;

        // Walked away from the body: the legacy window closed itself the same way.
        auto other = smart_cast<CGameObject*>(s_cb_others);
        if (other && actor->Position().distance_to(other->Position()) - other->Radius() - actor->Radius() > actor->inventory().GetTakeDist() + 0.5f)
        {
            UIRml::InvClose();
            return;
        }
    }

    if (s_rebuild)
        rebuild();
    else
        refresh_stats();
}

// ======================================================== the gamepad ========
//
// ⭐WHAT THE BUTTONS DO IN THE INVENTORY (the owner's design, 06-10):
//   D-pad / left stick   the pointer snaps to the next cell
//   A                    on an item: its action list; on a sort button: apply it
//   action list          up / down walk it, A runs the lit row, Y drops, B cancels
//   B                    close the menu; else drop the selection; else leave
//   L / R                the previous / next sort category
// The list is built from the same entries as the mouse's menu (build_menu), so
// the two can never disagree about what an item can do -- only about the order:
// the main thing first (use, or the move that puts it where it is worn), so A, A
// does what a double click would. (06-10: it was a cross of four directions; it
// covered half the screen, left holes where an item had fewer actions and cut
// the long ones -- the owner asked for something that sits with the rest.)
enum class Pad
{
    None,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    L,
    R,
};
// The PDA gets the button as UIRmlInternal::PadBtn, by number.
static_assert(int(Pad::A) == int(PadBtn::A) && int(Pad::R) == int(PadBtn::R), "Pad and PadBtn must list the buttons in one order");

Pad pad_of(int dik)
{
    switch (dik)
    {
    case DIK_UP: return Pad::Up;
    case DIK_DOWN: return Pad::Down;
    case DIK_LEFT: return Pad::Left;
    case DIK_RIGHT: return Pad::Right;
    case DIK_RETURN:
    case DIK_NUMPADENTER: return Pad::A;
    case DIK_BACK: return Pad::B;
#ifdef __SWITCH__
    // xr_input_sdl.cpp kPadKeys: the D-pad is F1..F4 (the quick slots) in the game.
    case DIK_F1: return Pad::Up;
    case DIK_F2: return Pad::Right;
    case DIK_F3: return Pad::Down;
    case DIK_F4: return Pad::Left;
    case DIK_W: return Pad::Up; // the left stick
    case DIK_S: return Pad::Down;
    case DIK_A: return Pad::Left;
    case DIK_D: return Pad::Right;
    case DIK_F: return Pad::A;
    case DIK_SPACE: return Pad::B;
    // the pad's X sends Y (wpn_next) and its Y sends R (reload) -- xr_input_sdl.cpp kPadKeys
    case DIK_Y: return Pad::X;
    case DIK_R: return Pad::Y;
    case DIK_LCONTROL: return Pad::L;
    case DIK_LSHIFT: return Pad::R;
#endif
    default: return Pad::None;
    }
}

// The rows of the action list, in the order shown: indices into s_menu, or
// kPadQuick for "to a quick slot..." (the second stage).
constexpr int kPadQuick = -2;
xr_vector<int> s_pad_rows;
int s_pad_drop = -1; // Y
int s_pad_cur = 0; // the lit row (of s_pad_rows, or the slot 0..3 in the quick stage)
u16 s_pad_item = u16(-1); // the item the menu is for
float s_pad_l = 0.f, s_pad_t = 0.f, s_pad_r = 0.f, s_pad_b = 0.f; // its cell: the list sits beside it

xr_string html_safe(const xr_string& cp1251)
{
    const xr_string utf = ToUtf8(cp1251.c_str());
    xr_string out;
    for (char ch : utf)
        out += (ch == '<') ? xr_string("&lt;") : (ch == '&') ? xr_string("&amp;") : xr_string(1, ch);
    return out;
}

// ---- the snap -----------------------------------------------------------------

struct NavCand
{
    Rml::Element* box{}; // what it is (a cell, a panel, a button)
    Rml::Element* aim{}; // where the pointer goes (a slot's picture, else the box)
    float cx{}, cy{}, l{}, t{}, r{}, b{};
};

void nav_collect(xr_vector<NavCand>& out)
{
    if (!s_doc)
        return;
    auto push = [&](Rml::Element* box, Rml::Element* aim) {
        const Rml::Vector2f o = aim->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f s = aim->GetBox().GetSize(Rml::BoxArea::Border);
        if (s.x <= 1.f || s.y <= 1.f)
            return;
        out.push_back({box, aim, o.x + s.x * 0.5f, o.y + s.y * 0.5f, o.x, o.y, o.x + s.x, o.y + s.y});
    };
    Rml::ElementList l;
    if (s_screen == Screen::Dialog)
    {
        // a script dialog: what its markup marks .nav, in the document's order -- the first is where the pad starts
        s_doc->GetElementsByClassName(l, "nav");
        for (Rml::Element* e : l)
            push(e, e);
        return;
    }
    s_doc->GetElementsByClassName(l, "slot-panel");
    for (Rml::Element* e : l)
    {
        Rml::ElementList pic;
        e->GetElementsByClassName(pic, "ci");
        push(e, pic.empty() ? e : pic.front());
    }
    for (const char* cls : {"bgc", "flt", "qs-cell", "tbtn", "nav"}) // nav: what a script dialog offers the pad
    {
        l.clear();
        s_doc->GetElementsByClassName(l, cls);
        for (Rml::Element* e : l)
            push(e, e);
    }
}

// Puts the pointer on a candidate, bringing a bag cell into view first.
void nav_snap(const NavCand& n)
{
    // Inside a frame that scrolls (a bag, the trade table): bring it into view first.
    for (Rml::Element* p = n.box->GetParentNode(); p && p != s_doc; p = p->GetParentNode())
        if (p->GetScrollHeight() > p->GetClientHeight() + 1.f)
        {
            n.box->ScrollIntoView(false);
            s_doc->UpdateDocument();
            break;
        }
    const Rml::Vector2f o = n.aim->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f s = n.aim->GetBox().GetSize(Rml::BoxArea::Border);
    s_snapping = true;
    CursorTo(int(o.x + s.x * 0.5f), int(o.y + s.y * 0.5f));
    s_snapping = false;
    s_pad_mode = true;
}

void nav_move(Pad dir)
{
    xr_vector<NavCand> c;
    nav_collect(c);
    if (c.empty())
        return;

    // The first press after the mouse: no move, just show where the focus is --
    // the first thing in the bag, else the first thing anywhere.
    if (!s_pad_mode)
    {
        // the actor's bag; at a body or a box, what lies there
        // -- on its first ITEM; an empty one (a looted body) hands over to the actor's bag.
        const char* homes[] = {s_screen == Screen::Trade ? "dd-our-bag" : s_screen == Screen::CarBody ? "dd-other" : "dd-bag", "dd-our"};
        int first = 0;
        for (const char* home : homes)
        {
            Rml::Element* bag = by_id(home);
            Rml::ElementList items;
            if (bag)
                bag->GetElementsByClassName(items, "ci");
            if (items.empty())
                continue;
            // the candidate (a background cell) under the item's centre
            const Rml::Vector2f o = items.front()->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f s = items.front()->GetBox().GetSize(Rml::BoxArea::Border);
            const float x = o.x + s.x * 0.5f, y = o.y + s.y * 0.5f;
            float best = 1e30f;
            for (size_t i = 0; i < c.size(); ++i)
            {
                const float d = (c[i].cx - x) * (c[i].cx - x) + (c[i].cy - y) * (c[i].cy - y);
                if (d < best)
                    best = d, first = int(i);
            }
            break;
        }
        nav_snap(c[first]);
        return;
    }

    // Where the pointer is: the smallest candidate containing it, else the nearest.
    int cur = -1;
    float best = 1e30f;
    for (size_t i = 0; i < c.size(); ++i)
        if (s_mouse_x >= c[i].l && s_mouse_x <= c[i].r && s_mouse_y >= c[i].t && s_mouse_y <= c[i].b)
        {
            const float area = (c[i].r - c[i].l) * (c[i].b - c[i].t);
            if (area < best)
            {
                best = area;
                cur = int(i);
            }
        }
    if (cur < 0)
        for (size_t i = 0; i < c.size(); ++i)
        {
            const float d = (c[i].cx - s_mouse_x) * (c[i].cx - s_mouse_x) + (c[i].cy - s_mouse_y) * (c[i].cy - s_mouse_y);
            if (d < best)
            {
                best = d;
                cur = int(i);
            }
        }
    if (cur < 0)
        return;

    // The nearest one that lies in that direction; being off the line costs more
    // than being further along it, which keeps a column a column.
    int pick = -1;
    float score = 1e30f;
    for (size_t i = 0; i < c.size(); ++i)
    {
        if (int(i) == cur)
            continue;
        const float dx = c[i].cx - c[cur].cx, dy = c[i].cy - c[cur].cy;
        float along = 0.f, perp = 0.f;
        switch (dir)
        {
        case Pad::Up: along = -dy, perp = std::fabs(dx); break;
        case Pad::Down: along = dy, perp = std::fabs(dx); break;
        case Pad::Left: along = -dx, perp = std::fabs(dy); break;
        default: along = dx, perp = std::fabs(dy); break;
        }
        if (along < 4.f)
            continue;
        const float s = along + 3.f * perp;
        if (s < score)
        {
            score = s;
            pick = int(i);
        }
    }
    if (pick >= 0)
        nav_snap(c[pick]);
}

// ---- the action list ------------------------------------------------------------

void clear_selection()
{
    s_sel = u16(-1);
    Rml::ElementList cells;
    if (s_doc)
        s_doc->GetElementsByClassName(cells, "ci");
    for (Rml::Element* c : cells)
        c->SetClass("sel", false);
}

// The order of the rows. See the table at the top of this block.
void pad_assign(const Located& at)
{
    s_pad_rows.clear();
    s_pad_drop = -1;

    xr_vector<int> slots, mods, drops, moves;
    int belt = -1, bag = -1, eat = -1, unload = -1;
    for (int i = 0; i < int(s_menu.size()); ++i)
        switch (s_menu[i].act)
        {
        case Act::ToSlot: slots.push_back(i); break;
        case Act::ToBelt: belt = i; break;
        case Act::ToBag: bag = i; break;
        case Act::Eat: eat = i; break;
        case Act::Unload: unload = i; break;
        case Act::Drop: s_pad_drop = i; drops.push_back(i); break;
        case Act::DropAll: drops.push_back(i); break;
        case Act::Move:
        case Act::MoveAll: moves.push_back(i); break;
        default: mods.push_back(i); break; // taking an addon off, putting one on
        }
    std::sort(slots.begin(), slots.end(), [](int a, int b) { return s_menu[a].data < s_menu[b].data; });

    // The search and the trade: "take" / "to the table" first, so A, A does what a double click does.
    for (int i : moves)
        s_pad_rows.push_back(i);
    // Use first; then the moves, the main one first: something already worn is put
    // away (to the bag), something in the bag goes where it is worn.
    if (eat >= 0)
        s_pad_rows.push_back(eat);
    if (at.list && at.list->kind != Kind::Bag && bag >= 0)
        s_pad_rows.push_back(bag), bag = -1;
    for (int i : slots)
        s_pad_rows.push_back(i);
    if (belt >= 0)
        s_pad_rows.push_back(belt);
    if (bag >= 0)
        s_pad_rows.push_back(bag);
    // Food and medicine can also be hung on a quick slot.
    if (eat >= 0 && s_screen == Screen::Inventory)
        s_pad_rows.push_back(kPadQuick);
    if (unload >= 0)
        s_pad_rows.push_back(unload);
    for (int i : mods)
        s_pad_rows.push_back(i);
    for (int i : drops)
        s_pad_rows.push_back(i);
}

// Beside the cell -- right of it, or left when the screen ends -- top edges level,
// so the item itself stays in sight. Kept inside the screen.
void pad_place(Rml::Element* e)
{
    e->SetProperty("display", "block");
    s_doc->UpdateDocument();
    const Rml::Vector2f sz = e->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2i scr = Context()->GetDimensions();
    const float gap = std::max(4.f, (s_pad_b - s_pad_t) * 0.06f);
    float x = s_pad_r + gap;
    if (x + sz.x > float(scr.x))
        x = s_pad_l - gap - sz.x;
    x = std::min(std::max(0.f, x), float(scr.x) - sz.x);
    const float y = std::min(std::max(0.f, s_pad_t), float(scr.y) - sz.y);
    e->SetProperty("left", Rml::String(std::to_string(int(std::max(0.f, x))) + "px"));
    e->SetProperty("top", Rml::String(std::to_string(int(std::max(0.f, y))) + "px"));
}

// The pad menu's words live in the markup (data-t-* on #padmenu): a Cyrillic literal in this
// file would be cp1251 and fail the build.
xr_string pad_t(const char* key)
{
    Rml::Element* e = by_id("padmenu");
    return e ? xr_string(e->GetAttribute<Rml::String>(key, "").c_str()) : xr_string();
}

// The game's captions are lower case ("drop", "attach ..."); a row starts with a capital.
// Done on the UTF-8 (the string table's text already is, whatever html_safe gets).
xr_string pad_caption(const xr_string& text)
{
    xr_string s = html_safe(text);
    auto b = [&](size_t i) -> unsigned char& { return reinterpret_cast<unsigned char&>(s[i]); };
    if (s.size() >= 2 && b(0) == 0xD0 && b(1) >= 0xB0 && b(1) <= 0xBF) // a..pe
        b(1) = u8(b(1) - 0x20);
    else if (s.size() >= 2 && b(0) == 0xD1 && b(1) >= 0x80 && b(1) <= 0x8F) // er..ya
        b(0) = 0xD0, b(1) = u8(b(1) + 0x20);
    else if (s.size() >= 2 && b(0) == 0xD1 && b(1) == 0x91) // yo
        b(0) = 0xD0, b(1) = 0x81;
    else if (!s.empty() && b(0) >= 'a' && b(0) <= 'z')
        b(0) = u8(b(0) - 0x20);
    return s;
}

// The item's description popups (ui_inv_descr) would lie under or over the list:
// hidden while it is open. Visibility, not display -- display is the script's.
void pad_descr_visible(bool on)
{
    for (const char* id : {"descr", "descr2"})
        if (Rml::Element* d = by_id(id))
            d->SetProperty("visibility", on ? "visible" : "hidden");
}

// The item's picture for the head of the list, fitted into 150 x 76 dp.
xr_string pad_icon(PIItem it)
{
    const shared_str sect = it->object().cNameSect();
    const shared_str from = READ_IF_EXISTS(pSettings, r_string, sect, "icon_section", sect);
    if (!pSettings->line_exist(from, "inv_grid_width"))
        return {};
    const float k = (s_doc_space == Space::Screen1440) ? 1440.f / 768.f : 1.f; // LuaItemIcon's unit
    const float gw = std::max(1.f, pSettings->r_float(from, "inv_grid_width")) * INV_GRID_WIDTH * k;
    const float gh = std::max(1.f, pSettings->r_float(from, "inv_grid_height")) * INV_GRID_HEIGHT * k;
    return UIRml::LuaItemIcon(sect.c_str(), std::min(150.f / gw, 76.f / gh));
}

xr_string pad_key(const char* k, const char* word) { return xr_string("<span class=\"pm-hint\"><span class=\"pm-k\">") + k + "</span>" + pad_t(word) + "</span>"; }

void pad_render(const xr_string& head, const xr_vector<xr_string>& rows, const xr_string& foot)
{
    Rml::Element* e = by_id("padmenu");
    if (!e)
        return;
    xr_string rml = head + "<div class=\"pm-rows\">";
    for (size_t i = 0; i < rows.size(); ++i)
        rml += xr_string("<div class=\"pm-row") + (int(i) == s_pad_cur ? " cur" : "") + "\">" + rows[i] + "</div>";
    rml += "</div><div class=\"pm-foot\">" + foot + "</div>";
    e->SetInnerRML(rml.c_str());
    pad_descr_visible(false);
    pad_place(e);
}

void pad_show_list()
{
    const Located at = locate(s_pad_item);
    if (!at.cell || s_pad_rows.empty())
        return;
    s_pad_cur = std::min(std::max(0, s_pad_cur), int(s_pad_rows.size()) - 1);
    const xr_string head = "<div class=\"pm-head\"><div class=\"pm-ico\">" + pad_icon(at.cell->item) + "</div><div class=\"pm-name\">" +
        html_safe(at.cell->item->NameShort()) + "</div></div>";
    xr_vector<xr_string> rows;
    for (int idx : s_pad_rows)
        rows.push_back(idx == kPadQuick ? pad_t("data-t-quick") : pad_caption(s_menu[idx].text));
    xr_string foot = pad_key("A", "data-t-choose");
    if (s_pad_drop >= 0)
        foot += pad_key("Y", "data-t-drop");
    foot += pad_key("B", "data-t-cancel");
    s_pad_menu = PadMenu::List;
    pad_render(head, rows, foot);
}

void pad_show_quick()
{
    // In the order of the D-pad in the game: 1 up, 2 right, 3 down, 4 left.
    const xr_string slot = pad_t("data-t-slot");
    const xr_string head = "<div class=\"pm-head noico\"><div class=\"pm-name\">" + pad_t("data-t-quicktitle") + "</div></div>";
    xr_vector<xr_string> rows;
    for (int i = 1; i <= 4; ++i)
        rows.push_back(slot + " " + std::to_string(i).c_str());
    s_pad_menu = PadMenu::Quick;
    pad_render(head, rows, pad_key("A", "data-t-choose") + pad_key("B", "data-t-back"));
}

// A: the item under the pointer. Its list, or (on a button) its action.
void pad_a()
{
    Rml::Element* hov = Context() ? Context()->GetHoverElement() : nullptr;
    // A button (trade, search): pressed as the mouse would press it, so it goes through the same handler.
    if (up_to_class(hov, "tbtn"))
    {
        Context()->ProcessMouseButtonDown(0, 0);
        Context()->ProcessMouseButtonUp(0, 0);
        return;
    }
    if (Rml::Element* flt = up_to_class(hov, "flt"))
    {
        press_filter(flt);
        s_rebuild = true;
        return;
    }
    Rml::Element* cell = up_to_class(hov, "ci");
    if (!cell)
        if (Rml::Element* panel = up_to_class(hov, "slot-panel"))
        {
            Rml::ElementList pic;
            panel->GetElementsByClassName(pic, "ci");
            if (!pic.empty())
                cell = pic.front();
        }
    if (!cell)
        return;
    // A slot's list goes beside the whole plate, not over its caption.
    Rml::Element* panel = up_to_class(cell, "slot-panel");
    Rml::Element* box = panel ? panel : cell;
    const u16 id = id_of(cell);
    clear_selection();
    select(id);
    const Located at = locate(id);
    if (!at.cell)
        return;
    build_menu(at);
    if (s_menu.empty())
        return;
    // The trade has one thing to do with a single item: on the table, or off it. No list for that.
    if (s_screen == Screen::Trade && s_menu.size() == 1)
    {
        run_menu(0);
        return;
    }
    s_pad_item = id;
    const Rml::Vector2f o = box->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f s = box->GetBox().GetSize(Rml::BoxArea::Border);
    s_pad_l = o.x, s_pad_t = o.y, s_pad_r = o.x + s.x, s_pad_b = o.y + s.y;
    pad_assign(at);
    s_pad_cur = 0;
    pad_show_list();
    play(SndProperties);
}

void pad_close_menu()
{
    hide_menu(); // the mouse's list too
}

bool pad_press(Pad p)
{
    if (p == Pad::None || (s_screen != Screen::Inventory && s_screen != Screen::Trade && s_screen != Screen::CarBody))
        return false;
    const bool dir = p == Pad::Up || p == Pad::Down || p == Pad::Left || p == Pad::Right;

    switch (s_pad_menu)
    {
    case PadMenu::List:
    case PadMenu::Quick:
    {
        const bool quick = s_pad_menu == PadMenu::Quick;
        const int n = quick ? 4 : int(s_pad_rows.size());
        if ((p == Pad::Up || p == Pad::Down) && n > 0)
        {
            s_pad_cur = (s_pad_cur + (p == Pad::Down ? 1 : n - 1)) % n;
            Rml::ElementList rows;
            if (Rml::Element* m = by_id("padmenu"))
                m->GetElementsByClassName(rows, "pm-row");
            for (size_t i = 0; i < rows.size(); ++i)
                rows[i]->SetClass("cur", int(i) == s_pad_cur);
        }
        else if (p == Pad::A && quick)
        {
            LuaCall c;
            c.func = "ogsr_quick_slots.on_rml_drop";
            c.id = (xr_string("qs-") + std::to_string(s_pad_cur + 1).c_str());
            c.item = s_pad_item;
            s_lua_calls.push_back(std::move(c));
            pad_close_menu();
        }
        else if (p == Pad::A && s_pad_cur >= 0 && s_pad_cur < n)
        {
            const int idx = s_pad_rows[s_pad_cur];
            if (idx == kPadQuick)
            {
                s_pad_cur = 0;
                pad_show_quick();
            }
            else
            {
                pad_close_menu();
                run_menu(size_t(idx));
            }
        }
        else if (p == Pad::Y && !quick && s_pad_drop >= 0)
        {
            pad_close_menu();
            run_menu(size_t(s_pad_drop));
        }
        else if (p == Pad::B && quick)
        {
            // back to the list, on the row that led here
            const auto it = std::find(s_pad_rows.begin(), s_pad_rows.end(), kPadQuick);
            s_pad_cur = int(it - s_pad_rows.begin());
            pad_show_list();
        }
        else if (p == Pad::B)
        {
            // and the selection with it: the next B leaves the screen, not a third press
            pad_close_menu();
            clear_selection();
        }
        return true;
    }
    case PadMenu::None: break;
    }

    // Nothing open.
    // A before any move: the (hidden) pointer may be anywhere -- show the focus first, as a move would.
    if (dir || (p == Pad::A && !s_pad_mode))
        nav_move(p);
    else if (p == Pad::A)
        pad_a();
    else if (p == Pad::B)
    {
        if (s_sel != u16(-1))
            clear_selection();
        else
            UIRml::InvClose();
    }
    else if (p == Pad::L || p == Pad::R)
    {
        // The sort of the bag the pointer is over: in the trade the partner's is the left half.
        const bool theirs = s_screen == Screen::Trade && s_pad_mode && s_mouse_x < Context()->GetDimensions().x / 2;
        Filter& f = theirs ? s_filter_their : s_filter;
        const int n = int(std::size(s_filter_names));
        f = Filter((int(f) + (p == Pad::R ? 1 : n - 1)) % n);
        s_rebuild = true;
    }
    else if (p == Pad::X)
    {
        if (s_screen == Screen::Trade)
            tr_perform(); // "confirm"
        else if (s_screen == Screen::CarBody)
            cb_take_all();
    }
    else if (p == Pad::Y && s_screen == Screen::Trade)
        tr_clear_table();
    return true;
}

bool UIRmlInternal::InvPadMode()
{
    if (!s_doc || !s_doc->IsVisible())
        return false;
    return s_screen == Screen::Pda ? PdaPadMode() : s_pad_mode;
}

// ⭐THE TALK WITH THE PAD (10-10): up/down walk the answers (class `cur`), A says the one pointed at. The Switch had only
// the number keys -- and its A is the use key, which ENDS the talk; B ends it now, Y opens the trade.
void talk_mark_cur()
{
    if (!s_doc)
        return;
    Rml::ElementList qs;
    s_doc->GetElementsByClassName(qs, "q");
    for (size_t i = 0; i < qs.size(); ++i)
    {
        qs[i]->SetClass("cur", int(i) == s_talk_cur);
        if (int(i) == s_talk_cur)
            qs[i]->ScrollIntoView(false);
    }
}

bool talk_pad(Pad p)
{
    const int n = int(s_talk_q.size());
    switch (p)
    {
    case Pad::Up:
    case Pad::Down:
        if (n > 0)
        {
            s_talk_cur = s_talk_cur < 0 ? 0 : (s_talk_cur + (p == Pad::Down ? 1 : n - 1)) % n;
            talk_mark_cur();
        }
        return true;
    case Pad::A:
        if (s_talk_cur < 0 || s_talk_cur >= n)
            return false; // Enter with nothing picked: as before
        s_talk_pending_q = s_talk_q[size_t(s_talk_cur)].value;
        s_talk_pending_q_set = true;
        return true;
#ifdef __SWITCH__
    case Pad::B:
        if (tk_may_break())
            UIRml::InvClose(); // ends the talk
        return true;
    case Pad::Y:
        if (!s_talk_osoznanie)
            s_talk_pending_trade = true;
        return true;
#endif
    default: return false;
    }
}

// A script dialog (rml.open) on the pad: the D-pad walks its .nav elements (nav_move), A presses the one under the
// pointer as the mouse would -- through the same data-onclick -- B closes. Arrows and Enter do the same on a keyboard.
static bool dialog_key(int dik)
{
    const Pad p = pad_of(dik);
    switch (p)
    {
    case Pad::Up:
    case Pad::Down:
    case Pad::Left:
    case Pad::Right: nav_move(p); return true;
    case Pad::A:
        if (Rml::Context* ctx = Context())
        {
            // on a hook: pressed as the mouse would press it; on nothing: the markup's data-pad="a" (Enter = yes)
            if (up_to_attr(ctx->GetHoverElement(), "data-onclick"))
            {
                ctx->ProcessMouseButtonDown(0, 0);
                ctx->ProcessMouseButtonUp(0, 0);
            }
            else if (s_doc)
            {
                Rml::ElementList l;
                s_doc->QuerySelectorAll(l, "[data-pad=a]");
                if (!l.empty() && l.front()->HasAttribute("data-onclick"))
                    queue_lua(l.front(), "data-onclick", u16(-1));
            }
        }
        return true;
    case Pad::B: UIRml::InvClose(); return true;
    case Pad::X:
    case Pad::Y:
        // a button the markup gave that pad button: data-pad="x" / "y"
        if (s_doc)
        {
            Rml::ElementList l;
            s_doc->QuerySelectorAll(l, p == Pad::X ? "[data-pad=x]" : "[data-pad=y]");
            if (!l.empty() && l.front()->HasAttribute("data-onclick"))
                queue_lua(l.front(), "data-onclick", u16(-1));
        }
        return true;
    default: return false;
    }
}

bool UIRmlInternal::InvKey(int dik)
{
    if (!InvVisible())
        return false;

    if (s_screen == Screen::Talk)
        return talk_pad(pad_of(dik)) || talk_key(dik);

    if (s_screen == Screen::Pda)
        return PdaKey(dik, PadBtn(int(pad_of(dik)))); // Pad and PadBtn list the buttons in one order

    if (s_screen == Screen::Dialog)
        return dialog_key(dik);

    if (s_screen == Screen::Trade || s_screen == Screen::CarBody)
    {
        const Pad p = pad_of(dik);
        if (p != Pad::None && pad_press(p))
            return true;
    }

    if (s_screen == Screen::Trade)
    {
        // The "leave" button's accelerator in the mod's XML (accel="kX").
        if (dik == DIK_X)
        {
            UIRml::InvClose();
            return true;
        }
        return false;
    }

    if (s_screen == Screen::CarBody)
    {
        // The use key closes the search, as it opened it.
        if (is_binded(kUSE, dik))
        {
            UIRml::InvClose();
            return true;
        }
        // The "take all" button's accelerator in the mod's XML (accel="kX").
        if (dik == DIK_X)
        {
            cb_take_all();
            return true;
        }
        if (is_binded(kDROP, dik))
        {
            cb_drop(false);
            return true;
        }
        return false;
    }

    // ⭐The pad (see the block above): the inventory screen only.
    {
        const Pad p = pad_of(dik);
        if (p != Pad::None && pad_press(p))
            return true;
    }

    // ⭐TAB, to a script: the comparison with what is worn (True Stalker). While a
    // screen is up Level_input never reaches the scripts' on_key_down, so the key
    // goes through the same deferred call as data-onclick, named by data-ontab.
    // On the Switch there is no TAB: the right stick click (DIK_L, the torch in the
    // field) does it -- no use for a torch with a screen up.
#ifdef __SWITCH__
    if (dik == DIK_L)
        dik = DIK_TAB;
#endif
    if (dik == DIK_TAB && s_doc && s_doc->HasAttribute("data-ontab"))
    {
        queue_lua(s_doc, "data-ontab", u16(-1));
        return true;
    }

    // The drop key drops the selected item, as in the legacy window.
    if (is_binded(kDROP, dik))
    {
        drop_selected(false);
        return true;
    }
    return false;
}

bool UIRml::InvOpen()
{
    if (s_tk_active)
        return false; // no inventory in the middle of a talk (it was a dialog of its own)
    InvClose();
    const u64 t0 = CPU::QPC();
    if (!open_screen(Screen::Inventory, "inventory.rml"))
        return false;
    const double build_ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
    s_inv_stamp = actor_inventory()->ModifyFrame();

    // What the legacy Show() did to the world beyond drawing itself.
    if (auto actor = Actor())
    {
        if (Core.Features.test(xrCore::Feature::more_hide_weapon))
            actor->SetWeaponHideState(INV_STATE_INV_WND, true);
        if (Core.Features.test(xrCore::Feature::engine_ammo_repacker) && !Core.Features.test(xrCore::Feature::hard_ammo_reload))
            actor->RepackAmmo();
    }
    // ⚠The mod's scripts listen for these two infos (bind_stalker's
    // on_inventory_open/hide, ogse_car_control, xr_conditions).
    InventoryUtilities::SendInfoToActor("ui_inventory");
    play(SndOpen);

    Msg("~ [rml] inventory: open (built in %.1f ms)", build_ms);
    return true;
}

bool UIRml::CarBodyOpen(CInventoryOwner* others, IInventoryBox* box)
{
    if ((!others && !box) || s_tk_active)
        return false;
    InvClose();
    s_cb_others = others;
    s_cb_box = box;
    if (box)
        box->m_in_use = true; // before the build: the box only lists its items while in use

    if (!open_screen(Screen::CarBody, "carbody.rml"))
    {
        if (box)
            box->m_in_use = false;
        s_cb_others = nullptr;
        s_cb_box = nullptr;
        return false;
    }

    CActor* actor = Actor();
    // Who is on each side -- the legacy CUICharacterInfo pair, as names.
    if (actor)
        set_text("our-name", static_cast<CInventoryOwner*>(actor)->Name());
    if (auto go = smart_cast<CGameObject*>(others))
        set_text("other-name", smart_cast<CBaseMonster*>(go) ? "" : others->Name());

    // CUICarBodyWnd::InitCarBody's side effects, in its order.
    if (box)
    {
        if (auto obj = smart_cast<CInventoryBox*>(box))
            obj->callback(GameObject::eOnInvBoxOpen)();
    }
    else if (auto go = smart_cast<CGameObject*>(others); go && actor && !smart_cast<CBaseMonster*>(go))
    {
        // A dead stalker's known info portions pass to the actor.
        CInfoPortionWrapper* known = xr_new<CInfoPortionWrapper>();
        known->registry().init(go->ID());
        for (auto& info : known->registry().objects()) // non-const: w_stringZ takes shared_str&
        {
            NET_Packet P;
            CGameObject::u_EventGen(P, GE_INFO_TRANSFER, actor->ID());
            P.w_u16(0); // not used
            P.w_stringZ(info.info_id);
            P.w_u8(1); // add
            CGameObject::u_EventSend(P);
        }
        known->registry().objects().clear();
        xr_delete(known);
    }

    // CUICarBodyWnd::Show.
    if (actor)
    {
        if (Core.Features.test(xrCore::Feature::more_hide_weapon))
            actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, true);
        if (Core.Features.test(xrCore::Feature::engine_ammo_repacker) && !Core.Features.test(xrCore::Feature::hard_ammo_reload))
            actor->RepackAmmo();
    }
    // ⚠bind_stalker turns these into the on_body_open / on_body_hide signals.
    InventoryUtilities::SendInfoToActor("ui_car_body");
    play(SndOpen);

    Msg("~ [rml] carbody: open (%s)", box ? "a box" : "a body");
    return true;
}

// ---------------------------------------------------------------- the talk ----

void UIRml::TalkStart()
{
    CActor* actor = Actor();
    if (!actor || !actor->IsTalking())
        return;
    if (s_tk_active)
        tk_end();
    InvClose();

    CInventoryOwner* other = actor->GetTalkPartner();
    auto our_mgr = smart_cast<CPhraseDialogManager*>(static_cast<CInventoryOwner*>(actor));
    auto other_mgr = smart_cast<CPhraseDialogManager*>(other);
    if (!other || !our_mgr || !other_mgr)
    {
        actor->StopTalk();
        return;
    }

    s_tk_actor = actor;
    s_tk_our = actor;
    s_tk_other = other;
    s_tk_our_mgr = our_mgr;
    s_tk_other_mgr = other_mgr;
    tk_to_topic();
    s_talk_our = s_tk_our->Name();
    s_talk_other = other->Name();
    s_talk_osoznanie = other->NeedOsoznanieMode();
    s_talk_log.clear();
    s_talk_q.clear();
    s_tk_need_questions = false;

    // CUITalkWnd::Show.
    if (Core.Features.test(xrCore::Feature::more_hide_weapon))
        actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, true);
    s_tk_active = true;
    tk_open_view();
    if (!InvVisible())
    {
        tk_end();
        return;
    }
    Msg("~ [rml] talk: open with %s", other->Name());

    // InitTalkDialog: the partner speaks first if it has something to say.
    tk_others_start();
    s_tk_need_questions = true;
    talk_changed();
}

bool UIRml::TalkActive() { return s_tk_active; }

void UIRml::TalkNeedUpdateQuestions()
{
    if (s_tk_active)
        s_tk_need_questions = true;
}

// AI_PhraseDialogManager: the partner answered.
void UIRml::TalkPartnerSaid(LPCSTR phrase_text, LPCSTR speaker)
{
    if (s_tk_active)
        tk_add_answer(phrase_text, speaker);
}

// game_object:add_talk_message: a line from a script, as is -- no voice, no
// translation (CUITalkWnd::AddAnswerScript).
void UIRml::TalkScriptAnswer(LPCSTR text, bool actor)
{
    if (!s_tk_active)
        return;
    tk_log_line(actor ? s_tk_our->Name() : s_tk_other->Name(), text, actor);
}

void UIRml::TalkIconedMessage(LPCSTR text, LPCSTR texture, const Frect& rect, LPCSTR templ)
{
    if (!s_tk_active)
        return;
    TalkLine l;
    l.text = text ? text : "";
    // The template says whose side: actor_iconed_answer_item / other_iconed_answer_item.
    l.actor = templ && strstr(templ, "actor") != nullptr;
    l.icon = texture ? texture : "";
    l.rect = rect;
    s_talk_log.push_back(std::move(l));
    talk_changed();
}

// CUITalkWnd::SwitchToTrade -- the trade button, the X key, or a script.
void UIRml::TalkSwitchToTrade()
{
    if (!s_tk_active || s_screen != Screen::Talk || s_talk_osoznanie)
        return;
    if (!s_tk_our->IsTradeEnabled() || !s_tk_other->IsTradeEnabled())
        return;
    tk_stop_voice();
    close_doc();
    InventoryUtilities::SendInfoToActor("ui_talk_hide"); // CUITalkDialogWnd::Hide
    if (!tr_open())
        tk_open_view();
}

CInventoryOwner* UIRml::TalkPartner() { return s_tk_active ? s_tk_other : nullptr; }

// ---------------------------------------------------- the script dialogs ----
//
// ⭐10-10 (the owner: "sleep, repair, the level change -- they come up all the time"). A script's window was a
// CUIScriptWnd of legacy statics; now it can be a document of its own. rml.open("sleep.rml", "ui_sleep.on_rml_close")
// opens it as a screen like the others -- the cursor, Esc and B close it, level.main_input_receiver() answers the
// proxy -- the script fills it with rml.set_inner / show / set_class and hangs data-onclick="module.func" on what can
// be pressed; the class `nav` on those lets the pad walk them (dialog_key). The close function, when given, is called
// once the document is gone, however it went.
static xr_string s_dialog_on_close;

static void dialog_closed()
{
    // ⚠After close_doc, and everything moved out first: either may open the next dialog at once.
    const xr_string f = std::move(s_dialog_on_close);
    s_dialog_on_close.clear();
    const std::function<void()> native = std::move(s_dialog_native_close);
    s_dialog_native_close = nullptr;
    s_dialog_native_act = nullptr;
    Msg("~ [rml] dialog: close");
    if (!f.empty())
        call_lua({f, "", u16(-1)});
    if (native)
        native();
}

bool UIRml::DialogOpen(LPCSTR leaf, LPCSTR on_close)
{
    if (s_tk_active || !leaf || !*leaf)
        return false;
    InvClose();
    if (!open_screen(Screen::Dialog, leaf))
    {
        Msg("! [rml] dialog: '%s' did not open", leaf);
        return false;
    }
    s_dialog_on_close = on_close ? on_close : "";
    s_dialog_native_act = nullptr;
    s_dialog_native_close = nullptr;
    Msg("~ [rml] dialog: open '%s'", leaf);
    return true;
}

bool UIRml::DialogOpenNative(LPCSTR leaf, std::function<void(LPCSTR)> on_act, std::function<void()> on_close)
{
    if (!DialogOpen(leaf, ""))
        return false;
    s_dialog_native_act = std::move(on_act);
    s_dialog_native_close = std::move(on_close);
    return true;
}

// The pad has no pointer to start from: the first .nav gets it, once the script has filled the document. On a PC
// the mouse stays where the hand left it (the arrows still walk the .nav elements, the first press finds the first).
void UIRml::DialogFocus()
{
#ifdef __SWITCH__
    if (s_screen != Screen::Dialog || !s_doc)
        return;
    s_doc->UpdateDocument();
    s_pad_mode = false;
    nav_move(Pad::Down); // the first press after the mouse only shows where the focus is
#endif
}

// ----------------------------------------------------------- closing ----

void UIRml::InvClose()
{
    if (!s_doc)
        return;

    switch (s_screen)
    {
    case Screen::Talk: tk_end(); return; // Esc or F on the talk ends it
    case Screen::Trade: tr_close(true); return; // the trade's end is the talk's again
    default: break;
    }

    const Screen was = s_screen;
    if (was != Screen::Pda)
        play(SndClose);
    close_doc();

    if (was == Screen::Dialog)
    {
        dialog_closed();
        return;
    }

    if (was == Screen::Pda)
    {
        // CUIPdaWnd::Hide, and the 3D PDA back out of the hands.
        InventoryUtilities::SendInfoToActor("ui_pda_hide");
        if (s_pda_blocked)
            if (auto actor = Actor())
                actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, false);
        s_pda_blocked = false;
        PdaClosed();
        Msg("~ [rml] pda: close");
        return;
    }

    if (was == Screen::CarBody)
    {
        InventoryUtilities::SendInfoToActor("ui_car_body_hide");
        if (s_cb_box)
            s_cb_box->m_in_use = false;
        s_cb_others = nullptr;
        s_cb_box = nullptr;
        if (auto actor = Actor())
            if (Core.Features.test(xrCore::Feature::more_hide_weapon))
                actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, false);
        Msg("~ [rml] carbody: close");
        return;
    }

    InventoryUtilities::SendInfoToActor("ui_inventory_hide");
    if (auto actor = Actor())
        if (Core.Features.test(xrCore::Feature::more_hide_weapon))
            actor->SetWeaponHideState(INV_STATE_INV_WND, false);

    Msg("~ [rml] inventory: close");
}

bool UIRml::InvActive() { return UIRmlInternal::InvVisible(); }

// ---------------------------------------------------------------- the PDA ----

bool UIRml::PdaOpen(int tab)
{
    if (s_tk_active)
        return false;
    if (s_screen == Screen::Pda && InvVisible())
        return true;
    InvClose();
    s_pda_open_tab = tab;
    const u64 t0 = CPU::QPC();
    if (!open_screen(Screen::Pda, "pda.rml"))
        return false;
    // CUIPdaWnd::Show. ⚠Not with the 3D PDA in the hands: blocking every slot puts the
    // active item away (CInventory::SetSlotsBlocked) -- the PDA itself, which closes this.
    s_pda_blocked = false;
    if (auto actor = Actor())
        if (Core.Features.test(xrCore::Feature::more_hide_weapon) && !smart_cast<CPda*>(actor->inventory().ActiveItem()))
        {
            actor->SetWeaponHideState(INV_STATE_BLOCK_ALL, true);
            s_pda_blocked = true;
        }
    InventoryUtilities::SendInfoToActor("ui_pda"); // bind_stalker: on_pda_open; ogse_car_control, xr_conditions
    Msg("~ [rml] pda: open (built in %.1f ms)", 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq()));
    return true;
}

bool UIRml::PdaActive() { return s_screen == Screen::Pda && UIRmlInternal::InvVisible(); }

void UIRml::InvInvalidate() { s_rebuild = true; }

CGameObject* UIRml::CarBodyTarget()
{
    if (s_screen != Screen::CarBody)
        return nullptr;
    if (s_cb_others)
        return smart_cast<CGameObject*>(s_cb_others);
    return s_cb_box ? &s_cb_box->object() : nullptr;
}

CInventoryItem* UIRml::InvSelectedItem() { return s_screen == Screen::Inventory && s_sel != u16(-1) ? item_by_id(s_sel) : nullptr; }

namespace
{
Screen screen_by_name(LPCSTR name)
{
    if (!name)
        return Screen::None;
    if (!xr_strcmp(name, "inventory"))
        return Screen::Inventory;
    if (!xr_strcmp(name, "carbody"))
        return Screen::CarBody;
    if (!xr_strcmp(name, "trade"))
        return Screen::Trade;
    if (!xr_strcmp(name, "talk"))
        return Screen::Talk;
    if (!xr_strcmp(name, "pda"))
        return Screen::Pda;
    if (!xr_strcmp(name, "dialog"))
        return Screen::Dialog;
    return Screen::None;
}
} // namespace

// ⚠"talk" is shown for the whole talk, its trade included -- as the legacy talk
// window was: the trade was a child of it.
bool UIRml::ScreenShown(LPCSTR name)
{
    const Screen s = screen_by_name(name);
    if (s == Screen::Talk)
        return s_tk_active;
    return s != Screen::None && InvVisible() && s_screen == s;
}

void UIRml::ScreenClose(LPCSTR name)
{
    const Screen s = screen_by_name(name);
    if (s == Screen::Talk)
        tk_end();
    else if (s != Screen::None && InvVisible() && s_screen == s)
        InvClose();
}

namespace
{
// Never started, never drawn, never freed (a dialog destroyed in the atexit chain
// would outlive the UI it belongs to). Only its identity and IsShown() matter.
CUIDialogWnd* proxy()
{
    static CUIDialogWnd* p = [] {
        auto w = xr_new<CUIDialogWnd>();
        w->SetWindowName("rml_screen");
        return w;
    }();
    return p;
}
} // namespace

CUIDialogWnd* UIRml::ScreenProxy()
{
    const bool up = InvActive() || s_tk_active;
    CUIDialogWnd* p = proxy();
    p->CUIWindow::Show(up);
    return up ? p : nullptr;
}

bool UIRml::CloseIfProxy(CUIDialogWnd* dlg)
{
    if (!dlg || dlg != proxy())
        return false;
    Msg("~ [rml] a script closed the open screen through main_input_receiver()");
    if (s_tk_active)
        tk_end();
    else
        InvClose();
    proxy()->CUIWindow::Show(false);
    return true;
}

// --------------------------------------------------------- the `rml` module ----

bool UIRml::LuaSetInner(LPCSTR id, LPCSTR rml)
{
    Rml::Element* e = by_id(id);
    if (!e)
        return false;
    e->SetInnerRML(rml ? rml : "");
    return true;
}

bool UIRml::LuaShow(LPCSTR id, bool show)
{
    Rml::Element* e = by_id(id);
    if (!e)
        return false;
    e->SetClass("shown", show);
    e->SetProperty("display", show ? "block" : "none");
    return true;
}

bool UIRml::LuaSetClass(LPCSTR id, LPCSTR cls, bool on)
{
    Rml::Element* e = by_id(id);
    if (!e || !cls)
        return false;
    e->SetClass(cls, on);
    return true;
}

// ⭐The legacy description popup's placement (ui_inv_descr): right of the pointer
// by 40 units in the left half of the screen, left of it in the right half, and
// never off the bottom. In the coordinates of the element's containing box.
bool UIRml::LuaPopupAtMouse(LPCSTR id)
{
    Rml::Element* e = by_id(id);
    if (!e)
        return false;
    e->SetProperty("display", "block");
    s_doc->UpdateDocument();

    Rml::Element* box = e->GetOffsetParent();
    if (!box)
        box = s_doc;
    const float bx = box->GetAbsoluteLeft(), by = box->GetAbsoluteTop();
    const float bw = box->GetClientWidth(), bh = box->GetClientHeight();
    // A legacy unit in physical pixels: the stage is 1024 wide there. The 1440
    // screens have no such stage, but "40 units" means the same distance, 1.875
    // of their dp (1440 / 768).
    const float unit = (s_doc_space == Space::Screen1440) ? DpRatio() * (1440.f / 768.f) : (bw > 0.f ? bw / 1024.f : 1.f);
    const float w = e->GetOffsetWidth(), h = e->GetOffsetHeight();

    float x = s_mouse_x - bx;
    x = x >= bw * 0.5f ? x - w : x + 40.f * unit;
    float y = std::min(s_mouse_y - by, bh - h);
    x = std::max(0.f, x);
    y = std::max(0.f, y);
    char v[32];
    xr_sprintf(v, sizeof(v), "%.0fpx", x);
    e->SetProperty("left", v);
    xr_sprintf(v, sizeof(v), "%.0fpx", y);
    e->SetProperty("top", v);
    return true;
}

bool UIRml::LuaPopupBeside(LPCSTR id, LPCSTR anchor)
{
    Rml::Element* e = by_id(id);
    Rml::Element* a = by_id(anchor);
    if (!e || !a || !s_doc)
        return false;
    e->SetProperty("display", "block");
    s_doc->UpdateDocument();

    Rml::Element* box = e->GetOffsetParent();
    if (!box)
        box = s_doc;
    const float bx = box->GetAbsoluteLeft(), by = box->GetAbsoluteTop();
    const float bw = box->GetClientWidth(), bh = box->GetClientHeight();
    const float gap = 16.f * DpRatio();
    const float w = e->GetOffsetWidth(), h = e->GetOffsetHeight();
    const float ax = a->GetAbsoluteLeft() - bx, ay = a->GetAbsoluteTop() - by, aw = a->GetOffsetWidth();

    float x = ax + aw + gap; // to the right of it
    if (x + w > bw)
        x = ax - gap - w; // no room: to the left
    float y = std::min(ay, bh - h);
    x = std::max(0.f, x);
    y = std::max(0.f, y);
    char v[32];
    xr_sprintf(v, sizeof(v), "%.0fpx", x);
    e->SetProperty("left", v);
    xr_sprintf(v, sizeof(v), "%.0fpx", y);
    e->SetProperty("top", v);
    return true;
}

xr_string UIRml::LuaText(LPCSTR legacy_text) { return legacy_text_to_rml(legacy_text); }

// An item's picture as markup: the same sprite a cell draws, at the size the
// legacy icon had (50 units a grid cell) times `scale`.
xr_string UIRml::LuaItemIcon(LPCSTR section, float scale)
{
    if (scale <= 0.f)
        scale = 1.f;
    if (!section || !pSettings->section_exist(section))
        return {};
    const shared_str from = READ_IF_EXISTS(pSettings, r_string, section, "icon_section", section);
    if (!pSettings->line_exist(from, "inv_grid_width"))
        return {};
    // ⚠Scripts size icons in legacy units (50 per grid cell, times their scale);
    // the 1440 screens' dp is 768/1440 as big, so the same picture needs 1.875x.
    const float k = (s_doc_space == Space::Screen1440) ? 1440.f / 768.f : 1.f;
    const float w = k * scale * INV_GRID_WIDTH * pSettings->r_float(from, "inv_grid_width");
    const float h = k * scale * INV_GRID_HEIGHT * pSettings->r_float(from, "inv_grid_height");
    const char* icon_file = pSettings->line_exist(from, "icon_name") ? pSettings->r_string(from, "icon_name") : nullptr;
    char buf[512];
    xr_sprintf(buf, sizeof(buf), "<div class=\"item-icon\" style=\"decorator: %s; width: %.0fdp; height: %.0fdp;\"></div>", item_decorator(section, icon_file).c_str(), w, h);
    return buf;
}

// ------------------------------------------------------------ `ui_test` ----
//
// ⭐A DRIVER FOR UNATTENDED RUNS (03-10). The owner has no keyboard scenario to
// walk these screens through, so a scheduled console script does it: the cursor
// is put on an element by RmlUi's own ProcessMouseMove and the buttons pressed
// with ProcessMouseButtonDown/Up -- the same events a hand on the mouse makes,
// hover, drag threshold and double-click timing included. Every step logs one
// "[rmltest]" line; `expect` lines say PASS or FAIL.
//
// Targets:  #id                 an element of the screen
//           item:<section>      the first cell holding such an item
//           item:<section>@<id> ... inside that list
//           act:<Name>          a right-click menu entry (ToSlot, ToBelt, ToBag,
//                               Drop, DropAll, Eat, Attach, Unload, Move, ...)
//           q:<n>               the n-th answer of the talk (0-based)
namespace
{
constexpr const char* s_act_names[] = {"ToSlot", "ToBelt", "ToBag", "Drop", "DropAll", "Eat", "Attach", "DetachScope", "DetachSilencer", "DetachGL", "Unload", "Move", "MoveAll"};

const char* screen_name(Screen s)
{
    switch (s)
    {
    case Screen::Inventory: return "inventory";
    case Screen::CarBody: return "carbody";
    case Screen::Trade: return "trade";
    case Screen::Talk: return "talk";
    case Screen::Pda: return "pda";
    case Screen::Dialog: return "dialog";
    default: return "none";
    }
}

Rml::Element* test_cell(const xr_string& section, const char* list_id)
{
    for (List& l : lists())
    {
        if (list_id && xr_strcmp(l.id, list_id))
            continue;
        for (Cell& c : l.cells)
            if (c.item && section == c.item->object().cNameSect().c_str())
            {
                Rml::ElementList cells;
                s_doc->GetElementsByClassName(cells, "ci");
                for (Rml::Element* e : cells)
                    if (id_of(e) == c.item->object().ID())
                        return e;
            }
    }
    return nullptr;
}

Rml::Element* test_target(const xr_string& t)
{
    if (!s_doc || t.empty())
        return nullptr;
    if (t[0] == '#')
        return by_id(t.c_str() + 1);
    if (t.rfind("css:", 0) == 0)
        return s_doc->QuerySelector(t.c_str() + 4); // the first match: "css:#rp-list .row" (a script dialog's rows)
    if (t.rfind("item:", 0) == 0)
    {
        xr_string sect = t.substr(5);
        xr_string list;
        if (const size_t at = sect.find('@'); at != xr_string::npos)
        {
            list = sect.substr(at + 1);
            sect = sect.substr(0, at);
        }
        return test_cell(sect, list.empty() ? nullptr : list.c_str());
    }
    if (t.rfind("first:", 0) == 0)
    {
        // The first cell of a list: what a partner or a box holds is not known
        // in advance.
        if (Rml::Element* host = by_id(t.c_str() + 6))
        {
            Rml::ElementList cells;
            host->GetElementsByClassName(cells, "ci");
            return cells.empty() ? nullptr : cells[0];
        }
        return nullptr;
    }
    if (t.rfind("act:", 0) == 0)
    {
        const xr_string name = t.substr(4);
        for (size_t i = 0; i < s_menu.size(); ++i)
            if (name == s_act_names[size_t(s_menu[i].act)])
            {
                Rml::ElementList acts;
                s_doc->GetElementsByClassName(acts, "act");
                for (Rml::Element* e : acts)
                    if (e->GetAttribute<int>("data-act", -1) == int(i))
                        return e;
            }
        return nullptr;
    }
    if (t.rfind("q:", 0) == 0)
    {
        const int n = atoi(t.c_str() + 2);
        Rml::ElementList qs;
        s_doc->GetElementsByClassName(qs, "q");
        return n >= 0 && size_t(n) < qs.size() ? qs[n] : nullptr;
    }
    return nullptr;
}

bool test_point(Rml::Element* e, int& x, int& y)
{
    if (!e)
        return false;
    s_doc->UpdateDocument();
    const Rml::Vector2f pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    x = int(pos.x + size.x * 0.5f);
    y = int(pos.y + size.y * 0.5f);
    return true;
}

void test_press(int button)
{
    if (Rml::Context* ctx = UIRmlInternal::Context())
    {
        ctx->ProcessMouseButtonDown(button, 0);
        ctx->ProcessMouseButtonUp(button, 0);
    }
}

// Which list holds an item of this section, if any.
const char* test_where(const xr_string& section)
{
    for (List& l : lists())
        for (Cell& c : l.cells)
        {
            if (c.item && section == c.item->object().cNameSect().c_str())
                return l.id;
            for (PIItem m : c.more)
                if (m && section == m->object().cNameSect().c_str())
                    return l.id;
        }
    return nullptr;
}

int test_count(const xr_string& section)
{
    int n = 0;
    if (CInventory* inv = actor_inventory())
        for (PIItem it : inv->m_all)
            if (it && section == it->object().cNameSect().c_str())
                ++n;
    return n;
}

CInventoryOwner* test_find_owner(const char* name)
{
    // "nearest" / "nearest_dead": the closest character, alive or not.
    if (!xr_strcmp(name, "nearest") || !xr_strcmp(name, "nearest_dead"))
    {
        const bool dead = !xr_strcmp(name, "nearest_dead");
        CInventoryOwner* best = nullptr;
        float best_d = flt_max;
        for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
        {
            CObject* o = Level().Objects.o_get_by_iterator(i);
            auto owner = smart_cast<CInventoryOwner*>(o);
            auto alive = smart_cast<CEntityAlive*>(o);
            if (!owner || !alive || o == Actor() || smart_cast<CBaseMonster*>(o) || !!alive->g_Alive() == dead)
                continue;
            const float d = o->Position().distance_to(Actor()->Position());
            if (d < best_d)
                best_d = d, best = owner;
        }
        if (best)
            Msg("~ [rmltest] %s = '%s' %.0f m away", name, smart_cast<CObject*>(best)->cName().c_str(), best_d);
        return best;
    }
    if (CObject* o = Level().Objects.FindObjectByName(name))
        return smart_cast<CInventoryOwner*>(o);
    return nullptr;
}

void test_go_near(CObject* o)
{
    CActor* actor = Actor();
    if (!actor || !o)
        return;
    Fvector dir;
    dir.sub(actor->Position(), o->Position());
    dir.y = 0.f;
    if (dir.square_magnitude() < EPS)
        dir.set(1.f, 0.f, 0.f);
    dir.normalize();
    Fvector pos;
    pos.mad(o->Position(), dir, 1.2f);
    Fvector ang{0.f, 0.f, 0.f};
    actor->MoveActor(pos, ang);
}
} // namespace

void UIRml::TestCmd(LPCSTR args)
{
    string1024 buf;
    xr_strcpy(buf, args ? args : "");
    char* verb = buf;
    char* rest = strchr(buf, ' ');
    if (rest)
        *rest++ = 0;
    const xr_string a1 = rest ? rest : "";
    xr_string arg = a1, arg2;
    if (const size_t sp = a1.find(' '); sp != xr_string::npos)
    {
        arg = a1.substr(0, sp);
        arg2 = a1.substr(sp + 1);
    }

    int x, y;
    if (!xr_strcmp(verb, "pda"))
    {
        // `ui_test pda <verb> [arg]` -- ui_rml_pda.cpp (open/tab/pad/row/expect/dump)
        PdaTest(arg, arg2);
    }
    else if (!xr_strcmp(verb, "give"))
    {
        const int n = arg2.empty() ? 1 : std::max(1, atoi(arg2.c_str()));
        for (int i = 0; i < n; ++i)
            Console->Execute((xr_string("g_spawn_to_inventory ") + arg).c_str());
        Msg("~ [rmltest] give %s x%d", arg.c_str(), n);
    }
    else if (!xr_strcmp(verb, "open"))
    {
        bool ok = false;
        if (arg == "inv")
            ok = InvOpen();
        else if (arg == "talk" || arg == "body")
        {
            CInventoryOwner* who = test_find_owner(arg2.c_str());
            if (who)
            {
                test_go_near(smart_cast<CObject*>(who));
                if (arg == "talk")
                {
                    if (auto actor = Actor())
                        actor->RunTalkDialog(who);
                    ok = s_tk_active;
                }
                else
                    ok = CarBodyOpen(who, nullptr);
            }
            else
                Msg("! [rmltest] no inventory owner named '%s'", arg2.c_str());
        }
        else if (arg == "box")
        {
            // The nearest inventory box (or the one named), walked up to.
            CInventoryBox* best = nullptr;
            float best_d = flt_max;
            for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
                if (auto b = smart_cast<CInventoryBox*>(Level().Objects.o_get_by_iterator(i)))
                {
                    if (!arg2.empty() && arg2 != b->cName().c_str())
                        continue;
                    const float d = b->Position().distance_to(Actor()->Position());
                    if (d < best_d)
                        best_d = d, best = b;
                }
            if (best)
            {
                test_go_near(best);
                ok = CarBodyOpen(nullptr, best);
                Msg("~ [rmltest] box '%s' was %.0f m away", best->cName().c_str(), best_d);
            }
            else
                Msg("! [rmltest] no inventory box on the level");
        }
        Msg("~ [rmltest] open %s %s -> %s (screen %s)", arg.c_str(), arg2.c_str(), ok ? "ok" : "FAILED", screen_name(s_screen));
    }
    else if (!xr_strcmp(verb, "npcs"))
    {
        // Inventory owners within 200 m, nearest first: who can be talked to.
        xr_vector<std::pair<float, CObject*>> found;
        for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
        {
            CObject* o = Level().Objects.o_get_by_iterator(i);
            if (o && o != Actor() && smart_cast<CInventoryOwner*>(o))
            {
                const float d = o->Position().distance_to(Actor()->Position());
                if (d < 200.f)
                    found.emplace_back(d, o);
            }
        }
        std::sort(found.begin(), found.end(), [](auto& l, auto& r) { return l.first < r.first; });
        for (size_t i = 0; i < found.size() && i < 25; ++i)
        {
            auto e = smart_cast<CEntityAlive*>(found[i].second);
            Msg("~ [rmltest] npc %-28s %-24s %5.0f m %s", found[i].second->cName().c_str(), found[i].second->cNameSect().c_str(), found[i].first,
                e && !e->g_Alive() ? "DEAD" : "");
        }
    }
    else if (!xr_strcmp(verb, "goto"))
    {
        // goto trader | goto <name>: to a character on THIS level by its ALife
        // record -- far ones are offline and invisible to Level().Objects.
        CActor* actor = Actor();
        if (!actor || !ai().get_alife())
            return;
        const u32 my_level = ai().game_graph().vertex(actor->ai_location().game_vertex_id())->level_id();
        CSE_ALifeDynamicObject* best = nullptr;
        float best_d = flt_max;
        for (const auto& kv : ai().alife().objects().objects())
        {
            CSE_ALifeDynamicObject* so = kv.second;
            auto human = smart_cast<CSE_ALifeHumanAbstract*>(so);
            auto trader = smart_cast<CSE_ALifeTrader*>(so);
            auto box = smart_cast<CSE_InventoryBox*>(so);
            if (!human && !trader && !box)
                continue;
            const bool corpse = arg == "corpse";
            if (arg == "trader" ? !trader : (arg == "stalker" || corpse) ? !human : arg == "box" ? !box : arg != so->name_replace())
                continue;
            // a corpse is a dead human; everyone else must be alive
            if (auto ca = smart_cast<CSE_ALifeCreatureAbstract*>(so); ca && !ca->g_Alive() != corpse)
                continue;
            if (corpse && !smart_cast<CSE_ALifeCreatureAbstract*>(so))
                continue;
            if (ai().game_graph().vertex(so->m_tGraphID)->level_id() != my_level)
                continue;
            const float d = so->o_Position.distance_to(actor->Position());
            if (d < best_d)
                best_d = d, best = so;
        }
        if (!best)
        {
            Msg("! [rmltest] goto %s: nobody on this level", arg.c_str());
            return;
        }
        Fvector pos = best->o_Position;
        pos.x += 1.2f;
        Fvector ang{0.f, 0.f, 0.f};
        actor->MoveActor(pos, ang);
        Msg("~ [rmltest] goto %s -> '%s' (%s) was %.0f m away", arg.c_str(), best->name_replace(), best->s_name.c_str(), best_d);
    }
    else if (!xr_strcmp(verb, "kill"))
    {
        // kill nearest: the closest living character (not the trader) dies, for a body to search
        CEntity* best = nullptr;
        float best_d = flt_max;
        for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
        {
            CObject* o = Level().Objects.o_get_by_iterator(i);
            auto ent = smart_cast<CEntityAlive*>(o);
            if (!ent || o == Actor() || !ent->g_Alive() || !smart_cast<CInventoryOwner*>(o) || smart_cast<CBaseMonster*>(o) || smart_cast<CAI_Trader*>(o))
                continue;
            const float d = o->Position().distance_to(Actor()->Position());
            if (d < best_d)
                best_d = d, best = ent;
        }
        if (!best)
        {
            Msg("! [rmltest] kill: nobody alive nearby");
            return;
        }
        Msg("~ [rmltest] kill '%s' %.0f m away", best->cName().c_str(), best_d);
        best->KillEntity(Actor()->ID());
    }
    else if (!xr_strcmp(verb, "move"))
    {
        // move sprint|run|walk|stop: holds the movement keys until the next `move` (DirectInput takes no synthetic keys)
        CActor* actor = Actor();
        g_actor_test_move = arg == "sprint" ? (mcFwd | mcSprint) : arg == "run" ? mcFwd : arg == "walk" ? (mcFwd | mcAccel) : 0u;
        if (actor)
        {
            if (g_actor_test_move & mcSprint)
                actor->set_state_wishful(actor->get_state_wishful() | mcSprint);
            else
                actor->set_state_wishful(actor->get_state_wishful() & ~mcSprint);
        }
        Msg("~ [rmltest] move %s (flags 0x%x)", arg.c_str(), g_actor_test_move);
    }
    else if (!xr_strcmp(verb, "editor"))
    {
        // editor on|off: the embedded ImGui editor (Shift+F10), with the weather window open
        EditorSetOpenForTest(arg == "on");
        Msg("~ [rmltest] editor %s", arg.c_str());
    }
    else if (!xr_strcmp(verb, "turn"))
    {
        // turn <yaw deg> [pitch deg]: rotates the active camera by a delta
        CActor* actor = Actor();
        if (actor && actor->cam_Active())
        {
            actor->cam_Active()->yaw += deg2rad(float(atof(arg.c_str())));
            if (!arg2.empty())
                actor->cam_Active()->pitch += deg2rad(float(atof(arg2.c_str())));
        }
        Msg("~ [rmltest] turn %s %s", arg.c_str(), arg2.c_str());
    }
    else if (!xr_strcmp(verb, "look"))
    {
        // look trader|stalker: aims the camera at the head of the nearest such living character
        // (after `goto` the view points wherever the save left it -- useless for a render A/B)
        CActor* actor = Actor();
        if (!actor || !actor->cam_Active())
            return;
        CEntityAlive* best = nullptr;
        float best_d = flt_max;
        for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
        {
            auto ent = smart_cast<CEntityAlive*>(Level().Objects.o_get_by_iterator(i));
            if (!ent || ent == actor || !ent->g_Alive() || !smart_cast<CInventoryOwner*>(ent) || smart_cast<CBaseMonster*>(ent))
                continue;
            if ((arg == "trader") != !!smart_cast<CAI_Trader*>(ent))
                continue;
            const float d = ent->Position().distance_to(actor->Position());
            if (d < best_d)
                best_d = d, best = ent;
        }
        if (!best)
        {
            Msg("! [rmltest] look %s: nobody nearby", arg.c_str());
            return;
        }
        Fvector head = best->Position();
        head.y += arg == "trader" ? 1.0f : 1.4f;   // the trader sits
        Fvector dir;
        dir.sub(head, Device.vCameraPosition).normalize_safe();
        float h, p;
        dir.getHP(h, p);
        actor->cam_Active()->yaw = -h;
        actor->cam_Active()->pitch = -p;
        Msg("~ [rmltest] look %s -> '%s' %.1f m away", arg.c_str(), best->cName().c_str(), best_d);
    }
    else if (!xr_strcmp(verb, "spawn"))
    {
        Console->Execute((xr_string("g_spawn ") + arg).c_str());
        Msg("~ [rmltest] spawn %s at the actor", arg.c_str());
    }
    else if (!xr_strcmp(verb, "close"))
    {
        InvClose();
        Msg("~ [rmltest] close -> screen %s, talk %d", screen_name(s_screen), int(s_tk_active));
    }
    else if (!xr_strcmp(verb, "pad"))
    {
        // A gamepad press, by name: up down left right a b x y l r.
        static const struct
        {
            const char* name;
            Pad pad;
        } names[] = {{"up", Pad::Up}, {"down", Pad::Down}, {"left", Pad::Left}, {"right", Pad::Right}, {"a", Pad::A},
                     {"b", Pad::B},   {"x", Pad::X},       {"y", Pad::Y},       {"l", Pad::L},         {"r", Pad::R}};
        Pad p = Pad::None;
        for (const auto& n : names)
            if (arg == n.name)
                p = n.pad;
        const bool taken = pad_press(p);
        Msg("~ [rmltest] pad %s %s -> menu %d, pad_mode %d, sel %u", arg.c_str(), taken ? "taken" : "not taken", int(s_pad_menu), int(s_pad_mode), unsigned(s_sel));
    }
    else if (!xr_strcmp(verb, "press"))
    {
        // A key as the GAME gets it (CLevel, not the screens): "press p", "press 25".
        int dik = atoi(arg.c_str());
        if (arg.size() == 1 && arg[0] >= 'a' && arg[0] <= 'z')
        {
            static const int letters[26] = {DIK_A, DIK_B, DIK_C, DIK_D, DIK_E, DIK_F, DIK_G, DIK_H, DIK_I, DIK_J, DIK_K, DIK_L, DIK_M,
                                            DIK_N, DIK_O, DIK_P, DIK_Q, DIK_R, DIK_S, DIK_T, DIK_U, DIK_V, DIK_W, DIK_X, DIK_Y, DIK_Z};
            dik = letters[arg[0] - 'a'];
        }
        if (g_pGameLevel)
        {
            Level().IR_OnKeyboardPress(dik);
            Level().IR_OnKeyboardRelease(dik);
        }
        Msg("~ [rmltest] press %s (dik %d)", arg.c_str(), dik);
    }
    else if (!xr_strcmp(verb, "hint"))
    {
        // The use hint with a string of the table, keys substituted: "hint dead_character_use_or_drag".
        UIRml::HudTestHint(arg.c_str());
        Msg("~ [rmltest] hint %s", arg.c_str());
    }
    else if (!xr_strcmp(verb, "news"))
    {
        // A pop-up news line, the whole rest of the command as its text ("news" alone: a sample from Sidorovich).
        UIRml::HudTestNews(a1.c_str());
        Msg("~ [rmltest] news %s", a1.c_str());
    }
    else if (!xr_strcmp(verb, "static"))
    {
        // A HUD static of ui_custom_msgs.xml for 6 s, as a script puts it up: "static gun_jammed", "static main_task
        // %c[255,238,155,23]Task:\n%c[default]text" (the text replaces the XML's when given).
        if (CUIGameCustom* game = HUD().GetUI() ? HUD().GetUI()->UIGame() : nullptr)
            if (SDrawStaticStruct* st = game->AddCustomStatic(arg.c_str(), true))
            {
                if (!arg2.empty())
                    st->wnd()->SetText(arg2.c_str());
                st->m_endTime = Device.fTimeGlobal + 6.f;
            }
        Msg("~ [rmltest] static %s", arg.c_str());
    }
    else if (!xr_strcmp(verb, "damage"))
    {
        // Every weapon and outfit the actor carries at that condition: "damage 0.6" (the repair window lists 0.2..0.99).
        const float v = clampr(float(atof(arg.c_str())), 0.f, 1.f);
        int n = 0;
        if (CInventory* inv = actor_inventory())
            for (PIItem it : inv->m_all)
                if (smart_cast<CWeapon*>(it) || smart_cast<CCustomOutfit*>(it))
                {
                    it->SetCondition(v);
                    ++n;
                }
        Msg("~ [rmltest] damage %.2f: %d items", v, n);
    }
    else if (!xr_strcmp(verb, "lua"))
    {
        // A script function with no arguments: "lua ui_sleep.open", "lua ogse_repair.open_repair_dialog".
        call_lua({arg, "", u16(-1)});
        Msg("~ [rmltest] lua %s", arg.c_str());
    }
    else if (!xr_strcmp(verb, "lchange"))
    {
        // The level change question as a level changer asks it, with a made-up level, for the cancel only: "yes"
        // would send the actor to its own spot.
        if (auto sp = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame()); sp && Actor())
        {
            CChangeLevelWnd* w = sp->UIChangeLevelWnd;
            if (CUIWindow* t = w->FindChild("message_box_change_level:message_text"))
                if (auto st = smart_cast<CUIStatic*>(t))
                    st->SetText(a1.empty() ? (xr_string(CStringTable().translate("l01_escape").c_str()) + ": " + CStringTable().translate("level_changer_invitation").c_str()).c_str()
                                           : a1.c_str());
            w->m_game_vertex_id = Actor()->ai_location().game_vertex_id();
            w->m_level_vertex_id = Actor()->ai_location().level_vertex_id();
            w->m_position = Actor()->Position();
            w->m_angles.set(0.f, 0.f, 0.f);
            w->m_b_position_cancel = false;
            Msg("~ [rmltest] lchange %s", w->ShowRml() ? "shown" : "NOT shown");
        }
    }
    else if (!xr_strcmp(verb, "wheel"))
    {
        // The weapon wheel: "wheel 3" holds it open pointing at sector 3 (0 = top,
        // clockwise), "wheel 3 take" lets go there, "wheel -1 take" lets go anywhere.
        const int sector = atoi(arg.c_str());
        const bool take = arg2.find("take") != xr_string::npos;
        UIRml::WheelTest(sector, take);
        Msg("~ [rmltest] wheel %d%s -> open %d", sector, take ? " take" : "", int(UIRml::WheelOpen()));
    }
    else if (!xr_strcmp(verb, "key"))
    {
        // A key as the screens get it: through UIRml::OnKey, like Level_input.
        int dik = atoi(arg.c_str());
        if (arg == "esc")
            dik = DIK_ESCAPE;
        else if (arg == "x")
            dik = DIK_X;
        else if (arg == "tab")
            dik = DIK_TAB;
        else if (arg == "up")
            dik = DIK_UP;
        else if (arg == "down")
            dik = DIK_DOWN;
        else if (arg == "enter")
            dik = DIK_RETURN;
        else if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '9')
            dik = arg[0] == '0' ? DIK_0 : DIK_1 + (arg[0] - '1');
        const bool taken = OnKey(dik, true);
        OnKey(dik, false);
        Msg("~ [rmltest] key %s (dik %d) %s -> screen %s, talk %d", arg.c_str(), dik, taken ? "taken" : "not taken", screen_name(s_screen), int(s_tk_active));
    }
    else if (!xr_strcmp(verb, "hover") || !xr_strcmp(verb, "click") || !xr_strcmp(verb, "rclick") || !xr_strcmp(verb, "dblclick"))
    {
        Rml::Element* e = test_target(arg);
        if (!test_point(e, x, y))
        {
            Msg("! [rmltest] %s %s: FAIL, no such element", verb, arg.c_str());
            return;
        }
        UIRmlInternal::CursorTo(x, y);
        if (!xr_strcmp(verb, "click"))
            test_press(0);
        else if (!xr_strcmp(verb, "rclick"))
            test_press(1);
        else if (!xr_strcmp(verb, "dblclick"))
        {
            test_press(0);
            test_press(0);
        }
        Msg("~ [rmltest] %s %s at %d,%d", verb, arg.c_str(), x, y);
    }
    else if (!xr_strcmp(verb, "drag"))
    {
        Rml::Element* from = test_target(arg);
        Rml::Element* to = test_target(arg2);
        int x2, y2;
        if (!test_point(from, x, y) || !test_point(to, x2, y2))
        {
            Msg("! [rmltest] drag %s -> %s: FAIL, %s missing", arg.c_str(), arg2.c_str(), from ? "target" : "source");
            return;
        }
        Rml::Context* ctx = UIRmlInternal::Context();
        UIRmlInternal::CursorTo(x, y);
        ctx->ProcessMouseButtonDown(0, 0);
        for (int i = 1; i <= 8; ++i) // past the drag threshold, then across
            UIRmlInternal::CursorTo(x + (x2 - x) * i / 8, y + (y2 - y) * i / 8);
        ctx->ProcessMouseButtonUp(0, 0);
        Msg("~ [rmltest] drag %s (%d,%d) -> %s (%d,%d)", arg.c_str(), x, y, arg2.c_str(), x2, y2);
    }
    else if (!xr_strcmp(verb, "expect"))
    {
        // expect <section> <list-id | none | count=N>  ;  expect screen <name>
        bool pass;
        xr_string got;
        if (arg == "screen")
        {
            got = screen_name(s_screen);
            pass = got == arg2;
        }
        else if (arg == "talk")
        {
            got = s_tk_active ? "1" : "0";
            pass = got == arg2;
        }
        else if (arg == "shown")
        {
            got = (by_id(arg2.c_str()) && by_id(arg2.c_str())->IsVisible()) ? "visible" : "hidden";
            pass = got == "visible";
        }
        else if (arg == "hidden")
        {
            got = (by_id(arg2.c_str()) && by_id(arg2.c_str())->IsVisible()) ? "visible" : "hidden";
            pass = got == "hidden";
        }
        else if (arg == "has")
        {
            // expect has <id> <text>: the element's markup contains the text
            const size_t sp = arg2.find(' ');
            const xr_string id = arg2.substr(0, sp), what = sp == xr_string::npos ? "" : arg2.substr(sp + 1);
            Rml::Element* e = by_id(id.c_str());
            const Rml::String inner = e ? e->GetInnerRML() : "";
            pass = e && !what.empty() && inner.find(what.c_str()) != Rml::String::npos;
            got = inner.substr(0, 120).c_str();
        }
        else if (arg == "class")
        {
            // expect class <id> <cls> | expect class <id> !<cls>
            const size_t sp = arg2.find(' ');
            const xr_string id = arg2.substr(0, sp), cls = sp == xr_string::npos ? "" : arg2.substr(sp + 1);
            const bool neg = !cls.empty() && cls[0] == '!';
            Rml::Element* e = by_id(id.c_str());
            const bool has = e && e->IsClassSet((neg ? cls.substr(1) : cls).c_str());
            got = has ? "set" : "unset";
            pass = e && has != neg;
        }
        else if (arg2.rfind("count=", 0) == 0)
        {
            const int n = test_count(arg);
            got = std::to_string(n).c_str();
            pass = n == atoi(arg2.c_str() + 6);
        }
        else
        {
            const char* where = test_where(arg);
            got = where ? where : "none";
            pass = got == arg2;
        }
        Msg("%s [rmltest] expect %s %s: %s (got %s)", pass ? "~" : "!", arg.c_str(), arg2.c_str(), pass ? "PASS" : "FAIL", got.c_str());
    }
    else if (!xr_strcmp(verb, "dump"))
    {
        Msg("~ [rmltest] dump: screen %s, talk %d, proxy %s, focus %u", screen_name(s_screen), int(s_tk_active), ScreenProxy() ? "up" : "none", u32(s_focus));
        for (List& l : lists())
        {
            xr_string line;
            for (Cell& c : l.cells)
                if (c.item)
                {
                    line += c.item->object().cNameSect().c_str();
                    if (!c.more.empty())
                        line += xr_string("x") + std::to_string(c.more.size() + 1).c_str();
                    line += " ";
                }
            Msg("~ [rmltest]   %-16s %s", l.id, line.c_str());
        }
        for (size_t i = 0; i < s_menu.size(); ++i)
            Msg("~ [rmltest]   menu %u %s", u32(i), s_act_names[size_t(s_menu[i].act)]);
        for (size_t i = 0; i < s_talk_q.size(); ++i)
            Msg("~ [rmltest]   q %u -> %s", u32(i), s_talk_q[i].value.c_str());
        Msg("~ [rmltest]   talk log %u line(s)", u32(s_talk_log.size()));
    }
    else if (!xr_strcmp(verb, "shot"))
    {
        string_path p;
        xr_sprintf(p, "vk_shot %s", arg.c_str());
        Console->Execute(p);
        Msg("~ [rmltest] shot %s", arg.c_str());
    }
    else
        Msg("! [rmltest] unknown verb '%s'", verb);
}

// The item sprites are not the inventory's alone: the HUD draws the picture of the
// ammo in the active weapon's panel from the same sheet.
void UIRmlInternal::AttachItemSprites(Rml::ElementDocument* doc) { attach_item_sprites(doc); }

xr_string UIRmlInternal::ItemSpriteName(const char* section) { return sprite_name(section); }

xr_string UIRmlInternal::ItemDecorator(const char* section, const char* icon_file) { return item_decorator(section, icon_file); }

extern int g_rml_live_rcss;   // ui_rml.cpp

// ⭐10-10: the first inventory opened in a rainy firefight was a 129 ms frame on the Switch: the document built in 66 ms
// (BuildDocument dropped RmlUi's sheet cache on every build -- ui_rml_live_rcss now, off in the Switch preset) and the
// first draw read ui_inventory.dds (15 ms) and uploaded it. Here, behind the load screen: the document built and closed
// at once (never drawn: Close() takes it out of the tree before the frame renders; its sheets stay cached), and the
// screen's own texture held for the run, so no open reads it again.
void UIRml::PrewarmInventory()
{
    static bool s_done = false;   // the sheet cache and the held texture outlive a level
    if (s_done || s_doc)
        return;
    if (!UIRml::Ready() && !UIRml::Init())
        return;
    s_done = true;
    CTimer t;
    t.Start();
    if (Rml::ElementDocument* doc = UIRmlInternal::BuildDocument("inventory.rml"))
    {
        // and the two generated sheets an open combines in, in the order build() does: the combined containers are cached
        // per document (CombineGeneratedSheet), 6 + 5 ms of the first open. s_doc is null here, so their text and size
        // setters do nothing; the portrait needs the actor (spawned before this).
        attach_item_sprites(doc);
        attach_portrait(doc);
        doc->Close();
    }
    static ui_shader* s_tex = nullptr;   // leaked on purpose: lives as long as the game
    if (!s_tex)
    {
        s_tex = xr_new<ui_shader>();
        (*s_tex)->create("hud\\default", "ui\\ui_inventory");
    }
    Msg("~ [rml] inventory prewarmed in %.1f ms (sheets %s)", t.GetElapsed_sec() * 1000.f, g_rml_live_rcss ? "re-read on every open: ui_rml_live_rcss 1" : "cached");
}
