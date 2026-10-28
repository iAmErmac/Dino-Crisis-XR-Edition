cmake_minimum_required(VERSION 3.20)
project(DinoCrisis1Recomp C CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "DC_UPDATED_RUNTIME currently requires Windows MSVC")
endif()

option(DC_HOST_PROFILER "Build the optional host CPU sampler" OFF)
option(PSX_GCP_PC_TRACK
    "Compile opt-in last-noted guest-PC/source profiling hooks" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DC_TARGET DinoCrisisME)
set(PRODUCT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Product checkout root")
set(PSXRECOMP_ROOT "" CACHE PATH "Prepared execution core root")
set(SDL2_ROOT "" CACHE PATH "SDL2 MSVC package root")
set(VULKAN_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root")
set(LIBCHDR_ROOT "" CACHE PATH "libchdr source root")
set(ZLIB_ROOT_LOCAL "" CACHE PATH "zlib source root")
set(DC_GENERATED_ROOT "" CACHE PATH "Generated game source root")
set(DC_RESOURCE_ROOT "" CACHE PATH "Runtime resource root")
set(DC_OVERLAY_STATIC_C "" CACHE FILEPATH
    "Optional generated static-overlay dispatcher")

if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake" OR
   NOT EXISTS "${PSXRECOMP_ROOT}/dc_execution_core.json")
    message(FATAL_ERROR
        "PSXRECOMP_ROOT must contain runtime/runtime.cmake and dc_execution_core.json")
endif()
if(NOT EXISTS "${PRODUCT_ROOT}/mod/src/runtime/pt_renderer.c")
    message(FATAL_ERROR "PRODUCT_ROOT does not contain the retained product sources")
endif()
if(NOT SDL2_ROOT OR NOT EXISTS "${SDL2_ROOT}/include/SDL.h" OR
   NOT EXISTS "${SDL2_ROOT}/lib/x64/SDL2.lib")
    message(FATAL_ERROR "SDL2_ROOT must point to an SDL2 MSVC package")
endif()
if(NOT VULKAN_ROOT OR NOT EXISTS "${VULKAN_ROOT}/Include/vulkan/vulkan.h")
    message(FATAL_ERROR "VULKAN_ROOT must point to a Vulkan SDK")
endif()
if(NOT LIBCHDR_ROOT OR NOT EXISTS "${LIBCHDR_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LIBCHDR_ROOT must point to the libchdr source")
endif()
if(NOT ZLIB_ROOT_LOCAL OR NOT EXISTS "${ZLIB_ROOT_LOCAL}/zlib.h")
    message(FATAL_ERROR "ZLIB_ROOT_LOCAL must point to the zlib source")
endif()
if(NOT DC_GENERATED_ROOT OR NOT DC_RESOURCE_ROOT)
    message(FATAL_ERROR
        "DC_GENERATED_ROOT and DC_RESOURCE_ROOT must point to external prepared data roots")
endif()

file(READ "${PSXRECOMP_ROOT}/dc_execution_core.json" _dc_core_manifest)
string(JSON _dc_core_revision ERROR_VARIABLE _dc_core_json_error
    GET "${_dc_core_manifest}" framework_revision)
if(_dc_core_json_error OR
   NOT _dc_core_revision STREQUAL "31df2e1e0e0a6525733e68ff70e72ecbef84bb09")
    message(FATAL_ERROR
        "dc_execution_core.json does not identify the selected execution-core revision")
endif()

set(PSXRECOMP_ROOT "${PSXRECOMP_ROOT}" CACHE PATH
    "Prepared execution core root" FORCE)
set(PSX_SDL_BACKEND SDL2 CACHE STRING "Use the existing SDL2 package" FORCE)
set(SDL2_INCLUDE_DIRS "${SDL2_ROOT}/include" CACHE STRING
    "SDL2 include root" FORCE)
set(SDL2_LIBRARIES "${SDL2_ROOT}/lib/x64/SDL2.lib" CACHE STRING
    "SDL2 library" FORCE)
set(PSX_RECOMP_UI OFF CACHE BOOL "Disable the generic launcher" FORCE)
set(PSX_NETPLAY OFF CACHE BOOL "Disable netplay" FORCE)
set(PSX_SETUP_WIZARD OFF CACHE BOOL "Disable the setup wizard" FORCE)
set(PSX_OPENXR OFF CACHE BOOL "Disable OpenXR for this Windows target" FORCE)
set(PSX_REWIND OFF CACHE BOOL "Disable generic rewind" FORCE)
set(PSXRECOMP_BIOS_STEMS "DC_PRODUCT_NONE" CACHE STRING
    "No generated retail BIOS backends" FORCE)
set(PSXRECOMP_ALLOW_NO_BIOS ON CACHE BOOL
    "Allow the generated zero-backend registry" FORCE)
set(PSX_ZLIB_FETCH OFF CACHE BOOL "Use the selected local zlib source" FORCE)
set(PSX_SDL3_FETCH OFF CACHE BOOL "Use SDL2" FORCE)
set(PSX_DEBUG_TOOLS OFF CACHE BOOL "Disable the debug server" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

add_library(zlibstatic_local STATIC
    "${ZLIB_ROOT_LOCAL}/adler32.c"
    "${ZLIB_ROOT_LOCAL}/compress.c"
    "${ZLIB_ROOT_LOCAL}/crc32.c"
    "${ZLIB_ROOT_LOCAL}/deflate.c"
    "${ZLIB_ROOT_LOCAL}/gzclose.c"
    "${ZLIB_ROOT_LOCAL}/gzlib.c"
    "${ZLIB_ROOT_LOCAL}/gzread.c"
    "${ZLIB_ROOT_LOCAL}/gzwrite.c"
    "${ZLIB_ROOT_LOCAL}/inflate.c"
    "${ZLIB_ROOT_LOCAL}/infback.c"
    "${ZLIB_ROOT_LOCAL}/inftrees.c"
    "${ZLIB_ROOT_LOCAL}/inffast.c"
    "${ZLIB_ROOT_LOCAL}/trees.c"
    "${ZLIB_ROOT_LOCAL}/uncompr.c"
    "${ZLIB_ROOT_LOCAL}/zutil.c")
target_include_directories(zlibstatic_local PUBLIC "${ZLIB_ROOT_LOCAL}")
add_library(ZLIB::ZLIB ALIAS zlibstatic_local)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build libchdr statically" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "Do not install libchdr" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "Use selected libchdr miniz" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "Use selected libchdr zstd" FORCE)
set(CHDR_WANT_RAW_DATA_SECTOR ON CACHE BOOL "Keep complete CD sectors" FORCE)
set(CHDR_WANT_SUBCODE ON CACHE BOOL "Keep CD subcode" FORCE)
set(CHDR_VERIFY_BLOCK_CRC ON CACHE BOOL "Verify CHD block CRC" FORCE)
if(NOT TARGET chdr-static)
    add_subdirectory("${LIBCHDR_ROOT}" "${CMAKE_BINARY_DIR}/libchdr" EXCLUDE_FROM_ALL)
endif()

add_library(SDL2::SDL2 SHARED IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_IMPLIB_RELEASE "${SDL2_ROOT}/lib/x64/SDL2.lib"
    IMPORTED_LOCATION_RELEASE "${SDL2_ROOT}/lib/x64/SDL2.dll"
    IMPORTED_IMPLIB_RELWITHDEBINFO "${SDL2_ROOT}/lib/x64/SDL2.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${SDL2_ROOT}/lib/x64/SDL2.dll"
    IMPORTED_IMPLIB_DEBUG "${SDL2_ROOT}/lib/x64/SDL2.lib"
    IMPORTED_LOCATION_DEBUG "${SDL2_ROOT}/lib/x64/SDL2.dll"
    IMPORTED_IMPLIB_MINSIZEREL "${SDL2_ROOT}/lib/x64/SDL2.lib"
    IMPORTED_LOCATION_MINSIZEREL "${SDL2_ROOT}/lib/x64/SDL2.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_ROOT}/include")

set(STAGE_INCLUDE "${PSXRECOMP_ROOT}/runtime/include")
set(STAGE_SRC "${PSXRECOMP_ROOT}/runtime/src")
set(RECOMP_INCLUDE
    "${PSXRECOMP_ROOT}/recompiler/include"
    "${PSXRECOMP_ROOT}/recompiler/src"
    "${PSXRECOMP_ROOT}/recompiler/lib/fmt/include"
    "${PSXRECOMP_ROOT}/recompiler/lib/toml11")

set(INCOMPATIBLE_GENERIC_NAMES
    main.cpp
    gpu.c
    gpu_sw_renderer.c
    gpu_render.c
    gpu_vk_renderer.c
    gpu_gl_renderer.c
    gl_batch_policy_hle.c
    gl_batch_policy_lle.c
    gl_batch_policy_select.c
    render_thread.c
    present_thread.c
    render_pass.c
    render_pass_frame.c
    render_pass_motion.c
    render_pass_projection.cpp
    dynamic_resolution.c
    mod_builtin_speed.c
    mod_builtin_bezel.c
    psx_bios_module.c
    psx_fiber.c
    bios_hle.c
    bios_hle_plan.c
    game_dispatch_compat.c)

set(SELECTED_FRAMEWORK_SOURCES)
foreach(_source IN LISTS PSXRECOMP_RUNTIME_SOURCES)
    get_filename_component(_name "${_source}" NAME)
    if(NOT _name IN_LIST INCOMPATIBLE_GENERIC_NAMES)
        list(APPEND SELECTED_FRAMEWORK_SOURCES "${_source}")
    endif()
endforeach()

set(STAGE_PRODUCT_SOURCES
    "${STAGE_SRC}/dc_anim_calls.c"
    "${STAGE_SRC}/dc_audio_host.cpp"
    "${STAGE_SRC}/dc_audio_output.c"
    "${STAGE_SRC}/dc_blood_calls.c"
    "${STAGE_SRC}/dc_call_state.c"
    "${STAGE_SRC}/dc_continuation_wire.c"
    "${STAGE_SRC}/dc_dma_policy.c"
    "${STAGE_SRC}/dc_frontend.cpp"
    "${STAGE_SRC}/dc_gpu_snapshot.inc"
    "${STAGE_SRC}/dc_gpu_state.c"
    "${STAGE_SRC}/dc_gpu_sw_renderer.c"
    "${STAGE_SRC}/dc_gpu.c"
    "${STAGE_SRC}/dc_grenade_calls.c"
    "${STAGE_SRC}/dc_gte_capture.c"
    "${STAGE_SRC}/dc_gte_camera.c"
    "${STAGE_SRC}/pgxp_addr.c"
    "${STAGE_SRC}/dc_guest_registers.c"
    "${STAGE_SRC}/dc_kernel_backend.c"
    "${STAGE_SRC}/dc_kernel_events.c"
    "${STAGE_SRC}/dc_kernel_snapshot.inc"
    "${STAGE_SRC}/dc_kernel.c"
    "${STAGE_SRC}/dc_machine_state.c"
    "${STAGE_SRC}/dc_fk_policy.c"
    "${STAGE_SRC}/dc_creature_blood.c"
    "${STAGE_SRC}/dc_background.c"
    "${STAGE_SRC}/dc_game_entry.c"
    "${STAGE_SRC}/dc_screen_capture.cpp"
    "${STAGE_SRC}/dc_memory_extra.c"
    "${STAGE_SRC}/dc_memory_policy.c"
    "${STAGE_SRC}/dc_mesh_calls.c"
    "${STAGE_SRC}/dc_pose_calls.c"
    "${STAGE_SRC}/dc_projection_cache.c"
    "${STAGE_SRC}/dc_save_files.cpp"
    "${STAGE_SRC}/dc_save_session.cpp"
    "${STAGE_SRC}/dc_shot_calls.c"
    "${STAGE_SRC}/dc_shot.c"
    "${STAGE_SRC}/dc_sound_calls.c"
    "${STAGE_SRC}/dc_sound_swap.c"
    "${STAGE_SRC}/dc_spu_effects.c"
    "${STAGE_SRC}/dc_spu_music.c"
    "${STAGE_SRC}/dc_spu_pages.c"
    "${STAGE_SRC}/dc_spu_policy.c")

set(RETAINED_PRODUCT_SOURCES
    "${PRODUCT_ROOT}/mod/src/runtime/dc_binds.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_boot.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_font.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_meshrep.cpp"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_mods.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_memcard_snapshot.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_movie.cpp"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_pak.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_reticle.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_settings.c"
    "${PRODUCT_ROOT}/mod/src/runtime/dc_splash.cpp"
    "${PRODUCT_ROOT}/mod/src/runtime/gpu_vk_raster.cpp"
    "${PRODUCT_ROOT}/mod/src/runtime/pt_anim.c"
    "${PRODUCT_ROOT}/mod/src/runtime/pt_renderer.c"
    "${PRODUCT_ROOT}/mod/src/runtime/pt_vulkan.cpp"
    "${PRODUCT_ROOT}/mod/src/runtime/ufbx.c")

foreach(_source IN LISTS STAGE_PRODUCT_SOURCES RETAINED_PRODUCT_SOURCES)
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR "Selected product source is missing: ${_source}")
    endif()
endforeach()

set(DC_OVERLAY_STATIC_SOURCES)
if(DC_OVERLAY_STATIC_C)
    if(NOT EXISTS "${DC_OVERLAY_STATIC_C}")
        message(FATAL_ERROR "DC_OVERLAY_STATIC_C does not exist: ${DC_OVERLAY_STATIC_C}")
    endif()
    get_filename_component(_overlay_static_dir "${DC_OVERLAY_STATIC_C}" DIRECTORY)
    get_filename_component(_overlay_static_stem "${DC_OVERLAY_STATIC_C}" NAME_WE)
    file(GLOB DC_OVERLAY_STATIC_UNITS CONFIGURE_DEPENDS
        "${_overlay_static_dir}/${_overlay_static_stem}_[0-9][0-9][0-9][0-9].c")
    if(NOT DC_OVERLAY_STATIC_UNITS)
        message(FATAL_ERROR
            "DC_OVERLAY_STATIC_C has no numbered generated units: ${DC_OVERLAY_STATIC_C}")
    endif()
    list(SORT DC_OVERLAY_STATIC_UNITS)
    list(APPEND DC_OVERLAY_STATIC_SOURCES
        "${DC_OVERLAY_STATIC_C}" ${DC_OVERLAY_STATIC_UNITS})
endif()

file(GLOB GENERATED_FULL CONFIGURE_DEPENDS
    "${DC_GENERATED_ROOT}/SLUS_009.22_full_*.c")
list(LENGTH GENERATED_FULL GENERATED_FULL_COUNT)
if(NOT GENERATED_FULL_COUNT EQUAL 43)
    message(FATAL_ERROR
        "Expected 43 hook-shot generated shards, found ${GENERATED_FULL_COUNT}")
endif()
set(GENERATED_DISPATCH "${DC_GENERATED_ROOT}/SLUS_009.22_dispatch.c")
set(GENERATED_DECLS "${DC_GENERATED_ROOT}/SLUS_009.22_decls.h")
if(NOT EXISTS "${GENERATED_DISPATCH}" OR NOT EXISTS "${GENERATED_DECLS}")
    message(FATAL_ERROR "Hook-shot dispatcher/declarations are missing")
endif()

set(DCME_ICON "${DC_RESOURCE_ROOT}/dc_icon.ico")
set(DCME_PAK "${DC_RESOURCE_ROOT}/dcme_data.pak")
set(DCME_MANIFEST "${DC_RESOURCE_ROOT}/dc_utf8.manifest")
set(DCME_GAME_CONFIG "${DC_RESOURCE_ROOT}/game.toml")
foreach(_resource IN ITEMS "${DCME_ICON}" "${DCME_PAK}"
    "${DCME_MANIFEST}" "${DCME_GAME_CONFIG}")
    if(NOT EXISTS "${_resource}")
        message(FATAL_ERROR "Runtime resource is missing: ${_resource}")
    endif()
endforeach()

file(TO_NATIVE_PATH "${DCME_ICON}" DCME_ICON_NATIVE)
file(TO_NATIVE_PATH "${DCME_PAK}" DCME_PAK_NATIVE)
string(REPLACE "\\" "\\\\" DCME_ICON_RC "${DCME_ICON_NATIVE}")
string(REPLACE "\\" "\\\\" DCME_PAK_RC "${DCME_PAK_NATIVE}")
file(WRITE "${CMAKE_BINARY_DIR}/dcxr_product.rc"
    "1 ICON \"${DCME_ICON_RC}\"\n"
    "DCME_DATA RCDATA \"${DCME_PAK_RC}\"\n")

add_executable(${DC_TARGET}
    ${SELECTED_FRAMEWORK_SOURCES}
    ${STAGE_PRODUCT_SOURCES}
    ${RETAINED_PRODUCT_SOURCES}
    ${DC_OVERLAY_STATIC_SOURCES}
    ${GENERATED_FULL}
    "${GENERATED_DISPATCH}"
    ${PSXRECOMP_BIOS_GENERATED}
    "${CMAKE_BINARY_DIR}/dcxr_product.rc")
target_sources(${DC_TARGET} PRIVATE "${DCME_MANIFEST}")
psxrecomp_sha256_file(${DC_TARGET})

target_include_directories(${DC_TARGET} PRIVATE
    "${STAGE_INCLUDE}"
    "${STAGE_SRC}"
    "${PRODUCT_ROOT}/mod/src/include"
    "${PRODUCT_ROOT}/mod/src/runtime"
    "${DC_GENERATED_ROOT}"
    "${LIBCHDR_ROOT}/include"
    "${LIBCHDR_ROOT}/src"
    "${ZLIB_ROOT_LOCAL}"
    "${VULKAN_ROOT}/Include"
    ${RECOMP_INCLUDE}
    "${SDL2_ROOT}/include")

set(PRODUCT_DEFINITIONS
    DC_TCB_CPS=1
    DC_MEMORY_POLICY=1
    PSX_PGXP=1
    DC_PGXP_ADDR=1
    DC_SPU_PAGES=1
    DC_SPU_POLICY=1
    DC_SPU_MUSIC=1
    DC_SPU_EFFECTS=1
    DC_SAVE_SESSION=1
    PSX_HLE_BIOS=1
    PSX_HAS_GAME_DISPATCH=1
    PSX_NATIVE_BUILD=1
    PSX_ENABLE_BLOCK_CYCLES=1
    PSX_NO_DEBUG_TOOLS=1
    DCME_RELEASE=1
    MCM_NO_IMGUI=1
    MCM_NO_PT=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    SDL_MAIN_HANDLED
    FMT_HEADER_ONLY=1
    DEFAULT_DEBUG_PORT=4472
    "PSX_WINDOW_TITLE=\"Dino Crisis\""
    "PSX_DEFAULT_GAME_CONFIG_PATH=\"game.toml\"")
if(DC_OVERLAY_STATIC_SOURCES)
    list(APPEND PRODUCT_DEFINITIONS PSX_HAS_OVERLAY_DISPATCH=1)
endif()
if(PSX_GCP_PC_TRACK)
    list(APPEND PRODUCT_DEFINITIONS PSX_GCP_PC_TRACK=1)
endif()
target_compile_definitions(${DC_TARGET} PRIVATE ${PRODUCT_DEFINITIONS})

if(DC_HOST_PROFILER)
    foreach(_profile_source IN ITEMS host_sampler.c dc_frontend.cpp)
        set_property(SOURCE "${STAGE_SRC}/${_profile_source}" APPEND PROPERTY
            COMPILE_DEFINITIONS DC_HOST_PROFILER=1)
    endforeach()
endif()

target_compile_features(${DC_TARGET} PRIVATE c_std_11 cxx_std_17)
set(_C11_ATOMIC_SOURCES
    ${SELECTED_FRAMEWORK_SOURCES}
    ${STAGE_PRODUCT_SOURCES}
    ${RETAINED_PRODUCT_SOURCES}
    ${DC_OVERLAY_STATIC_SOURCES}
    ${GENERATED_FULL}
    "${GENERATED_DISPATCH}"
    ${PSXRECOMP_BIOS_GENERATED})
list(FILTER _C11_ATOMIC_SOURCES INCLUDE REGEX "\\.c$")
set_source_files_properties(${_C11_ATOMIC_SOURCES}
    PROPERTIES COMPILE_OPTIONS "/experimental:c11atomics")

set(DC_GENERATED_OPTIMIZATION "O2" CACHE STRING
    "MSVC generated-code optimization")
set_property(CACHE DC_GENERATED_OPTIMIZATION PROPERTY STRINGS O1 O2 Od)
if(NOT DC_GENERATED_OPTIMIZATION MATCHES "^(O1|O2|Od)$")
    message(FATAL_ERROR "DC_GENERATED_OPTIMIZATION must be O1, O2 or Od")
endif()
set_source_files_properties(${GENERATED_FULL}
    PROPERTIES COMPILE_OPTIONS "/${DC_GENERATED_OPTIMIZATION};/experimental:c11atomics")

target_compile_options(${DC_TARGET} PRIVATE
    /W4 /we4013 /MP4
    /FI"${STAGE_INCLUDE}/dc_generated.h")

target_link_libraries(${DC_TARGET} PRIVATE
    SDL2::SDL2
    chdr-static
    psx_texture_image_decode
    ZLIB::ZLIB
    ws2_32
    iphlpapi
    dbghelp
    comdlg32
    ole32
    windowscodecs
    mfuuid
    shell32
    d3d11
    dxgi)
if(EXISTS "${VULKAN_ROOT}/Lib/vulkan-1.lib")
    target_link_libraries(${DC_TARGET} PRIVATE "${VULKAN_ROOT}/Lib/vulkan-1.lib")
else()
    message(FATAL_ERROR "Vulkan import library is missing under VULKAN_ROOT")
endif()

target_compile_options(${DC_TARGET} PRIVATE /EHsc /Gy /Gw)
target_link_options(${DC_TARGET} PRIVATE
    /OPT:REF /OPT:ICF /INCREMENTAL:NO
    /MAP:${CMAKE_CURRENT_BINARY_DIR}/DinoCrisisME.map)
