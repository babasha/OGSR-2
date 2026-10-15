#pragma once

// Video For Windows -- deprecated even on Windows since Vista, genuinely
// Windows-only, no portable equivalent. Only ever forward-declared
// (Layers/xrRender/SH_Texture.h's `CAviPlayerCustom* pAVI`) or included
// here (grepped, tntQAVI.cpp is the sole includer), so gating the whole
// header costs nothing -- no other TU needs CAviPlayerCustom complete.
#ifdef _MSC_VER

#include "vfw.h"

class ENGINE_API CAviPlayerCustom
{
protected:
    CAviPlayerCustom* alpha{};

    AVIINDEXENTRY* m_pMovieIndex{};
    BYTE* m_pMovieData{};
    HIC m_aviIC{};
    BYTE* m_pDecompressedBuf{};

    BITMAPINFOHEADER m_biOutFormat{};
    BITMAPINFOHEADER m_biInFormat{};

    float m_fRate{}; // стандартная скорость, fps
    float m_fCurrentRate{}; // текущая скорость, fps

    DWORD m_dwFrameTotal{};
    DWORD m_dwFrameCurrent{};
    u32 m_dwFirstFrameOffset{};

    DWORD CalcFrame();

    BOOL DecompressFrame(DWORD dwFrameNum);
    VOID PreRoll(DWORD dwFrameNum);

public:
    CAviPlayerCustom();
    ~CAviPlayerCustom();

    DWORD m_dwWidth{}, m_dwHeight{};

    VOID GetSize(DWORD* dwWidth, DWORD* dwHeight);

    BOOL Load(char* fname);
    BOOL GetFrame(BYTE** pDest);

    BOOL NeedUpdate() { return CalcFrame() != m_dwFrameCurrent; }
    INT SetSpeed(INT nPercent);
};

#endif // _MSC_VER
