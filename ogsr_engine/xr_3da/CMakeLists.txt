# Linux build of the engine executable (XR_3DA_Vulkan.vcxproj). Source list is generated from
# the vcxproj — see CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the
# Windows project; do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

if(ANDROID)
    # Android: the engine is libmain.so; SDLActivity loads it and calls SDL_main (x_ray.cpp renames main).
    add_library(xr_3da SHARED ${XR3DA_SOURCES})
    set_target_properties(xr_3da PROPERTIES OUTPUT_NAME main)
else()
    add_executable(xr_3da ${XR3DA_SOURCES})
endif()

target_include_directories(xr_3da PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OGSR_ENGINE_DIR}/xrOnlineNet
    ${OGSR_ENGINE_DIR}/Include
    # eax.h's OPENAL branch (sound-env editor bits compiled into the exe too).
    ${OGSR_3RD_PARTY_DIR}/OpenAL_Soft/include/AL
)
target_compile_definitions(xr_3da PRIVATE
    NDEBUG
)
# _WINDOWS: same call as every module — not carried over.

# The engine modules reference each other in cycles (xrGame <-> xrRenderVulkan <-> the exe's own
# objects via xrCore callbacks, etc.): MSVC's linker resolves that for free, GNU ld needs the
# archives rescanned as a group.
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # devkitPro's toolchain does not implement LINK_GROUP:RESCAN; spell the group out.
    target_link_libraries(xr_3da PRIVATE
        -Wl,--start-group xrGame xrRenderVulkan xrSound xrParticles xrCDB Luabind xrCore squashfs lzo xr_nx_libc -Wl,--end-group)
else()
    target_link_libraries(xr_3da PRIVATE
        "$<LINK_GROUP:RESCAN,xrGame,xrRenderVulkan,xrSound,xrParticles,xrCDB,Luabind,xrCore>")
endif()
target_link_libraries(xr_3da PRIVATE
    luajit
    ode
    theora
    imgui
    RmlUi::Core
)
if(TARGET GameNetworkingSockets::static)
    target_link_libraries(xr_3da PRIVATE GameNetworkingSockets::static)
endif()
if(OGSR_NGX_LIBRARY)
    target_link_libraries(xr_3da PRIVATE ${OGSR_NGX_LIBRARY} ${CMAKE_DL_LIBS})
endif()

if(ANDROID)
    # Xr_input.cpp is an inert stub off-MSVC: SDL keyboard/mouse/game controller instead.
    get_target_property(_xr_srcs xr_3da SOURCES)
    list(FILTER _xr_srcs EXCLUDE REGEX "[Xx]r_input\\.cpp$")
    set_property(TARGET xr_3da PROPERTY SOURCES ${_xr_srcs})
    target_sources(xr_3da PRIVATE xr_input_sdl.cpp android/xr_android_pad.cpp)
    target_link_libraries(xr_3da PRIVATE SDL2::SDL2 log android)
    # A missing symbol must fail the link, not the System.loadLibrary() on the phone.
    target_link_options(xr_3da PRIVATE -Wl,--no-undefined)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Horizon HID lives in a C file: <switch.h> collides with the engine's types in C++ TUs.
    # xr_nx_sdl_audio.c: the SDL audio entry points OpenAL Soft needs, on libnx audout (no SDL library linked).
    target_sources(xr_3da PRIVATE nx/xr_nx_pad.c nx/xr_nx_sdl_audio.c nx/xr_nx_throw.cpp nx/xr_nx_prof.cpp nx/xr_nx_overlay.cpp)
    # xr_nx_link.c: live log + console commands over Wi-Fi (TCP 7000), see the file header.
    target_sources(xr_3da PRIVATE nx/xr_nx_link.c)
    # xr_nx_rwlock.c: std::shared_mutex (pthread_rwlock) without a kernel call per unlock -- see the file header.
    target_sources(xr_3da PRIVATE nx/xr_nx_rwlock.c)
    target_link_options(xr_3da PRIVATE -Wl,--wrap=pthread_rwlock_rdlock -Wl,--wrap=pthread_rwlock_wrlock -Wl,--wrap=pthread_rwlock_unlock -Wl,--wrap=pthread_rwlock_tryrdlock -Wl,--wrap=pthread_rwlock_trywrlock)
    # xr_nx_prof.cpp: every thread (pthreads too) and the libnx calls that cost IPC (nx_prof.txt / nx_cores.txt).
    target_link_options(xr_3da PRIVATE -Wl,--wrap=threadCreate -Wl,--wrap=threadClose -Wl,--wrap=nvFenceWait -Wl,--wrap=nvGpuChannelKickoff -Wl,--wrap=nwindowQueueBuffer -Wl,--wrap=nwindowDequeueBuffer -Wl,--wrap=svcSleepThread -Wl,--wrap=armDCacheClean)
    # Xr_input.cpp is an inert stub off-MSVC (CInput::OnFrame is empty): swap in the real pad backend.
    get_target_property(_xr_srcs xr_3da SOURCES)
    list(FILTER _xr_srcs EXCLUDE REGEX "[Xx]r_input\\.cpp$")
    set_property(TARGET xr_3da PROPERTY SOURCES ${_xr_srcs})
    target_sources(xr_3da PRIVATE xr_input_sdl.cpp)
    # Log every C++ throw (type + call chain) into stderr.log: the NRO has no debugger.
    target_link_options(xr_3da PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_ZNSt6thread6detachEv -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=dup -Wl,--wrap=pthread_create -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign -Wl,--wrap=aligned_alloc -Wl,--wrap=posix_memalign -Wl,--wrap=free -Wl,--wrap=vkAllocateMemory -Wl,--wrap=vkFreeMemory -Wl,--wrap=vkCmdDrawIndexedIndirectCount -Wl,--wrap=vkCmdDrawIndirectCount -Wl,--wrap=vkCmdCopyBuffer -Wl,--wrap=vkCmdFillBuffer -Wl,--wrap=vkCmdUpdateBuffer -Wl,--wrap=vkCmdCopyImageToBuffer -Wl,--wrap=vkCmdCopyBufferToImage -Wl,--wrap=vkCmdCopyImage -Wl,--wrap=vkCmdBlitImage -Wl,--wrap=vkCmdClearColorImage -Wl,--wrap=vkCmdResetQueryPool -Wl,--wrap=vkCmdPipelineBarrier -Wl,--wrap=vkCmdPipelineBarrier2 -Wl,--wrap=vkCmdDispatch)
    target_include_directories(xr_3da PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # The --wrap targets live here: keep them out of LTO so the linker sees real __wrap_/__real_ symbols.
    set_source_files_properties(nx/xr_nx_throw.cpp nx/xr_nx_prof.cpp nx/xr_nx_overlay.cpp nx/xr_nx_pad.c nx/xr_nx_sdl_audio.c nx/xr_nx_rwlock.c
                                PROPERTIES COMPILE_OPTIONS "-fno-lto")
    if(NX_SECTION_ORDER)
        target_link_options(xr_3da PRIVATE "-Wl,--section-ordering-file=${NX_SECTION_ORDER}")
        set_property(TARGET xr_3da APPEND PROPERTY LINK_DEPENDS ${NX_SECTION_ORDER})
    endif()
    target_link_options(xr_3da PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/xr_3da.map")

    # elf -> .nro (homebrew menu / Title Override launch). Target: xr_3da_nro -> ogsr.nro.
    nx_generate_nacp(OUTPUT ogsr.nacp NAME "OGSR Online" AUTHOR "ogsr" VERSION "0.1.0")
    nx_create_nro(xr_3da NACP ogsr.nacp OUTPUT ogsr.nro)
endif()
