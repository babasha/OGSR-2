// vk_hom.cpp -- see vk_hom.h. Port of OGSR Layers/xrRender/HOM.cpp, occRasterizer.cpp, occRasterizer_core.cpp.
// Kept line-for-line where it matters (rasterizer, propagation, box test, depth quantisation); comments marked
// "port:" say what changed.
#include "stdafx.h"
#include "vk_hom.h"

#include "../../xrCDB/xrCDB.h"
#include "../../xrCDB/Frustum.h"

#include <algorithm>
#include <chrono>

namespace VK::Hom
{
namespace
{
constexpr int occ_dim_0 = 64;
constexpr int occ_dim = occ_dim_0 + 4; // 2 pixel border around frame

struct occTri
{
    occTri* adjacent[3];
    Fvector raster[3];
    Fplane plane;
    float area;
    u32 flags;
    u32 skip;
    Fvector center;
};

constexpr float occQ_s32 = float(0x40000000); // [-2..2]
typedef s32 occD;

// ---------------------------------------------------------------------------------------------------------------------
// occRasterizer
struct occRasterizer
{
    occTri* bufFrame[occ_dim][occ_dim];
    float bufDepth[occ_dim][occ_dim];
    occD bufDepth_0[occ_dim_0][occ_dim_0];

    static int df_2_s32(float d) { return iFloor(d * occQ_s32); }
    static int df_2_s32up(float d) { return iCeil(d * occQ_s32); }

    occTri** get_frame() { return &(bufFrame[0][0]); }
    float* get_depth() { return &(bufDepth[0][0]); }
    occD* get_depth_level() { return &(bufDepth_0[0][0]); }

    void clear()
    {
        std::fill(&bufFrame[0][0], &bufFrame[0][0] + occ_dim * occ_dim, nullptr);
        std::fill(&bufDepth[0][0], &bufDepth[0][0] + occ_dim * occ_dim, 1.f);
    }
    void propagade();
    u32 rasterize(occTri* T);
    BOOL test(float x0, float y0, float x1, float y1, float z);
};
occRasterizer Raster;

inline bool shared(occTri* T1, occTri* T2)
{
    if (T1 == T2)
        return true;
    if (T1->adjacent[0] == T2)
        return true;
    if (T1->adjacent[1] == T2)
        return true;
    if (T1->adjacent[2] == T2)
        return true;
    return false;
}

void occRasterizer::propagade()
{
    // Clip-and-propagade zero level
    occTri** pFrame = get_frame();
    float* pDepth = get_depth();
    for (int y = 0; y < occ_dim_0; y++)
    {
        for (int x = 0; x < occ_dim_0; x++)
        {
            int ox = x + 2, oy = y + 2;

            // Y2-connect
            const int pos = oy * occ_dim + ox;
            const int pos_up = pos - occ_dim;
            const int pos_down = pos + occ_dim;
            const int pos_down2 = pos_down + occ_dim;

            occTri* Tu1 = pFrame[pos_up];
            if (Tu1)
            {
                // We has pixel 1scan up
                if (shared(Tu1, pFrame[pos_down]))
                {
                    // We has pixel 1scan down
                    const float ZR = (pDepth[pos_up] + pDepth[pos_down]) / 2;
                    if (ZR < pDepth[pos])
                    {
                        pFrame[pos] = Tu1;
                        pDepth[pos] = ZR;
                    }
                }
                else if (shared(Tu1, pFrame[pos_down2]))
                {
                    // We has pixel 2scan down
                    const float ZR = (pDepth[pos_up] + pDepth[pos_down2]) / 2;
                    if (ZR < pDepth[pos])
                    {
                        pFrame[pos] = Tu1;
                        pDepth[pos] = ZR;
                    }
                }
            }

            float d = pDepth[pos];
            clamp(d, -1.99f, 1.99f);
            bufDepth_0[y][x] = df_2_s32(d);
        }
    }
}

inline BOOL test_Level(occD* depth, int dim, float _x0, float _y0, float _x1, float _y1, occD z)
{
    int x0 = iFloor(_x0 * dim + .5f);
    clamp(x0, 0, dim - 1);
    int x1 = iFloor(_x1 * dim + .5f);
    clamp(x1, x0, dim - 1);
    int y0 = iFloor(_y0 * dim + .5f);
    clamp(y0, 0, dim - 1);
    int y1 = iFloor(_y1 * dim + .5f);
    clamp(y1, y0, dim - 1);

    for (int y = y0; y <= y1; y++)
    {
        occD* base = depth + y * dim;
        const occD* it = base + x0;
        const occD* end = base + x1;
        for (; it <= end; it++)
            if (z < *it)
                return TRUE;
    }
    return FALSE;
}

BOOL occRasterizer::test(float _x0, float _y0, float _x1, float _y1, float _z)
{
    const occD z = df_2_s32up(_z) + 1;
    return test_Level(get_depth_level(), occ_dim_0, _x0, _y0, _x1, _y1, z);
}

// ---------------------------------------------------------------------------------------------------------------------
// occRasterizer_core
occTri* currentTri = nullptr;
u32 dwPixels = 0;
float currentA[3], currentB[3], currentC[3];

const int BOTTOM = 0, TOP = 1;

void i_order(float* A, float* B, float* C)
{
    float *min, *max, *mid;
    if (A[1] <= B[1])
    {
        if (B[1] <= C[1])
        {
            min = A;
            mid = B;
            max = C;
        }
        else // C < B
            if (A[1] <= C[1])
            {
                min = A;
                mid = C;
                max = B;
            }
            else
            {
                min = C;
                mid = A;
                max = B;
            }
    }
    else // B < A
    {
        if (A[1] <= C[1])
        {
            min = B;
            mid = A;
            max = C;
        }
        else // C < A
            if (B[1] <= C[1])
            {
                min = B;
                mid = C;
                max = A;
            }
            else
            {
                min = C;
                mid = B;
                max = A;
            }
    }

    currentA[0] = min[0] + 2;
    currentB[0] = mid[0] + 2;
    currentC[0] = max[0] + 2;
    currentA[1] = min[1] + 2;
    currentB[1] = mid[1] + 2;
    currentC[1] = max[1] + 2;
    currentA[2] = min[2];
    currentB[2] = mid[2];
    currentC[2] = max[2];
}

inline void Vclamp(int& v, int a, int b)
{
    if (v < a)
        v = a;
    else if (v >= b)
        v = b - 1;
}

// Rasterize a scan line between given X point values, corresponding Z values and current color
void i_scan(int curY, float leftX, float lhx, float rightX, float rhx, float startZ, float endZ)
{
    // calculate span(s)
    const float start_c = leftX + lhx;
    const float end_c = rightX + rhx;

    const float startR = leftX - lhx;
    const float endR = rightX - rhx;

    float startT = startR, endT = end_c;
    float startX = start_c, endX = endR;
    if (start_c < startR)
    {
        startT = start_c;
        startX = startR;
    }
    if (end_c < endR)
    {
        endT = endR;
        endX = end_c;
    }

    // guard-banding and clipping
    int minT = iFloor(startT) - 1, maxT = iCeil(endT) + 1;
    Vclamp(minT, 1, occ_dim - 1);
    Vclamp(maxT, 1, occ_dim - 1);
    if (minT >= maxT)
        return;

    int minX = iCeil(startX), maxX = iFloor(endX);
    Vclamp(minX, 0, occ_dim);
    Vclamp(maxX, 0, occ_dim);
    int limLeft, limRight;
    if (minX > maxX)
    {
        limLeft = maxX;
        limRight = minX;
    }
    else
    {
        limLeft = minX;
        limRight = maxX;
    }

    // interpolate
    const float lenR = endR - startR;
    const float Zlen = endZ - startZ;
    float Z = startZ + (minT - startR) / lenR * Zlen; // interpolate Z to the start
    const float Zend = startZ + (maxT - startR) / lenR * Zlen; // interpolate Z to the end
    const float dZ = (Zend - Z) / (maxT - minT); // increment in Z / pixel wrt dX

    // Move to far my dz/5 to place the pixel at the center of face that it covers.
    // This will make sure that objects will not be clipped for just standing next to the home from outside.
    Z += 0.5f * _abs(dZ);

    // gain access to buffers
    occTri** pFrame = Raster.get_frame();
    float* pDepth = Raster.get_depth();

    // left connector
    const int i_base = curY * occ_dim;
    int i = i_base + minT;
    int limit = i_base + limLeft;
    for (; i < limit; i++, Z += dZ)
    {
        if (shared(currentTri, pFrame[i - 1]))
        {
            if (Z < pDepth[i])
            {
                pFrame[i] = currentTri;
                pDepth[i] = std::max(Z, pDepth[i - 1]);
                dwPixels++;
            }
        }
    }

    // compute the scanline
    limit = i_base + maxX;
    for (; i < limit; i++, Z += dZ)
    {
        if (Z < pDepth[i])
        {
            pFrame[i] = currentTri;
            pDepth[i] = Z;
            dwPixels++;
        }
    }

    // right connector
    i = i_base + maxT - 1;
    limit = i_base + limRight;
    Z = Zend - dZ;
    for (; i >= limit; i--, Z -= dZ)
    {
        if (shared(currentTri, pFrame[i + 1]))
        {
            if (Z < pDepth[i])
            {
                pFrame[i] = currentTri;
                pDepth[i] = std::max(Z, pDepth[i + 1]);
                dwPixels++;
            }
        }
    }
}

/*
Rasterises 1 section of the triangle a 'section' of a triangle is the portion of a triangle between
2 horizontal scan lines corresponding to 2 vertices of a triangle
p2.y >= p1.y, p1, p2 are start/end vertices
E1 E2 are the triangle edge differences of the 2 bounding edges for this section
*/
inline void i_section(int Sect, BOOL bMiddle)
{
    // Find the start/end Y pixel coord, set the starting pts for scan line ends
    int startY, endY;
    float *startp1, *startp2;
    float E1[3], E2[3];

    if (Sect == BOTTOM)
    {
        startY = iCeil(currentA[1]);
        endY = iFloor(currentB[1]) - 1;
        startp1 = startp2 = currentA;
        if (bMiddle)
            endY++;

        // check 'endY' for out-of-triangle
        const int test = iFloor(currentC[1]);
        if (endY >= test)
            endY--;

        // Find the edge differences
        E1[0] = currentB[0] - currentA[0];
        E2[0] = currentC[0] - currentA[0];
        E1[1] = currentB[1] - currentA[1];
        E2[1] = currentC[1] - currentA[1];
        E1[2] = currentB[2] - currentA[2];
        E2[2] = currentC[2] - currentA[2];
    }
    else
    {
        startY = iCeil(currentB[1]);
        endY = iFloor(currentC[1]);
        startp1 = currentA;
        startp2 = currentB;
        if (bMiddle)
            startY--;

        // check 'startY' for out-of-triangle
        const int test = iCeil(currentA[1]);
        if (startY < test)
            startY++;

        // Find the edge differences
        E1[0] = currentC[0] - currentA[0];
        E2[0] = currentC[0] - currentB[0];
        E1[1] = currentC[1] - currentA[1];
        E2[1] = currentC[1] - currentB[1];
        E1[2] = currentC[2] - currentA[2];
        E2[2] = currentC[2] - currentB[2];
    }
    Vclamp(startY, 0, occ_dim);
    Vclamp(endY, 0, occ_dim);
    if (startY >= endY)
        return;

    // Compute the inverse slopes of the lines, ie rate of change of X by Y
    const float mE1 = E1[0] / E1[1];
    const float mE2 = E2[0] / E2[1];

    // Initial Y offset for left and right (due to pixel rounding)
    float e1_init_dY = float(startY) - startp1[1], e2_init_dY = float(startY) - startp2[1];
    float t, leftX, leftZ, rightX, rightZ, left_dX, right_dX, left_dZ, right_dZ;

    // find initial values, step values
    if (((mE1 < mE2) && (Sect == BOTTOM)) || ((mE1 > mE2) && (Sect == TOP)))
    {
        // E1 is on the Left
        t = e1_init_dY / E1[1]; // Initial fraction of offset
        leftX = startp1[0] + E1[0] * t;
        left_dX = mE1;
        leftZ = startp1[2] + E1[2] * t;
        left_dZ = E1[2] / E1[1];

        t = e2_init_dY / E2[1];
        rightX = startp2[0] + E2[0] * t;
        right_dX = mE2;
        rightZ = startp2[2] + E2[2] * t;
        right_dZ = E2[2] / E2[1];
    }
    else
    {
        // E2 is on left
        t = e2_init_dY / E2[1];
        leftX = startp2[0] + E2[0] * t;
        left_dX = mE2;
        leftZ = startp2[2] + E2[2] * t;
        left_dZ = E2[2] / E2[1];

        t = e1_init_dY / E1[1];
        rightX = startp1[0] + E1[0] * t;
        right_dX = mE1;
        rightZ = startp1[2] + E1[2] * t;
        right_dZ = E1[2] / E1[1];
    }

    // Now scan all lines in this section
    const float lhx = left_dX / 2;
    leftX += lhx; // half pixel
    const float rhx = right_dX / 2;
    rightX += rhx; // half pixel
    for (; startY <= endY; startY++)
    {
        i_scan(startY, leftX, lhx, rightX, rhx, leftZ, rightZ);
        leftX += left_dX;
        rightX += right_dX;
        leftZ += left_dZ;
        rightZ += right_dZ;
    }
}

u32 occRasterizer::rasterize(occTri* T)
{
    // Order the vertices by Y
    currentTri = T;
    dwPixels = 0;
    i_order(&(T->raster[0].x), &(T->raster[1].x), &(T->raster[2].x));

    // Rasterize sections
    if (currentB[1] - iFloor(currentB[1]) > .5f)
    {
        i_section(BOTTOM, 1); // Rasterise First Section
        i_section(TOP, 0);    // Rasterise Second Section
    }
    else
    {
        i_section(BOTTOM, 0);
        i_section(TOP, 1);
    }
    return dwPixels;
}

// ---------------------------------------------------------------------------------------------------------------------
// CHOM
#pragma pack(push, 4)
struct HOM_poly
{
    Fvector v1, v2, v3;
    u32 flags;
};
#pragma pack(pop)

CDB::COLLIDER s_xrc;   // port: xrXRC -> the collider it wraps
CDB::MODEL* s_model = nullptr;
occTri* s_tris = nullptr;
Fmatrix s_xform, s_xform_01;
bool s_frameValid = false;
u32 s_frustumTris = 0, s_rasterTris = 0;
float s_rasterMs = 0.f;

float area(const Fvector& v0, const Fvector& v1, const Fvector& v2)
{
    const float e1 = v0.distance_to(v1);
    const float e2 = v0.distance_to(v2);
    const float e3 = v1.distance_to(v2);

    const float p = (e1 + e2 + e3) / 2.f;
    return _sqrt(p * (p - e1) * (p - e2) * (p - e3));
}

void Render_DB(CFrustum& base)
{
    // Update projection matrices on every frame to ensure valid HOM culling
    constexpr float view_dim = occ_dim_0;
    const Fmatrix m_viewport{view_dim / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, -view_dim / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, view_dim / 2.f + 0 + 0, view_dim / 2.f + 0 + 0, 0.0f, 1.0f};
    const Fmatrix m_viewport_01{1.f / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, -1.f / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.f / 2.f + 0 + 0, 1.f / 2.f + 0 + 0, 0.0f, 1.0f};

    s_xform.mul(m_viewport, Device.mFullTransform);
    s_xform_01.mul(m_viewport_01, Device.mFullTransform);

    // Query DB
    s_xrc.frustum_query(0, s_model, base);
    if (0 == s_xrc.r_count())
        return;

    // Prepare
    CDB::RESULT* it = s_xrc.r_begin();
    CDB::RESULT* end = std::remove_if(it, s_xrc.r_end(), [&](const CDB::RESULT& _1) {
        const occTri& T = s_tris[_1.id];
        return T.skip > Device.dwFrame;
    });

    const Fvector& COP = Device.vCameraPosition;
    std::sort(it, end, [&](const CDB::RESULT& _1, const CDB::RESULT& _2) {
        const occTri& t0 = s_tris[_1.id];
        const occTri& t1 = s_tris[_2.id];
        return COP.distance_to_sqr(t0.center) < COP.distance_to_sqr(t1.center);
    });

    // Build frustum with near plane only
    CFrustum clip;
    Fmatrix full = Device.mFullTransform;   // port: CreateFromMatrix takes a non-const ref here
    clip.CreateFromMatrix(full, FRUSTUM_P_NEAR);
    sPoly src, dst;
    const u32 _frame = Device.dwFrame;

    s_frustumTris = u32(s_xrc.r_count());

    // Perfrom selection, sorting, culling
    for (; it != end; it++)
    {
        // Control skipping
        occTri& T = s_tris[it->id];
        const u32 next = _frame + ::Random.randI(3, 10);

        // Test for good occluder - should be improved :)
        if (!(T.flags || (T.plane.classify(COP) > 0)))
        {
            T.skip = next;
            continue;
        }

        // Access to triangle vertices
        CDB::TRI& t = s_model->get_tris()[it->id];
        const Fvector* v = s_model->get_verts();
        src.clear();
        dst.clear();
        src.push_back(v[t.verts[0]]);
        src.push_back(v[t.verts[1]]);
        src.push_back(v[t.verts[2]]);
        sPoly* P = clip.ClipPoly(src, dst);
        if (nullptr == P)
        {
            T.skip = next;
            continue;
        }

        // XForm and Rasterize
        s_rasterTris++;
        u32 pixels = 0;
        const int limit = int(P->size()) - 1;
        for (int vi = 1; vi < limit; vi++)
        {
            s_xform.transform(T.raster[0], (*P)[0]);
            s_xform.transform(T.raster[1], (*P)[vi + 0]);
            s_xform.transform(T.raster[2], (*P)[vi + 1]);
            pixels += Raster.rasterize(&T);
        }
        if (0 == pixels)
        {
            T.skip = next;
            continue;
        }
    }
}

inline BOOL xform_b0(Fvector2& min, Fvector2& max, float& minz, const Fmatrix& X, const float _x, const float _y, const float _z)
{
    const float z = _x * X._13 + _y * X._23 + _z * X._33 + X._43;
    if (z < EPS)
        return TRUE;
    const float iw = 1.f / (_x * X._14 + _y * X._24 + _z * X._34 + X._44);
    min.x = max.x = (_x * X._11 + _y * X._21 + _z * X._31 + X._41) * iw;
    min.y = max.y = (_x * X._12 + _y * X._22 + _z * X._32 + X._42) * iw;
    minz = 0.f + z * iw;
    return FALSE;
}
inline BOOL xform_b1(Fvector2& min, Fvector2& max, float& minz, const Fmatrix& X, float _x, float _y, float _z)
{
    float t;
    const float z = _x * X._13 + _y * X._23 + _z * X._33 + X._43;
    if (z < EPS)
        return TRUE;
    const float iw = 1.f / (_x * X._14 + _y * X._24 + _z * X._34 + X._44);
    t = (_x * X._11 + _y * X._21 + _z * X._31 + X._41) * iw;
    if (t < min.x)
        min.x = t;
    else if (t > max.x)
        max.x = t;
    t = (_x * X._12 + _y * X._22 + _z * X._32 + X._42) * iw;
    if (t < min.y)
        min.y = t;
    else if (t > max.y)
        max.y = t;
    t = 0.f + z * iw;
    if (t < minz)
        minz = t;
    return FALSE;
}
} // namespace

void Load()
{
    Unload();
    // Find and open file
    string_path fName;
    FS.update_path(fName, fsgame::level, fsgame::level_files::level_hom);
    if (!FS.exist(fName))
    {
        Msg("! [VK HOM] occlusion map '%s' not found -- HOM off (everything visible)", fName);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();

    IReader* fs = FS.r_open(fName);

    // Load tris and merge them
    CDB::Collector CL;
    {
        IReader* S = fs->open_chunk(1);
        while (!S->eof())
        {
            HOM_poly P;
            S->r(&P, sizeof(P));
            CL.add_face_packed_D(P.v1, P.v2, P.v3, P.flags, 0.01f);
        }
        S->close();
    }
    FS.r_close(fs);

    // Determine adjacency
    xr_vector<u32> adjacency;
    CL.calc_adjacency(adjacency);

    // Create RASTER-triangles
    s_tris = xr_alloc<occTri>(CL.getTS());
    u32 bad = 0;
    for (size_t it{}; it < CL.getTS(); it++)
    {
        const CDB::TRI& clT = CL.getT()[it];
        occTri& rT = s_tris[it];
        const Fvector& v0 = CL.getV()[clT.verts[0]];
        const Fvector& v1 = CL.getV()[clT.verts[1]];
        const Fvector& v2 = CL.getV()[clT.verts[2]];
        rT.adjacent[0] = (0xffffffff == adjacency[3 * it + 0]) ? ((occTri*)(-1)) : (s_tris + adjacency[3 * it + 0]);
        rT.adjacent[1] = (0xffffffff == adjacency[3 * it + 1]) ? ((occTri*)(-1)) : (s_tris + adjacency[3 * it + 1]);
        rT.adjacent[2] = (0xffffffff == adjacency[3 * it + 2]) ? ((occTri*)(-1)) : (s_tris + adjacency[3 * it + 2]);
        rT.flags = static_cast<u32>(clT.dummy);
        rT.area = area(v0, v1, v2);
        if (rT.area < EPS_L)
            ++bad;   // port: counted, not printed one by one
        rT.plane.build(v0, v1, v2);
        rT.skip = 0;
        rT.center.add(v0, v1).add(v2).div(3.f);
    }

    // Create AABB-tree
    s_model = xr_new<CDB::MODEL>();
    s_model->build(CL.getV(), CL.getVS(), CL.getT(), CL.getTS());
    Msg("[VK HOM] loaded %s: %zu occluder triangles (%u degenerate), %zu verts in %.0f ms", fName, CL.getTS(), bad, CL.getVS(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void Unload()
{
    xr_delete(s_model);
    xr_free(s_tris);
    s_frameValid = false;
}

bool Ready() { return s_model != nullptr; }

void Frame()
{
    s_frameValid = false;
    s_frustumTris = s_rasterTris = 0;
    if (!s_model)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    CFrustum ViewBase;
    Fmatrix full = Device.mFullTransform;
    ViewBase.CreateFromMatrix(full, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
    Raster.clear();
    Render_DB(ViewBase);
    Raster.propagade();
    s_frameValid = true;
    s_rasterMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

bool VisibleBox(const Fbox& B)
{
    if (!s_frameValid)
        return true;
    // Find min/max points of xformed-box
    Fvector2 min, max;
    float z;
    if (xform_b0(min, max, z, s_xform_01, B.min.x, B.min.y, B.min.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.min.x, B.min.y, B.max.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.max.x, B.min.y, B.max.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.max.x, B.min.y, B.min.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.min.x, B.max.y, B.min.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.min.x, B.max.y, B.max.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.max.x, B.max.y, B.max.z))
        return true;
    if (xform_b1(min, max, z, s_xform_01, B.max.x, B.max.y, B.min.z))
        return true;
    return Raster.test(min.x, min.y, max.x, max.y, z) != FALSE;
}

void Stats(u32& frustumTris, u32& rasterTris, float& rasterMs)
{
    frustumTris = s_frustumTris;
    rasterTris = s_rasterTris;
    rasterMs = s_rasterMs;
}
} // namespace VK::Hom
