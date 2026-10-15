#!/bin/bash
set -u
cd "$(dirname "$0")"
ROOT=/mnt/d/OGSR-2/ogsr_engine
OUT=/tmp/xr3da_build
rm -rf "$OUT"
mkdir -p "$OUT"

INCLUDES=(
  -I "$ROOT/xr_3da"
  -I "$ROOT/xrCore"
  -I "$ROOT/xrCDB"
  -I "$ROOT/xrSound"
  -I "$ROOT/xrParticles"
  -I "$ROOT/Luabind"
  -I "$ROOT/LuaJIT/src"
  -I "$ROOT/xrGame"
  -I "$ROOT/xrOnlineNet"
  -I "$ROOT/Include"
  -I "$ROOT/../3rd_party/Src/tracy/public"
  -I "$ROOT/../3rd_party/Src/imgui"
  -I "$ROOT/../3rd_party/Src/Xiph/libogg/include"
  -I "$ROOT/../3rd_party/Src/Xiph/libtheora/include"
  -I "$ROOT/../3rd_party/GameNetworkingSockets/include"
)
DEFS=(-DXRGAME_STATIC -DXRRENDER_STATIC -DXRCORE_STATIC -DXRCDB_STATIC -DXRSOUND_STATIC -DXR_PARTICLES_STATIC -DNDEBUG -D_WINDOWS -DXRENGINE_STATIC -D_MT -DLUABIND_STATIC)
FLAGS=(-std=c++20 -c -w)

FILES=(
  "../xrOnlineNet/NET_GNS_Transport.cpp"
  "../xrOnlineNet/content_integrity.cpp"
  CameraBase.cpp
  CameraManager.cpp
  CustomHUD.cpp
  Device_Initialize.cpp
  Device_create.cpp
  Device_destroy.cpp
  Device_wndproc.cpp
  DiscordRPC.cpp
  EffectorPP.cpp
  Engine.cpp
  EngineAPI.cpp
  Envelope.cpp
  Environment.cpp
  Environment_misc.cpp
  Environment_render.cpp
  EventAPI.cpp
  FDemoPlay.cpp
  FDemoRecord.cpp
  Feel_Touch.cpp
  Feel_Vision.cpp
  GameFont.cpp
  GameMtlLib.cpp
  GameMtlLib_Engine.cpp
  ICollidable.cpp
  IGame_Level.cpp
  IGame_Level_check_textures.cpp
  IGame_ObjectPool.cpp
  IGame_Persistent.cpp
  IInputReceiver.cpp
  IRenderable.cpp
  ISheduled.cpp
  LightAnimLibrary.cpp
  MbHelpers.cpp
  "NET_Server_Trash/NET_Client.cpp"
  "NET_Server_Trash/NET_Server.cpp"
  ObjectAnimator.cpp
  PS_instance.cpp
  Rain.cpp
  ReShadeCompat.cpp
  Render.cpp
  SkeletonMotions.cpp
  Stats.cpp
  XR_IOConsole.cpp
  XR_IOConsole_callback.cpp
  XR_IOConsole_control.cpp
  XR_IOConsole_get.cpp
  Xr_input.cpp
  bone.cpp
  cf_dynamic_mesh.cpp
  defines.cpp
  device.cpp
  ed_facade.cpp
  edit_actions.cpp
  interp.cpp
  line_edit_control.cpp
  line_editor.cpp
  motion.cpp
  os_clipboard.cpp
  perlin.cpp
  pure.cpp
  pure_relcase.cpp
  splash.cpp
  stdafx.cpp
  thunderbolt.cpp
  tntQAVI.cpp
  x_ray.cpp
  xrHemisphere.cpp
  xrSheduler.cpp
  xrTheora_Stream.cpp
  xrTheora_Surface.cpp
  xr_area.cpp
  xr_area_query.cpp
  xr_area_raypick.cpp
  xr_collide_form.cpp
  xr_efflensflare.cpp
  xr_ioc_cmd.cpp
  xr_object.cpp
  xr_object_list.cpp
)

for CXX in g++ clang++; do
  echo "=== $CXX ==="
  PASS=0
  FAIL=0
  for f in "${FILES[@]}"; do
    base=$(basename "$f" .cpp)
    obj="$OUT/${base}_$CXX.o"
    if "$CXX" "${FLAGS[@]}" "${DEFS[@]}" "${INCLUDES[@]}" "$f" -o "$obj" 2> "$OUT/${base}_$CXX.log"; then
      PASS=$((PASS+1))
    else
      FAIL=$((FAIL+1))
      echo "FAIL: $f"
    fi
  done
  echo "$CXX: $PASS pass, $FAIL fail"
done
