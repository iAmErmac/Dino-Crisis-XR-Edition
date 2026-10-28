cmake_minimum_required(VERSION 3.20)
project(DcSchedulerTests C CXX)
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/src/traps.c")
    message(FATAL_ERROR "PSXRECOMP_ROOT must identify the configured runtime")
endif()
enable_testing()
if(DC_GENERATED_DIR)
    file(GLOB generated_shards CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/*_full_*.c")
    file(GLOB generated_dispatch CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/*_dispatch.c")
    if(NOT generated_shards)
        message(FATAL_ERROR "DC_GENERATED_DIR contains no generated C shards")
    endif()
    add_library(dc_generated_objects OBJECT ${generated_shards} ${generated_dispatch})
    target_include_directories(dc_generated_objects PRIVATE "${PSXRECOMP_ROOT}/runtime/include")
    target_compile_definitions(dc_generated_objects PRIVATE PSX_PGXP=1 PSX_ENABLE_BLOCK_CYCLES=1 DC_MEMORY_POLICY=1)
    target_compile_features(dc_generated_objects PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(dc_generated_objects PRIVATE /W3 /we4013)
        target_compile_options(dc_generated_objects PRIVATE /FI"${PSXRECOMP_ROOT}/runtime/include/dc_generated.h")
    else()
        target_compile_options(dc_generated_objects PRIVATE -Werror=implicit-function-declaration)
        target_compile_options(dc_generated_objects PRIVATE -include "${PSXRECOMP_ROOT}/runtime/include/dc_generated.h")
    endif()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME dc_spu_sample_frontend_test
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_sample_frontend_test.py"
        "${PSXRECOMP_ROOT}/runtime/src/dc_frontend.cpp"
        "${PSXRECOMP_ROOT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    make_entry_fixture.py make_store_fixture.py make_render_fixture.py make_call_fixture.py make_sound_fixture.py ../../tools/hook_migrate.py)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_entry_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/entry_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_entry_hook_test "${CMAKE_CURRENT_BINARY_DIR}/entry_hook_fixture.c")
target_include_directories(dc_entry_hook_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_entry_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_entry_hook_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_entry_hook_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_entry_hook_test COMMAND dc_entry_hook_test)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_store_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/store_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_store_hook_test "${CMAKE_CURRENT_BINARY_DIR}/store_hook_fixture.c")
target_include_directories(dc_store_hook_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_store_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_store_hook_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_store_hook_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_store_hook_test COMMAND dc_store_hook_test)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_render_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/render_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_render_hook_test "${CMAKE_CURRENT_BINARY_DIR}/render_hook_fixture.c")
target_include_directories(dc_render_hook_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_render_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_render_hook_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_render_hook_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_render_hook_test COMMAND dc_render_hook_test)
add_executable(dc_pgxp_test pgxp_test.c ../../src/runtime/pgxp_addr.c)
target_include_directories(dc_pgxp_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_pgxp_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_pgxp_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_pgxp_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_pgxp_test COMMAND dc_pgxp_test)
if(EXISTS "${PSXRECOMP_ROOT}/runtime/include/dc_pgxp.h")
    add_executable(dc_pgxp_bridge_test pgxp_test.c
        "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
        "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp")
    target_include_directories(dc_pgxp_bridge_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(dc_pgxp_bridge_test PRIVATE DC_TEST_PGXP_BRIDGE DC_PGXP_ADDR=1)
    if(MSVC)
        target_compile_options(dc_pgxp_bridge_test PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(dc_pgxp_bridge_test PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(dc_pgxp_bridge_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(dc_pgxp_bridge_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME dc_pgxp_bridge_test COMMAND dc_pgxp_bridge_test)
    add_test(NAME dc_pgxp_dual_test COMMAND dc_pgxp_bridge_test enabled)
    add_executable(dc_gte_pgxp_test gte_pgxp_test.cpp
        "${PSXRECOMP_ROOT}/runtime/src/gte.cpp"
        "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
        "${PSXRECOMP_ROOT}/runtime/src/psx_ram_geometry.c"
        "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
    target_include_directories(dc_gte_pgxp_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(dc_gte_pgxp_test PRIVATE DC_PGXP_ADDR=1 PSX_NO_DEBUG_TOOLS=1 PSX_ENABLE_BLOCK_CYCLES=1)
    target_compile_features(dc_gte_pgxp_test PRIVATE c_std_11 cxx_std_17)
    if(MSVC)
        target_compile_options(dc_gte_pgxp_test PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(dc_gte_pgxp_test PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(dc_gte_pgxp_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(dc_gte_pgxp_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME dc_gte_pgxp_test COMMAND dc_gte_pgxp_test)
    add_executable(dc_register_precision_test register_precision_test.cpp
        ../../src/runtime/pgxp_addr.c
        "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp")
    target_include_directories(dc_register_precision_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}" ../../src/include)
    target_compile_definitions(dc_register_precision_test PRIVATE DC_PGXP_ADDR=1)
    target_compile_features(dc_register_precision_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(dc_register_precision_test PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(dc_register_precision_test PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(dc_register_precision_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(dc_register_precision_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME dc_register_precision_test COMMAND dc_register_precision_test)
endif()
add_executable(dc_scheduler_test scheduler_test.c unrelated_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_scheduler_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_scheduler_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_scheduler_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_scheduler_test PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(dc_scheduler_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_scheduler_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_scheduler_test COMMAND dc_scheduler_test)
add_executable(dc_anim_call_test scheduler_test.c unrelated_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_anim_calls.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_anim_call_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_anim_call_test PRIVATE DC_TEST_CALLS)
target_compile_features(dc_anim_call_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_anim_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_anim_call_test PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(dc_anim_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_anim_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_anim_call_test COMMAND dc_anim_call_test)
add_executable(dc_blood_call_test scheduler_test.c unrelated_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_blood_calls.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_blood_call_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_blood_call_test PRIVATE DC_TEST_BLOOD)
target_compile_features(dc_blood_call_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_blood_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_blood_call_test PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(dc_blood_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_blood_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_blood_call_test COMMAND dc_blood_call_test)
if(EXISTS "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
    add_executable(dc_kernel_thread_test scheduler_test.c unrelated_seams.c kernel_thread_cycle_seams.c
        "${PSXRECOMP_ROOT}/runtime/src/traps.c"
        "${PSXRECOMP_ROOT}/runtime/src/dc_call_state.c"
        "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
    target_include_directories(dc_kernel_thread_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include")
    target_compile_features(dc_kernel_thread_test PRIVATE c_std_11)
    target_compile_definitions(dc_kernel_thread_test PRIVATE DC_TEST_KERNEL PSX_HLE_BIOS=1)
    if(MSVC)
        target_compile_options(dc_kernel_thread_test PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(dc_kernel_thread_test PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(dc_kernel_thread_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(dc_kernel_thread_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME dc_kernel_thread_test COMMAND dc_kernel_thread_test)
    add_executable(dc_kernel_backend_test backend_test.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/runtime/dc_kernel_backend.c"
        "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c")
    target_include_directories(dc_kernel_backend_test PRIVATE ../../src/include
        "${PSXRECOMP_ROOT}/runtime/include")
    target_compile_features(dc_kernel_backend_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(dc_kernel_backend_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(dc_kernel_backend_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME dc_kernel_backend_test COMMAND dc_kernel_backend_test)
    add_executable(dc_kernel_irq_test kernel_irq_test.c kernel_irq_seams.c
        ../../src/runtime/dc_anim_calls.c
        "${PSXRECOMP_ROOT}/runtime/src/traps.c"
        "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
        "${PSXRECOMP_ROOT}/runtime/src/dc_call_state.c"
        "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
    target_include_directories(dc_kernel_irq_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
    target_compile_definitions(dc_kernel_irq_test PRIVATE PSX_HLE_BIOS=1)
    target_compile_features(dc_kernel_irq_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(dc_kernel_irq_test PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(dc_kernel_irq_test PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(dc_kernel_irq_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(dc_kernel_irq_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME dc_kernel_irq_test COMMAND dc_kernel_irq_test)
    set_tests_properties(dc_kernel_irq_test PROPERTIES TIMEOUT 15)
endif()

add_executable(dc_pose_restore_test pose_restore_test.c ../../src/runtime/pt_anim.c)
target_include_directories(dc_pose_restore_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
add_executable(dc_pose_call_test scheduler_test.c unrelated_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_pose_calls.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_pose_call_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_pose_call_test PRIVATE DC_TEST_POSE)
foreach(pose_target dc_pose_restore_test dc_pose_call_test)
    target_compile_features(${pose_target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${pose_target} PRIVATE /Gy /Gw /UNDEBUG)
        target_link_options(${pose_target} PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
    else()
        target_compile_options(${pose_target} PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(${pose_target} PRIVATE -Wl,--gc-sections)
        target_link_libraries(${pose_target} PRIVATE m)
    endif()
    add_test(NAME ${pose_target} COMMAND ${pose_target})
endforeach()

execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_call_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/call_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_call_hook_test "${CMAKE_CURRENT_BINARY_DIR}/call_hook_fixture.c"
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_anim_calls.c)
target_include_directories(dc_call_hook_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_call_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_call_hook_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_call_hook_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_call_hook_test COMMAND dc_call_hook_test)

set_tests_properties(dc_pose_restore_test PROPERTIES ENVIRONMENT
    "DC_POSE_LOG=0;DC_STRAFE_DBG=0;DC_LADDER_SHIFT=200;DC_LADDER_SHIFT_UP=320")

foreach(call_target dc_anim_call_test dc_blood_call_test dc_kernel_thread_test
                    dc_kernel_irq_test dc_pose_call_test dc_call_hook_test)
    if(TARGET ${call_target})
        target_sources(${call_target} PRIVATE ../../src/runtime/dc_sound_swap.c sound_unreached.c)
    endif()
endforeach()
target_compile_definitions(dc_call_hook_test PRIVATE DC_SOUND_NO_RAM)

add_executable(dc_sound_call_test scheduler_test.c unrelated_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_calls.c
    ../../src/runtime/dc_sound_swap.c "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_sound_call_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_sound_call_test PRIVATE DC_TEST_SOUND)
target_compile_features(dc_sound_call_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_sound_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_sound_call_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_sound_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_sound_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_sound_call_test COMMAND dc_sound_call_test)

execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_sound_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/sound_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_sound_hook_test "${CMAKE_CURRENT_BINARY_DIR}/sound_hook_fixture.c"
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_calls.c ../../src/runtime/dc_sound_swap.c)
target_include_directories(dc_sound_hook_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_sound_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_sound_hook_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_sound_hook_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_sound_hook_test COMMAND dc_sound_hook_test)

add_library(dc_mods_objects OBJECT ../../src/runtime/dc_mods.c)
target_include_directories(dc_mods_objects PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_mods_objects PRIVATE DC_TCB_CPS=1)
target_compile_features(dc_mods_objects PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_mods_objects PRIVATE /W3 /we4013)
else()
    target_compile_options(dc_mods_objects PRIVATE -Werror=implicit-function-declaration)
endif()

add_executable(dc_mesh_call_test scheduler_test.c unrelated_seams.c sound_unreached.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_mesh_calls.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_mesh_call_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_mesh_call_test PRIVATE DC_TEST_MESH)
target_compile_features(dc_mesh_call_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_mesh_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_mesh_call_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_mesh_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_mesh_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_mesh_call_test COMMAND dc_mesh_call_test)
add_library(dc_meshrep_objects OBJECT ../../src/runtime/dc_meshrep.cpp)
target_include_directories(dc_meshrep_objects PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_meshrep_objects PRIVATE DC_TCB_CPS=1 NOMINMAX)
target_compile_features(dc_meshrep_objects PRIVATE cxx_std_17)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS make_mesh_fixture.py)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_mesh_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/mesh_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_mesh_hook_test "${CMAKE_CURRENT_BINARY_DIR}/mesh_hook_fixture.c"
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_mesh_calls.c)
target_include_directories(dc_mesh_hook_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dc_mesh_hook_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_mesh_hook_test PRIVATE /UNDEBUG /we4013)
else()
    target_compile_options(dc_mesh_hook_test PRIVATE -UNDEBUG -Werror=implicit-function-declaration)
endif()
add_test(NAME dc_mesh_hook_test COMMAND dc_mesh_hook_test)

add_executable(dc_grenade_call_test scheduler_test.c unrelated_seams.c sound_unreached.c precision_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_grenade_calls.c
    ../../src/runtime/dc_guest_registers.c "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_grenade_call_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_grenade_call_test PRIVATE DC_TEST_GRENADE DC_PGXP_ADDR=1)
target_compile_features(dc_grenade_call_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_grenade_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_grenade_call_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_grenade_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_grenade_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_grenade_call_test COMMAND dc_grenade_call_test)

target_compile_definitions(dc_pose_restore_test PRIVATE DC_TCB_CPS=1)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS make_grenade_fixture.py)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_grenade_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/grenade_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_grenade_hook_test "${CMAKE_CURRENT_BINARY_DIR}/grenade_hook_fixture.c" precision_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_grenade_calls.c
    ../../src/runtime/dc_guest_registers.c "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_grenade_hook_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dc_grenade_hook_test PRIVATE DC_PGXP_ADDR=1)
target_compile_features(dc_grenade_hook_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_grenade_hook_test PRIVATE /Gy /Gw /UNDEBUG /we4013)
    target_link_options(dc_grenade_hook_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_grenade_hook_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG -Werror=implicit-function-declaration)
    target_link_options(dc_grenade_hook_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_grenade_hook_test COMMAND dc_grenade_hook_test)

add_executable(dc_shot_test shot_test.c ../../src/runtime/dc_shot.c)
target_include_directories(dc_shot_test PRIVATE ../../src/include)
target_compile_features(dc_shot_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_shot_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_shot_test PRIVATE -UNDEBUG)
    target_link_libraries(dc_shot_test PRIVATE m)
endif()
add_test(NAME dc_shot_test COMMAND dc_shot_test)
add_test(NAME dc_shot_disabled_test COMMAND dc_shot_test disabled)

add_executable(dc_shot_call_test scheduler_test.c unrelated_seams.c sound_unreached.c precision_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_shot_calls.c
    ../../src/runtime/dc_shot.c ../../src/runtime/dc_guest_registers.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_shot_call_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_shot_call_test PRIVATE DC_TEST_SHOT DC_PGXP_ADDR=1)
target_compile_features(dc_shot_call_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_shot_call_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_shot_call_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_shot_call_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_shot_call_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_shot_call_test COMMAND dc_shot_call_test)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS make_shot_fixture.py)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_shot_fixture.py"
    "${CMAKE_CURRENT_BINARY_DIR}/shot_hook_fixture.c"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(dc_shot_hook_test "${CMAKE_CURRENT_BINARY_DIR}/shot_hook_fixture.c" precision_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_shot_calls.c
    ../../src/runtime/dc_guest_registers.c "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_shot_hook_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dc_shot_hook_test PRIVATE DC_PGXP_ADDR=1)
target_compile_features(dc_shot_hook_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_shot_hook_test PRIVATE /Gy /Gw /UNDEBUG /we4013)
    target_link_options(dc_shot_hook_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_shot_hook_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG -Werror=implicit-function-declaration)
    target_link_options(dc_shot_hook_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_shot_hook_test COMMAND dc_shot_hook_test)

set_tests_properties(dc_shot_call_test PROPERTIES ENVIRONMENT
    "DC_SHOT_PICK=1;DC_SHOT_SPREAD3=1;DC_TREX_BLOOD=1;DC_SHOTPICK_DBG=0")

add_executable(dc_kernel_irq_backend_test kernel_irq_test.c kernel_irq_seams.c sound_unreached.c
    ../../src/runtime/dc_anim_calls.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_backend.c "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c"
    "${PSXRECOMP_ROOT}/runtime/src/traps.c" "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
    ../../src/runtime/dc_call_state.c "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
target_include_directories(dc_kernel_irq_backend_test PRIVATE
    ../../src/include "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_definitions(dc_kernel_irq_backend_test PRIVATE PSX_HLE_BIOS=1 DC_TEST_KERNEL_BACKEND)
target_compile_features(dc_kernel_irq_backend_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_kernel_irq_backend_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_irq_backend_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_irq_backend_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_irq_backend_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_irq_backend_test COMMAND dc_kernel_irq_backend_test)
set_tests_properties(dc_kernel_irq_backend_test PROPERTIES TIMEOUT 15)

foreach(event_target dc_kernel_thread_test dc_kernel_irq_test dc_kernel_irq_backend_test dc_kernel_backend_test)
    target_sources(${event_target} PRIVATE ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c
        "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c" precision_seams.c)
    target_compile_definitions(${event_target} PRIVATE DC_PGXP_ADDR=1)
    target_compile_features(${event_target} PRIVATE cxx_std_17)
endforeach()
add_executable(dc_kernel_event_backend_test kernel_event_backend_test.c kernel_irq_seams.c sound_unreached.c
    ../../src/runtime/dc_kernel_backend.c ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c"
    "${PSXRECOMP_ROOT}/runtime/src/interrupts.c" "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
target_include_directories(dc_kernel_event_backend_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_definitions(dc_kernel_event_backend_test PRIVATE DC_TEST_KERNEL_BACKEND DC_PGXP_ADDR=1 PSX_HLE_BIOS=1)
target_compile_features(dc_kernel_event_backend_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_kernel_event_backend_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_event_backend_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_event_backend_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_event_backend_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_event_backend_test COMMAND dc_kernel_event_backend_test)
set_tests_properties(dc_kernel_event_backend_test PROPERTIES TIMEOUT 15)

add_executable(dc_background_test background_test.c
    ../../src/runtime/dc_background.c ../../src/runtime/dc_call_state.c
    ../../src/runtime/dc_continuation_wire.c ../../src/runtime/dc_kernel_events.c
    ../../src/runtime/dc_guest_registers.c ../../src/runtime/dc_sound_swap.c
    precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_background_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_background_test PRIVATE DC_PGXP_ADDR=1)
target_compile_features(dc_background_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_background_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_background_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_background_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_background_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_background_test COMMAND dc_background_test)
add_test(NAME dc_background_disabled_test COMMAND dc_background_test disabled)
set_tests_properties(dc_background_test dc_background_disabled_test PROPERTIES TIMEOUT 15)
set_tests_properties(dc_background_disabled_test PROPERTIES ENVIRONMENT "DC_BG_SAMETICK=0")

add_executable(dc_background_backend_test background_backend_test.c
    ../../src/runtime/dc_kernel_backend.c ../../src/runtime/dc_background.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_guest_registers.c
    ../../src/runtime/dc_sound_swap.c precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_background_backend_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_background_backend_test PRIVATE DC_PGXP_ADDR=1)
target_compile_features(dc_background_backend_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_background_backend_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_background_backend_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_background_backend_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_background_backend_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_background_backend_test COMMAND dc_background_backend_test)
set_tests_properties(dc_background_backend_test PROPERTIES TIMEOUT 15)

add_executable(dc_kernel_event_state_test kernel_event_state_test.c precision_seams.c sound_unreached.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_kernel_event_state_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_kernel_event_state_test PRIVATE DC_PGXP_ADDR=1 DC_SOUND_NO_RAM)
target_compile_features(dc_kernel_event_state_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_kernel_event_state_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_event_state_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_event_state_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_event_state_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_event_state_test COMMAND dc_kernel_event_state_test)

add_executable(dc_kernel_card_backend_test kernel_card_backend_test.c kernel_irq_seams.c sound_unreached.c
    ../../src/runtime/dc_kernel_backend.c ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c"
    "${PSXRECOMP_ROOT}/runtime/src/interrupts.c" "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
target_include_directories(dc_kernel_card_backend_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_definitions(dc_kernel_card_backend_test PRIVATE DC_TEST_KERNEL_BACKEND DC_TEST_CARD DC_PGXP_ADDR=1 PSX_HLE_BIOS=1)
target_compile_features(dc_kernel_card_backend_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_kernel_card_backend_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_card_backend_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_card_backend_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_card_backend_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_card_backend_test COMMAND dc_kernel_card_backend_test)
add_test(NAME dc_kernel_card_delayed_test COMMAND dc_kernel_card_backend_test delayed)
set_tests_properties(dc_kernel_card_backend_test PROPERTIES ENVIRONMENT "DC_CARD_DELAY=0")
set_tests_properties(dc_kernel_card_delayed_test PROPERTIES ENVIRONMENT "DC_CARD_DELAY=2" TIMEOUT 15)
set_tests_properties(dc_kernel_card_backend_test PROPERTIES TIMEOUT 15)

add_executable(dc_kernel_boot_tcb_contract_test kernel_boot_tcb_test.c kernel_irq_seams.c sound_unreached.c
    ../../src/runtime/dc_kernel_backend.c ../../src/runtime/dc_background.c background_unreached.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c"
    "${PSXRECOMP_ROOT}/runtime/src/interrupts.c" "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
target_include_directories(dc_kernel_boot_tcb_contract_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_definitions(dc_kernel_boot_tcb_contract_test PRIVATE DC_TEST_KERNEL_BACKEND
    DC_TEST_BOOT_CONTRACT DC_PGXP_ADDR=1 PSX_HLE_BIOS=1)
target_compile_features(dc_kernel_boot_tcb_contract_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_kernel_boot_tcb_contract_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_boot_tcb_contract_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_boot_tcb_contract_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_boot_tcb_contract_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_boot_tcb_contract_test COMMAND dc_kernel_boot_tcb_contract_test)
set_tests_properties(dc_kernel_boot_tcb_contract_test PROPERTIES TIMEOUT 15)

add_executable(dc_kernel_wait_timing_test kernel_wait_timing_test.c wait_event_timing_seams.c sound_unreached.c
    ../../src/runtime/dc_kernel_backend.c ../../src/runtime/dc_background.c background_unreached.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c precision_seams.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c"
    "${PSXRECOMP_ROOT}/runtime/src/interrupts.c" "${PSXRECOMP_ROOT}/runtime/src/dc_kernel.c")
target_include_directories(dc_kernel_wait_timing_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_definitions(dc_kernel_wait_timing_test PRIVATE DC_TEST_KERNEL_BACKEND
    DC_TEST_BOOT_CONTRACT DC_PGXP_ADDR=1 PSX_HLE_BIOS=1)
target_compile_features(dc_kernel_wait_timing_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_kernel_wait_timing_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_kernel_wait_timing_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_wait_timing_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_kernel_wait_timing_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_wait_timing_test COMMAND dc_kernel_wait_timing_test)
set_tests_properties(dc_kernel_wait_timing_test PROPERTIES TIMEOUT 15)


add_library(dc_memory_policy_objects OBJECT ../../src/runtime/dc_memory_policy.c)
target_include_directories(dc_memory_policy_objects PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_memory_policy_objects PRIVATE c_std_11)
target_compile_definitions(dc_memory_policy_objects PRIVATE DC_MEMORY_POLICY=1 DC_TCB_CPS=1)

add_executable(dc_memory_extra_test memory_extra_test.c ../../src/runtime/dc_memory_extra.c)
target_include_directories(dc_memory_extra_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_memory_extra_test PRIVATE c_std_11)
add_test(NAME dc_memory_extra_test COMMAND dc_memory_extra_test)

add_executable(dc_memory_core_test memory_core_test.c memory_core_seams.c memory_dma_test.c
    ../../src/runtime/dc_memory_extra.c
    "${PSXRECOMP_ROOT}/runtime/src/memory.c"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c"
    "${PSXRECOMP_ROOT}/runtime/src/kernel_patch_ranges.c"
    "${PSXRECOMP_ROOT}/runtime/src/dma_gpu_ll.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_ram_geometry.c")
target_include_directories(dc_memory_core_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_memory_core_test PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(dc_memory_core_test PRIVATE DC_MEMORY_POLICY=1 DC_TCB_CPS=1
    PSX_STUBS_REAL_PGXP=1 DC_PGXP_ADDR=1 PSX_ENABLE_BLOCK_CYCLES=1 PSX_NO_DEBUG_TOOLS=1)
add_test(NAME dc_memory_core_test COMMAND dc_memory_core_test)
set_tests_properties(dc_memory_core_test dc_memory_extra_test PROPERTIES TIMEOUT 20)
foreach(memory_target dc_memory_policy_objects dc_memory_extra_test dc_memory_core_test)
    if(MSVC)
        target_compile_options(${memory_target} PRIVATE /Gy /Gw /W3 /we4013 /UNDEBUG)
        if(NOT memory_target STREQUAL "dc_memory_policy_objects")
            target_link_options(${memory_target} PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
        endif()
    else()
        target_compile_options(${memory_target} PRIVATE -ffunction-sections -fdata-sections -Werror=implicit-function-declaration -UNDEBUG)
        if(NOT memory_target STREQUAL "dc_memory_policy_objects")
            target_link_options(${memory_target} PRIVATE -Wl,--gc-sections)
            target_link_libraries(${memory_target} PRIVATE m)
        endif()
    endif()
endforeach()

add_executable(dc_spu_pages_test spu_pages_test.c ../../src/runtime/dc_spu_pages.c)
add_executable(dc_spu_pages_backend_test spu_pages_backend_test.c ../../src/runtime/dc_spu_pages.c
    "${PSXRECOMP_ROOT}/runtime/src/crc32.c")
add_executable(dc_spu_guest_clock_test spu_guest_clock_test.c ../../src/runtime/dc_spu_pages.c)
add_executable(dc_spu_sample_banks_test spu_sample_banks_test.c ../../src/runtime/dc_spu_pages.c)
foreach(spu_target dc_spu_pages_test dc_spu_pages_backend_test dc_spu_guest_clock_test dc_spu_sample_banks_test)
    target_include_directories(${spu_target} PRIVATE ../../src/include
        "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests")
    target_compile_features(${spu_target} PRIVATE c_std_11)
    if(NOT spu_target STREQUAL "dc_spu_guest_clock_test")
        target_compile_definitions(${spu_target} PRIVATE DC_SPU_PAGES=1)
    endif()
    if(MSVC)
        target_compile_options(${spu_target} PRIVATE /W3 /we4013 /UNDEBUG)
    else()
        target_compile_options(${spu_target} PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
        target_link_libraries(${spu_target} PRIVATE m)
    endif()
    add_test(NAME ${spu_target} COMMAND ${spu_target})
    set_tests_properties(${spu_target} PROPERTIES TIMEOUT 20)
endforeach()

add_executable(dc_spu_music_backend_test spu_music_backend_test.c
    ../../src/runtime/dc_spu_pages.c ../../src/runtime/dc_spu_music.c
    "${PSXRECOMP_ROOT}/runtime/src/crc32.c")
target_include_directories(dc_spu_music_backend_test PRIVATE ../../src/include ../../src/runtime
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_spu_music_backend_test PRIVATE c_std_11)
target_compile_definitions(dc_spu_music_backend_test PRIVATE DC_SPU_PAGES=1 DC_SPU_MUSIC=1)
if(MSVC)
    target_compile_options(dc_spu_music_backend_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_music_backend_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
    target_link_libraries(dc_spu_music_backend_test PRIVATE m)
endif()
add_test(NAME dc_spu_music_backend_test COMMAND dc_spu_music_backend_test)
set_tests_properties(dc_spu_music_backend_test PROPERTIES TIMEOUT 20)

add_executable(dc_spu_music_test spu_music_test.c ../../src/runtime/dc_spu_music.c)
target_include_directories(dc_spu_music_test PRIVATE ../../src/include ../../src/runtime)
target_compile_features(dc_spu_music_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_spu_music_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_music_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_spu_music_test COMMAND dc_spu_music_test)
set_tests_properties(dc_spu_music_test PROPERTIES TIMEOUT 20)

add_executable(dc_spu_effects_backend_test spu_effects_backend_test.c
    ../../src/runtime/dc_spu_policy.c ../../src/runtime/dc_spu_pages.c ../../src/runtime/dc_spu_music.c ../../src/runtime/dc_spu_effects.c
    "${PSXRECOMP_ROOT}/runtime/src/crc32.c")
target_include_directories(dc_spu_effects_backend_test PRIVATE ../../src/include ../../src/runtime
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_spu_effects_backend_test PRIVATE c_std_11)
target_compile_definitions(dc_spu_effects_backend_test PRIVATE DC_SPU_PAGES=1 DC_SPU_MUSIC=1 DC_SPU_EFFECTS=1 DC_SPU_POLICY=1)
if(MSVC)
    target_compile_options(dc_spu_effects_backend_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_effects_backend_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
    target_link_libraries(dc_spu_effects_backend_test PRIVATE m)
endif()
add_test(NAME dc_spu_effects_backend_test COMMAND dc_spu_effects_backend_test)
set_tests_properties(dc_spu_effects_backend_test PROPERTIES TIMEOUT 20)

add_executable(dc_spu_effects_test spu_effects_test.c ../../src/runtime/dc_spu_effects.c)
target_include_directories(dc_spu_effects_test PRIVATE ../../src/include ../../src/runtime)
target_compile_features(dc_spu_effects_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_spu_effects_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_effects_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
    target_link_libraries(dc_spu_effects_test PRIVATE m)
endif()
add_test(NAME dc_spu_effects_test COMMAND dc_spu_effects_test)
set_tests_properties(dc_spu_effects_test PROPERTIES TIMEOUT 20)

add_executable(dc_spu_policy_backend_test spu_policy_backend_test.c
    ../../src/runtime/dc_spu_pages.c ../../src/runtime/dc_spu_policy.c
    "${PSXRECOMP_ROOT}/runtime/src/crc32.c")
target_include_directories(dc_spu_policy_backend_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_spu_policy_backend_test PRIVATE c_std_11)
target_compile_definitions(dc_spu_policy_backend_test PRIVATE DC_SPU_PAGES=1 DC_SPU_POLICY=1)
if(MSVC)
    target_compile_options(dc_spu_policy_backend_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_policy_backend_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
    target_link_libraries(dc_spu_policy_backend_test PRIVATE m)
endif()
add_test(NAME dc_spu_policy_backend_test COMMAND dc_spu_policy_backend_test)
set_tests_properties(dc_spu_policy_backend_test PROPERTIES TIMEOUT 20
    ENVIRONMENT "DC_CD_LEAD_PAD=3;DC_CD_LEAD=1;DC_SPU_END_GUARD=1")

add_executable(dc_spu_policy_test spu_policy_test.c ../../src/runtime/dc_spu_policy.c)
target_include_directories(dc_spu_policy_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_spu_policy_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_spu_policy_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_spu_policy_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_spu_policy_test COMMAND dc_spu_policy_test)
set_tests_properties(dc_spu_policy_test PROPERTIES TIMEOUT 20
    ENVIRONMENT "DC_CD_LEAD_PAD=3;DC_CD_LEAD=1;DC_SPU_END_GUARD=1")

set(DC_SDL2_ROOT "" CACHE PATH "SDL2 development package for host audio integration checks")
if(DC_SDL2_ROOT)
    add_executable(dc_audio_host_backend_test audio_host_backend_test.c
        ../../src/runtime/dc_audio_host.cpp ../../src/runtime/dc_audio_output.c
        ../../src/runtime/dc_spu_pages.c "${PSXRECOMP_ROOT}/runtime/src/crc32.c")
    target_include_directories(dc_audio_host_backend_test PRIVATE ../../src/include
        "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests"
        "${DC_SDL2_ROOT}/include")
    target_compile_definitions(dc_audio_host_backend_test PRIVATE
        DC_SPU_PAGES=1 SDL_MAIN_HANDLED DC_AUDIO_HOST_TEST=1)
    target_compile_features(dc_audio_host_backend_test PRIVATE c_std_11 cxx_std_17)
    if(MSVC)
        target_compile_options(dc_audio_host_backend_test PRIVATE /W3 /we4013 /UNDEBUG)
        target_link_libraries(dc_audio_host_backend_test PRIVATE "${DC_SDL2_ROOT}/lib/x64/SDL2.lib")
        add_custom_command(TARGET dc_audio_host_backend_test POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DC_SDL2_ROOT}/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:dc_audio_host_backend_test>/SDL2.dll")
    else()
        message(FATAL_ERROR "Host audio package layout requires a Windows MSVC build")
    endif()
    add_test(NAME dc_audio_host_backend_test COMMAND dc_audio_host_backend_test)
    add_test(NAME dc_audio_host_drc_test COMMAND dc_audio_host_backend_test)
    set_tests_properties(dc_audio_host_backend_test dc_audio_host_drc_test PROPERTIES TIMEOUT 20
        ENVIRONMENT "SDL_AUDIODRIVER=dummy;DC_AUDIO_DRC=0;DC_AUDIO_WAV=;DC_AUDIO_LOG=0;DC_MENU_AUDIO_HOLD=1;DC_AUDIO_MUTE_OUT=0;DC_MENU_AUDIO_FADEIN_MS=40")
    set_property(TEST dc_audio_host_drc_test APPEND PROPERTY ENVIRONMENT "DC_AUDIO_DRC=1;DC_AUDIO_DRC_MAX=0.01;DC_AUDIO_TARGET_MS=60;DC_AUDIO_AHEAD_MS=30")
endif()

add_executable(dc_audio_output_test audio_output_test.c ../../src/runtime/dc_audio_output.c)
target_include_directories(dc_audio_output_test PRIVATE ../../src/include)
target_compile_features(dc_audio_output_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_audio_output_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_audio_output_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
    target_link_libraries(dc_audio_output_test PRIVATE m)
endif()
add_test(NAME dc_audio_output_test COMMAND dc_audio_output_test)
set_tests_properties(dc_audio_output_test PROPERTIES TIMEOUT 20)

if(DC_SDL2_ROOT)
    add_executable(dc_software_present_test software_present_test.c
        "${PSXRECOMP_ROOT}/runtime/src/gpu_sw_renderer.c" "${PSXRECOMP_ROOT}/runtime/src/gpu_vram_dirty.c")
    target_include_directories(dc_software_present_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include" "${DC_SDL2_ROOT}/include")
    target_compile_definitions(dc_software_present_test PRIVATE SDL_MAIN_HANDLED)
    target_compile_features(dc_software_present_test PRIVATE c_std_11)
    target_compile_options(dc_software_present_test PRIVATE /Gy /Gw /W3 /we4013 /UNDEBUG)
    target_link_options(dc_software_present_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
    target_link_libraries(dc_software_present_test PRIVATE "${DC_SDL2_ROOT}/lib/x64/SDL2.lib")
    add_custom_command(TARGET dc_software_present_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DC_SDL2_ROOT}/lib/x64/SDL2.dll"
        "$<TARGET_FILE_DIR:dc_software_present_test>/SDL2.dll")
    add_test(NAME dc_software_present_test COMMAND dc_software_present_test)
    set_tests_properties(dc_software_present_test PROPERTIES TIMEOUT 20 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()

add_executable(dc_continuation_wire_test continuation_wire_test.c precision_seams.c sound_unreached.c
    ../../src/runtime/dc_continuation_wire.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_call_state.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp" "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_continuation_wire_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_continuation_wire_test PRIVATE DC_PGXP_ADDR=1 DC_SOUND_NO_RAM)
target_compile_features(dc_continuation_wire_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_continuation_wire_test PRIVATE /Gy /Gw /W3 /we4013 /UNDEBUG)
    target_link_options(dc_continuation_wire_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_continuation_wire_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_continuation_wire_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_continuation_wire_test COMMAND dc_continuation_wire_test)
set_tests_properties(dc_continuation_wire_test PROPERTIES TIMEOUT 20)

foreach(save_profile development release)
    add_executable(dc_save_files_${save_profile}_test save_files_test.cpp ../../src/runtime/dc_save_files.cpp)
    target_include_directories(dc_save_files_${save_profile}_test PRIVATE ../../src/include)
    target_compile_features(dc_save_files_${save_profile}_test PRIVATE cxx_std_17)
    if(save_profile STREQUAL "release")
        target_compile_definitions(dc_save_files_${save_profile}_test PRIVATE DCME_RELEASE=1)
    endif()
    if(MSVC)
        target_compile_definitions(dc_save_files_${save_profile}_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_options(dc_save_files_${save_profile}_test PRIVATE /W3 /UNDEBUG /utf-8)
    else()
        target_compile_options(dc_save_files_${save_profile}_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME dc_save_files_${save_profile}_test COMMAND dc_save_files_${save_profile}_test "${CMAKE_CURRENT_BINARY_DIR}/save-file-fixtures")
    set_tests_properties(dc_save_files_${save_profile}_test PROPERTIES TIMEOUT 20)
endforeach()

add_executable(dc_kernel_snapshot_test kernel_snapshot_test.c kernel_irq_seams.c sound_unreached.c precision_seams.c
    ../../src/runtime/dc_anim_calls.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c" "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
    "${PSXRECOMP_ROOT}/runtime/src/dc_call_state.c" "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_compile_definitions(dc_kernel_snapshot_test PRIVATE PSX_HLE_BIOS=1 DC_PGXP_ADDR=1)
target_include_directories(dc_kernel_snapshot_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests" ../../src/include)
target_compile_features(dc_kernel_snapshot_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_kernel_snapshot_test PRIVATE /Gy /Gw /W3 /we4013 /UNDEBUG)
    target_link_options(dc_kernel_snapshot_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_kernel_snapshot_test PRIVATE -ffunction-sections -fdata-sections -Werror=implicit-function-declaration -UNDEBUG)
    target_link_options(dc_kernel_snapshot_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_kernel_snapshot_test COMMAND dc_kernel_snapshot_test)
set_tests_properties(dc_kernel_snapshot_test PROPERTIES TIMEOUT 20)

add_executable(dc_memory_snapshot_test memory_snapshot_test.c ../../src/runtime/dc_memory_extra.c)
target_include_directories(dc_memory_snapshot_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_memory_snapshot_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_memory_snapshot_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_memory_snapshot_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_memory_snapshot_test COMMAND dc_memory_snapshot_test)
set_tests_properties(dc_memory_snapshot_test PROPERTIES TIMEOUT 20
    ENVIRONMENT "DC_ARENA_MB=1;DC_MOD_RAM_MB=2;DC_NODE_EXTRA=1")

add_test(NAME dc_memory_snapshot_zero_mod_test COMMAND dc_memory_snapshot_test zero)
set_tests_properties(dc_memory_snapshot_zero_mod_test PROPERTIES TIMEOUT 20
    ENVIRONMENT "DC_ARENA_MB=1;DC_MOD_RAM_MB=0;DC_NODE_EXTRA=1")

add_executable(dc_gpu_state_test gpu_state_test.c ../../src/runtime/dc_gpu_state.c)
target_include_directories(dc_gpu_state_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_gpu_state_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_gpu_state_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_gpu_state_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_gpu_state_test COMMAND dc_gpu_state_test)
set_tests_properties(dc_gpu_state_test PROPERTIES TIMEOUT 20)

add_executable(dc_gpu_snapshot_test gpu_snapshot_test.c ../../src/runtime/dc_gpu_state.c)
target_include_directories(dc_gpu_snapshot_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/tests" ../../src/include ../../src/runtime)
target_compile_features(dc_gpu_snapshot_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_gpu_snapshot_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_gpu_snapshot_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_gpu_snapshot_test COMMAND dc_gpu_snapshot_test)
set_tests_properties(dc_gpu_snapshot_test PROPERTIES TIMEOUT 20)

add_executable(dc_dma_policy_test dma_policy_test.c ../../src/runtime/dc_dma_policy.c)
target_include_directories(dc_dma_policy_test PRIVATE ../../src/include
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_dma_policy_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_dma_policy_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_dma_policy_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_dma_policy_test COMMAND dc_dma_policy_test)
set_tests_properties(dc_dma_policy_test PROPERTIES TIMEOUT 20)

add_executable(dc_cdrom_hooks_test cdrom_hooks_test.c)
target_include_directories(dc_cdrom_hooks_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests" ../../src/include)
target_compile_features(dc_cdrom_hooks_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_cdrom_hooks_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_cdrom_hooks_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_cdrom_hooks_test COMMAND dc_cdrom_hooks_test)
set_tests_properties(dc_cdrom_hooks_test PROPERTIES TIMEOUT 20)

foreach(cache_target dc_gte_pgxp_test dc_memory_core_test)
    target_sources(${cache_target} PRIVATE ../../src/runtime/dc_projection_cache.c)
    target_include_directories(${cache_target} PRIVATE ../../src/include)
endforeach()
target_compile_definitions(dc_gte_pgxp_test PRIVATE DC_TCB_CPS=1)

add_executable(dc_projection_cache_test projection_cache_test.c ../../src/runtime/dc_projection_cache.c)
target_include_directories(dc_projection_cache_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/tests")
target_compile_features(dc_projection_cache_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_projection_cache_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_projection_cache_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_projection_cache_test COMMAND dc_projection_cache_test)
set_tests_properties(dc_projection_cache_test PROPERTIES TIMEOUT 20)

add_executable(dc_dma_controller_test dma_controller_test.c ../../src/runtime/dc_dma_policy.c
    "${PSXRECOMP_ROOT}/runtime/src/dma.c" "${PSXRECOMP_ROOT}/runtime/src/dma_gpu_ll.c"
    "${PSXRECOMP_ROOT}/runtime/src/gpu_timeline.c")
target_include_directories(dc_dma_controller_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests" ../../src/include)
target_compile_features(dc_dma_controller_test PRIVATE c_std_11)
target_compile_definitions(dc_dma_controller_test PRIVATE DC_TCB_CPS=1 DC_MEMORY_POLICY=1)
if(MSVC)
    target_compile_options(dc_dma_controller_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_dma_controller_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_dma_controller_test COMMAND dc_dma_controller_test)
set_tests_properties(dc_dma_controller_test PROPERTIES TIMEOUT 20)

target_sources(dc_gte_pgxp_test PRIVATE ../../src/runtime/dc_gte_capture.c)

target_sources(dc_gte_pgxp_test PRIVATE ../../src/runtime/dc_gte_camera.c)

foreach(camera_profile camera camera-no-mac camera-wide camera-off)
    add_test(NAME dc_gte_${camera_profile}_test COMMAND dc_gte_pgxp_test ${camera_profile})
    set_tests_properties(dc_gte_${camera_profile}_test PROPERTIES TIMEOUT 20
        ENVIRONMENT "DC_PGXP_ADDR=1;DC_UNCLIP=1;DC_REAIM_MACIR=1;DC_SZ_CLAMP=0")
endforeach()
set_property(TEST dc_gte_camera-no-mac_test APPEND PROPERTY ENVIRONMENT "DC_REAIM_MACIR=0")

add_executable(dc_machine_state_test machine_state_test.c kernel_irq_seams.c sound_unreached.c precision_seams.c
    ../../src/runtime/dc_memcard_snapshot.c "${PSXRECOMP_ROOT}/runtime/src/memcard.c"
    ../../src/runtime/dc_machine_state.c ../../src/runtime/dc_fk_policy.c ../../src/runtime/dc_continuation_wire.c ../../src/runtime/dc_memory_extra.c
    ../../src/runtime/dc_anim_calls.c ../../src/runtime/dc_sound_swap.c
    ../../src/runtime/dc_kernel_events.c ../../src/runtime/dc_guest_registers.c
    "${PSXRECOMP_ROOT}/runtime/src/traps.c" "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
    "${PSXRECOMP_ROOT}/runtime/src/dc_call_state.c" "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c")
target_include_directories(dc_machine_state_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/src" "${PSXRECOMP_ROOT}/runtime/tests" ../../src/include)
target_compile_features(dc_machine_state_test PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(dc_machine_state_test PRIVATE PSX_HLE_BIOS=1 DC_PGXP_ADDR=1 DC_REAL_MEMCARD=1)
if(MSVC)
    target_compile_options(dc_machine_state_test PRIVATE /Gy /Gw /W3 /we4013 /UNDEBUG)
    target_link_options(dc_machine_state_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_machine_state_test PRIVATE -ffunction-sections -fdata-sections -Werror=implicit-function-declaration -UNDEBUG)
    target_link_options(dc_machine_state_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_machine_state_test COMMAND dc_machine_state_test)
set_tests_properties(dc_machine_state_test PROPERTIES TIMEOUT 20 ENVIRONMENT "DC_ARENA_MB=1;DC_MOD_RAM_MB=2")

add_executable(dc_memcard_snapshot_test memcard_snapshot_test.c
    ../../src/runtime/dc_memcard_snapshot.c
    "${PSXRECOMP_ROOT}/runtime/src/memcard.c")
target_include_directories(dc_memcard_snapshot_test PRIVATE
    "${PSXRECOMP_ROOT}/runtime/include"
    "${PSXRECOMP_ROOT}/runtime/src"
    ../../src/include ../../src/runtime)
target_compile_features(dc_memcard_snapshot_test PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(dc_memcard_snapshot_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(dc_memcard_snapshot_test PRIVATE /W3 /we4013 /UNDEBUG)
else()
    target_compile_options(dc_memcard_snapshot_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_memcard_snapshot_test COMMAND dc_memcard_snapshot_test)
set_tests_properties(dc_memcard_snapshot_test PROPERTIES TIMEOUT 20
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/memcard-snapshot-test")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/memcard-snapshot-test")

add_executable(dc_save_session_test save_session_test.cpp
    ../../src/runtime/dc_save_session.cpp ../../src/runtime/dc_save_files.cpp)
target_include_directories(dc_save_session_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_save_session_test PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(dc_save_session_test PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(dc_save_session_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_save_session_test COMMAND dc_save_session_test)
set_tests_properties(dc_save_session_test PROPERTIES TIMEOUT 20)

set(DC_LIBCHDR_ROOT "" CACHE PATH "Pinned libchdr source for disc policy checks")
if(DC_LIBCHDR_ROOT)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${DC_LIBCHDR_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/libchdr" EXCLUDE_FROM_ALL)
    add_executable(dc_iso_policy_test iso_policy_test.cpp
        "${PSXRECOMP_ROOT}/runtime/src/iso_reader.cpp"
        "${PSXRECOMP_ROOT}/runtime/src/cue_sheet.cpp")
    target_include_directories(dc_iso_policy_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/recompiler/include")
    target_compile_features(dc_iso_policy_test PRIVATE cxx_std_17)
    target_link_libraries(dc_iso_policy_test PRIVATE chdr-static)
    if(MSVC)
        target_compile_options(dc_iso_policy_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(dc_iso_policy_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME dc_iso_policy_test COMMAND dc_iso_policy_test)
    set_tests_properties(dc_iso_policy_test PROPERTIES TIMEOUT 20)
endif()

get_property(dc_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(dc_test_target IN LISTS dc_test_targets)
    get_target_property(dc_test_sources "${dc_test_target}" SOURCES)
    if("${dc_test_sources}" MATCHES "/runtime/src/(traps|interrupts|psx_bios_backend)\\.c")
        target_compile_definitions("${dc_test_target}" PRIVATE DC_TCB_CPS=1)
        if("${dc_test_sources}" MATCHES "/pgxp_addr\\.c" AND
           NOT "${dc_test_sources}" MATCHES "/dc_projection_cache\\.c")
            target_sources("${dc_test_target}" PRIVATE ../../src/runtime/dc_projection_cache.c)
        endif()
    endif()
endforeach()

add_test(NAME dc_spu_cd_reset_events_test COMMAND dc_spu_policy_backend_test)
set_tests_properties(dc_spu_cd_reset_events_test PROPERTIES TIMEOUT 20
    ENVIRONMENT "DC_CD_LEAD_PAD=3;DC_CD_LEAD=1;DC_SPU_END_GUARD=1;DC_TALK_RESET_OLD=1")

add_test(NAME dc_gte_light_capture_test COMMAND dc_gte_pgxp_test)
set_tests_properties(dc_gte_light_capture_test PROPERTIES TIMEOUT 20 ENVIRONMENT "DC_LIGHT_GTE=1")

add_executable(dc_fk_policy_test fk_policy_test.c ../../src/runtime/dc_fk_policy.c)
target_include_directories(dc_fk_policy_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_fk_policy_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_fk_policy_test PRIVATE /we4013 /UNDEBUG)
else()
    target_compile_options(dc_fk_policy_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_fk_policy_test COMMAND dc_fk_policy_test)
set_tests_properties(dc_fk_policy_test PROPERTIES TIMEOUT 20)

add_executable(dc_creature_blood_test scheduler_test.c unrelated_seams.c sound_unreached.c precision_seams.c
    ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c ../../src/runtime/dc_creature_blood.c ../../src/runtime/dc_fk_policy.c ../../src/runtime/dc_continuation_wire.c ../../src/runtime/dc_kernel_events.c
    ../../src/runtime/dc_guest_registers.c "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp_addr.c" "${PSXRECOMP_ROOT}/runtime/src/traps.c")
target_include_directories(dc_creature_blood_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_definitions(dc_creature_blood_test PRIVATE DC_TEST_CREATURE_BLOOD DC_PGXP_ADDR=1)
target_compile_features(dc_creature_blood_test PRIVATE c_std_11 cxx_std_17)
if(MSVC)
    target_compile_options(dc_creature_blood_test PRIVATE /Gy /Gw /UNDEBUG)
    target_link_options(dc_creature_blood_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(dc_creature_blood_test PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(dc_creature_blood_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME dc_creature_blood_test COMMAND dc_creature_blood_test)
target_compile_definitions(dc_creature_blood_test PRIVATE DC_TCB_CPS=1)

target_sources(dc_creature_blood_test PRIVATE ../../src/runtime/dc_projection_cache.c)

add_executable(dc_game_entry_test dc_game_entry_test.c ../../src/runtime/dc_game_entry.c)
target_include_directories(dc_game_entry_test PRIVATE ../../src/include "${PSXRECOMP_ROOT}/runtime/include")
target_compile_features(dc_game_entry_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dc_game_entry_test PRIVATE /we4013 /UNDEBUG)
else()
    target_compile_options(dc_game_entry_test PRIVATE -Werror=implicit-function-declaration -UNDEBUG)
endif()
add_test(NAME dc_game_entry_test COMMAND dc_game_entry_test)
set_tests_properties(dc_game_entry_test PROPERTIES TIMEOUT 20)

foreach(creature_backend dc_kernel_backend_test dc_kernel_irq_backend_test dc_kernel_event_backend_test dc_kernel_card_backend_test)
    target_sources(${creature_backend} PRIVATE ../../src/runtime/dc_creature_blood.c creature_unreached.c)
    target_include_directories(${creature_backend} PRIVATE ../../src/include)
endforeach()
foreach(background_backend dc_kernel_backend_test dc_kernel_irq_backend_test dc_kernel_event_backend_test dc_kernel_card_backend_test)
    target_sources(${background_backend} PRIVATE ../../src/runtime/dc_background.c background_unreached.c)
endforeach()
target_sources(dc_kernel_backend_test PRIVATE ../../src/runtime/dc_call_state.c ../../src/runtime/dc_sound_swap.c sound_unreached.c)
target_compile_definitions(dc_kernel_backend_test PRIVATE DC_SOUND_NO_RAM)
if(MSVC)
    target_compile_options(dc_kernel_backend_test PRIVATE /Gy /Gw)
    target_link_options(dc_kernel_backend_test PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
endif()

add_executable(dc_screen_capture_test screen_capture_test.cpp ../../src/runtime/dc_screen_capture.cpp)
target_include_directories(dc_screen_capture_test PRIVATE ../../src/include)
target_compile_features(dc_screen_capture_test PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(dc_screen_capture_test PRIVATE /UNDEBUG)
else()
    target_compile_options(dc_screen_capture_test PRIVATE -UNDEBUG)
endif()
add_test(NAME dc_screen_capture_test COMMAND dc_screen_capture_test)

add_test(NAME dc_memory_render_context_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory_render_context_test.py" "$<TARGET_FILE:dc_memory_core_test>")
set_tests_properties(dc_memory_render_context_test PROPERTIES TIMEOUT 40)
