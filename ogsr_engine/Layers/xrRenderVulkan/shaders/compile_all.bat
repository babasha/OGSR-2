@echo off
REM ===========================================================================
REM Compile ALL Vulkan stage shaders (vert/frag/comp .glsl) in this directory
REM to SPIR-V (.spv) next to the source.
REM
REM Shared includes (light_ubo.glsl, shadow_common.glsl, wet_common.glsl,
REM froxel.glsl, ...) are pulled in automatically by glslc via #include
REM (GL_GOOGLE_include_directive). They have no stage suffix so the patterns
REM below skip them - they are not compiled standalone.
REM
REM Usage:  compile_all.bat            compile, then deploy if the game dir exists
REM         compile_all.bat nodeploy   compile only
REM ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Locate glslc: prefer the VULKAN_SDK env var, else the known install here.
if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "GLSLC=%VULKAN_SDK%\Bin\glslc.exe"
if not exist "%GLSLC%" (
    echo ERROR: glslc.exe not found at "%GLSLC%"
    echo Set the VULKAN_SDK environment variable to your Vulkan SDK install.
    exit /b 1
)
echo Using %GLSLC%
echo.

set ERR=0
for %%f in (*.vert.glsl) do call :one "%%f" vertex
for %%f in (*.frag.glsl) do call :one "%%f" fragment
for %%f in (*.comp.glsl) do call :one "%%f" compute
for %%f in (*.tesc.glsl) do call :one "%%f" tesscontrol
for %%f in (*.tese.glsl) do call :one "%%f" tesseval

REM EARLY_ZTEST twins of the world FS (forced early depth test; bound only to
REM no-z-write statics pipelines by EarlyTwin in vk_pipeline_cache.cpp). Same
REM sources, -DEARLY_ZTEST, distinct .spv names.
call :onedef "world_lmap.frag.glsl"      fragment EARLY_ZTEST "world_lmap_earlyz.frag.spv"
call :onedef "world_vlit.frag.glsl"      fragment EARLY_ZTEST "world_vlit_earlyz.frag.spv"
call :onedef "world_lmap_fade.frag.glsl" fragment EARLY_ZTEST "world_lmap_fade_earlyz.frag.spv"
call :onedef "world_vlit_fade.frag.glsl" fragment EARLY_ZTEST "world_vlit_fade_earlyz.frag.spv"

REM Pool mask with the SECOND target the shore wetness runs on (live water
REM surface + ground height per tile texel). Same source, one more attachment;
REM the plain twin above still drives the 2048^2 level map, which would pay
REM 16 MB for a second target that means nothing at 2 m per texel.
call :onedef "water_mask.frag.glsl"      fragment WATER_MASK_SURF "water_mask_surf.frag.spv"

REM LEAN water surface (r_wtr_lean, the Switch preset): no spectral field, no ripple
REM sim, no shelter/refraction/shore break, six swell octaves, R1 sun lookup.
call :onedef "water.frag.glsl"           fragment WATER_LEAN "water_lean.frag.spv"

REM FLAT twin of the tree colour FS (r_tree_flat probe): identical alpha test,
REM constant colour instead of the ubershader. Decides whether Trees/Mesh cost
REM lives in the shading work or in the raster+alpha floor.
call :onedef "tree.frag.glsl"            fragment TREE_FLAT "tree_flat.frag.spv"
REM NOTE: there is no tree_terms.frag.spv any more. The per-term mask (bit per
REM shading term, folded at pipeline creation) moved INTO tree.frag.spv as a
REM specialization constant with an all-on default - see tree_variants.glsl. The
REM crown pipelines are now built per world state from that one module, and
REM r_tree_terms just forces a mask through the same path.
REM r_tree_gather 0 twin: the crown FS with the screen-space gather tap in
REM ddgiAmbientFromField compiled OUT (probe field only). Separate .spv because the
REM tap costs by being PRESENT, not by executing - a uniform gate returns nothing.
call :onedef "tree.frag.glsl"            fragment TREE_NO_GATHER "tree_nogather.frag.spv"
REM r_tree_gi_inst 1 twin of the crown VERTEX shader: reads the per-tree probe
REM field tree_gi.comp wrote instead of walking the probe atlases per vertex.
call :onedef "tree.vert.glsl"            vertex   TREE_GI_INST "tree_giinst.vert.spv"
REM r_grass_gi_inst 1 twin of the grass FS: reads the per-tuft probe field
REM detail_generate.comp packed into the instance record (rgb9e5 in color.g)
REM instead of walking ~16 atlas taps per fragment. Same cure as the crown.
call :onedef "detail.frag.glsl"          fragment GRASS_GI_INST "detail_giinst.frag.spv"

REM Reduced-cost twins the minimal (Switch) preset selects by cvar; the PC defaults never bind them.
REM r_grass_r1 1/2: R1 grass (sun x baked occlusion + hemi + ambient + fog) per pixel / per vertex.
REM (detail_r1v.frag.glsl is a stage file of its own and comes from the wildcard loop.)
call :onedef "detail.frag.glsl"          fragment DETAIL_R1 "detail_r1.frag.spv"
call :onedef "detail.vert.glsl"          vertex   DETAIL_R1V "detail_r1v.vert.spv"
REM r_grass_gen 1/2/3: lean generator, lean2 (no local memory / HZB code), the tuft cache bake + cull.
call :onedef  "detail_generate.comp.glsl" compute DETAIL_GEN_LEAN "detail_generate_lean.comp.spv"
call :onedef2 "detail_generate.comp.glsl" compute DETAIL_GEN_LEAN DETAIL_GEN_LEAN2 "detail_generate_lean2.comp.spv"
call :onedef  "detail_generate.comp.glsl" compute DETAIL_CACHE_BAKE "detail_cache_bake.comp.spv"
call :onedef  "detail_generate.comp.glsl" compute DETAIL_CACHE_CULL "detail_cache_cull.comp.spv"
REM r_tree_sky_push 1: the crown FS with the up-facing sky term from the push constant (one CPU value per frame).
call :onedef "tree.frag.glsl"            fragment TREE_SKY_PUSH "tree_skypush.frag.spv"
REM r_tree_r1 1: the R1 crown VS (light + live sun shadow + fog per vertex); its FS tree_r1.frag.glsl is wildcard.
call :onedef "tree.vert.glsl"            vertex   TREE_R1 "tree_r1.vert.spv"

REM ZOFF twin of the terrain FS: exports a displaced gl_FragDepth (layout
REM depth_greater) so SSAO and shadows land inside the POM cracks. This one used
REM to be compiled BY HAND and was therefore the only .spv this script did not
REM own -- the committed binary had already drifted away from its source (see
REM the terrain-splatting notes). Building it here keeps the pair honest.
call :onedef "world_terrain.frag.glsl"   fragment ZOFF "world_terrain_zoff.frag.spv"

REM RG16F fallback of the DDGI distance blend, for a device without linear
REM filtering of RG32F (vk_ddgi.cpp picks the atlas format and this .spv to match).
call :onedef "ddgi_blend_dep.comp.glsl"  compute  DDGI_DEP_FP16 "ddgi_blend_dep_fp16.comp.spv"

REM ALPHA-TESTED twin of the statics NORMAL G-buffer (GTAO real normals for
REM grates/bushes baked into the level). Same sources, -DNORMAL_AT: the VS gains
REM the base UV, the FS the diffuse punch-out. The solid twin above is built by
REM the wildcard loop.
call :onedef "world_normal.vert.glsl"    vertex   NORMAL_AT "world_normal_at.vert.spv"
call :onedef "world_normal.frag.glsl"    fragment NORMAL_AT "world_normal_at.frag.spv"

REM r_world_flat 1 twins (R1-class statics/terrain + the live sun shadow, nfs43-47;
REM the Switch preset turns it on). The console shipped them as world_*_r8[_earlyz]
REM and world_terrain_r4/r5/r6 -- vk_pipeline_cache.cpp loads these names now.
call :onedef  "world_lmap.frag.glsl"      fragment WORLD_FLAT "world_lmap_flat.frag.spv"
call :onedef  "world_vlit.frag.glsl"      fragment WORLD_FLAT "world_vlit_flat.frag.spv"
call :onedef  "world_lmap_fade.frag.glsl" fragment WORLD_FLAT "world_lmap_fade_flat.frag.spv"
call :onedef  "world_vlit_fade.frag.glsl" fragment WORLD_FLAT "world_vlit_fade_flat.frag.spv"
call :onedef2 "world_lmap.frag.glsl"      fragment WORLD_FLAT EARLY_ZTEST "world_lmap_flat_earlyz.frag.spv"
call :onedef2 "world_vlit.frag.glsl"      fragment WORLD_FLAT EARLY_ZTEST "world_vlit_flat_earlyz.frag.spv"
call :onedef2 "world_lmap_fade.frag.glsl" fragment WORLD_FLAT EARLY_ZTEST "world_lmap_fade_flat_earlyz.frag.spv"
call :onedef2 "world_vlit_fade.frag.glsl" fragment WORLD_FLAT EARLY_ZTEST "world_vlit_fade_flat_earlyz.frag.spv"
call :onedef  "world_terrain.frag.glsl"   fragment TERRAIN_FLAT "world_terrain_flat.frag.spv"
REM + forced early depth test, no z-write (the depth prepass lays the terrain): nx_statab 31 on the Switch.
call :onedef2 "world_terrain.frag.glsl"   fragment TERRAIN_FLAT EARLY_ZTEST "world_terrain_flat_earlyz.frag.spv"
REM nx_statab v144: which property of the R1 terrain FS costs per fragment -- the 1-line FS + its 5 inputs (35),
REM + a discard (36), and the R1 terrain without its discard (37).
call :onedef  "world_terrain_min.frag.glsl" fragment MIN_INPUTS "world_terrain_min_in.frag.spv"
call :onedef  "world_terrain_min.frag.glsl" fragment MIN_KILL   "world_terrain_min_kill.frag.spv"
call :onedef2 "world_terrain.frag.glsl"   fragment TERRAIN_FLAT NO_DISCARD "world_terrain_flat_nokill.frag.spv"
REM v150: far terrain -- the R1 terrain with each detail layer at its mean colour (coarsest mip) and no lamps;
REM nx_statab 47 puts it on ALL terrain to price it.
call :onedef2 "world_terrain.frag.glsl"   fragment TERRAIN_FLAT TERRAIN_FAR "world_terrain_far.frag.spv"

REM r_skin_ubo twins: the bone palette as a dynamic-UBO window (skin_bones.glsl) --
REM the constant bank on pre-Turing NVK instead of memory loads.
call :onedef "skinned.vert.glsl"               vertex SKIN_BONES_UBO "skinned_ubo.vert.spv"
call :onedef "shadow_skinned.vert.glsl"        vertex SKIN_BONES_UBO "shadow_skinned_ubo.vert.spv"
call :onedef "shadow_skinned_at.vert.glsl"     vertex SKIN_BONES_UBO "shadow_skinned_at_ubo.vert.spv"
call :onedef "shadow_skinned_normal.vert.glsl" vertex SKIN_BONES_UBO "shadow_skinned_normal_ubo.vert.spv"

REM r_world_bindless twins (10-09, world_bindless.glsl): the GPU world statics draw material-agnostic groups and
REM index the textures per draw. Each twin = the module above it + "_bl" (vk_pipeline_cache BindlessTwin).
call :onedef  "world_lmap.vert.glsl"      vertex WORLD_BINDLESS "world_lmap_bl.vert.spv"
call :onedef  "world_vlit.vert.glsl"      vertex WORLD_BINDLESS "world_vlit_bl.vert.spv"
call :onedef  "world_lmap_fade.vert.glsl" vertex WORLD_BINDLESS "world_lmap_fade_bl.vert.spv"
call :onedef  "world_vlit_fade.vert.glsl" vertex WORLD_BINDLESS "world_vlit_fade_bl.vert.spv"
call :onedef  "world_lmap.frag.glsl"      fragment WORLD_BINDLESS "world_lmap_bl.frag.spv"
call :onedef  "world_vlit.frag.glsl"      fragment WORLD_BINDLESS "world_vlit_bl.frag.spv"
call :onedef  "world_lmap_fade.frag.glsl" fragment WORLD_BINDLESS "world_lmap_fade_bl.frag.spv"
call :onedef  "world_vlit_fade.frag.glsl" fragment WORLD_BINDLESS "world_vlit_fade_bl.frag.spv"
call :onedef2 "world_lmap.frag.glsl"      fragment WORLD_BINDLESS EARLY_ZTEST "world_lmap_earlyz_bl.frag.spv"
call :onedef2 "world_vlit.frag.glsl"      fragment WORLD_BINDLESS EARLY_ZTEST "world_vlit_earlyz_bl.frag.spv"
call :onedef2 "world_lmap_fade.frag.glsl" fragment WORLD_BINDLESS EARLY_ZTEST "world_lmap_fade_earlyz_bl.frag.spv"
call :onedef2 "world_vlit_fade.frag.glsl" fragment WORLD_BINDLESS EARLY_ZTEST "world_vlit_fade_earlyz_bl.frag.spv"
call :onedef2 "world_lmap.frag.glsl"      fragment WORLD_BINDLESS WORLD_FLAT "world_lmap_flat_bl.frag.spv"
call :onedef2 "world_vlit.frag.glsl"      fragment WORLD_BINDLESS WORLD_FLAT "world_vlit_flat_bl.frag.spv"
call :onedef2 "world_lmap_fade.frag.glsl" fragment WORLD_BINDLESS WORLD_FLAT "world_lmap_fade_flat_bl.frag.spv"
call :onedef2 "world_vlit_fade.frag.glsl" fragment WORLD_BINDLESS WORLD_FLAT "world_vlit_fade_flat_bl.frag.spv"
call :onedef3 "world_lmap.frag.glsl"      fragment WORLD_BINDLESS WORLD_FLAT EARLY_ZTEST "world_lmap_flat_earlyz_bl.frag.spv"
call :onedef3 "world_vlit.frag.glsl"      fragment WORLD_BINDLESS WORLD_FLAT EARLY_ZTEST "world_vlit_flat_earlyz_bl.frag.spv"
call :onedef3 "world_lmap_fade.frag.glsl" fragment WORLD_BINDLESS WORLD_FLAT EARLY_ZTEST "world_lmap_fade_flat_earlyz_bl.frag.spv"
call :onedef3 "world_vlit_fade.frag.glsl" fragment WORLD_BINDLESS WORLD_FLAT EARLY_ZTEST "world_vlit_fade_flat_earlyz_bl.frag.spv"

if %ERR% NEQ 0 (
    echo.
    echo BUILD FAILED: %ERR% shader had errors.
    exit /b 1
)
echo.
echo All shaders compiled OK.

REM Optional deploy to the test game install (see memory: vulkan-build-deploy).
if /I "%~1"=="nodeploy" goto :done
set "DEPLOY=D:\OGSR Gunslinger Addon\gamedata\shaders"
if exist "%DEPLOY%\" (
    echo Deploying *.spv to "%DEPLOY%" ...
    copy /Y *.spv "%DEPLOY%\" >nul
    echo Deploy complete.
) else (
    echo Deploy dir "%DEPLOY%" not found - skipping deploy.
)

:done
exit /b 0

:one
REM %1 = source .glsl (quoted), %2 = shader stage; output = name-without-.glsl + .spv
REM --target-env=vulkan1.3: the engine requires a VK 1.3 device (HWCaps_Vulkan.h),
REM and vsm_mark.comp subgroup ops need SPIR-V >= 1.3 (default target is vulkan1.0).
"%GLSLC%" --target-env=vulkan1.3 -fshader-stage=%2 "%~1" -o "%~n1.spv"
if errorlevel 1 (
    echo   FAILED: %~1
    set /a ERR+=1
) else (
    echo   ok: %~1
)
goto :eof

:onedef
REM %1 = source .glsl (quoted), %2 = stage, %3 = preprocessor define, %4 = output .spv (quoted)
"%GLSLC%" --target-env=vulkan1.3 -fshader-stage=%2 -D%3 "%~1" -o "%~4"
if errorlevel 1 (
    echo   FAILED: %~1 [%3]
    set /a ERR+=1
) else (
    echo   ok: %~4
)
goto :eof

:onedef2
REM %1 = source .glsl (quoted), %2 = stage, %3 + %4 = two preprocessor defines, %5 = output .spv (quoted)
"%GLSLC%" --target-env=vulkan1.3 -fshader-stage=%2 -D%3 -D%4 "%~1" -o "%~5"
if errorlevel 1 (
    echo   FAILED: %~1 [%3 %4]
    set /a ERR+=1
) else (
    echo   ok: %~5
)
goto :eof

:onedef3
REM %1 = source .glsl (quoted), %2 = stage, %3 %4 %5 = three preprocessor defines, %6 = output .spv (quoted)
"%GLSLC%" --target-env=vulkan1.3 -fshader-stage=%2 -D%3 -D%4 -D%5 "%~1" -o "%~6"
if errorlevel 1 (
    echo   FAILED: %~1 [%3 %4 %5]
    set /a ERR+=1
) else (
    echo   ok: %~6
)
goto :eof
