#include "stdafx.h"


#include "bone.h"
#include "GameMtlLib.h"

// ---------------------------------------------------------------------------
// Белый список костных колбэков, пригодных к чужому потоку (см. bone.h).
//
// ⚠POD-массив, а не контейнер, И ЭТО ВАЖНО: регистрация идёт из статических
// инициализаторов xrGame, то есть ДО main и в неопределённом порядке между
// единицами трансляции. POD обнуляется на этапе статической инициализации,
// которая ВСЕГДА предшествует динамической, поэтому массив гарантированно
// готов к первому `MarkMtSafe`. У контейнера такой гарантии нет.
// Записей единицы (перепись нашла 14 живых колбэков всего), чтение -- линейный
// проход по указателям и только при сборе параллельного набора.
// ---------------------------------------------------------------------------
namespace
{
constexpr u32 kMtSafeMax = 32;
BoneCallback s_mtSafe[kMtSafeMax];
u32 s_mtSafeN = 0;
}

void BoneCallbackMarkMtSafe(BoneCallback fn)
{
    if (!fn) return;
    for (u32 i = 0; i < s_mtSafeN; ++i)
        if (s_mtSafe[i] == fn) return;
    if (s_mtSafeN >= kMtSafeMax)
        return;   // молча НЕ расширяем: переполнение = все лишние считаются небезопасными
    s_mtSafe[s_mtSafeN++] = fn;
}

bool BoneCallbackIsMtSafe(BoneCallback fn)
{
    if (!fn) return true;   // нет колбэка -- нечему быть небезопасным
    for (u32 i = 0; i < s_mtSafeN; ++i)
        if (s_mtSafe[i] == fn) return true;
    return false;
}

//////////////////////////////////////////////////////////////////////////
// BoneInstance methods

void ENGINE_API CBoneInstance::set_param(u32 idx, float data)
{
    VERIFY(idx < MAX_BONE_PARAMS);
    param[idx] = data;
}
float ENGINE_API CBoneInstance::get_param(u32 idx)
{
    VERIFY(idx < MAX_BONE_PARAMS);
    return param[idx];
}

void ENGINE_API CBoneData::CalculateM2B(const Fmatrix& parent)
{
    // Build matrix
    m2b_transform.mul_43(parent, bind_transform);

    // Calculate children
    for (auto& C : children)
        C->CalculateM2B(m2b_transform);

    m2b_transform.invert();
}

//u16 CBoneData::GetNumChildren() const { return (u16)children.size(); }
//IBoneData& CBoneData::GetChild(u16 id) { return *children[id]; }
//const IBoneData& CBoneData::GetChild(u16 id) const { return *children[id]; }
