#pragma once

extern ECORE_API u32 ps_r_pp_aa_mode;
extern ECORE_API u32 ps_r_dlss_preset;

extern ECORE_API u32 ps_r_ao_quality; //	=	0;

extern ECORE_API float ps_r__WallmarkTTL;

extern ECORE_API float ps_r__GLOD_ssa_start;
extern ECORE_API float ps_r__GLOD_ssa_end;
extern ECORE_API float ps_r__LOD;

extern ECORE_API float ps_r__ssaDISCARD;
extern ECORE_API int ps_r__tf_Anisotropic;

extern float ps_r2_no_details_radius;
extern float ps_r2_no_rain_radius;

extern ECORE_API float ps_r2_img_exposure; // r2-only
extern ECORE_API float ps_r2_img_gamma; // r2-only
extern ECORE_API float ps_r2_img_saturation; // r2-only

extern ECORE_API Fvector ps_r2_img_cg; // r2-only

// Screen Space Shaders Stuff
extern Fvector4 ps_ssfx_int_grass_params_1;
extern ECORE_API Fvector4 ps_ssfx_rain_1;

// textures
extern ECORE_API int psTextureLOD;

extern void xrRender_initconsole();

// Postprocess anti-aliasing types
enum
{
    NO_AA,
    DLSS,
    FSR2,
    TAA,
    SMAA,
};
