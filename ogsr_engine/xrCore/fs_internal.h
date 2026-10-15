#pragma once

#include "lzhuf.h"
#ifdef _MSC_VER
#include <io.h>
#include <share.h>
#else
#include <sys/file.h> // flock -- POSIX замена SH_DENYWR (см. CFileWriter ниже)
#endif
#include <fcntl.h>
#include <sys/stat.h>

class CFileWriter : public IWriter
{
private:
    FILE* hf;

public:
    CFileWriter(const char* name, bool exclusive)
    {
        R_ASSERT(name && name[0]);
        fName = name;
        VerifyPath(*fName);
        if (exclusive)
        {
#ifdef _MSC_VER
            int handle = _sopen(*fName, _O_WRONLY | _O_TRUNC | _O_CREAT | _O_BINARY, SH_DENYWR);
            hf = _fdopen(handle, "wb");
#else
            // SH_DENYWR (deny other processes write access while open) has no
            // 1:1 POSIX flag -- flock(LOCK_EX) on the open fd is the standard
            // replacement (advisory, but every reader/writer in this engine
            // goes through this same class, so it's honored in practice).
            int handle = open(XR_OSPATH(*fName), O_WRONLY | O_TRUNC | O_CREAT, 0644);
            if (handle >= 0)
                flock(handle, LOCK_EX);
            hf = handle >= 0 ? fdopen(handle, "wb") : nullptr;
#endif
        }
        else
        {
            hf = fopen(XR_OSPATH(*fName), "wb");
#ifdef __SWITCH__
            // A save called "День 1. Убить Стрелка.sav" (autosave of the first quest) failed here with ENOENT on the console's
            // SD card while "all.sav" worked (30-09). Retry once with the Cyrillic letters of the NAME transliterated to Latin;
            // fName is replaced so the registration after w_close and later lookups use the name that really exists.
            if (hf == 0)
            {
                const int err = errno;
                const char* src = *fName;
                bool nonAscii = false;
                for (const char* c = src; *c; ++c)
                    if ((unsigned char)*c >= 0x80) { nonAscii = true; break; }
                if (nonAscii)
                {
                    static const char* const up[] = {"A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "J", "K", "L", "M", "N", "O", "P",
                                                     "R", "S", "T", "U", "F", "H", "C", "Ch", "Sh", "Sch", "", "Y", "", "E", "Yu", "Ya"};
                    static const char* const lo[] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "j", "k", "l", "m", "n", "o", "p",
                                                     "r", "s", "t", "u", "f", "h", "c", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya"};
                    string_path ascii;
                    size_t o = 0;
                    for (const unsigned char* c = (const unsigned char*)src; *c && o + 6 < sizeof(ascii);)
                    {
                        if (*c < 0x80) { ascii[o++] = char(*c++); continue; }
                        const char* rep = "_";
                        size_t adv = 1;
                        if (c[0] == 0xD0 && c[1] >= 0x90 && c[1] <= 0xAF) { rep = up[c[1] - 0x90]; adv = 2; }
                        else if (c[0] == 0xD0 && c[1] >= 0xB0 && c[1] <= 0xBF) { rep = lo[c[1] - 0xB0]; adv = 2; }
                        else if (c[0] == 0xD1 && c[1] >= 0x80 && c[1] <= 0x8F) { rep = lo[c[1] - 0x80 + 16]; adv = 2; }
                        else if (c[0] == 0xD0 && c[1] == 0x81) { rep = "Yo"; adv = 2; }
                        else if (c[0] == 0xD1 && c[1] == 0x91) { rep = "yo"; adv = 2; }
                        else if (c[0] >= 0xC0) { adv = (c[0] >= 0xF0) ? 4 : (c[0] >= 0xE0) ? 3 : 2; }
                        for (const char* r = rep; *r; ++r)
                            ascii[o++] = *r;
                        for (size_t k = 0; k < adv && *c; ++k)
                            ++c;
                    }
                    ascii[o] = 0;
                    Msg("![fs] fopen('%s') failed errno=%d (%s); retrying as '%s'", src, err, strerror(err), ascii);
                    fName = ascii;
                    hf = fopen(XR_OSPATH(*fName), "wb");
                }
            }
#endif
            if (hf == 0)
                Msg("!Can't write file: '%s'. Error: '%s'.", *fName, strerror(errno));
        }
    }

    virtual ~CFileWriter()
    {
        if (0 != hf)
        {
            fclose(hf);
#ifdef _MSC_VER
            // release RO attrib
            DWORD dwAttr = GetFileAttributes(*fName);
            if ((dwAttr != u32(-1)) && (dwAttr & FILE_ATTRIBUTE_READONLY))
            {
                dwAttr &= ~FILE_ATTRIBUTE_READONLY;
                SetFileAttributes(*fName, dwAttr);
            }
#endif
            // POSIX has no separate "read-only attribute" to clear after the
            // fact -- the open() above would already have failed if the file's
            // permission bits didn't allow writing.
        }
    }
    // kernel
    virtual void w(const void* _ptr, size_t count)
    {
        if ((0 != hf) && (0 != count))
        {
            const size_t W = fwrite(_ptr, sizeof(char), count, hf);
            R_ASSERT3(W == count, "Can't write mem block to file. Disk maybe full.", strerror(errno));
        }
    };
    virtual void seek(size_t pos)
    {
        if (0 != hf)
            fseek(hf, pos, SEEK_SET);
    };
    virtual size_t tell() { return (0 != hf) ? ftell(hf) : 0; };
    virtual bool valid() { return (0 != hf); }
    virtual size_t flush() { return fflush(hf); }
};

// It automatically frees memory after destruction
class CTempReader : public IReader
{
public:
    CTempReader(void* _data, size_t _size, size_t _iterpos) : IReader(_data, _size, _iterpos) {}
    virtual ~CTempReader();
};

class CPackReader : public IReader
{
    void* base_address;

public:
    CPackReader(void* _base, void* _data, size_t _size) : IReader(_data, _size) { base_address = _base; }
    virtual ~CPackReader();
};

class CVirtualFileReader : public IReader
{
private:
    void *hSrcFile, *hSrcMap;

public:
    CVirtualFileReader(const char* cFileName);
    virtual ~CVirtualFileReader();
};
