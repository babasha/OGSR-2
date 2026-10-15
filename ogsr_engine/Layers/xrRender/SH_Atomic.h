#pragma once

#include "../../xrCore/xr_resource.h"
#include "tss_def.h"
#include "xrD3DDefs.h" // opaque handle typedefs for the shader objects below

#pragma pack(push, 4)

//////////////////////////////////////////////////////////////////////////
// Atomic resources
//
// `sh` is an opaque renderer-side shader handle. The geometry / hull / domain /
// compute stage objects that used to sit here existed only for the D3D10/D3D11
// paths and were never referenced by anything else; they went out with them.
//////////////////////////////////////////////////////////////////////////
struct ECORE_API SVS : public xr_resource_named
{
    ID3DVertexShader* sh;
    R_constant_table constants;

    SVS();
    ~SVS();
};
typedef resptr_core<SVS, resptr_base<SVS>> ref_vs;

//////////////////////////////////////////////////////////////////////////
struct ECORE_API SPS : public xr_resource_named
{
    ID3DPixelShader* sh;
    R_constant_table constants;
    ~SPS();
};
typedef resptr_core<SPS, resptr_base<SPS>> ref_ps;

//////////////////////////////////////////////////////////////////////////
struct ECORE_API SState : public xr_resource_flagged
{
    ID3DState* state;
    SimulatorStates state_code;
    ~SState();
};
typedef resptr_core<SState, resptr_base<SState>> ref_state;

//////////////////////////////////////////////////////////////////////////
struct ECORE_API SDeclaration : public xr_resource_flagged
{
    //	Vertex declaration as stored in the level / model formats.
    xr_vector<D3DVERTEXELEMENT9> dcl_code;
    ~SDeclaration();
};
typedef resptr_core<SDeclaration, resptr_base<SDeclaration>> ref_declaration;

#pragma pack(pop)
