REM DirectXTex / DirectXMesh / DirectXMath are gone: the only consumers were the
REM D3D11 renderer (screenshot encode, dx10Texture, 3D-fluid math), which no
REM longer exists. Nothing in the Vulkan tree includes them.

RD /s /q 3rd_party\Src\concurrentqueue\concurrentqueue
git clone --branch master --depth 1 https://github.com/cameron314/concurrentqueue.git 3rd_party\Src\concurrentqueue\concurrentqueue

RD /s /q 3rd_party\Src\libsquashfs\squashfs-tools-ng
git clone --branch master --depth 1 https://github.com/AgentD/squashfs-tools-ng.git 3rd_party\Src\libsquashfs\squashfs-tools-ng

RD /s /q 3rd_party\Src\lz4\lz4
git clone --branch dev --depth 1 https://github.com/lz4/lz4.git 3rd_party\Src\lz4\lz4

RD /s /q 3rd_party\Src\zstd\zstd
git clone --branch dev --depth 1 https://github.com/facebook/zstd.git 3rd_party\Src\zstd\zstd

RD /s /q 3rd_party\Src\mimalloc\mimalloc
git clone --branch dev2 --depth 1 https://github.com/microsoft/mimalloc.git 3rd_party\Src\mimalloc\mimalloc
REM Our Release builds mimalloc with MI_GUARDED=1 and no MI_PADDING: without this patch mi_usable_size counts the
REM guard page in and realloc of a guarded block faults (the CMemoryWriter start-up crash, 10-10-2026).
git -C 3rd_party\Src\mimalloc\mimalloc apply ..\patches\0001-guarded-usable-size-without-padding.patch

RD /s /q 3rd_party\Src\NVIDIA_DLSS\DLSS
git clone --branch v310.4.0 --depth 1 https://github.com/NVIDIA/DLSS.git 3rd_party\Src\NVIDIA_DLSS\DLSS

RD /s /q 3rd_party\Src\cpputils\cpputils
git clone --branch main --depth 1 https://github.com/tzcnt/cpputils.git 3rd_party\Src\cpputils\cpputils

REM ===== Vulkan renderer dependencies =====
REM VMA — header-only, used by xrRenderVulkan. Pinned to v3.4.x range (matches monolith).
RD /s /q 3rd_party\Src\VulkanMemoryAllocator\VulkanMemoryAllocator
git clone --branch master --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git 3rd_party\Src\VulkanMemoryAllocator\VulkanMemoryAllocator

REM NGX (DLSS for Vulkan) — currently *deferred*, kept commented so structure is ready.
REM Existing 3rd_party\Src\NVIDIA_DLSS\DLSS clone (above) is the same SDK used for Vulkan;
REM Vulkan renderer just needs a different lib variant from the same tree.
REM Reflex SDK — also deferred until requested by OGSR team.
REM RD /s /q 3rd_party\Src\NVIDIA_Reflex\Reflex
REM git clone --branch main --depth 1 https://github.com/NVIDIAGameWorks/Reflex.git 3rd_party\Src\NVIDIA_Reflex\Reflex

REM NVIDIA Streamline (SL 2.12.0) + meshoptimizer (v1.2) — needed by the Vulkan renderer
REM (vk_sl.cpp / vk_dlss.cpp / vk_meshopt.cpp), referenced from vulkan_renderer.props.
REM Both are gitignored (see the root .gitignore): unlike every dep above, they unpack
REM into the dependency folder ITSELF rather than a nested clone dir, and meshoptimizer
REM ships its own .gitignore that must not be replaced.
REM
REM Deliberately NOT executable yet: the delete-then-clone pattern used above removes the
REM folder FIRST, so a wrong URL or a moved tag would destroy a working SDK and fail to
REM replace it. Verify upstream before enabling (NVIDIA has renamed the Streamline org).
REM   meshoptimizer: clone https://github.com/zeux/meshoptimizer.git (tag v1.2) directly
REM                  into 3rd_party\Src\meshoptimizer
REM   Streamline:    shipped as a RELEASE asset (prebuilt bin/include/lib), not a source
REM                  clone — unpack the v2.12.0 release into 3rd_party\Src\NVIDIA_Streamline

REM ===== RmlUi 6.2 + freetype 2.13.3 — the HTML/CSS UI layer (xrGame\ui_rml.cpp) =====
REM Gitignored for the same reason as the two above: they unpack FLAT into their
REM dependency folder, not into a nested clone dir. Unlike every other dependency
REM here they are also BUILT in place, and xrGame.vcxproj links the result by path:
REM     3rd_party\Src\RmlUi\Include
REM     3rd_party\Src\RmlUi\build\Release\rmlui.lib
REM     3rd_party\Src\freetype\build\Release\freetype.lib
REM Move these folders and the game project stops linking.
REM
REM Deliberately NOT executable, same reasoning as above: delete-then-clone removes
REM the folder FIRST, so a moved tag destroys a working dependency and cannot put it
REM back. Here it would also throw away the CMake build tree, which is most of the
REM 216 MB these two occupy.
REM
REM   RD /s /q 3rd_party\Src\freetype
REM   git clone --branch VER-2-13-3 --depth 1 https://gitlab.freedesktop.org/freetype/freetype.git 3rd_party\Src\freetype
REM   cmake -S 3rd_party\Src\freetype -B 3rd_party\Src\freetype\build -A x64
REM   cmake --build 3rd_party\Src\freetype\build --config Release
REM
REM   RD /s /q 3rd_party\Src\RmlUi
REM   git clone --branch 6.2 --depth 1 https://github.com/mikke89/RmlUi.git 3rd_party\Src\RmlUi
REM   cmake -S 3rd_party\Src\RmlUi -B 3rd_party\Src\RmlUi\build -A x64 -DBUILD_SHARED_LIBS=OFF -DRMLUI_SAMPLES=OFF
REM   cmake --build 3rd_party\Src\RmlUi\build --config Release
REM
REM freetype FIRST: RmlUi's font engine links against it, so its lib must exist before
REM RmlUi is configured. RMLUI_STATIC_LIB is already defined in xrGame.vcxproj to match
REM BUILD_SHARED_LIBS=OFF.

REM ===== AMD FidelityFX SDK 1.1.4 — FSR for the Vulkan renderer =====
REM Not vendored (585 MB). Engine_properties.props links 3rd_party\Src\AMD_FidelityFX\sdk\bin\ffx_sdk,
REM so after the clone build the SDK's Vulkan backend once, as its own readme describes.
REM Manual for the same reason as RmlUi: delete-then-clone would throw away the built SDK.
REM
REM   RD /s /q 3rd_party\Src\AMD_FidelityFX
REM   git clone --branch v1.1.4 --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git 3rd_party\Src\AMD_FidelityFX

pause