#!/bin/bash
# Builds nxa_conv (WSL) against the engine's own vendored libogg/libvorbis (no -dev package needed) and the engine's
# DSP-ADPCM codec (xrSound/SoundRender_Adpcm.cpp, copied next to a stand-in stdafx.h).
#   bash _tools/switch_pack/build_nxa_conv.sh [out_dir=/root/nxa]
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
X=$R/3rd_party/Src/Xiph
OUTD=${1:-/root/nxa}
O=$OUTD/obj; mkdir -p $O
if [ ! -f $O/libxiph.a ]; then
  (cd $O
   for f in $X/libogg/src/bitwise.c $X/libogg/src/framing.c; do gcc -O2 -c -I$X/libogg/include $f; done
   for f in analysis bitrate block codebook envelope floor0 floor1 info lookup lpc lsp mapping0 mdct psy registry res0 sharedbook smallft synthesis vorbisfile window; do
     gcc -O2 -D__cdecl= -c -I$X/libogg/include -I$X/libvorbis/include -I$X/libvorbis/lib $X/libvorbis/lib/$f.c
   done
   ar rcs libxiph.a *.o)
fi
cp $R/ogsr_engine/xrSound/SoundRender_Adpcm.cpp $R/ogsr_engine/xrSound/SoundRender_Adpcm.h $O/
cp $R/_tools/switch_pack/shim/nxa_shim.h $O/stdafx.h
g++ -O2 -std=c++17 -I$O -I$X/libogg/include -I$X/libvorbis/include $R/_tools/switch_pack/nxa_conv.cpp $O/SoundRender_Adpcm.cpp $O/libxiph.a -lm -o $OUTD/nxa_conv
echo "built $OUTD/nxa_conv"
