////////////////////////////////////////////////////////////////////////////
// Module : os_clipboard.cpp
// Created : 21.02.2008
// Author : Evgeniy Sokolov
// Description : os clipboard class implementation
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "os_clipboard.h"

#ifdef _MSC_VER

void os_clipboard::copy_to_clipboard(LPCSTR buf)
{
	if (!OpenClipboard(nullptr))
		return;

	size_t handle_size = strlen(buf) + 1;
	HGLOBAL handle = GlobalAlloc(GHND, handle_size);
	if (!handle)
	{
		CloseClipboard();
		return;
	}

	char* memory = (char*)GlobalLock(handle);
	xr_strcpy(memory, handle_size, buf);
	GlobalUnlock(handle);
	EmptyClipboard();
	SetClipboardData(CF_TEXT, handle);
	CloseClipboard();
}

void os_clipboard::paste_from_clipboard(LPSTR buffer, u32 const& buffer_size)
{
	VERIFY(buffer);
	VERIFY(buffer_size > 0);

	if (!OpenClipboard(nullptr))
		return;

	HGLOBAL hmem = GetClipboardData(CF_TEXT);
    if (!hmem)
    {
        CloseClipboard();
        return;
    }

	LPCSTR clipdata = (LPCSTR)GlobalLock(hmem);
	strncpy_s(buffer, buffer_size, clipdata, buffer_size - 1);
	buffer[buffer_size - 1] = 0;
	for (size_t i = 0; i < strlen(buffer); ++i)
	{
		char c = buffer[i];
		if (((isprint(c) == 0) && (c != char(-1))) || c == '\t' || c == '\n')
		{
			buffer[i] = ' ';
		}
	}

	GlobalUnlock(hmem);
	CloseClipboard();
}

#else

// No system clipboard backend on this platform yet (would need SDL_Get/
// SetClipboardText or an X11/Wayland integration -- a real subsystem
// replacement, not a compile shim, same class of deferred decision as
// input/window in platform-seam-step3-input-keys). Inert stubs so callers
// (console/chat copy-paste) compile and no-op instead of failing to link.
void os_clipboard::copy_to_clipboard(LPCSTR) {}
void os_clipboard::paste_from_clipboard(LPSTR buffer, u32 const& buffer_size)
{
    VERIFY(buffer);
    VERIFY(buffer_size > 0);
    buffer[0] = 0;
}

#endif
