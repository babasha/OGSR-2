#pragma once

// RmlUi (HTML/CSS) as a second UI system, alongside the legacy XML one.
//
// ⭐WHY. The X-Ray XML UI is hand-placed pixel coordinates with no layout
// engine, every screen nailed to C++, and an iteration loop that goes through a
// rebuild. This is the other track from the 23-07 plan: RmlUi for new screens,
// legacy XML left alone on the HUD and inventory, both rendered through the one
// Vulkan UI pipeline. A big-bang rewrite of hundreds of screens would be
// suicide; coexistence is the whole design.
//
// ⚠The plan assumed the render backend could sit on IUIRender as it stood. It
// could not: IUIShader::create takes texture NAMES, and RmlUi rasterises its
// font atlas into memory. IUIRender therefore grew DynTextureCreate /
// SetDynTexture (see Include/xrRender/UIRender.h) -- a small, general addition
// that any code holding pixels can now use.
//
// ⚠Not built into the headless server: it links these same sources but has no
// renderer, no fonts and no business drawing. Everything below is compiled out
// under XR_ONLINE_SERVER and the entry points become no-ops.
class CInventoryOwner;
class IInventoryBox;
class CGameObject;
class CInventoryItem;
class CUIDialogWnd;
struct GAME_NEWS_DATA;

namespace UIRml
{
// Brings up RmlUi, the font, and the context. Safe to call repeatedly; the
// first call after a level loads is the one that does the work.
bool Init();
void Shutdown();

// True once Init has succeeded.
bool Ready();

// 10-10: behind the load screen -- the inventory document built once (its sheets cached) and its texture held, so the first
// inventory of a fight is not a 129 ms frame. Once per run.
void PrewarmInventory();

// Per frame, before rendering.
void Update();
// During UI rendering, from CHUDManager::RenderUI.
void Render();

// Input. Return true when RmlUi consumed the event.
bool OnKey(int dik, bool down);
bool OnChar(u16 ch);
// ⚠Deltas, not absolutes: in game the mouse is captured for looking and the
// engine only ever reports movement. The cursor position is accumulated here.
bool OnMouseDelta(int dx, int dy);
bool OnWheel(int direction);

// ⭐Swallows keyboard and mouse for a few seconds (`ui_input_mute 60`), so that
// a scripted run of the screens cannot be derailed by whoever is at the machine.
// Twice in one session a sweep was thrown off course by a single click that
// arrived while the window was taking focus -- once into the options, once into
// `lastsave`, which loaded a game and left every later shot photographing the
// wrong thing. A stray click leaves NO trace of itself in the log, only its
// consequence, so the failure reads as a bug in the menu.
//
// ⚠A deadline, never a flag: the road back from "the machine answers to nothing"
// is the console, and the console is opened with a key. It expires on its own.
void InputMute(float seconds);
bool InputMuted();

// Does a document currently want the pointer? While true the game should not
// also act on clicks.
bool WantsInput();

// The anomaly picker: the first screen on this track.
void ShowZonePicker(bool show);
bool ZonePickerVisible();

// The model picker, for the furniture editor.
//
// ⚠Two levels, not one flat list, and that is a content decision rather than a
// styling one: this install has 647 placeable models and a scroll through all of
// them is not a choice anybody makes. The first level is folders (physics\box,
// physics\stol, ...), the second is what is in one.
void ShowPropPicker(bool show);
bool PropPickerVisible();

// ------------------------------------------------------- the game screens ----
//
// ⭐THE ONLY ONES. The inventory, the search (bodies and boxes), the trade and the
// talk are RmlUi documents in ui_rml_inv.cpp; their legacy XML windows
// (CUIInventoryWnd, CUICarBodyWnd, CUITradeWnd, CUITalkWnd/CUITalkDialogWnd) were
// deleted 03-10 together with the `ui_new_inv` gate. Scripts reach them through
// the `rml` Lua module (see level_script.cpp), not through window objects.
//
// ⚠A GAME SCREEN ON THE IN-GAME HOST, not a third host: it updates, renders and
// takes input through the same three points the pickers do. What it does need
// that they do not is the LEGACY UNIT -- the mod's layouts are in X-Ray's
// 1024x768 space -- so while one is up the context runs in Space::Menu.
bool InvOpen();
// Closes whichever screen is up. The talk ends its dialog (TalkStop).
void InvClose();
// The search screen: a body (others) or a box.
bool CarBodyOpen(CInventoryOwner* others, IInventoryBox* box);
bool InvActive();

// The contents changed under the screen -- an item was picked up, a script moved
// something. Marks it for a rebuild on the next frame rather than rebuilding
// here: the caller is often inside the very event that changed things.
void InvInvalidate();

// ⭐The talk, whole: CUITalkWnd's dialog driving (the phrase managers, topics,
// phrases, voice, the PDA log) and its window, in one place.
void TalkStart(); // the actor started talking (CUIGameSP::StartTalk)
bool TalkActive();
void TalkNeedUpdateQuestions(); // infos/phrases changed what may be asked
// The partner said a phrase (CAI_PhraseDialogManager) -- the text is an id.
void TalkPartnerSaid(LPCSTR phrase_text, LPCSTR speaker);
// Scripts: a line in the talk log (text as given), and an iconed one.
void TalkScriptAnswer(LPCSTR text, bool actor);
void TalkIconedMessage(LPCSTR text, LPCSTR texture, const Frect& rect, LPCSTR templ);
void TalkSwitchToTrade(); // script switch_to_trade(), and the trade button
CInventoryOwner* TalkPartner(); // level.get_second_talker()

// ⭐THE PDA (ui_rml_pda.cpp, 10-10): a full-screen document with six tabs -- tasks,
// map, journal, contacts, statistics, encyclopedia. CUIPdaWnd stays as the MODEL the
// engine notifies (PdaContentsChanged, the HUD's flashing icon) and is never shown.
// tab: EPdaTabs (ui/UIPdaAux.h); -1 = the one it was left on.
bool PdaOpen(int tab);
// A 3D PDA was taken into the hands (CUIGameSP::ShowHidePda): the screen opens once
// the draw animation has played, and closing it puts the PDA away again.
void PdaOpenWhenDrawn(int tab);
void PdaCancelPending();
bool PdaActive();
// The engine added a task, a news line, an article: the open tab is rebuilt
// (pda_section::part, ui/UIPdaAux.h).
void PdaContentsChanged(u32 part);

// What scripts and engine code ask about the screens.
CGameObject* CarBodyTarget(); // level.get_car_body_target()
CInventoryItem* InvSelectedItem(); // CInventory.selected_item (scripts)
// By name: "inventory", "carbody", "trade", "talk".
bool ScreenShown(LPCSTR name);
void ScreenClose(LPCSTR name);
// ⭐What `level.main_input_receiver()` answers while one of these screens is up
// (null otherwise): a stand-in dialog that is never started. Dozens of the mod's
// scripts ask it "is any window open?", and some close "whatever is open" with
// level.start_stop_menu(level.main_input_receiver(), true) -- CDialogHolder
// hands such a start/stop to CloseIfProxy, which closes the screen.
CUIDialogWnd* ScreenProxy();
bool CloseIfProxy(CUIDialogWnd* dlg);
// `ui_test <verb> ...`: drives the screens with RmlUi mouse events for
// unattended runs and logs "[rmltest]" lines (see the note in ui_rml_inv.cpp).
void TestCmd(LPCSTR args);

// ⭐THE LUA SIDE OF A SCREEN (the `rml` module). A script fills an element of the
// screen that is up -- by id, with RML -- and is called back through data-onclick
// / data-ondrop attributes naming a Lua function ("module.func"). This is what
// the mod's own panels (item description, quick slots, exo control) are built
// with since the legacy windows they were attached to are gone.
bool LuaSetInner(LPCSTR id, LPCSTR rml); // false: no such element on screen
bool LuaShow(LPCSTR id, bool show);
bool LuaSetClass(LPCSTR id, LPCSTR cls, bool on);
bool LuaPopupAtMouse(LPCSTR id); // next to the pointer, inside the screen
// Beside another popup (the comparison): to its right, or to its left when there
// is no room, top edges level, inside the screen. Both must already be shown.
bool LuaPopupBeside(LPCSTR id, LPCSTR anchor);
xr_string LuaText(LPCSTR legacy_text); // %c[...] and \n of the legacy UI, to RML
// A script's own window as a screen (10-10, ui_rml_inv.cpp "the script dialogs"): the document gamedata\config\ui_rml\
// <leaf>, closed by Esc / B / rml.close("dialog"); on_close ("module.func", may be empty) is called once it is gone.
bool DialogOpen(LPCSTR leaf, LPCSTR on_close);
// The same screen driven from C++ (the level change): a click on data-onclick="native:<act>" calls on_act(act),
// on_close runs once the document is gone, however it went -- on_act closing it with ScreenClose included.
bool DialogOpenNative(LPCSTR leaf, std::function<void(LPCSTR act)> on_act, std::function<void()> on_close);
void DialogFocus(); // the pad's pointer onto the first .nav (call after filling the document)
xr_string LuaItemIcon(LPCSTR section, float scale); // markup: a div.item-icon, 50dp a grid cell times scale
// The HUD's quick slot n (1..): the item's picture fitted into the cell #hqs-n,
// its count, dimmed when none is carried; section "" empties it (06-10).
void LuaHudSlot(int n, LPCSTR section, int count, bool have);

// ------------------------------------------------------------------ the HUD ----
//
// ⭐THE VIEW OF A MODEL THAT STAYS WHERE IT WAS. CUIMainIngameWnd keeps computing
// everything -- health, condition, thresholds, the active item, the warning
// icons, what Lua does to them through setup_game_icon / GetStatic -- and each
// frame hands a plain snapshot to this document, which only DRAWS it. Nothing
// the mod scripts reach changes; what changes is who paints the bars and icons.
//
// ⭐EVERYTHING the window shows is the document's, with one exception: the
// minimap's PICTURE -- a level texture plus spots, rotated and clipped -- is
// still drawn by CUIZoneMap, into the hole the document leaves for it
// (#map-view, see HudMapView). The frame around it, the compass and the distance
// are the document's, and the document draws after the legacy UI, so the frame
// lies on top of the map exactly as it did.
//
// 03-10: the ONLY HUD -- the `ui_new_hud` gate and the legacy drawing behind it
// were removed. CUIMainIngameWnd remains as the model (and what Lua reaches).
//
// Layout is in dp anchored to a 1080-tall reference with a floor of 1 (the
// in-game space), so on a 720p handheld one dp is one pixel and everything comes
// out proportionally larger than on a desktop monitor. That is the point.
struct HudState
{
    bool actor{}; // there is somebody to show
    float health{}; // 0..1
    bool armor_on{};
    float armor{}; // 0..1

    bool weapon_on{}; // the active item wants a brief-info panel
    xr_string weapon_name; // cp1251, as the engine produced it
    xr_string ammo_text;
    xr_string ammo_section; // item section whose picture goes next to the count
    xr_string ammo_icon_file; // CIconParams::name: the picture is a file of its own
    float ammo_grid_w{1.f};
    // The same, split for the True Stalker panel (06-10): rounds in the magazine
    // (-1: the item has no magazine), the rest carried ("--" unlimited), the
    // loaded type's short name (cp1251) and the fire mode -- 0 none, -1 automatic,
    // n a burst/single of n; gl = the grenade launcher is the active barrel.
    int ammo_mag{-1};
    xr_string ammo_reserve;
    xr_string ammo_type;
    int fire_mode{};
    bool gl_mode{};

    // Carrying too much: 0 no, 1 close to the limit, 2 over it (06-10).
    int overweight{};

    // Indexed by CUIMainIngameWnd::EWarningIcons (1..7); 0 is unused.
    static constexpr int ICONS = 8;
    bool icon_on[ICONS]{};
    u32 icon_argb[ICONS]{};

    bool hint_on{};
    xr_string hint; // cp1251

    // The motion icon: stance (CUIMotionIcon::EState, 0..5) and three bars, 0..1.
    bool motion_on{};
    int motion_state{};
    float stamina{}, noise{}, light{};

    // The car panel: shown only while the actor drives; health 0..1.
    bool car_on{};
    float car_health{};

    // The minimap's frame. The map picture is CUIZoneMap's (see HudMapView).
    bool map_on{};
    float map_heading{}; // radians, the camera's own heading (the legacy statics got -h)
    xr_string map_dist; // "12.3 m." to the active pointer, or empty

    // The item under the crosshair that can be picked up: the picture and the
    // addons on it, in the item icon's own pixels (the legacy cell geometry).
    // Rebuilt only when pickup_serial moves -- the window bumps it whenever it
    // composes a new picture.
    struct IconPart
    {
        xr_string section; // the sprite is published under this name...
        xr_string file; // ...unless the item draws from a file of its own
        float x{}, y{}, w{}, h{};
    };
    static constexpr int PICKUP_PARTS = 4; // the item and up to three addons
    bool pickup_on{};
    u32 pickup_serial{};
    int pickup_parts{};
    IconPart pickup[PICKUP_PARTS];

    // CUIMainIngameWnd::EFlashingIcons: 0 the PDA task, 1 mail.
    static constexpr int FLASHES = 2;
    bool flash_on[FLASHES]{};
};

// Is the document up and answering? While true, the legacy window skips drawing
// the parts the document replaces; while false (gate shut, or the content could
// not be built) the legacy HUD draws as it always did.
bool HudActive();
// The legacy window was drawn this frame: the HUD should be on screen.
void HudDrawn();
// The snapshot for this frame.
void HudPush(const HudState& s);
// Where the minimap picture goes: the #map-view box as fractions of the screen
// (x1, y1, x2, y2). False while the document is not on screen or the box has no
// size yet -- the caller then draws no map at all rather than one in the old
// place for a frame.
bool HudMapView(Frect& r01);
// The box asks for a round map (data-round on #map-view, 06-10).
bool HudMapRound();

// The weapon wheel (ui_rml_hud.cpp, 06-10): the wpn_next key tapped = the next
// weapon (Switch) or ammo type (keyboard), held = the wheel. WheelKey takes the
// key's press and release, WheelMouse the pointer motion while the wheel is open.
bool WheelKey(int dik, bool down);
bool WheelMouse(int dx, int dy);
bool WheelOpen();
void WheelTest(int sector, bool release); // ui_test wheel
void HudTestHint(LPCSTR string_id); // ui_test hint: that string of the table as the use hint, 8 s
// ⭐THE NEWS THAT POPS UP (10-10, the owner: "the pop-up messages look bad, put them in the bottom left"). While the
// document is up, CUIMainIngameWnd::ReceiveNews hands every news line here instead of the legacy log (CUIGameLog,
// messages_window.xml): a card at the bottom left -- the picture, who sends it, the time, the text.
void HudNews(const GAME_NEWS_DATA& news);
void HudTestNews(LPCSTR text); // ui_test news: a news line with that text (legacy %c[...] and \n, as scripts send it)
// ⭐THE MOD'S OWN HUD PIECES. Scripts add statics of their own to the HUD
// (get_hud():AddCustomStatic -- this mod's radiation and psy bars, the clock on
// the minimap, the sleep icon...), placed in the legacy layout's coordinates. The
// document takes them over by name (hud.rml: data-adopt, data-place, data-shift):
// an ADOPTED static is drawn by the document from the static's own state and must
// not draw itself -- CUIGameCustom::Render asks this. Placed and shifted ones
// still draw themselves, only somewhere else.
bool HudAdopts(const shared_str& name);

// ---------------------------------------------------------- the main menu ----
//
// ⚠A SECOND HOST, and that is the whole reason these are separate entry points
// rather than the picker ones with a different document. Everything above hangs
// off the level: Update from Level.cpp, Render from CHUDManager::RenderUI, input
// from Level_input.cpp. In the main menu not one of those runs -- CMainMenu puts
// ITSELF into Device.seqRender, gets input through its own IR_* handlers, and
// exists before any level does. So the menu is driven from CMainMenu instead,
// and the two hosts never overlap: activating the menu removes g_pGameLevel from
// seqFrame/seqRender, so exactly one of them updates the context on a frame.
//
// 18-08: this is the ONLY main menu. The `ui_new_menu` gate and the legacy Lua
// screen behind it were removed once the port covered every screen -- see the
// note at the top of ui_rml_menu.cpp for what went and what deliberately stayed.
//
// Builds and shows the document. False means it could not start (no renderer),
// and there is nothing else to raise: the caller refuses to activate.
bool MenuOpen();
void MenuClose();
bool MenuActive();

// From CMainMenu::OnFrame -- the layout pass, and where a queued command runs.
void MenuFrame();
// From CMainMenu::OnRender, after the legacy dialogs and RenderFont.
void MenuRender();

// Presses a menu button from the console (ui_menu_act) -- the screens are only
// reachable by clicking, so without this half of the port cannot be looked at
// without a person at the keyboard.
void MenuAction(const char* action);

bool MenuOnKey(int dik, bool down);
bool MenuOnMouseDelta(int dx, int dy);
bool MenuOnWheel(int direction);

// ---------------------------------------------------------- the load screen ----
//
// ⭐A THIRD HOST (ui_rml_load.cpp, 10-10): the gauge and the captions over the
// level picture, loading.rml. Driven by UILoadingScreen, which keeps drawing the
// backdrop and the picture itself and only drops its own captions and bar while
// this is up. Its OWN context: on a load from a running game the HUD document is
// still in the main one, and a context draws everything it holds.
//
// Open false = no RmlUi or no document; the legacy screen then draws as it always
// did. Strings in the engine's encoding (cp1251 or UTF-8, see ToUtf8).
bool LoadScreenOpen();
void LoadScreenClose();
void LoadScreenProgress(float progress01);
void LoadScreenStage(const char* text);
void LoadScreenLevel(const char* name, const char* description);
// From UILoadingScreen::Update, after the legacy window drew.
void LoadScreenRender();
} // namespace UIRml

// ---------------------------------------------------------------- internals ----
//
// Shared between ui_rml.cpp (the core: interfaces, context, cursor, pickers) and
// ui_rml_menu.cpp (the menu: screens, options, saves, key bindings). Not part of
// the API above -- nothing outside those two files has any business here.
namespace Rml
{
class Context;
class Element;
class ElementDocument;
} // namespace Rml

namespace UIRmlInternal
{
Rml::Context* Context();

// Where the .rml/.rcss live: next to the rest of the game's content, so a screen
// can be edited and reloaded without a build.
xr_string ContentPath(const char* leaf);

// ⚠Two hosts, two coordinate spaces, and one context between them.
//
// "dp" is anchored to the screen height, so a number in the CSS is a fixed
// fraction of the screen. WHICH fraction differs by host: the in-game panels
// were authored against a 1080-tall reference, and the menu is a port of screens
// laid out in X-Ray's own 1024x768 UI space -- there every legacy coordinate is
// a dp and lands exactly where it did. The ratio is therefore switched when a
// host takes over, not fixed once.
enum class Space
{
    InGame, // dp = screen height / 1080
    Menu, // dp = screen height / 768, i.e. the legacy UI unit
    // dp = screen height / 1080 EXACTLY -- no floor at 1.0 and no stretch. One
    // layout for every screen: 720p handheld draws the same picture as 1440p
    // at two thirds the pixels, which is what "the menu just scales" means.
    Screen,
    // dp = screen height / 1440 EXACTLY: the True Stalker inventory is laid out
    // on 2560x1440, so its numbers (cell 152x140, slot 505x94) go into the CSS
    // verbatim instead of being multiplied by 0.75 and rounded. Same rules as
    // Screen otherwise; a document asks for it with <body data-space="screen1440">.
    Screen1440,
};
void UseSpace(Space s);
Space CurrentSpace();
float DpRatio();
// Raw engine wheel value -> RmlUi notches (DirectInput sends ±120 per click, SDL ±1).
float WheelNotches(int direction);

// ⚠⚠X-RAY STRETCHES THE LAYOUT AND NOT THE FONT. dp carries the vertical half of
// the menu's stretch; this is the horizontal remainder, the scaleX the menu puts
// on #stage. It is here rather than in the menu because the font engine needs it
// too -- with the opposite sign. See the note over text_stretch_x().
float StageStretchX();

// The pointer, drawn by us: RmlUi only names a system cursor, and a game with
// the mouse captured has none to show.
void CursorTo(int x, int y);
void CursorBy(int dx, int dy);
void DrawCursor();

// A key press as text, for the edit boxes. Returns false when the key produces
// no character (arrows, F-keys, modifiers).
bool FeedText(int dik);

// The engine's string table speaks cp1251; RmlUi speaks UTF-8.
xr_string Translate(const char* id);
xr_string ToUtf8(const char* cp1251);
// Latin and Cyrillic to capitals in place (UTF-8): RmlUi's text-transform is
// ASCII-only. True when anything changed. Lives in ui_rml_menu.cpp.
bool CapsUtf8(xr_string& s);

// Replaces the contents of every element carrying data-loc with the string table
// entry it names. Run once per document build, before it is shown.
void Localise(Rml::Element* root);

// Makes the legacy UI's sprite library -- every name in ui\textures_descr\*.xml,
// which is what the old screens are drawn from -- available to a document as
// RCSS sprites, so `decorator: image(ui_button_ordinary)` works.
void AttachSprites(Rml::ElementDocument* doc);

// Puts a generated sprite sheet UNDER a document's own rules (so a screen can
// always override it). The RCSS is parsed once per RmlUi lifetime, keyed by
// the address of the caller's static string -- 📏04-10-2026 Switch: every open
// of the inventory re-parsed ~2200 sprite lines, ~75 ms of frame_move a time.
void CombineGeneratedSheet(Rml::ElementDocument* doc, const std::string& rcss, const char* what); // std::string == Rml::String

// Builds a document from a file, localises it, attaches the sprites and shows
// it. Returns null and says why on failure.
Rml::ElementDocument* BuildDocument(const char* leaf);

// ⭐The item icons, shared by the inventory and the HUD (ui_rml_inv.cpp): the
// generated @spritesheet of every inv_grid_* rectangle in the configs, and the
// sprite name a section gets in it. Built once.
void AttachItemSprites(Rml::ElementDocument* doc);
xr_string ItemSpriteName(const char* section);
// The `decorator` value that draws an item's picture: its own file when
// CIconParams names one (icon_name), the published sprite otherwise. ⚠Asking for
// the sprite of an icon_name item gets a white box -- the name is then read as a
// file that does not exist.
xr_string ItemDecorator(const char* section, const char* icon_file);

// ⭐The HUD, seen from the core. HudPreUpdate runs BEFORE the ready check -- it is
// what brings RmlUi up for a level that has never opened a panel -- and HudFrame
// runs just before the context updates.
void HudPreUpdate();
void HudFrame();
// The menu is a different host over the same context: the HUD must not be on it.
void HudHide();
// From Shutdown, after Rml::Shutdown has released the documents.
void HudShutdown();
// From Shutdown, BEFORE Rml::Shutdown: the load screen's context goes with it.
void LoadShutdown();

// The menu holds GPU objects of its own (the background .ogm shader). Dropped
// from Shutdown, before the renderer goes.
void MenuReleaseGpu();

// ⭐The inventory, seen from the core. Two questions only, both asked every
// frame: is it on screen (which decides who owns the pointer and which unit the
// context lays out in), and does it have work to do before the context updates.
bool InvVisible();
// The pointer was put on a cell by the pad: the arrow is not drawn then.
bool InvPadMode();
// The space the open screen asked for (<body data-space>): the legacy 1024x768
// one unless the document says otherwise. Meaningful while InvVisible().
Space InvSpace();
void InvFrame();
// A key the screen acts on itself (the drop key). True when it was used.
bool InvKey(int dik);

// ⭐The PDA, seen from the screen machinery of ui_rml_inv.cpp: the PDA is one of its
// screens (one document at a time, the same Esc / cursor / proxy / ui_test), and only
// the CONTENT is ui_rml_pda.cpp's. The pad's buttons as ui_rml_inv.cpp decodes them.
enum class PadBtn
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
void PdaBuilt(Rml::ElementDocument* doc, int tab); // the document is up: fill it
void PdaFrame(); // every frame while it is up
bool PdaKey(int dik, PadBtn pad); // true: the PDA used the key
void PdaClosed(); // the document went (whoever closed it)
void PdaPendingFrame(); // the 3D PDA's draw animation, watched from UIRml::Update
bool PdaPadMode(); // the pad drives the lists: no arrow drawn
void PdaTest(const xr_string& verb, const xr_string& arg); // `ui_test pda ...`
} // namespace UIRmlInternal
