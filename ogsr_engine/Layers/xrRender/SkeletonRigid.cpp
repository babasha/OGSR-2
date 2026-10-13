#include "stdafx.h"

#include "SkeletonCustom.h"
#include "../../xrCore/cpu_budget.h"        // CpuBudget::enabled / CyclesPerMs — прибор ниже
#include "../../xrCore/xr_platform_time.h"  // xrPlatform::TickCount64 (отчёт по реальным часам)
#include "../../xrCore/stacktrace_collector.h" // ResolveCodeAddress — имя колбэка по адресу
#include <atomic>

extern int psSkeletonUpdate;

// ---------------------------------------------------------------------------
// ⭐ПРИБОР `[Bones]`: ИЗ ЧЕГО СОСТОИТ CalculateBones (при `cpu_budget >= 2`).
//
// Зачем. 19-09 фаза prepare показала, что Skinned_UploadBones стоит 0.38-0.45
// core-ms/кадр -- третья статья главного потока. Но «вынести на воркеры» для
// трёх её частей значит РАЗНОЕ по риску: треки гоняют колбэки конца анимации в
// игровой код, костные матрицы зовут костные колбэки (физика, прицел), а
// перерасчёт бокса дёргает ГЛОБАЛЬНЫЙ ::Random. Прежде чем что-то делить, надо
// знать, какая из трёх и есть эта цена.
//
// ⚠⚠ЦЕНА САМОГО ПРИБОРА НЕ МАЛА: четыре `QueryThreadCycleTime` на вызов ×~100
// вызовов = ~400 в кадр, плюс ~565 от `[VK Bones/up]` — порядка 0.1 core-ms при
// `cpu_budget 2`. Для A/B это безразлично (одинаково в обеих ветках), но
// АБСОЛЮТНЫЕ числа кадра под ним завышены, и сравнивать их с прогоном без
// `cpu_budget 2` нельзя. Та же семья ловушек, что `r_profiler`.
// ⚠ЦИКЛЫ ПОТОКА, не время стены (см. cpu_budget.h). ⚠Только внешний уровень:
// костный колбэк может позвать CalculateBones другого объекта (мьютекс тут
// рекурсивный не случайно), и без счётчика глубины вложенное время посчиталось
// бы дважды. ⚠Счётчики атомарные: эта функция УЖЕ зовётся с нескольких потоков.
// ---------------------------------------------------------------------------
namespace
{
std::atomic<u64> s_bnTracks{0}, s_bnBones{0}, s_bnBox{0}, s_bnCalls{0}, s_bnBoxCalls{0};
// ⭐Сколько ИГРОВОГО кода уехало на чужой поток вместе с костями (`r_bones_mt`):
// сами вызовы CalculateBones и, отдельно, сработавшие костные колбэки.
std::atomic<u64> s_bnOffMain{0}, s_bnCbOffMain{0};

// ---------------------------------------------------------------------------
// ⭐⭐ПЕРЕПИСЬ КОСТНЫХ КОЛБЭКОВ: КТО ИМЕННО, СКОЛЬКО РАЗ, С КАКОГО ПОТОКА.
//
// Зачем. Вынос CalculateBones на пул (`r_bones_mt 1`) даёт −0.32 core-ms, но
// уносит туда 107-171 костный колбэк в кадр — игровой код xrGame. Решать,
// законно это или нет, по СПИСКУ РЕГИСТРАЦИЙ нельзя: в коде 46 мест
// `set_callback`, а в конкретной сцене живут единицы, и какие именно — зависит
// от того, кто рядом стоит. Нужен не список, а ПЕРЕПИСЬ: адрес функции →
// имя → счётчики.
//
// Горячий путь без блокировок: таблица фиксированного размера, вставка через
// CAS в пустой слот, счётчики атомарные. Различных колбэков единицы, поэтому
// линейный поиск по ≤32 записям дешевле любой карты. Имя разрешается ОДИН РАЗ
// при печати (ResolveCodeAddress ходит в dbghelp под мьютексом — на горячем
// пути ему делать нечего).
// ---------------------------------------------------------------------------
constexpr u32 kCbMax = 32;
struct BoneCbRec
{
    std::atomic<void*> fn{nullptr};
    std::atomic<u32>   type{0};
    std::atomic<u64>   calls{0};
    std::atomic<u64>   offMain{0};
};
BoneCbRec s_cbTab[kCbMax];
std::atomic<u64> s_cbLost{0};   // не хватило слотов -- честнее, чем молча терять

void BoneCbCount(void* fn, u32 type, bool offMain)
{
    for (u32 i = 0; i < kCbMax; ++i)
    {
        void* cur = s_cbTab[i].fn.load(std::memory_order_acquire);
        if (cur == fn)
        {
            s_cbTab[i].calls.fetch_add(1, std::memory_order_relaxed);
            if (offMain) s_cbTab[i].offMain.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cur == nullptr)
        {
            void* expected = nullptr;
            if (s_cbTab[i].fn.compare_exchange_strong(expected, fn, std::memory_order_acq_rel))
            {
                s_cbTab[i].type.store(type, std::memory_order_relaxed);
                s_cbTab[i].calls.fetch_add(1, std::memory_order_relaxed);
                if (offMain) s_cbTab[i].offMain.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            --i;   // слот занял другой поток -- пересмотреть его же
        }
    }
    s_cbLost.fetch_add(1, std::memory_order_relaxed);
}

void BoneCbReport(double frames)
{
    const char* kType[] = { "dummy", "PHYSICS", "custom" };
    u64 total = 0, totalOff = 0;
    for (u32 i = 0; i < kCbMax; ++i) {
        total    += s_cbTab[i].calls.load(std::memory_order_relaxed);
        totalOff += s_cbTab[i].offMain.load(std::memory_order_relaxed);
    }
    Msg("[BoneCb] census: %.0f fires/frame (%.0f off the main thread), %llu distinct%s",
        double(total) / frames, double(totalOff) / frames,
        [] { u64 n = 0; for (u32 i = 0; i < kCbMax; ++i) if (s_cbTab[i].fn.load()) ++n; return n; }(),
        s_cbLost.load() ? " [!] TABLE FULL, some lost" : "");
    for (u32 i = 0; i < kCbMax; ++i)
    {
        void* fn = s_cbTab[i].fn.load(std::memory_order_relaxed);
        if (!fn) continue;
        const u64 c = s_cbTab[i].calls.exchange(0, std::memory_order_relaxed);
        const u64 o = s_cbTab[i].offMain.exchange(0, std::memory_order_relaxed);
        if (!c) continue;
        const u32 t = s_cbTab[i].type.load(std::memory_order_relaxed);
        Msg("[BoneCb]   %-7s %8.1f/frame  off-main %7.1f  %s", t < 3 ? kType[t] : "?",
            double(c) / frames, double(o) / frames, ResolveCodeAddress(fn).c_str());
    }
}
u32 s_bnFrame0 = 0, s_bnLastMs = 0;
thread_local int s_bnDepth = 0;

inline u64 BnCycles()
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return u64(c);
}

void BnMaybeLog()
{
    if (!Device.OnMainThread()) return;
    const u32 now = (u32)xrPlatform::TickCount64();
    if (!s_bnLastMs) { s_bnLastMs = now; s_bnFrame0 = Device.dwFrame; return; }
    if (now - s_bnLastMs < 5000) return;

    const double cpm = CpuBudget::CyclesPerMs();
    const double frames = double(Device.dwFrame > s_bnFrame0 ? Device.dwFrame - s_bnFrame0 : 1);
    const u64 tr = s_bnTracks.exchange(0), bo = s_bnBones.exchange(0), bx = s_bnBox.exchange(0);
    const u64 nc = s_bnCalls.exchange(0), nb = s_bnBoxCalls.exchange(0);
    const u64 om = s_bnOffMain.exchange(0), ocb = s_bnCbOffMain.exchange(0);
    Msg("[Bones] CalculateBones %.1f calls/frame | tracks %.3f + bonecalc %.3f + visibox %.3f = %.3f core-ms/frame"
        " (visibox on %.2f of them)",
        double(nc) / frames, double(tr) / cpm / frames, double(bo) / cpm / frames, double(bx) / cpm / frames,
        double(tr + bo + bx) / cpm / frames, double(nb) / frames);
    // ⭐Сколько игрового кода уехало на чужой поток. Ноль во второй графе значит,
    // что параллелится ЧИСТАЯ математика и колбэки xrGame тут ни при чём.
    Msg("[Bones]   off the main thread: %.1f calls/frame, %.1f bone callbacks/frame",
        double(om) / frames, double(ocb) / frames);
    BoneCbReport(frames);
    s_bnLastMs = now;
    s_bnFrame0 = Device.dwFrame;
}
}   // namespace

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s);
#endif

void CKinematics::CalculateBones(BOOL bForceExact)
{
    std::scoped_lock UCalc_Lock(UCalc_Mutex);

    // early out.
    // check if the info is still relevant
    // skip all the computations - assume nothing changes in a small period of time :)
    if (Device.dwTimeGlobal == UCalc_Time)
        return; // early out for "fast" update

    ZoneScoped;

    if (!bForceExact && (Device.dwTimeGlobal < (UCalc_Time + UCalc_Interval)))
        return; // early out for "slow" update

    // Прибор: только ВНЕШНИЙ уровень (см. шапку) и только при cpu_budget >= 2.
    const bool bnMeasure = (CpuBudget::enabled >= 2) && (s_bnDepth == 0);
    ++s_bnDepth;
    u64 bnMark = bnMeasure ? BnCycles() : 0;
    if (bnMeasure)
    {
        s_bnCalls.fetch_add(1, std::memory_order_relaxed);
        if (!Device.OnMainThread()) s_bnOffMain.fetch_add(1, std::memory_order_relaxed);
    }

    OnCalculateBones();

    if (Update_Visibility)
        Visibility_Update();

    if (bnMeasure) { const u64 n = BnCycles(); s_bnTracks.fetch_add(n - bnMark, std::memory_order_relaxed); bnMark = n; }

    // here we have either:
    //	1:	timeout elapsed
    //	2:	exact computation required
    UCalc_Time = Device.dwTimeGlobal;

    // exact computation
    // Calculate bones
#ifdef DEBUG
    Device.Statistic->Animation.Begin();
#endif

    Bone_Calculate(bones->at(iRoot), &Fidentity);

#ifdef DEBUG
    check_kinematics(this, dbg_name.c_str());
    Device.Statistic->Animation.End();
#endif

    VERIFY(LL_GetBonesVisible() != 0);

    if (bnMeasure) { const u64 n = BnCycles(); s_bnBones.fetch_add(n - bnMark, std::memory_order_relaxed); bnMark = n; }

    // Calculate BOXes/Spheres if needed
    UCalc_Visibox++;
    if (UCalc_Visibox >= psSkeletonUpdate)
    {
        ZoneScopedN("Skeleton update");
        if (bnMeasure) s_bnBoxCalls.fetch_add(1, std::memory_order_relaxed);

        // mark
        // ⚠⚠ЗДЕСЬ БЫЛ ГЛОБАЛЬНЫЙ ГЕНЕРАТОР: `::Random.randI(psSkeletonUpdate - 1)`.
        // `CalculateBones` зовётся С НЕСКОЛЬКИХ ПОТОКОВ (на то и per-object
        // `UCalc_Mutex`, и на то же намекает гейт `TTAPI->on_pool_thread()` в
        // хвосте), а `CRandom` -- обычный LCG с одним общим seed: два потока
        // рвут его состояние. Это была единственная НЕЛОКАЛЬНАЯ запись во всей
        // функции — и первое, что поехало бы при выносе на воркеры.
        // ✅Смысл сохранён точь-в-точь: фаза нужна, чтобы обновления бокса не
        // сошлись в один кадр. Достаточно, чтобы она была РАЗНОЙ У РАЗНЫХ
        // объектов, а не случайной во времени — берём её из адреса объекта.
        // Заодно фаза становится стабильной, то есть воспроизводимой между
        // прогонами, чего со случайной не было.
        UCalc_Visibox = -(s32)((uintptr_t(this) >> 5) % u32(_max(psSkeletonUpdate - 1, 1)));

        u32 count_bones = 0;

        // the update itself
        Fbox Box;
        Box.invalidate();
        for (u32 b = 0; b < bones->size(); b++)
        {
            if (!LL_GetBoneVisible(u16(b)))
                continue;

            Fobb& obb = (*bones)[b]->obb;
            Fmatrix& Mbone = bone_instances[b].mTransform;
            Fmatrix Mbox;
            obb.xform_get(Mbox);
            Fmatrix X;
            X.mul_43(Mbone, Mbox);
            Fvector& S = obb.m_halfsize;

            Fvector P, A;
            A.set(-S.x, -S.y, -S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(-S.x, -S.y, S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(S.x, -S.y, S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(S.x, -S.y, -S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(-S.x, S.y, -S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(-S.x, S.y, S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(S.x, S.y, S.z);
            X.transform_tiny(P, A);
            Box.modify(P);
            A.set(S.x, S.y, -S.z);
            X.transform_tiny(P, A);
            Box.modify(P);

            count_bones++;
        }

        if (!bones->empty() && count_bones)
        {
            // previous frame we have updated box - update sphere
            vis.box.min = (Box.min);
            vis.box.max = (Box.max);
            vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        }

#ifdef DEBUG
        // Validate
        VERIFY(_valid(vis.box.min) && _valid(vis.box.max), "Invalid bones-xform in model", dbg_name.c_str());
        if (vis.sphere.R > 1000.f)
        {
            for (u16 ii = 0; ii < LL_BoneCount(); ++ii)
            {
                Fmatrix tr;
                tr = LL_GetTransform(ii);
                Log("bone ", LL_BoneName(ii));
                Log("bone_matrix", tr);
            }
            Log("end-------");
        }
        VERIFY(vis.sphere.R < 1000.f, "Invalid bones-xform in model", dbg_name.c_str());
#endif
    }

    if (bnMeasure) s_bnBox.fetch_add(BnCycles() - bnMark, std::memory_order_relaxed);
    --s_bnDepth;

    if (Device.OnMainThread() || TTAPI->on_pool_thread())
    {
        if (Update_Callback)
            Update_Callback(this);
    }

    if (bnMeasure) BnMaybeLog();
}

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s)
{
    CKinematics* K = _k;
    Fmatrix& MrootBone = K->LL_GetBoneInstance(K->LL_GetBoneRoot()).mTransform;
    if (MrootBone.c.y > 10000)
    {
        Msg("all bones transform:--------[%s]", s);

        for (u16 ii = 0; ii < K->LL_BoneCount(); ++ii)
        {
            Fmatrix tr;

            tr = K->LL_GetTransform(ii);
            Log("bone ", K->LL_BoneName(ii));
            Log("bone_matrix", tr);
        }
        Log("end-------");
        VERIFY(0, "check_kinematics failed for ", s);
    }
}
#endif

void CKinematics::BuildBoneMatrix(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /* = (1<<0)*/)
{
    bi.mTransform.mul_43(*parent, bd->bind_transform);
}

void CKinematics::CLBone(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
    ZoneScoped;

    const u16 SelfID = bd->GetSelfID();

    if (LL_GetBoneVisible(SelfID))
    {
        // ⭐СЧЁТЧИК ИГРОВОГО КОДА НА ЧУЖОМ ПОТОКЕ (`r_bones_mt`).
        // Костный колбэк -- это xrGame: физическая оболочка, прицел, руль.
        // Пока CalculateBones жила только на главном потоке, вопрос не стоял;
        // с выносом на пул он стал главным, и отвечать на него надо ЧИСЛОМ, а
        // не чтением ([[parallel-record-blocked-by-shared-mutable-state]]).
        if (bi.callback() && CpuBudget::enabled >= 2)
        {
            const bool offMain = !Device.OnMainThread();
            if (offMain) s_bnCbOffMain.fetch_add(1, std::memory_order_relaxed);
            BoneCbCount((void*)bi.callback(), bi.callback_type(), offMain);
        }

        if (bi.callback_overwrite())
        {
            if (bi.callback())
                bi.callback()(&bi);
        }
        else
        {
            BuildBoneMatrix(bd, bi, parent, channel_mask);
            if (bi.callback())
            {
                bi.callback()(&bi);
            }
        }

        bi.mRenderTransform.mul_43(bi.mTransform, bd->m2b_transform);
    }
}

void CKinematics::Bone_GetAnimPos(Fmatrix& pos, u16 id, u8 mask_channel, bool ignore_callbacks)
{
    ZoneScoped;

    ASSERT_FMT(id < LL_BoneCount(), "visual_name: %s, bone_id: %d", dbg_name.c_str(), id);
    CBoneInstance bi = LL_GetBoneInstance(id);
    const Fvector last_c = bi.mTransform.c;
    BoneChain_Calculate(&LL_GetData(id), bi, mask_channel, ignore_callbacks);
    pos.set(bi.mTransform);
    pos.c.set(last_c);
}

void CKinematics::Bone_Calculate(CBoneData* bd, Fmatrix* parent)
{
    ZoneScoped;

    const u16 SelfID = bd->GetSelfID();
    CBoneInstance& BONE_INST = LL_GetBoneInstance(SelfID);
    CLBone(bd, BONE_INST, parent, u8(-1));
    // Calculate children
    for (const auto& C : bd->children)
        Bone_Calculate(C, &BONE_INST.mTransform);
}

void CKinematics::BoneChain_Calculate(const CBoneData* bd, CBoneInstance& bi, u8 mask_channel, bool ignore_callbacks)
{
    ZoneScoped;

    const u16 SelfID = bd->GetSelfID();
    // CBlendInstance& BLEND_INST	= LL_GetBlendInstance(SelfID);
    // CBlendInstance::BlendSVec &Blend = BLEND_INST.blend_vector();
    // ignore callbacks
    const BoneCallback bc = bi.callback();
    const BOOL ow = bi.callback_overwrite();
    if (ignore_callbacks)
    {
        bi.set_callback(bi.callback_type(), nullptr, bi.callback_param(), 0);
    }
    if (SelfID == LL_GetBoneRoot())
    {
        CLBone(bd, bi, &Fidentity, mask_channel);
        // restore callback
        bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
        return;
    }
    const u16 ParentID = bd->GetParentID();
    R_ASSERT(ParentID != BI_NONE);
    const CBoneData* ParrentDT = &LL_GetData(ParentID);
    CBoneInstance parrent_bi = LL_GetBoneInstance(ParentID);
    BoneChain_Calculate(ParrentDT, parrent_bi, mask_channel, ignore_callbacks);
    CLBone(bd, bi, &parrent_bi.mTransform, mask_channel);
    // restore callback
    bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
}
