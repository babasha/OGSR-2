// ed_facade.cpp — Editor-host facade (architecture a2). See ed_facade.h.
//
// A2.1: a single exported liveness probe. Its only job is to prove the monolith
// builds as a DLL (EdDllBuild=true), that GetProcAddress resolves a facade symbol,
// and that calling it across the module boundary works. The real render facade
// (Ed_Init/Ed_RenderFrame/...) is added in A2.2.
#include "stdafx.h"
#include "ed_facade.h"

extern "C" __declspec(dllexport) int Ed_Ping()
{
    return 0x0A2D0001; // "a2" build tag — matches the loader's expected magic
}
