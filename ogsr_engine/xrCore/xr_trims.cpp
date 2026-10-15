#include "stdafx.h"

LPSTR _TrimLeft(LPSTR str)
{
    LPSTR p = str;
    while (*p && (u8(*p) <= u8(' ')))
        p++;
    if (p != str)
    {
        LPSTR t = str;
        for (; *p; t++, p++)
            *t = *p;
        *t = 0;
    }
    return str;
}

LPSTR _TrimRight(LPSTR str)
{
    LPSTR p = str + xr_strlen(str);
    while ((p != str) && (u8(*p) <= u8(' ')))
        p--;
    *(++p) = 0;
    return str;
}

LPSTR _Trim(LPSTR str)
{
    _TrimLeft(str);
    _TrimRight(str);
    return str;
}

LPCSTR _SetPos(LPCSTR src, u32 pos, char separator)
{
    LPCSTR res = src;
    u32 p = 0;
    while ((p < pos) && (0 != (res = strchr(res, separator))))
    {
        res++;
        p++;
    }
    return res;
}

int _GetItemCount(LPCSTR src, char separator)
{
    u32 cnt = 0;
    if (src && src[0])
    {
        LPCSTR res = src;
        LPCSTR last_res = res;
        while (0 != (res = strchr(res, separator)))
        {
            res++;
            last_res = res;
            cnt++;
            //if (res[0] == separator)
            //    break;
        }
        if (xr_strlen(last_res))
            cnt++;
    }
    return cnt;
}

LPCSTR _CopyVal(LPCSTR src, LPSTR dst, char separator)
{
    const char* p = strchr(src, separator);
    size_t n = p ? (p - src) : strlen(src);
    strncpy(dst, src, n);
    dst[n] = 0;
    return dst;
}

LPSTR _GetItem(LPCSTR src, int index, LPSTR dst, char separator, LPCSTR def, bool trim)
{
    LPCSTR ptr;
    ptr = _SetPos(src, index, separator);
    if (ptr)
        _CopyVal(ptr, dst, separator);
    else
        strcpy(dst, def);
    if (trim)
        _Trim(dst);
    return dst;
}

LPSTR _GetItems(LPCSTR src, int idx_start, int idx_end, LPSTR dst, char separator)
{
    LPSTR n = dst;
    int level = 0;
    for (LPCSTR p = src; *p != 0; p++)
    {
        if ((level >= idx_start) && (level < idx_end))
            *n++ = *p;
        if (*p == separator)
            level++;
        if (level >= idx_end)
            break;
    }
    *n = '\0';
    return dst;
}

void _SequenceToList(SStringVec& lst, LPCSTR in, char separator)
{
    lst.clear();
    int t_cnt = _GetItemCount(in, separator);
    xr_string T;
    for (int i = 0; i < t_cnt; i++)
    {
        _GetItem(in, i, T, separator, 0);
        _Trim(T);
        if (T.size())
            lst.push_back(T.c_str());
    }
}

xr_string& _TrimLeft(xr_string& str)
{
    LPCSTR b = str.c_str();
    LPCSTR p = str.c_str();
    while (*p && (u8(*p) <= u8(' ')))
        p++;
    if (p != b)
        str.erase(0, p - b);
    return str;
}

xr_string& _TrimRight(xr_string& str)
{
    LPCSTR b = str.c_str();
    size_t l = str.length();
    if (l)
    {
        LPCSTR p = str.c_str() + l - 1;
        while ((p != b) && (u8(*p) <= u8(' ')))
            p--;
        if (p != (str + b))
            str.erase(p - b + 1, l - (p - b));
    }
    return str;
}

xr_string& _Trim(xr_string& str)
{
    _TrimLeft(str);
    _TrimRight(str);
    return str;
}

LPCSTR _CopyVal(LPCSTR src, xr_string& dst, char separator)
{
    const char* p = strchr(src, separator);
    size_t n = p ? (p - src) : strlen(src);
    dst = src;
    dst = dst.erase(n, dst.length());
    return dst.c_str();
}

LPCSTR _GetItem(LPCSTR src, int index, xr_string& dst, char separator, LPCSTR def, bool trim)
{
    LPCSTR ptr;
    ptr = _SetPos(src, index, separator);
    if (ptr)
        _CopyVal(ptr, dst, separator);
    else
        dst = def;
    if (trim)
        _Trim(dst);
    return dst.c_str();
}
