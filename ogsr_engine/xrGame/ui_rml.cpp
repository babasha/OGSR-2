#include "StdAfx.h"
#include "ui_rml.h"

// ⚠There is no compile-time split for the headless server, and the attempt to
// have one is worth recording: the server links the SAME xrGame.lib the client
// builds, so an #ifdef on XR_ONLINE_SERVER never reaches the object that ends up
// in the link -- it just produced unresolved RmlUi symbols and, because a failed
// link deletes its output, cost the server binary.
//
// The guard is a runtime one instead, and a sound one: Init() refuses without a
// UIRender, which is exactly the headless case. RmlUi is linked into both
// binaries and simply never initialises in the one with no renderer.
#include "xr_platform_keys.h"
#include "cl_zone_edit.h"
#include "cl_prop_edit.h"
#include "xr_level_controller.h" // MOUSE_1
#include "ui_defs.h" // ui_shader -- images come from the engine's texture system
#include "ui/UITextureMaster.h" // the legacy sprite library
#include "string_table.h"
#include "../Include/xrRender/UIRender.h"
#include "../xr_3da/xr_input.h" // DikToChar -- typing into an edit box

// ⚠windowsx.h (pulled in by the engine's precompiled header) defines these as
// one-argument macros over GetWindow(). Rml::Element declares methods with the
// same names, and the macro turns the declaration into a comma expression --
// "error C2059: syntax error ','" pointing at a line that is plainly correct.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>
#include <RmlUi/Core/Factory.h> // ClearStyleSheetCache -- see build_picker()
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/RenderManager.h>

extern int g_touch_controls; // ui_rml_touch.cpp

namespace
{
// ============================================================== utilities ====

// The engine's string table, its XML files and every legacy caption are
// cp1251. RmlUi is UTF-8 throughout. One table, one loop; the alternative --
// MultiByteToWideChar twice per string -- drags a locale dependency into text
// that has no business having one.
const u16 CP1251_HIGH[128] = {0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
                              0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
                              0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
                              0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
                              0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
                              0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
                              0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
                              0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F};

void append_utf8(xr_string& out, u32 cp)
{
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800)
    {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    }
    else
    {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// Walks a UTF-8 string one code point at a time. Malformed input yields the raw
// byte rather than stopping: a font atlas missing one glyph is a better failure
// than a caption that ends halfway.
u32 next_utf8(const char*& p, const char* end)
{
    const u8 b0 = u8(*p++);
    if (b0 < 0x80)
        return b0;
    if ((b0 & 0xE0) == 0xC0 && p < end)
        return ((b0 & 0x1Fu) << 6) | (u8(*p++) & 0x3Fu);
    if ((b0 & 0xF0) == 0xE0 && p + 1 < end)
    {
        const u32 c = ((b0 & 0x0Fu) << 12) | ((u8(p[0]) & 0x3Fu) << 6) | (u8(p[1]) & 0x3Fu);
        p += 2;
        return c;
    }
    if ((b0 & 0xF8) == 0xF0 && p + 2 < end)
    {
        const u32 c = ((b0 & 0x07u) << 18) | ((u8(p[0]) & 0x3Fu) << 12) | ((u8(p[1]) & 0x3Fu) << 6) | (u8(p[2]) & 0x3Fu);
        p += 3;
        return c;
    }
    return b0;
}

// ---------------------------------------------------------------- system ----
class XraySystem final : public Rml::SystemInterface
{
public:
    // ⚠⚠dwTimeContinual, NOT dwTimeGlobal. The global clock STOPS while the main
    // menu is up over a game (Device.Pause with bTimer) and restarts from zero on
    // a level load; RmlUi runs smooth scrolling and every Animate() off this
    // clock, so on the global one the wheel moved the list by one frame's worth
    // and froze, and the menu's selector drum never arrived. The continual clock
    // (TimerMM) never pauses.
    double GetElapsedTime() override { return double(Device.dwTimeContinual) / 1000.0; }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        // RmlUi's own diagnostics, kept in the game log rather than swallowed:
        // a CSS mistake otherwise shows up as "nothing drew" with no reason.
        const char* tag = (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) ? "!" : "~";
        Msg("%s [rml] %s", tag, message.c_str());
        return true;
    }

    // ⚠⚠The one hook that lets a document name an ENGINE texture.
    //
    // RmlUi resolves every href and src against the directory of the document
    // that mentioned it -- correct for a web page, wrong for us: `ui\ui_magnifier`
    // is not a file next to the .rml, it is a name the engine's texture system
    // resolves through $game_textures$ and the archives. Without this the
    // backend would be handed `...\config\ui_rml\ui\ui_magnifier` and every
    // image in the menu would be missing.
    //
    // Stylesheets and sub-documents ARE files and do want joining -- but against
    // our content directory rather than the document path, so that a document
    // built from memory (which has no path) still finds them.
    void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override
    {
        const auto ends_with = [&path](const char* ext) {
            const size_t n = xr_strlen(ext);
            return path.size() >= n && _stricmp(path.c_str() + path.size() - n, ext) == 0;
        };

        if (ends_with(".rcss") || ends_with(".rml"))
        {
            translated_path = UIRmlInternal::ContentPath(path.c_str()).c_str();
            return;
        }

        (void)document_path;
        translated_path = path; // an engine texture name, verbatim
    }
};

// =============================================================== textures ====
//
// Two kinds, one handle type. A document's images are FILES and go through the
// engine's texture system (which knows the archives, the .seq animations and the
// .ogm videos); a font atlas or a saved-game screenshot is PIXELS we already
// hold and goes through DynTextureCreate. RenderGeometry cannot tell them apart
// from the handle alone, so the handle points at this instead of at either.
struct RmlTexture
{
    ui_shader* shader = nullptr; // file-backed
    void* dynamic = nullptr; // pixel-backed
};

// ⚠⚠A 1x1 opaque white pixel of our own, and the reason it has to exist:
// SetDynTexture(nullptr) falls through to the UI pipeline's "white" fallback,
// which is deliberately NOT white -- it is 0xFF1A2018, a dim olive-grey chosen
// so that a menu with a missing texture reads as Zone mood instead of a white
// wall (vk_UIPipeline.cpp, and the comment there says not to change it).
//
// The shader multiplies vertex colour by that texture, so every untextured
// thing RmlUi draws -- which is every background, border and hover fill --
// came out at about a tenth of its CSS colour. The panel was not "dark themed",
// it was being multiplied by 0.1.
void* s_white_tex = nullptr;

// ---------------------------------------------------- saved-game previews ----
//
// ⚠The load screen's thumbnail is written by the renderer as a 24-bit TGA under
// a .dds name (CRender_Vulkan.cpp: ScreenshotDrain), and it lives in
// $game_saves$ rather than $game_textures$ -- so the engine's own texture path
// cannot load it and never could on this renderer. Read it here instead: the
// format is the simplest one there is, and the result goes up as pixels.
bool load_save_preview(const char* save_name, xr_vector<u8>& rgba, u32& out_w, u32& out_h)
{
    string_path fn;
    if (!FS.exist(fn, "$game_saves$", save_name, ".dds"))
        return false;

    IReader* r = FS.r_open(fn);
    if (!r)
        return false;

    // TGA header: 18 bytes, and the two fields that decide everything are the
    // image type at [2] and the depth at [16].
    if (r->elapsed() < 18)
    {
        FS.r_close(r);
        return false;
    }
    u8 hdr[18];
    r->r(hdr, 18);

    const u32 w = u32(hdr[12]) | (u32(hdr[13]) << 8);
    const u32 h = u32(hdr[14]) | (u32(hdr[15]) << 8);
    const u32 bpp = hdr[16] / 8;
    const bool top_down = (hdr[17] & 0x20) != 0;

    if (hdr[2] != 2 || (bpp != 3 && bpp != 4) || !w || !h || r->elapsed() < int(w * h * bpp) + int(hdr[0]))
    {
        FS.r_close(r);
        Msg("~ [rml] '%s' is not a preview this build can read (type %u, %u bpp)", fn, hdr[2], hdr[16]);
        return false;
    }
    r->advance(hdr[0]); // image id, normally empty

    // ⭐Downsampled on the way in rather than on the way out. A 2560-wide
    // screenshot is 12 MB and would be uploaded whole for a thumbnail the
    // document draws at a couple of hundred pixels; picking every Nth texel
    // costs one multiply and saves the whole transfer.
    const u32 step = _max(1u, w / 480u);
    out_w = _max(1u, w / step);
    out_h = _max(1u, h / step);
    rgba.assign(size_t(out_w) * out_h * 4, 255);

    xr_vector<u8> row(size_t(w) * bpp);
    for (u32 y = 0; y < h; ++y)
    {
        r->r(row.data(), int(row.size()));
        if ((y % step) != 0)
            continue;
        const u32 dy_src = y / step;
        if (dy_src >= out_h)
            continue;
        // TGA stores bottom-up unless the descriptor says otherwise.
        const u32 dy = top_down ? dy_src : (out_h - 1 - dy_src);
        u8* dst = rgba.data() + (size_t(dy) * out_w) * 4;
        for (u32 x = 0, dx = 0; x < w && dx < out_w; x += step, ++dx)
        {
            const u8* s = row.data() + size_t(x) * bpp;
            dst[dx * 4 + 0] = s[2]; // TGA is BGR
            dst[dx * 4 + 1] = s[1];
            dst[dx * 4 + 2] = s[0];
            dst[dx * 4 + 3] = 255;
        }
    }
    FS.r_close(r);
    return true;
}

// ---------------------------------------------------------------- render ----
// A vertex in the form the UI pipeline actually wants, prepared once.
//
// ⭐RmlUi compiles a piece of geometry once and draws it every frame for as long
// as it is on screen. Anything that can be decided at compile time must be, and
// the colour conversion below is the whole reason this struct exists rather than
// keeping Rml::Vertex: it used to run three integer divisions per vertex per
// frame to produce a value that never changes.
struct RmlVert
{
    float x, y;
    float u, v;
    u32 colour;
};

struct RmlGeom
{
    xr_vector<RmlVert> verts;
    xr_vector<int> idx;
};

class XrayRender final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
    {
        auto* g = xr_new<RmlGeom>();

        g->verts.reserve(vertices.size());
        for (const Rml::Vertex& v : vertices)
        {
            // ⚠RmlUi 6 hands over PREMULTIPLIED colours, and this pipeline blends
            // with SRC_ALPHA / ONE_MINUS_SRC_ALPHA -- straight alpha. Feeding it
            // premultiplied values multiplies alpha in twice and everything
            // translucent comes out too dark. Undone here rather than by fighting
            // the blend state, which is shared with the whole legacy UI.
            u32 a = v.colour.alpha;
            u32 r = v.colour.red, gg = v.colour.green, b = v.colour.blue;
            if (a > 0 && a < 255)
            {
                r = _min(255u, r * 255u / a);
                gg = _min(255u, gg * 255u / a);
                b = _min(255u, b * 255u / a);
            }
            g->verts.push_back({v.position.x, v.position.y, v.tex_coord.x, v.tex_coord.y, color_rgba(r, gg, b, a)});
        }

        // ⚠Validated once, and ALL-OR-NOTHING. The old check skipped a bad index
        // while drawing, which quietly shifted every following vertex into the
        // wrong triangle -- corruption dressed up as safety. RmlUi does not emit
        // these; if one ever appears it is worth a line in the log, not a shrug.
        const int n = int(g->verts.size());
        for (int i : indices)
        {
            if (i < 0 || i >= n)
            {
                Msg("! [rml] geometry has index %d outside 0..%d -- dropped", i, n - 1);
                g->idx.clear();
                break;
            }
            g->idx.push_back(i);
        }

        return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override
    {
        auto* g = reinterpret_cast<RmlGeom*>(geometry);
        xr_delete(g);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override
    {
        auto* g = reinterpret_cast<RmlGeom*>(geometry);
        if (!g || g->idx.empty() || !UIRender)
            return;

        // Nothing to decide here: every colour and index was settled at compile
        // time. This loop runs every frame for everything on screen.
        UIRender->StartPrimitive(u32(g->idx.size()), IUIRender::ptTriList, IUIRender::pttTL);
        if (!m_xform)
        {
            for (int i : g->idx)
            {
                const RmlVert& v = g->verts[i];
                UIRender->PushPoint(v.x + translation.x, v.y + translation.y, 0.f, v.colour, v.u, v.v);
            }
        }
        else
        {
            for (int i : g->idx)
            {
                const RmlVert& v = g->verts[i];
                Fvector2 p;
                Project(p, v.x + translation.x, v.y + translation.y);
                UIRender->PushPoint(p.x, p.y, 0.f, v.colour, v.u, v.v);
            }
        }
        Bind(texture);
        UIRender->FlushPrimitive();
    }

    // ⚠⚠A TRANSFORM IS NOT OPTIONAL TO IMPLEMENT -- IT IS OPTIONAL TO *DRAW*.
    // Rml::RenderInterface::SetTransform has an empty body in the library, so a
    // backend that never overrides it compiles, links and silently draws every
    // transformed element at its untransformed place. Layout and HIT TESTING
    // still go through the matrix (Element::IsPointWithinElement projects), so
    // the failure is not "the transform did nothing": the picture and the mouse
    // disagree, and the clicks land where the element would have been. That is
    // exactly what the menu's scaleX (see stretch_stage) hit -- the column drew
    // a third too far left AND could not be clicked where it was drawn.
    //
    // This pipeline takes screen-space points one at a time, so the matrix is
    // applied on the CPU here rather than by a vertex shader. RmlUi's contract
    // is the same either way: translate first, then transform, then divide by w.
    void SetTransform(const Rml::Matrix4f* transform) override
    {
        m_xform = (transform != nullptr) && (*transform != Rml::Matrix4f::Identity());
        if (m_xform)
            m_matrix = *transform;
    }

    void Project(Fvector2& out, float x, float y) const
    {
        const Rml::Vector4f p = m_matrix * Rml::Vector4f(x, y, 0.f, 1.f);
        const float iw = (p.w != 0.f) ? 1.f / p.w : 1.f;
        out.set(p.x * iw, p.y * iw);
    }

    // Binding is one place rather than three: the picker, the menu background
    // and the font all end up here, and the "no texture means OUR white, not the
    // pipeline's olive" rule has to hold for every one of them.
    static void Bind(Rml::TextureHandle texture)
    {
        auto* t = reinterpret_cast<RmlTexture*>(texture);
        if (t && t->shader && (*t->shader)->inited())
            UIRender->SetShader(**t->shader);
        else
            UIRender->SetDynTexture(t && t->dynamic ? t->dynamic : s_white_tex);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String& source) override
    {
        if (!UIRender)
            return Rml::TextureHandle(0);

        // ⭐A saved game's thumbnail has no engine texture behind it -- see
        // load_save_preview. Spelled as a scheme in the src so a document asks
        // for it the same way it asks for anything else.
        if (source.compare(0, 5, "save:") == 0)
        {
            xr_vector<u8> rgba;
            u32 w = 0, h = 0;
            if (!load_save_preview(source.c_str() + 5, rgba, w, h))
            {
                dims = Rml::Vector2i(0, 0);
                return Rml::TextureHandle(0);
            }
            auto* t = xr_new<RmlTexture>();
            t->dynamic = UIRender->DynTextureCreate(rgba.data(), w, h);
            dims = Rml::Vector2i(int(w), int(h));
            return reinterpret_cast<Rml::TextureHandle>(t);
        }

        // ⚠Slashes normalised to the engine's, and the reason is RCSS rather
        // than taste: a backslash inside `decorator: image(...)` is an escape to
        // a CSS parser, so a stylesheet can only spell a path with forward
        // slashes -- while the archives index every name with backslashes and
        // would answer "no such file". One replace here and both spellings work
        // from either place.
        xr_string name = source.c_str();
        for (char& c : name)
            if (c == '/')
                c = '\\';

        auto* t = xr_new<RmlTexture>();
        t->shader = xr_new<ui_shader>();
        // ⭐The renderer picks the loader by extension behind this call: .ogm
        // starts a looping Theora decode, .seq takes the first frame, otherwise
        // it is a .dds. The shader name is ignored by the Vulkan backend;
        // "hud\default" is what the legacy statics pass.
        (*t->shader)->create("hud\\default", name.c_str());

        // ⚠Bind-then-ask is the only way to size a texture through IUIRender,
        // and it is free: GetActiveTextureResolution reads the shader that was
        // just bound and issues nothing. Sizes matter here -- RmlUi solves the
        // layout of an <img> from them before it ever draws.
        UIRender->SetShader(**t->shader);
        Fvector2 res{1.f, 1.f};
        UIRender->GetActiveTextureResolution(res);
        dims = Rml::Vector2i(_max(1, int(res.x)), _max(1, int(res.y)));

        if (dims.x <= 1 && dims.y <= 1)
            Msg("! [rml] texture '%s' did not load -- it will draw as a white box", source.c_str());
        return reinterpret_cast<Rml::TextureHandle>(t);
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dims) override
    {
        // Pixels with no file behind them. This is the call the whole
        // DynTexture addition exists for -- see the note in UIRender.h.
        if (!UIRender || dims.x <= 0 || dims.y <= 0)
            return Rml::TextureHandle(0);

        // ⚠The atlas arrives PREMULTIPLIED, exactly like the vertex colours do,
        // and meets the same straight-alpha blend state. Undo it here too: for a
        // glyph atlas (rgb = white * coverage, a = coverage) this puts solid
        // white in the colour and leaves the coverage where the blend expects
        // it, in alpha. Without this every edge gets weighted by coverage twice
        // and the text comes out thin and grubby.
        const size_t texels = size_t(dims.x) * size_t(dims.y);
        xr_vector<u8> straight(texels * 4);
        const u8* src = reinterpret_cast<const u8*>(source.data());
        for (size_t i = 0; i < texels; ++i)
        {
            const u8 a = src[i * 4 + 3];
            for (int c = 0; c < 3; ++c)
                straight[i * 4 + c] = a ? u8(_min(255u, u32(src[i * 4 + c]) * 255u / a)) : 0;
            straight[i * 4 + 3] = a;
        }

        auto* t = xr_new<RmlTexture>();
        t->dynamic = UIRender->DynTextureCreate(straight.data(), u32(dims.x), u32(dims.y));
        if (!t->dynamic)
            Msg("! [rml] GenerateTexture %dx%d failed", dims.x, dims.y);
        return reinterpret_cast<Rml::TextureHandle>(t);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        auto* t = reinterpret_cast<RmlTexture*>(texture);
        if (!t)
            return;
        if (t->dynamic && UIRender)
            UIRender->DynTextureDestroy(t->dynamic);
        xr_delete(t->shader);
        xr_delete(t);
    }

    void EnableScissorRegion(bool enable) override
    {
        m_scissor_on = enable;
        ApplyClip();
    }

    void SetScissorRegion(Rml::Rectanglei region) override
    {
        m_scissor.set(region.Left(), region.Top(), region.Right(), region.Bottom());
        m_scissor_on = true;
        ApplyClip();
    }

    // ⚠⚠A CLIP MASK IS WHAT RMLUI SWITCHES TO WHEN THE CLIPPING ELEMENT IS
    // TRANSFORMED, and it does so on its own: ElementUtilities::GetClippingRegion
    // sets disable_scissor_clipping the moment the element carries a transform,
    // because in the general case the box projects to a rotated quad no scissor
    // can express. So the menu's scaleX turns EVERY overflow box in the document
    // -- the options list, the save list, the key list -- from a scissor into a
    // mask. Leave these two at their empty defaults and those lists simply stop
    // being clipped: they draw straight over the rest of the screen.
    //
    // The general case wants a stencil buffer, which this UI pipeline does not
    // have. But the mask geometry arrives here already transformed, and every
    // transform this UI uses is a scale about an axis, so its bounding box IS
    // the region -- exactly, not approximately. Rotations would over-clip to the
    // enclosing box; that is the honest limit of doing it this way, and it is
    // still incomparably better than not clipping at all.
    void EnableClipMask(bool enable) override
    {
        m_mask_on = enable;
        m_mask_set = false; // a mask always starts empty and is built by the calls below
        ApplyClip();
    }

    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override
    {
        auto* g = reinterpret_cast<RmlGeom*>(geometry);
        if (!g || g->verts.empty())
            return;

        Irect box;
        Bounds(box, *g, translation);

        switch (operation)
        {
        case Rml::ClipMaskOperation::Set:
            m_mask = box;
            m_mask_set = true;
            break;
        case Rml::ClipMaskOperation::Intersect:
            if (m_mask_set)
            {
                m_mask.x1 = _max(m_mask.x1, box.x1);
                m_mask.y1 = _max(m_mask.y1, box.y1);
                m_mask.x2 = _min(m_mask.x2, box.x2);
                m_mask.y2 = _min(m_mask.y2, box.y2);
            }
            else
            {
                m_mask = box;
                m_mask_set = true;
            }
            break;
        case Rml::ClipMaskOperation::SetInverse:
            // "Everything outside this shape" is not a rectangle. Nothing in
            // these documents asks for it (it comes from mask-image filters);
            // clipping to the shape instead would hide precisely what should
            // show, so the mask is dropped and the caller draws unclipped.
            m_mask_set = false;
            break;
        }
        ApplyClip();
    }

private:
    // The bounding box of a piece of geometry as it will actually land on the
    // screen -- through the same translate-then-transform path as RenderGeometry.
    void Bounds(Irect& out, const RmlGeom& g, Rml::Vector2f translation) const
    {
        float x1 = flt_max, y1 = flt_max, x2 = -flt_max, y2 = -flt_max;
        for (const RmlVert& v : g.verts)
        {
            Fvector2 p;
            if (m_xform)
                Project(p, v.x + translation.x, v.y + translation.y);
            else
                p.set(v.x + translation.x, v.y + translation.y);
            x1 = _min(x1, p.x);
            y1 = _min(y1, p.y);
            x2 = _max(x2, p.x);
            y2 = _max(y2, p.y);
        }
        out.set(iFloor(x1), iFloor(y1), iCeil(x2), iCeil(y2));
    }

    // Scissor and mask are two RmlUi concepts and one piece of engine state, so
    // they are combined here rather than each writing over the other's rect.
    void ApplyClip()
    {
        if (!UIRender)
            return;

        Irect r;
        bool have = false;
        if (m_scissor_on)
        {
            r = m_scissor;
            have = true;
        }
        if (m_mask_on && m_mask_set)
        {
            if (have)
            {
                r.x1 = _max(r.x1, m_mask.x1);
                r.y1 = _max(r.y1, m_mask.y1);
                r.x2 = _min(r.x2, m_mask.x2);
                r.y2 = _min(r.y2, m_mask.y2);
            }
            else
            {
                r = m_mask;
                have = true;
            }
        }

        if (!have)
        {
            UIRender->SetScissor(nullptr);
            return;
        }
        // An empty intersection means "show nothing", not "show everything":
        // handing the engine an inverted rectangle would do the latter.
        r.x2 = _max(r.x2, r.x1);
        r.y2 = _max(r.y2, r.y1);
        UIRender->SetScissor(&r);
    }

    Rml::Matrix4f m_matrix = Rml::Matrix4f::Identity();
    bool m_xform = false;

    Irect m_scissor{};
    bool m_scissor_on = false;
    Irect m_mask{};
    bool m_mask_on = false;
    bool m_mask_set = false;
};

// ==================================================== the engine's fonts ====
//
// ⭐⭐RmlUi rasterises TrueType through FreeType and knows nothing else. That is
// the wrong engine for a port of these screens: every caption in the old menu is
// drawn with an X-Ray bitmap font -- graffiti22 for the wheel, letterica16 for
// the option rows -- and those are a .dds atlas plus an .ini of glyph rectangles.
// Substituting a system TTF changes the look of every line of text in the game's
// front end, which is precisely what a port is not allowed to do.
//
// So the font engine is replaced instead of the fonts. The interface below reads
// the same section from system.ltx that CGameFont reads, the same .ini next to
// the same atlas, and hands RmlUi quads out of it. Cyrillic comes for free: the
// atlas has the glyphs, and the .ini is keyed by Unicode code point.

struct FontGlyph
{
    float x = 0, y = 0, w = 0; // in atlas pixels; height is the cell height
};

struct FontFace
{
    xr_string family; // what the CSS says
    xr_string texture; // engine texture name, prefix already applied
    float cell = 16.f; // glyph cell height in atlas pixels
    float space = 8.f; // ⚠the space is a zero-width glyph; this is its advance
    Fvector2 interval{1.f, 1.f};
    Fvector2 atlas{1024.f, 1024.f};
    xr_unordered_map<u32, FontGlyph> glyphs;

    // ⚠⚠THE ATLAS CELL IS NOT THE SIZE THE ENGINE DRAWS AT, and reading it as
    // one is how this port came out with text a third too large. CGameFont keeps
    // `fCurrentHeight`, initialised to the cell -- and then
    // CFontManager::InitializeFont OVERWRITES it from the section's `size`, and
    // multiplies by `scale_x`/`scale_y`. This mod leans on exactly that: every
    // alias (letterica16, letterica18, small ...) resolves to ONE 52-pixel atlas
    // and differs only in these numbers. Ignore them and every font in the game
    // is the same size -- the biggest one.
    float size = 0.f; // section `size`, 0 = "no override, use the cell"
    Fvector2 scale{1.f, 1.f}; // section `scale_x` / `scale_y`

    // What CGameFont would put on the screen, in PHYSICAL pixels: the height a
    // caption has in the legacy menu at any resolution.
    float natural() const { return (size > 0.f ? size : cell) * scale.y; }

    const FontGlyph* find(u32 cp) const
    {
        auto it = glyphs.find(cp);
        return it == glyphs.end() ? nullptr : &it->second;
    }
};

// A face at one size. RmlUi asks for (family, size) and gets a handle back; the
// scale between the atlas and that size is the only thing a handle adds.
struct FontSized
{
    FontFace* face = nullptr;
    // Two scales, not one. The legacy quad is `size*scale_y` tall and
    // `glyph_width*scale_x` wide -- the atlas is stretched independently on each
    // axis, and a mod that sets only `size` is deliberately squashing the glyph.
    // Asking for a different font-size scales that whole shape uniformly, so
    // font-size == face->natural() reproduces the old menu exactly.
    float sx = 1.f; // atlas glyph width -> screen
    float sy = 1.f; // atlas cell height -> screen
    int req = 0; // the size RmlUi asked for; metrics.size is that times the text scale (ui_scale)
    Rml::FontMetrics metrics{};
};

// ⭐The names the legacy XML uses, mapped to the sections system.ltx defines --
// exactly the table CUIXmlInit::InitFont keeps, and the reason a .rml can say
// `font-family: graffiti22` and mean the same font the old screen meant.
struct FontAlias
{
    const char* family;
    const char* section;
};
const FontAlias FONT_ALIASES[] = {
    {"graffiti19", "ui_font_graffiti19_russian"}, {"graffiti22", "ui_font_graffiti22_russian"}, {"graffiti32", "ui_font_graff_32"},
    {"graffiti40", "ui_font_graff_40"},           {"graffiti50", "ui_font_graff_50"},          {"letterica16", "ui_font_letterica16_russian"},
    {"letterica18", "ui_font_letterica18_russian"}, {"letterica25", "ui_font_letter_25"},      {"arial14", "ui_font_arial_14"},
    {"arial21", "ui_font_arial_21"},              {"medium", "hud_font_medium"},               {"small", "hud_font_small"},
};

// Defined with the other space conversions, further down -- the glyph width
// needs it and the reason it exists at all is written there.
float text_stretch_x();
// ui_scale on the text of the screens that do not scale as a whole (see ui_scale, with dp_ratio).
float text_scale();

class XrayFontEngine final : public Rml::FontEngineInterface
{
public:
    // Nothing to load from a file: a family names an engine section, and the
    // section is read the first time somebody asks for the family. Answering
    // true keeps Rml::LoadFontFace callers (the debugger plugin) quiet.
    bool LoadFontFace(const Rml::String&, int, bool, Rml::Style::FontWeight) override { return true; }
    bool LoadFontFace(Rml::Span<const Rml::byte>, int, const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, bool) override { return true; }

    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle, Rml::Style::FontWeight, int size) override
    {
        FontFace* face = Face(family.c_str());
        if (!face)
            return Rml::FontFaceHandle(0);

        // the text scale goes in here, so a box sized by its text (a menu row, a button) grows with it
        const int req = size;
        size = _max(1, iFloor(float(req) * text_scale() + 0.5f));
        for (auto& s : m_sized)
            if (s->face == face && s->req == req && int(s->metrics.size) == size)
                return reinterpret_cast<Rml::FontFaceHandle>(s.get());

        auto sized = std::make_unique<FontSized>();
        sized->face = face;
        sized->req = req;
        // k = how far the requested size is from what the engine draws at.
        const float k = float(size) / _max(1.f, face->natural());
        sized->sy = float(size) / _max(1.f, face->cell);
        sized->sx = face->scale.x * k;
        // ⚠These are the numbers RmlUi lays a line of text out with, and a
        // bitmap cell gives no baseline of its own. 0.8/0.2 is where the
        // baseline sits in these atlases; getting it wrong shows up as text
        // riding high or low inside its box, not as anything more dramatic.
        sized->metrics.size = size;
        sized->metrics.ascent = float(size) * 0.80f;
        sized->metrics.descent = float(size) * 0.20f;
        sized->metrics.line_spacing = float(size) * face->interval.y * 1.15f;
        sized->metrics.x_height = float(size) * 0.5f;
        sized->metrics.underline_position = float(size) * 0.12f;
        sized->metrics.underline_thickness = _max(1.f, float(size) * 0.06f);
        sized->metrics.has_ellipsis = face->find(0x2026) != nullptr;

        m_sized.push_back(std::move(sized));
        return reinterpret_cast<Rml::FontFaceHandle>(m_sized.back().get());
    }

    Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle, const Rml::FontEffectList&) override { return Rml::FontEffectsHandle(0); }

    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override
    {
        static Rml::FontMetrics none{};
        auto* s = reinterpret_cast<FontSized*>(handle);
        return s ? s->metrics : none;
    }

    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& ctx, Rml::Character) override
    {
        auto* s = reinterpret_cast<FontSized*>(handle);
        if (!s)
            return 0;
        float w = 0.f;
        const char* p = string.begin();
        for (const char* end = string.end(); p < end;)
            w += Advance(*s, next_utf8(p, end)) + ctx.letter_spacing;
        return int(w + 0.5f);
    }

    int GenerateString(Rml::RenderManager& rm, Rml::FontFaceHandle handle, Rml::FontEffectsHandle, Rml::StringView string, Rml::Vector2f position,
                       Rml::ColourbPremultiplied colour, float, const Rml::TextShapingContext& ctx, Rml::TexturedMeshList& mesh_list) override
    {
        auto* s = reinterpret_cast<FontSized*>(handle);
        if (!s)
            return 0;

        Rml::Mesh mesh;
        const float top = position.y - s->metrics.ascent;
        const float h = s->face->cell * s->sy;
        float x = position.x;

        const char* p = string.begin();
        for (const char* end = string.end(); p < end;)
        {
            const u32 cp = next_utf8(p, end);
            const FontGlyph* g = s->face->find(cp);
            if (g && g->w > 0.f)
            {
                const float w = g->w * s->sx * text_stretch_x();
                const float u0 = g->x / s->face->atlas.x, v0 = g->y / s->face->atlas.y;
                const float u1 = (g->x + g->w) / s->face->atlas.x, v1 = (g->y + s->face->cell) / s->face->atlas.y;

                const int base = int(mesh.vertices.size());
                mesh.vertices.push_back({{x, top}, colour, {u0, v0}});
                mesh.vertices.push_back({{x + w, top}, colour, {u1, v0}});
                mesh.vertices.push_back({{x + w, top + h}, colour, {u1, v1}});
                mesh.vertices.push_back({{x, top + h}, colour, {u0, v1}});
                mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
            x += Advance(*s, cp) + ctx.letter_spacing;
        }

        if (!mesh.indices.empty())
            mesh_list.push_back({std::move(mesh), rm.LoadTexture(s->face->texture.c_str())});
        return int(x - position.x + 0.5f);
    }

    int GetVersion(Rml::FontFaceHandle) override { return 1; }

    void ReleaseFontResources() override
    {
        m_sized.clear();
        m_faces.clear();
    }

private:
    // ⚠The space is stored as a ZERO-WIDTH glyph in these atlases and its gap
    // comes from a separate number. The Vulkan font renderer has the same
    // special case, with the same note next to it: without it words run
    // together and the symptom reads as "the font lost its spaces".
    // ⚠Read live rather than baked into FontSized: the stretch changes with the
    // resolution, and a vid_restart from this very options page changes it. A
    // handle cached at the old ratio would keep laying text out at the old width
    // until something else happened to invalidate it.
    static float Advance(const FontSized& s, u32 cp)
    {
        const float sx = s.sx * text_stretch_x();
        const FontGlyph* g = s.face->find(cp);
        const float w = (g ? g->w : 0.f) * sx;
        if (w <= 0.f && (cp == 0x20 || cp == 0x3000))
            return s.face->space * sx * s.face->interval.x;
        return w * s.face->interval.x;
    }

    FontFace* Face(const char* family)
    {
        for (auto& f : m_faces)
            if (_stricmp(f->family.c_str(), family) == 0)
                return f.get();

        const char* section = nullptr;
        for (const FontAlias& a : FONT_ALIASES)
            if (_stricmp(a.family, family) == 0)
            {
                section = a.section;
                break;
            }
        // A family that is not an alias is taken as a section name outright, so
        // a mod can add a font without touching this table.
        if (!section)
            section = family;

        // ⭐...or, with no section at all, an atlas outright: `font-family:
        // rml_roboto` reads textures\ui\fonts\rml_roboto.dds + .ini. That is how
        // the menu ships its own fonts (_tools/make_rml_font.py) without editing
        // the mod's system.ltx, which lives in an archive.
        string_path probe;
        const xr_string direct = xr_string("ui\\fonts\\") + family;
        const bool has_section = pSettings && pSettings->section_exist(section);
        if (!has_section && !FS.exist(probe, "$game_textures$", direct.c_str(), ".ini"))
        {
            Msg("! [rml] font family '%s' is neither a section in system.ltx nor textures\\%s.ini", family, direct.c_str());
            return nullptr;
        }

        auto face = std::make_unique<FontFace>();
        face->family = family;
        if (!Load(*face, has_section ? section : nullptr, has_section ? nullptr : direct.c_str()))
            return nullptr;

        m_faces.push_back(std::move(face));
        return m_faces.back().get();
    }

    // Mirrors CGameFont::Initialize and CFontManager::GetFontTexName. Kept as a
    // mirror on purpose: the alternative is reaching into CGameFont's protected
    // state for the two numbers it does not expose (the atlas size and the cell
    // height), and a friend declaration in an engine header is a worse debt than
    // forty lines that read the same ini.
    // `direct` (and no section): the atlas path itself, no size variants, no
    // language suffix, no scale keys -- the file is exactly what is drawn.
    bool Load(FontFace& face, const char* section, const char* direct = nullptr)
    {
        string_path resolved;
        if (direct)
            xr_strcpy(resolved, sizeof(resolved), direct);
        else
        {
            // The atlas comes in three sizes and the engine picks by screen height.
            const char* tex_keys[] = {"texture800", "texture", "texture1600"};
            int idx = (Device.dwHeight <= 600) ? 0 : (Device.dwHeight < 1024 ? 1 : 2);
            const char* tex = nullptr;
            for (; idx >= 0 && !tex; --idx)
                if (pSettings->line_exist(section, tex_keys[idx]))
                    tex = pSettings->r_string(section, tex_keys[idx]);
            if (!tex)
                tex = pSettings->r_string(section, "texture");

            // Language suffix, unless the section opts out or the name is one of the
            // three the engine hard-codes as language-free.
            const bool skip = READ_IF_EXISTS(pSettings, r_bool, section, "skip_prefix", false) || strstr(tex, "ui_font_hud_01") || strstr(tex, "ui_font_hud_02") ||
                strstr(tex, "ui_font_console_02");
            LPCSTR lang = pSettings->r_string("string_table", "font_prefix");
            if (lang && !skip)
                strconcat(sizeof(resolved), resolved, tex, lang);
            else
                xr_strcpy(resolved, sizeof(resolved), tex);
        }
        face.texture = resolved;

        // The glyph table sits next to the atlas, under the same name.
        string_path base, ini_path;
        xr_strcpy(base, sizeof(base), resolved);
        if (strext(base))
            *strext(base) = 0;
        if (!FS.exist(ini_path, "$game_textures$", base, ".ini"))
        {
            Msg("! [rml] font '%s': no glyph table next to '%s'", face.family.c_str(), resolved);
            return false;
        }

        CInifile ini{ini_path, TRUE};
        const float fwc = READ_IF_EXISTS((&ini), r_float, "font_width_correction", "value", 0.0f);

        if (ini.section_exist("mb_symbol_coords"))
        {
            CInifile::Sect& sect = ini.r_section("mb_symbol_coords");
            face.cell = ini.r_float("mb_symbol_coords", "height");
            face.space = ceilf(face.cell / 2.f);
            for (const auto& item : sect.Ordered_Data)
            {
                if (_stricmp(item.first.c_str(), "height") == 0)
                    continue;
                const u32 cp = u32(atoi(item.first.c_str()));
                const Fvector v = ini.r_fvector3("mb_symbol_coords", item.first.c_str());
                const float w = v.z - v.x + fwc;
                if (cp == 0x20)
                {
                    face.space = w; // the font's own idea of a space
                    continue; // ...and it stays out of the glyph table
                }
                face.glyphs[cp] = {v.x, v.y, w};
            }
        }
        else if (ini.section_exist("symbol_coords"))
        {
            face.cell = ini.r_float("symbol_coords", "height");
            face.space = ceilf(face.cell / 2.f);
            // Single-byte atlas: index IS the cp1251 byte, so the table is keyed
            // by the code point that byte stands for.
            for (u32 i = 0; i < 256; ++i)
            {
                string64 key;
                xr_sprintf(key, sizeof(key), "%03d", i);
                if (!ini.line_exist("symbol_coords", key))
                    continue;
                const Fvector v = ini.r_fvector3("symbol_coords", key);
                const u32 cp = (i < 0x80) ? i : CP1251_HIGH[i - 0x80];
                if (!cp)
                    continue;
                const float w = v.z - v.x + fwc;
                if (i == 0x20)
                    face.space = w;
                else
                    face.glyphs[cp] = {v.x, v.y, w};
            }
        }
        else if (ini.section_exist("char widths"))
        {
            face.cell = ini.r_float("char widths", "height");
            face.space = ceilf(face.cell / 2.f);
            for (u32 i = 0; i < 256; ++i)
            {
                string64 key;
                xr_sprintf(key, sizeof(key), "%d", i);
                if (!ini.line_exist("char widths", key))
                    continue;
                const float w = ini.r_float("char widths", key);
                const u32 cp = (i < 0x80) ? i : CP1251_HIGH[i - 0x80];
                if (!cp)
                    continue;
                face.glyphs[cp] = {float(i % 16) * face.cell, float(i / 16) * face.cell, w};
            }
        }
        else if (ini.section_exist("font_size"))
        {
            face.cell = ini.r_float("font_size", "height");
            const float w = ini.r_float("font_size", "width");
            const int cpl = ini.r_s32("font_size", "cpl");
            face.space = w;
            for (u32 i = 0; i < 256 && cpl > 0; ++i)
            {
                const u32 cp = (i < 0x80) ? i : CP1251_HIGH[i - 0x80];
                if (!cp)
                    continue;
                face.glyphs[cp] = {float(int(i) % cpl) * w, float(int(i) / cpl) * face.cell, w};
            }
        }
        else
        {
            Msg("! [rml] font '%s': '%s' has no glyph table this build understands", face.family.c_str(), ini_path);
            return false;
        }

        // The four keys CFontManager::InitializeFont applies on top of the atlas.
        // Skipped in the first port, which is why every alias came out at the
        // cell height of the one atlas this mod ships.
        if (section && pSettings->line_exist(section, "interval"))
            face.interval = pSettings->r_fvector2(section, "interval");
        if (section && pSettings->line_exist(section, "scale_x"))
            face.scale.x = pSettings->r_float(section, "scale_x");
        if (section && pSettings->line_exist(section, "scale_y"))
            face.scale.y = pSettings->r_float(section, "scale_y");
        if (section && pSettings->line_exist(section, "size"))
            face.size = pSettings->r_float(section, "size");

        // The atlas size, which the glyph rectangles are relative to. Same
        // bind-and-ask as an image; without it every texture coordinate is off
        // by whatever the real atlas turns out to be.
        if (UIRender)
        {
            ui_shader probe;
            probe->create("hud\\default", face.texture.c_str());
            UIRender->SetShader(*probe);
            Fvector2 res{1024.f, 1024.f};
            UIRender->GetActiveTextureResolution(res);
            face.atlas.set(_max(1.f, res.x), _max(1.f, res.y));
        }

        // ⭐`natural` is the number a stylesheet wants: put it in `font-size` and
        // the caption is the size the legacy screen drew it. Logged because it
        // cannot be read off anything -- it is three ini keys multiplied.
        Msg("~ [rml] font '%s' -> %s: %u glyph(s), cell %.0f, atlas %.0fx%.0f, size %.1f, scale %.2fx%.2f, interval %.2fx%.2f => natural %.1fpx", face.family.c_str(),
            face.texture.c_str(), u32(face.glyphs.size()), face.cell, face.atlas.x, face.atlas.y, face.size, face.scale.x, face.scale.y, face.interval.x,
            face.interval.y, face.natural());
        return true;
    }

    xr_vector<std::unique_ptr<FontFace>> m_faces;
    xr_vector<std::unique_ptr<FontSized>> m_sized;
};

// ================================================================= pickers ====
//
// Which of the two documents is up. One picker object serves both: they differ
// in the file they load, what fills the list and what a click means, and every
// other line -- the rebuild-on-open cycle, the input routing, the cursor -- is
// the same code. A second copy of all that would be a second place for the
// stylesheet-cache and scrollbar traps to be got wrong.
enum class Picker
{
    None,
    Zone,
    Prop
};
Picker s_kind = Picker::None;

// The folder the model picker is showing, empty for the top level.
xr_string s_prop_group;

bool build_picker(Picker kind);
void fill_picker(Picker kind);

// Clicking a row sets the editor's section and closes the picker.
struct PickListener final : public Rml::EventListener
{
    xr_string section;
    explicit PickListener(const char* s) : section(s) {}
    void ProcessEvent(Rml::Event& /*ev*/) override
    {
        ClZoneEdit::SetSection(section.c_str());
        // Choosing an anomaly from a list IS the intent to place one. The picker
        // only ever set the section, so the operator picked something, watched
        // the panel close, and nothing happened -- the editor was still off and
        // nothing said so. Arm it here; ESC still leaves it.
        if (!ClZoneEdit::Active())
            ClZoneEdit::Toggle();
        UIRml::ShowZonePicker(false);
    }
    // RmlUi owns the listener once attached with the owning overload; this one
    // is attached non-owning, so it lives as long as the document does and is
    // released here when the element goes away.
    void OnDetach(Rml::Element* /*e*/) override { delete this; }
};

// Clicking a row in the model picker: a folder opens it, a model chooses it.
struct ModelListener final : public Rml::EventListener
{
    xr_string value;
    bool is_group;
    ModelListener(const char* v, bool group) : value(v), is_group(group) {}

    void ProcessEvent(Rml::Event& /*ev*/) override
    {
        if (is_group)
        {
            // ⚠Rebuilt rather than filtered in place: the rows carry listeners
            // and rewriting the list's inner RML would leave them attached to
            // elements that no longer exist. build_picker tears the document
            // down properly, which is also the path that reloads the stylesheet.
            s_prop_group = value;
            build_picker(Picker::Prop);
            return;
        }

        ClPropEdit::SetModel(value.c_str());
        // Choosing a model IS the intent to place one -- the same lesson the
        // anomaly picker learned: an operator who picks something and watches
        // the panel close expects to be holding it.
        if (!ClPropEdit::Active())
            ClPropEdit::Toggle();
        UIRml::ShowPropPicker(false);
    }
    void OnDetach(Rml::Element* /*e*/) override { delete this; }
};

XraySystem* s_system = nullptr;
XrayRender* s_render = nullptr;
XrayFontEngine* s_fonts = nullptr;
Rml::Context* s_context = nullptr;
Rml::ElementDocument* s_picker = nullptr;
bool s_ready = false;
int s_mouse_x = 0, s_mouse_y = 0;

// Deadline of the input mute, in dwTimeContinual milliseconds. Zero = not muted.
// Shared by both hosts: the picker checks it in UIRml::On*, the menu in
// UIRml::MenuOn* (ui_rml_menu.cpp), through UIRml::InputMuted().
u32 s_input_mute_until = 0;

UIRmlInternal::Space s_space = UIRmlInternal::Space::InGame;
float s_dp_applied = 0.f;

// ⚠"dp" in RmlUi is a device-independent pixel and it is one PHYSICAL pixel
// until somebody says otherwise -- the default ratio is 1.0. A layout authored
// in dp therefore shrinks as the screen grows, which is the exact opposite of
// what the unit is for. Anchor it to a reference height so a number in the CSS
// means the same fraction of the screen everywhere; which reference depends on
// the host, see UIRmlInternal::Space.
float dp_ratio();

// ⭐13-10 (the owner: "bigger text and buttons on the small screens, the minimap too -- one scale from the screen").
// `ui_scale`, 0 = from the screen: the Switch's own panel (720p handheld) 1.2, a phone 1.25, else 1. It goes in
// two ways, because the screens are of two kinds:
//   - the HUD (Space::InGame: the minimap, the icons, the weapon wheel, the hints, the news) is anchored to the
//     screen's corners, so it scales WHOLE: dp_ratio below;
//   - the full-screen screens (the menu, the inventory, the PDA, trade, talk) fill the screen with their layout, and
//     scaled whole they would run off it, so only their TEXT scales (the font engine, text_scale) -- and with it
//     every box sized by its text: the menu's rows, the buttons, the list rows.
} // namespace
float g_ui_scale = 0.f; // ui_scale (console_commands.cpp)
namespace
{
float ui_scale()
{
    if (g_ui_scale > 0.f)
        return g_ui_scale;
#if defined(__ANDROID__)
    return 1.25f;
#elif defined(__SWITCH__)
    return Device.dwHeight <= 720 ? 1.2f : 1.f; // handheld; docked it is a TV
#else
    return 1.f;
#endif
}
float text_scale() { return s_space == UIRmlInternal::Space::InGame ? 1.f : ui_scale(); }
// The HUD (scaled whole): on a phone by the panel's density -- the 1.25 above drew the minimap and the icons a fingertip
// small on the OnePlus 12 (14-10). 300 dpi = 1.0 (509 dpi: 1.7 -- at 1.88 the fourth quick slot left the screen), kept
// within 1.25..2. The full-screen screens keep ui_scale's text.
float hud_scale()
{
#if defined(__ANDROID__)
    if (g_ui_scale <= 0.f && g_display_dpi > 0.f)
        return std::clamp(g_display_dpi / 300.f, 1.25f, 2.f);
#endif
    return ui_scale();
}

float dp_ratio()
{
    if (s_space == UIRmlInternal::Space::Menu)
        return float(Device.dwHeight) / 768.f; // the legacy UI unit
    if (s_space == UIRmlInternal::Space::Screen)
        return float(Device.dwHeight) / 1080.f; // ⚠no floor: below 1080p it must SHRINK
    if (s_space == UIRmlInternal::Space::Screen1440)
        return float(Device.dwHeight) / 1440.f;
    return _max(1.f, float(Device.dwHeight) / 1080.f) * hud_scale();
}

// ⚠⚠THE FONT MUST UNDO THE STAGE'S STRETCH, and this is the single least
// obvious thing about porting X-Ray's front end.
//
// X-Ray stretches its 1024x768 UI over the screen anisotropically -- but ONLY
// the layout. The font never goes through that: CGameFont hands the renderer a
// height in physical pixels and vkFontRender multiplies it by
// g_current_font_scale, which is 1.0 for the whole UI. So in the old menu the
// boxes are a third wider than their XML says on a 16:9 screen and the LETTERS
// ARE NOT.
//
// Here the whole document -- text included -- sits under #stage's scaleX, so
// every glyph came out 33% wide. On screen that reads as "the text is a bit
// squashed", which sounds like a font bug and is really a coordinate-space one.
// Undoing it in the glyph advance is the only place it can go: the box must keep
// stretching (it holds legacy coordinates), and text-align then centres the
// unstretched run inside the stretched box, exactly as CUIStatic's alCenter did.
float text_stretch_x()
{
    if (s_space != UIRmlInternal::Space::Menu)
        return 1.f;
    const float k = UIRmlInternal::StageStretchX();
    return (k > 0.f) ? 1.f / k : 1.f;
}

// Pushes the context's size and density, and only when one of them moved:
// SetDensityIndependentPixelRatio dirties the layout of every document.
void sync_context()
{
    if (!s_context)
        return;
    const Rml::Vector2i want(int(Device.dwWidth), int(Device.dwHeight));
    if (s_context->GetDimensions() != want)
        s_context->SetDimensions(want);
    const float dp = dp_ratio();
    if (!fsimilar(dp, s_dp_applied))
    {
        s_dp_applied = dp;
        s_context->SetDensityIndependentPixelRatio(dp);
    }
}

// The pointer. RmlUi never draws one -- it only tells the backend which system
// cursor it would like -- and a game with the mouse captured has no system
// cursor to show. So the arrow is ours: two triangle fans through the same UI
// pipeline the documents use, a dark one offset by a couple of pixels behind a
// white one, which is what keeps it readable over both sky and rusty metal.
void draw_cursor()
{
    if (!UIRender)
        return;

    // A pointer outline, tip at the origin, in the classic proportions. Star
    // shaped about the tip, so a fan from vertex 0 triangulates it correctly.
    static const Fvector2 SHAPE[7] = {{0.f, 0.f}, {0.f, 16.f}, {3.6f, 12.4f}, {6.2f, 17.6f}, {8.8f, 16.5f}, {6.1f, 11.5f}, {11.f, 11.5f}};
    constexpr u32 TRIS = 5; // fan: (0,1,2) (0,2,3) ... (0,5,6)

    const float s = _max(1.f, float(Device.dwHeight) / 1080.f) * 1.5f;
    const float px = float(s_mouse_x), py = float(s_mouse_y);

    struct Pass
    {
        float ox, oy;
        u32 colour;
    };
    const Pass passes[2] = {{2.f, 2.f, color_rgba(0, 0, 0, 170)}, {0.f, 0.f, color_rgba(255, 255, 255, 255)}};

    for (const Pass& p : passes)
    {
        UIRender->StartPrimitive(TRIS * 3, IUIRender::ptTriList, IUIRender::pttTL);
        for (u32 t = 0; t < TRIS; ++t)
        {
            const int corner[3] = {0, int(t) + 1, int(t) + 2};
            for (int c : corner)
            {
                const float x = px + p.ox + SHAPE[c].x * s;
                const float y = py + p.oy + SHAPE[c].y * s;
                UIRender->PushPoint(x, y, 0.f, p.colour, 0.f, 0.f);
            }
        }
        UIRender->SetDynTexture(s_white_tex);
        UIRender->FlushPrimitive();
    }
}

// ⚠Built from scratch on every open, and that is the point rather than an
// oversight. The document was previously loaded once and only re-Shown, so
// editing the .rcss and reopening the panel changed nothing until the game was
// restarted -- which quietly cost this track the exact loop it exists for. RmlUi
// also caches parsed stylesheets by path, so dropping the document is not enough
// on its own; the cache has to go too or the file is never read again.
bool build_picker(Picker kind)
{
    if (s_picker)
    {
        // ⚠Safe to call from inside a click on one of its own rows: RmlUi
        // detaches the tree here but defers the actual release to the next
        // Context::Update, so the element being dispatched on outlives the
        // handler that asked for this.
        s_picker->Close(); // detaches the listeners, which delete themselves
        s_picker = nullptr;
    }
    s_kind = kind;

    s_picker = UIRmlInternal::BuildDocument((kind == Picker::Prop) ? "prop_picker.rml" : "zone_picker.rml");
    if (!s_picker)
        return false;

    fill_picker(kind);
    return true;
}

// Fills the list of whichever picker was just loaded. ⚠Called during a click on
// one of the OLD document's rows (a folder row rebuilds the list), so nothing in
// here may run Context::Update -- that is where RmlUi releases the document the
// event is still being dispatched on.
void fill_picker(Picker kind)
{
    // The list is content, not markup: what goes in it comes from the configs
    // and the filesystem at runtime, so the document ships empty and is filled
    // here.
    Rml::Element* list = s_picker->GetElementById("list");
    if (!list)
    {
        Msg("! [rml] picker document has no element with id 'list'");
        return; // the document is still up; an empty panel is the symptom
    }

    auto add_row = [&](const char* label, const char* value, bool group, const char* css_class) {
        Rml::ElementPtr item = s_picker->CreateElement("div");
        item->SetClass("row", true);
        if (css_class)
            item->SetClass(css_class, true);
        item->SetAttribute("data-section", value);
        item->SetInnerRML(label);
        if (kind == Picker::Prop)
            item->AddEventListener(Rml::EventId::Click, new ModelListener(value, group), false);
        else
            item->AddEventListener(Rml::EventId::Click, new PickListener(value), false);
        list->AppendChild(std::move(item));
    };

    if (kind == Picker::Prop)
    {
        // ⚠As numeric entities, not as literal Cyrillic. This file compiles
        // under codepage 1252 (C4566 on anything outside it), so the text that
        // reaches a document from C++ has to be ASCII on the way -- RmlUi
        // decodes the entities when it parses the RML. Text that lives in the
        // .rml/.rcss content files has no such constraint and is written
        // normally there.
        if (Rml::Element* title = s_picker->GetElementById("title"))
            title->SetInnerRML(s_prop_group.empty() ? "&#1052;&#1054;&#1044;&#1045;&#1051;&#1048;" : s_prop_group.c_str());

        if (s_prop_group.empty())
        {
            xr_vector<shared_str> groups;
            ClPropEdit::EnumerateGroups(groups);
            for (const auto& g : groups)
                add_row(g.c_str(), g.c_str(), true, "group");
            Msg("~ [rml] model picker: %u group(s)", u32(groups.size()));
        }
        else
        {
            // ⭐The way back, as a row rather than as a key. A two-level list
            // whose only exit is a keystroke nobody was told about is a list
            // people get stuck in.
            add_row("&#8592; &#1085;&#1072;&#1079;&#1072;&#1076;", "", true, "group");

            xr_vector<shared_str> models;
            ClPropEdit::EnumerateModels(s_prop_group.c_str(), models);
            const size_t cut = s_prop_group.size() + 1; // show the leaf, not the path
            for (const auto& m : models)
            {
                const char* leaf = (xr_strlen(m.c_str()) > cut) ? m.c_str() + cut : m.c_str();
                add_row(leaf, m.c_str(), false, nullptr);
            }
            Msg("~ [rml] model picker: %u model(s) in '%s'", u32(models.size()), s_prop_group.c_str());
        }
        return;
    }

    xr_vector<shared_str> sections;
    ClZoneEdit::EnumerateSections(sections);
    for (const auto& s : sections)
        add_row(s.c_str(), s.c_str(), false, nullptr);
    Msg("~ [rml] picker filled with %u section(s)", u32(sections.size()));
}
} // namespace

// ========================================================= shared internals ====

Rml::Context* UIRmlInternal::Context() { return s_context; }

// The document lives next to the game's other content so it can be edited and
// reloaded without a build -- which is the entire point of HTML/CSS authoring.
xr_string UIRmlInternal::ContentPath(const char* leaf)
{
    string_path p;
    FS.update_path(p, "$game_config$", "");
    xr_string s = p;
    s += "ui_rml\\";
    s += leaf;
    return s;
}

void UIRmlInternal::UseSpace(Space s)
{
    s_space = s;
    sync_context();
}

UIRmlInternal::Space UIRmlInternal::CurrentSpace() { return s_space; }

float UIRmlInternal::DpRatio() { return dp_ratio(); }

float UIRmlInternal::StageStretchX() { return (float(Device.dwWidth) / 1024.f) / (float(Device.dwHeight) / 768.f); }

void UIRmlInternal::CursorTo(int x, int y)
{
    s_mouse_x = x;
    s_mouse_y = y;
    clamp(s_mouse_x, 0, int(Device.dwWidth) - 1);
    clamp(s_mouse_y, 0, int(Device.dwHeight) - 1);
    if (s_context)
        s_context->ProcessMouseMove(s_mouse_x, s_mouse_y, 0);
}

void UIRmlInternal::CursorBy(int dx, int dy) { CursorTo(s_mouse_x + dx, s_mouse_y + dy); }

// The text scale moved (ui_scale typed, the Switch docked or taken out): every element asks for its font again.
// Here, at the top of a frame, not in sync_context -- that one runs inside RmlUi's event dispatch too, and
// ReleaseFontResources updates every context.
void UIRmlInternal::ScaleFrame()
{
    static float s_applied = 0.f;
    const float s = ui_scale();
    if (fsimilar(s, s_applied))
        return;
    if (s_applied > 0.f)
    {
        Msg("~ [rml] ui_scale %.2f -> %.2f", s_applied, s);
        Rml::ReleaseFontResources();
    }
    s_applied = s;
}

// ⭐13-10 (the owner: "press the buttons on the touch screen in the menus, the map with gestures"). RmlUi 6.2 knows
// touch itself: a touch is the left button that scrolls the scroll container under it (with momentum when flicked)
// and clicks only if it did not move -- so taps, swipes through lists, dragging items and the PDA map's one-finger
// pan all come from it. Ours on top: a press held still ~0.55 s is the right button (the inventory's properties box,
// a mark on the PDA map), and two fingers are a pinch -- zoom and pan on the PDA map, nothing elsewhere.
void UIRmlInternal::TouchFrame()
{
    static u32 s_frame = u32(-1);
    if (s_frame == Device.dwFrame)
        return;
    s_frame = Device.dwFrame;

    struct Finger
    {
        bool down = false; // RmlUi has it (ProcessTouchStart sent)
        bool spent = false; // a long press or a pinch used it: nothing more until every finger is up
        Rml::TouchId id = 0;
        Rml::Vector2f start, last;
        u32 t0 = 0;
    };
    static Finger s_f;
    static bool s_pinch = false;
    static float s_pinch_d = 0.f;
    static Rml::Vector2f s_pinch_mid;

    const xr_pad_touch_state& t = g_pad_touch;
    const bool menu = UIRml::MenuActive(); // the main / pause menu: over any screen of the level
    const bool want =s_context && (menu || UIRml::WantsInput()) && !UIRml::InputMuted();
    const u32 n = (want && t.frame && Device.dwFrame - t.frame <= 1) ? t.count : 0;
    auto at = [&](int i) { return Rml::Vector2f(t.x[i] * float(Device.dwWidth), t.y[i] * float(Device.dwHeight)); };
    auto sync_pointer = [](const Rml::Vector2f& p) {
        s_mouse_x = iFloor(p.x);
        s_mouse_y = iFloor(p.y);
        clamp(s_mouse_x, 0, int(Device.dwWidth) - 1);
        clamp(s_mouse_y, 0, int(Device.dwHeight) - 1);
    };

    if (n == 0)
    {
        if (s_f.down && !s_f.spent && s_context)
        {
            s_context->ProcessTouchEnd(Rml::TouchList{Rml::Touch{s_f.id, s_f.last}}, 0);
            if (menu)
                s_context->ProcessMouseLeave(); // no pointer in the menu on the pad: a hover would stay a selection
        }
        s_f = Finger{};
        s_pinch = false;
        return;
    }

    if (n >= 2)
    {
        const Rml::Vector2f a = at(0), b = at(1), mid = (a + b) * 0.5f;
        const float d = _max(1.f, (a - b).Magnitude());
        if (!s_pinch)
        {
            // the first finger leaves RmlUi without a click: away from everything, then up
            if (s_f.down && !s_f.spent)
            {
                s_context->ProcessMouseLeave();
                s_context->ProcessTouchCancel(Rml::TouchList{Rml::Touch{s_f.id, s_f.last}});
            }
            s_f.down = s_f.spent = true;
            s_pinch = true;
            s_pinch_d = d;
            s_pinch_mid = mid;
            PdaPinch(true, 1.f, mid.x, mid.y, 0.f, 0.f);
            return;
        }
        PdaPinch(false, d / s_pinch_d, mid.x, mid.y, mid.x - s_pinch_mid.x, mid.y - s_pinch_mid.y);
        s_pinch_d = d;
        s_pinch_mid = mid;
        return;
    }

    // one finger
    if (s_f.spent)
        return;
    const Rml::Vector2f p = at(0);
    if (!s_f.down)
    {
        s_f.down = true;
        s_f.id = Rml::TouchId(t.id[0]);
        s_f.start = s_f.last = p;
        s_f.t0 = Device.dwTimeContinual;
        sync_pointer(p);
        s_context->ProcessTouchStart(Rml::TouchList{Rml::Touch{s_f.id, p}}, 0);
        return;
    }
    if (p.x != s_f.last.x || p.y != s_f.last.y)
    {
        s_f.last = p;
        sync_pointer(p);
        s_context->ProcessTouchMove(Rml::TouchList{Rml::Touch{s_f.id, p}}, 0);
    }
    const float still = 10.f * s_context->GetDensityIndependentPixelRatio();
    if (!menu && Device.dwTimeContinual - s_f.t0 >= 550 && (p - s_f.start).SquaredMagnitude() < still * still)
    {
        // held still: the press ends (a click on what is under it, as a mouse would select), then the right button
        s_context->ProcessTouchEnd(Rml::TouchList{Rml::Touch{s_f.id, p}}, 0);
        s_context->ProcessMouseButtonDown(1, 0);
        s_context->ProcessMouseButtonUp(1, 0);
        s_f.spent = true;
    }
}

void UIRmlInternal::DrawCursor() { draw_cursor(); }

bool UIRmlInternal::FeedText(int dik)
{
    if (!s_context || !pInput)
        return false;

    // ⭐The engine already owns the hard part -- the keyboard layout, the shift
    // state and the exclusive-mode workaround -- and hands back UTF-8 packed
    // into a u16. Unpack rather than reimplement: a second call to ToUnicodeEx
    // here would be a second place for the layout handling to drift.
    const u16 packed = pInput->DikToChar(dik, true);
    if (!packed)
        return false;

    const u8 b0 = u8(packed & 0xFF), b1 = u8(packed >> 8);
    u32 cp = b0;
    if (b0 >= 0xC0 && b1)
        cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
    if (cp < 0x20)
        return false;

    s_context->ProcessTextInput(Rml::Character(cp));
    return true;
}

xr_string UIRmlInternal::ToUtf8(const char* text)
{
    xr_string out;
    if (!text)
        return out;

    // ⚠⚠WHICH ENCODING THE GAME'S TEXT IS IN IS NOT A CONSTANT. Vanilla X-Ray
    // ships cp1251 string tables and its fonts carry cp1251 aliases for exactly
    // that reason; a modern OGSR mod ships them in UTF-8 instead, and both work
    // in the legacy UI because its multibyte conversion is byte-for-byte and the
    // atlas is keyed by code point. RmlUi is UTF-8 only, so guessing wrong is
    // not a subtle fault -- every Cyrillic caption comes out as "PPPIPC PёPiCP",
    // which is what converting already-UTF-8 text a second time looks like.
    //
    // ⭐The two are separable with certainty rather than by heuristic: cp1251
    // Cyrillic occupies C0..FF, and a valid UTF-8 lead byte in that range must
    // be followed by a continuation byte in 80..BF -- which no cp1251 Cyrillic
    // letter is. Real Russian text therefore cannot be valid UTF-8 by accident.
    auto is_utf8 = [](const u8* p) {
        bool multibyte = false;
        while (*p)
        {
            if (*p < 0x80)
            {
                ++p;
                continue;
            }
            int extra = ((*p & 0xE0) == 0xC0) ? 1 : ((*p & 0xF0) == 0xE0) ? 2 : ((*p & 0xF8) == 0xF0) ? 3 : -1;
            if (extra < 0)
                return false;
            ++p;
            for (int i = 0; i < extra; ++i, ++p)
                if ((*p & 0xC0) != 0x80)
                    return false;
            multibyte = true;
        }
        return multibyte;
    };

    const u8* src = reinterpret_cast<const u8*>(text);
    if (is_utf8(src))
        return xr_string(text);

    for (const u8* p = src; *p; ++p)
    {
        const u32 cp = (*p < 0x80) ? *p : CP1251_HIGH[*p - 0x80];
        if (cp)
            append_utf8(out, cp);
    }
    return out;
}

xr_string UIRmlInternal::Translate(const char* id)
{
    if (!id || !id[0])
        return xr_string();
    // ⚠The string table answers with the id itself when it has no entry, which
    // is the behaviour every legacy screen relies on -- a missing caption shows
    // its key rather than an empty box. Kept.
    return ToUtf8(CStringTable().translate(id).c_str());
}

void UIRmlInternal::Localise(Rml::Element* root)
{
    if (!root)
        return;

    const Rml::String id = root->GetAttribute<Rml::String>("data-loc", "");
    if (!id.empty())
    {
        const xr_string text = Translate(id.c_str());
        // ⚠SetInnerRML, not SetInnerXML-of-text: the string table is full of
        // \n line breaks that the old screens rendered as breaks. Escaping the
        // markup characters keeps a stray '<' in a translation from eating the
        // rest of the caption.
        xr_string html;
        for (const char* p = text.c_str(); *p; ++p)
        {
            if (*p == '<')
                html += "&lt;";
            else if (*p == '>')
                html += "&gt;";
            else if (*p == '&')
                html += "&amp;";
            else if (*p == '\\' && p[1] == 'n')
            {
                html += "<br/>";
                ++p;
            }
            else if (*p == '\n')
                html += "<br/>";
            else
                html += *p;
        }
        root->SetInnerRML(html.c_str());
    }

    for (int i = 0; i < root->GetNumChildren(); ++i)
        Localise(root->GetChild(i));
}

void UIRmlInternal::AttachSprites(Rml::ElementDocument* doc)
{
    if (!doc)
        return;

    // ⭐⭐The legacy screens are drawn almost entirely out of atlases: a name
    // like `ui_button_ordinary` is an entry in ui\textures_descr\*.xml that says
    // which file and which rectangle inside it. RmlUi has exactly this concept
    // -- @spritesheet -- but only as authored CSS, so the bridge is to generate
    // that CSS from the table the engine already parsed at menu construction.
    //
    // One block per file, because a spritesheet has a single source. Built once:
    // the table does not change while the game runs.
    static Rml::String s_rcss;
    static bool s_built = false;
    if (!s_built)
    {
        s_built = true;

        xr_map<xr_string, xr_string> by_file; // file -> accumulated sprite lines
        u32 sprites = 0, skipped = 0;
        for (const auto& kv : CUITextureMaster::Textures())
        {
            const char* name = kv.first.c_str();
            // RCSS identifiers: anything else would not parse, and a sprite
            // nobody can name is not worth a parse error in the log.
            bool ok = name && name[0];
            for (const char* c = name; ok && *c; ++c)
                ok = isalnum(u8(*c)) || *c == '_' || *c == '-';
            if (!ok)
            {
                ++skipped;
                continue;
            }

            const Frect& r = kv.second.rect;
            if (r.width() <= 0.f || r.height() <= 0.f)
            {
                ++skipped;
                continue;
            }

            string512 line;
            xr_sprintf(line, sizeof(line), "\t%s: %.0fpx %.0fpx %.0fpx %.0fpx;\n", name, r.x1, r.y1, r.width(), r.height());
            by_file[kv.second.file.c_str()] += line;
            ++sprites;
        }

        for (const auto& kv : by_file)
        {
            s_rcss += "@spritesheet xr_";
            // The block needs a name and the file is the only unique thing to
            // hand; punctuation in a path is not an identifier, so it is mangled
            // rather than quoted.
            for (const char* c = kv.first.c_str(); *c; ++c)
                s_rcss += (isalnum(u8(*c)) ? *c : '_');
            s_rcss += " {\n\tsrc: ";
            s_rcss += kv.first.c_str();
            s_rcss += ";\n";
            s_rcss += kv.second.c_str();
            s_rcss += "}\n";
        }
        Msg("~ [rml] sprite library: %u sprite(s) in %u file(s), %u skipped", sprites, u32(by_file.size()), skipped);
    }

    if (s_rcss.empty())
        return;

    CombineGeneratedSheet(doc, s_rcss, "sprite");
}

// Parsed generated sheets, by the address of the static string they came from.
// ⚠Dropped in UIRml::Shutdown BEFORE Rml::Shutdown: a container outliving
// RmlUi would release its decorators into a library that is already gone.
static xr_map<const Rml::String*, Rml::SharedPtr<Rml::StyleSheetContainer>> s_parsed_sheets;
static xr_map<std::pair<Rml::String, const Rml::String*>, Rml::SharedPtr<Rml::StyleSheetContainer>> s_combined_sheets;   // see CombineGeneratedSheet
// ui_rml_live_rcss (console_commands.cpp): see BuildDocument.
int g_rml_live_rcss = 1;

void UIRmlInternal::CombineGeneratedSheet(Rml::ElementDocument* doc, const Rml::String& rcss, const char* what)
{
    if (!doc || rcss.empty())
        return;

    // ⚠Sprites first, the document's own rules second: combining puts the
    // argument at the higher priority, and a screen must always be able to
    // override anything this generated. Combining makes a NEW container, so
    // the cached one is never handed to a document and never changes.
    const Rml::StyleSheetContainer* own = doc->GetStyleSheetContainer();
    if (!own)
    {
        // No sheet of its own (no screen does this today): the document gets
        // a container to itself, parsed fresh.
        if (auto extra = Rml::Factory::InstanceStyleSheetString(rcss))
            doc->SetStyleSheetContainer(std::move(extra));
        else
            Msg("! [rml] the generated %s sheet did not parse", what);
        return;
    }

    Rml::SharedPtr<Rml::StyleSheetContainer>& parsed = s_parsed_sheets[&rcss];
    if (!parsed)
    {
        parsed = Rml::Factory::InstanceStyleSheetString(rcss);
        if (!parsed)
        {
            Msg("! [rml] the generated %s sheet did not parse", what);
            s_parsed_sheets.erase(&rcss);
            return;
        }
    }
    // 10-10: the combined container too, per document and generated sheet. Combining copies all ~2200 sprites into a new
    // container (7-8 ms each on the Switch, three a time for the inventory: 20 of its 42 ms an open). The document's own
    // container cannot be the key -- every LoadDocument combines its <link> sheets into a NEW one -- but with the sheet
    // cache kept (ui_rml_live_rcss 0) its content is the same each time, and so is the order of the calls, so the chain
    // (sprites -> item icons -> portrait) comes back as the same three containers, compiled styles and all.
    if (!g_rml_live_rcss)
    {
        Rml::SharedPtr<Rml::StyleSheetContainer>& combined = s_combined_sheets[std::make_pair(doc->GetSourceURL(), &rcss)];
        if (!combined)
            combined = parsed->CombineStyleSheetContainer(*own);
        doc->SetStyleSheetContainer(combined);
        return;
    }
    doc->SetStyleSheetContainer(parsed->CombineStyleSheetContainer(*own));
}

Rml::ElementDocument* UIRmlInternal::BuildDocument(const char* leaf)
{
    if (!s_context)
        return nullptr;

    // RmlUi caches parsed stylesheets by path, so dropping the document is not
    // enough on its own to pick up an edited .rcss -- the cache has to go too.
    // 10-10: only while ui_rml_live_rcss (default 1, for the one editing a .rcss): every screen re-parsed its sheets on
    // every open -- the inventory built in 66 ms on the Switch, whose preset turns it off.
    if (g_rml_live_rcss)
    {
        Rml::Factory::ClearStyleSheetCache();
        s_combined_sheets.clear();   // built from the sheets just dropped
    }

    const xr_string path = ContentPath(leaf);
    const u64 t0 = CPU::QPC();
    Rml::ElementDocument* doc = s_context->LoadDocument(path.c_str());
    if (!doc)
    {
        Msg("! [rml] could not load '%s'", path.c_str());
        return nullptr;
    }
    const u64 t1 = CPU::QPC();

    // a phone (14-10): the sheets may make what a finger hits bigger -- body.touch (inventory.rcss: the item menu)
    if (g_touch_controls)
        doc->SetClass("touch", true);

    AttachSprites(doc);
    const u64 t2 = CPU::QPC();
    Localise(doc);
    const u64 t3 = CPU::QPC();

    // ⚠Showing is part of BUILDING, not part of opening, and putting it in the
    // caller cost the model picker its second level. A document that has just
    // been loaded is invisible, and RmlUi removes the old one from the tree the
    // moment Close() is called -- so a folder row rebuilt the list into a
    // document nobody ever showed: the panel vanished, WantsInput() went false
    // with it, and every click after that went to the game. From a chair it
    // read as "the picker ignores me".
    doc->Show();
    const u64 t4 = CPU::QPC();
    const double f = 1000.0 / double(CPU::QPCFreq());
    Msg("~ [rml] document '%s': load %.1f | sprites %.1f | localise %.1f | show %.1f ms", leaf, f * double(t1 - t0), f * double(t2 - t1),
        f * double(t3 - t2), f * double(t4 - t3));
    return doc;
}

// ============================================================ the core API ====

bool UIRml::Ready() { return s_ready; }

bool UIRml::Init()
{
    if (s_ready)
        return true;
    if (!UIRender)
        return false;

    s_system = xr_new<XraySystem>();
    s_render = xr_new<XrayRender>();
    s_fonts = xr_new<XrayFontEngine>();
    Rml::SetSystemInterface(s_system);
    Rml::SetRenderInterface(s_render);
    // ⚠Before Initialise, not after: RmlUi installs its own FreeType engine
    // during initialisation if nobody has claimed the slot, and the claim is
    // what keeps every caption on the game's own bitmap fonts.
    Rml::SetFontEngineInterface(s_fonts);

    if (!Rml::Initialise())
    {
        Msg("! [rml] Rml::Initialise failed");
        return false;
    }

    s_context = Rml::CreateContext("game", Rml::Vector2i(int(Device.dwWidth), int(Device.dwHeight)));
    if (!s_context)
    {
        Msg("! [rml] CreateContext failed");
        Rml::Shutdown();
        return false;
    }

    s_dp_applied = 0.f;
    sync_context();

    const u32 white = 0xFFFFFFFFu;
    s_white_tex = UIRender->DynTextureCreate(&white, 1, 1);
    if (!s_white_tex)
        Msg("! [rml] no white texture -- untextured fills will come out dark olive");

    s_ready = true;
    Msg("~ [rml] up: %ux%u, dp ratio %.3f", Device.dwWidth, Device.dwHeight, dp_ratio());
    return true;
}

void UIRml::Shutdown()
{
    if (!s_ready)
        return;
    s_picker = nullptr;
    // The menu's background shader holds a CTheoraSurface and a mapped staging
    // buffer; dropped before the renderer goes, not after.
    UIRmlInternal::MenuReleaseGpu();
    UIRmlInternal::LoadShutdown();
    s_parsed_sheets.clear(); // before the library they point into goes
    s_combined_sheets.clear();
    Rml::Shutdown(); // releases every texture through ReleaseTexture first
    UIRmlInternal::HudShutdown();
    UIRmlInternal::TouchControlsShutdown();
    if (s_white_tex && UIRender)
        UIRender->DynTextureDestroy(s_white_tex);
    s_white_tex = nullptr;
    xr_delete(s_render);
    xr_delete(s_system);
    xr_delete(s_fonts);
    s_context = nullptr;
    s_ready = false;
}

void UIRml::Update()
{
    // The HUD is the first thing to want RmlUi in a level that has never opened a
    // panel, so it gets to bring it up before the readiness check.
    UIRmlInternal::HudPreUpdate();
    if (!s_ready || !s_context)
        return;
    // ⚠The unit follows the SCREEN. The pickers were authored against a 1080-tall
    // reference; the inventory is a port of a legacy 1024x768 layout and needs
    // that space instead. One context means one ratio, so whichever is up wins,
    // and the inventory wins over a dev tool.
    UIRmlInternal::PdaPendingFrame(); // may open the PDA: before the space is chosen
    UIRmlInternal::UseSpace(UIRmlInternal::InvVisible() ? UIRmlInternal::InvSpace() : UIRmlInternal::Space::InGame);
    UIRmlInternal::ScaleFrame();
    UIRmlInternal::InvFrame();
    UIRmlInternal::HudFrame();
    UIRmlInternal::TouchControlsFrame();
    UIRmlInternal::TouchFrame();
    s_context->Update();
}

void UIRml::Render()
{
    if (!s_ready || !s_context)
        return;
    s_context->Render();
    if (UIRender)
        UIRender->SetScissor(nullptr); // leave the legacy UI the state it expects
    // After the scissor is off: the pointer belongs to the screen, not to
    // whichever clipped box the document happened to finish inside.
    if (WantsInput() && !UIRmlInternal::InvPadMode())
        draw_cursor();
}

bool UIRml::WantsInput() { return s_ready && ((s_picker && s_picker->IsVisible()) || UIRmlInternal::InvVisible()); }

// ⭐The unattended-run guard. See the note in ui_rml.h for what it is for.
//
// ⚠Measured on dwTimeContinual (wall clock minus the time the window spent
// inactive) rather than dwTimeGlobal, which is game time and stops with the
// simulation -- a mute set from the main menu would then never run out.
void UIRml::InputMute(float seconds)
{
    if (seconds <= 0.f)
    {
        s_input_mute_until = 0;
        Msg("~ [rml] input mute off");
        return;
    }
    // ⚠Capped, and the cap is the point: this exists to survive a sweep, and a
    // typo of one extra digit should not cost a session.
    if (seconds > 600.f)
        seconds = 600.f;
    s_input_mute_until = Device.dwTimeContinual + u32(seconds * 1000.f);
    Msg("~ [rml] input muted for %.0f s -- keyboard and mouse will not reach the UI (the console still works)", seconds);
}

bool UIRml::InputMuted()
{
    if (!s_input_mute_until)
        return false;
    if (Device.dwTimeContinual >= s_input_mute_until)
    {
        s_input_mute_until = 0;
        Msg("~ [rml] input mute expired");
        return false;
    }
    return true;
}

bool UIRml::OnMouseDelta(int dx, int dy)
{
    if (!WantsInput())
        return false;
    if (InputMuted())
        return true;
    UIRmlInternal::CursorBy(dx, dy);
    return true;
}

bool UIRml::OnWheel(int direction)
{
    if (!WantsInput())
        return false;
    if (InputMuted())
        return true;
    s_context->ProcessMouseWheel(-UIRmlInternal::WheelNotches(direction), 0);
    return true;
}

// ⚠⚠The engine passes the wheel through RAW: DirectInput's Z offset is ±120 per
// notch (WHEEL_DELTA), SDL's wheel.y is ±1. RmlUi reads its argument as NOTCHES
// and scrolls 80dp each (Context.cpp, UNIT_SCROLL_LENGTH), so on the PC one
// click asked for 9600dp -- every list jumped straight to its end. Small values
// are SDL's notches already; anything else is DirectInput units (a high-res
// wheel sends fractions of 120, which become fractions of a notch).
float UIRmlInternal::WheelNotches(int direction)
{
    if (direction > -10 && direction < 10)
        return float(direction);
    return float(direction) / 120.f;
}

bool UIRml::OnChar(u16 ch)
{
    if (!WantsInput())
        return false;
    if (InputMuted())
        return true;
    s_context->ProcessTextInput(Rml::Character(ch));
    return true;
}

bool UIRml::OnKey(int dik, bool down)
{
    if (!WantsInput())
        return false;
    if (InputMuted())
        return true;

    // ⚠The right button is forwarded too, and it is not decoration: the
    // inventory's properties box is a right-click menu, and RmlUi only ever sees
    // the buttons this function hands it.
    if (dik == MOUSE_1 || dik == MOUSE_2)
    {
        const int button = (dik == MOUSE_1) ? 0 : 1;
        if (down)
            s_context->ProcessMouseButtonDown(button, 0);
        else
            s_context->ProcessMouseButtonUp(button, 0);

        // ⭐A click in RmlUi is dispatched to the element that was under the
        // pointer at PRESS time, and only if the same one is still under it at
        // RELEASE. So "nothing happened" has several causes that look alike --
        // wrong element, no element, or the two disagreeing -- and printing both
        // ends separates them in one round.
        auto name = [](Rml::Element* e) -> xr_string {
            if (!e)
                return "<none>";
            xr_string s = e->GetTagName().c_str();
            const Rml::String id = e->GetId();
            if (!id.empty())
            {
                s += "#";
                s += id.c_str();
            }
            const Rml::String cls = e->GetAttribute<Rml::String>("data-section", "");
            if (!cls.empty())
            {
                s += "[";
                s += cls.c_str();
                s += "]";
            }
            return s;
        };
        Msg("~ [rml] mouse %d %s at %d,%d hover=%s focus=%s", button, down ? "DOWN" : "UP", s_mouse_x, s_mouse_y, name(s_context->GetHoverElement()).c_str(),
            name(s_context->GetFocusElement()).c_str());
        return true;
    }
    // ⭐The screen's own keys FIRST: the talk screen answers Esc itself (it must
    // end the legacy dialog under it, not just close its view).
    if (down && UIRmlInternal::InvKey(dik))
        return true;
    if (dik == DIK_ESCAPE && down)
    {
        // Whichever one is up. ⚠Hiding by kind rather than unconditionally would
        // leave the other document on screen with nothing listening to it.
        if (UIRmlInternal::InvVisible())
            UIRml::InvClose();
        else if (s_picker)
            s_picker->Hide();
        return true;
    }
    // ⭐The inventory key has to close the inventory FROM HERE. While a document
    // is up this function answers "mine" to every key, so the game's own
    // kINVENTORY branch never runs -- without this the screen would open on I and
    // only close on ESC, which is not how the legacy window behaves.
    if (down && UIRmlInternal::InvVisible() && is_binded(kINVENTORY, dik))
    {
        UIRml::InvClose();
        return true;
    }
    return true; // while a document is up it owns the keyboard
}

namespace
{
// Opening either picker is the same act; only which document gets built differs.
void show_picker(Picker kind, bool show)
{
    if (!show)
    {
        if (s_picker)
            s_picker->Hide();
        return;
    }

    if (!UIRml::Init())
        return;

    UIRmlInternal::UseSpace(UIRmlInternal::Space::InGame);

    // Start the pointer in the middle rather than at (0,0), so the first mouse
    // movement does not have to travel across the screen to reach the panel.
    UIRmlInternal::CursorTo(int(Device.dwWidth) / 2, int(Device.dwHeight) / 2);

    if (!build_picker(kind))
        return; // build_picker shows it -- so does the rebuild on a folder row

    // ⭐Where the box actually landed. "Nothing is on screen" has two causes
    // that look identical from a chair -- no geometry produced at all, or a box
    // laid out somewhere off-screen -- and one log line separates them. Update()
    // first: layout is not solved until the context runs.
    s_context->Update();
    if (Rml::Element* panel = s_picker->GetElementById("panel"))
    {
        const Rml::Vector2f at = panel->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f sz = panel->GetBox().GetSize(Rml::BoxArea::Border);
        Msg("~ [rml] panel at %.0f,%.0f size %.0fx%.0f on %ux%u", at.x, at.y, sz.x, sz.y, Device.dwWidth, Device.dwHeight);
    }
}
} // namespace

void UIRml::ShowZonePicker(bool show) { show_picker(Picker::Zone, show); }
bool UIRml::ZonePickerVisible() { return s_picker && s_kind == Picker::Zone && s_picker->IsVisible(); }

void UIRml::ShowPropPicker(bool show)
{
    // ⭐Always back to the top level on open. The folder it was left in is the
    // right memory WITHIN a browsing session and the wrong one between them: an
    // operator pressing F9 expects the library, not wherever they were an hour
    // ago.
    if (show)
        s_prop_group.clear();
    show_picker(Picker::Prop, show);
}
bool UIRml::PropPickerVisible() { return s_picker && s_kind == Picker::Prop && s_picker->IsVisible(); }
