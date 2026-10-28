#!/usr/bin/env python3
"""Assemble the pinned execution core and the game's BIOS-free kernel."""

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
from pathlib import Path

import posdiff


REVISION = "31df2e1e0e0a6525733e68ff70e72ecbef84bb09"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(framework, output, frontend_base=None):
    output = output.resolve()
    framework = framework.resolve()
    if any((parent / ".git").exists() for parent in (output, *output.parents)):
        raise ValueError("output must be outside Git checkouts")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be empty")
    archive = subprocess.check_output([
        "git", "-c", "core.autocrlf=false", "-C", str(framework),
        "archive", REVISION,
    ])
    output.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        source.extractall(output, filter="data")

    mod = Path(__file__).resolve().parents[1]
    core_patch = mod / "patches/psxrecomp-scheduler.dcpatch"
    idle_patch = mod / "patches/psxrecomp-idle.dcpatch"
    host_profile_patch = mod / "patches/psxrecomp-host-profile.dcpatch"
    kernel_patch = mod / "patches/dc-kernel-tcb.dcpatch"
    pgxp_patch = mod / "patches/psxrecomp-pgxp.dcpatch"
    pgxp_session_patch = mod / "patches/psxrecomp-pgxp-session.dcpatch"
    starvation_ring_patch = mod / "patches/psxrecomp-starvation-ring.dcpatch"
    gte_patch = mod / "patches/psxrecomp-gte.dcpatch"
    memory_patch = mod / "patches/psxrecomp-memory.dcpatch"
    spu_patch = mod / "patches/psxrecomp-spu.dcpatch"
    dma_patch = mod / "patches/psxrecomp-dma.dcpatch"
    cdrom_patch = mod / "patches/psxrecomp-cdrom.dcpatch"
    debug_patch = mod / "patches/psxrecomp-debug.dcpatch"
    mod_runtime_patch = mod / "patches/psxrecomp-mod-runtime.dcpatch"
    hd_patch = mod / "patches/psxrecomp-hd-textures.dcpatch"
    translation_patch = mod / "patches/psxrecomp-translation.dcpatch"
    selfcheck_patch = mod / "patches/psxrecomp-selfcheck.dcpatch"
    overlay_patch = mod / "patches/psxrecomp-overlay.dcpatch"
    crash_patch = mod / "patches/psxrecomp-crash.dcpatch"
    fntrace_patch = mod / "patches/psxrecomp-fntrace.dcpatch"
    bios_backend_patch = mod / "patches/psxrecomp-bios-backend.dcpatch"
    interpreter_patch = mod / "patches/psxrecomp-interpreter.dcpatch"
    iso_patch = mod / "patches/psxrecomp-iso.dcpatch"
    boot_patch = mod / "patches/psxrecomp-boot-state.dcpatch"
    gcp_pc_patch = mod / "patches/psxrecomp-gcp-pc.dcpatch"
    input_debug_patch = mod / "patches/dc-input-debug.dcpatch"
    audio_debug_patch = mod / "patches/dc-audio-debug.dcpatch"
    posdiff.apply(str(core_patch), str(output))
    posdiff.apply(str(idle_patch), str(output))
    posdiff.apply(str(host_profile_patch), str(output))
    posdiff.apply(str(pgxp_patch), str(output))
    posdiff.apply(str(pgxp_session_patch), str(output))
    posdiff.apply(str(starvation_ring_patch), str(output))
    posdiff.apply(str(gte_patch), str(output))
    posdiff.apply(str(memory_patch), str(output))
    posdiff.apply(str(spu_patch), str(output))
    posdiff.apply(str(dma_patch), str(output))
    posdiff.apply(str(cdrom_patch), str(output))
    posdiff.apply(str(debug_patch), str(output))
    posdiff.apply(str(crash_patch), str(output))
    posdiff.apply(str(overlay_patch), str(output))
    posdiff.apply(str(selfcheck_patch), str(output))
    posdiff.apply(str(hd_patch), str(output))
    posdiff.apply(str(translation_patch), str(output))
    posdiff.apply(str(mod_runtime_patch), str(output))
    posdiff.apply(str(fntrace_patch), str(output))
    posdiff.apply(str(bios_backend_patch), str(output))
    posdiff.apply(str(interpreter_patch), str(output))
    posdiff.apply(str(iso_patch), str(output))
    posdiff.apply(str(boot_patch), str(output))
    posdiff.apply(str(gcp_pc_patch), str(output))
    posdiff.apply(str(input_debug_patch), str(output))
    posdiff.apply(str(audio_debug_patch), str(output))
    for source, destination in (
        ("src/runtime/dc_save_session.cpp", "runtime/src/dc_save_session.cpp"),
        ("src/include/dc_save_session.h", "runtime/include/dc_save_session.h"),
        ("src/runtime/dc_machine_state.c", "runtime/src/dc_machine_state.c"),
        ("src/runtime/dc_fk_policy.c", "runtime/src/dc_fk_policy.c"),
        ("src/runtime/dc_creature_blood.c", "runtime/src/dc_creature_blood.c"),
        ("src/runtime/dc_background.c", "runtime/src/dc_background.c"),
        ("src/include/dc_background.h", "runtime/include/dc_background.h"),
        ("src/runtime/dc_game_entry.c", "runtime/src/dc_game_entry.c"),
        ("src/runtime/dc_screen_capture.cpp", "runtime/src/dc_screen_capture.cpp"),
        ("src/include/dc_screen_capture.h", "runtime/include/dc_screen_capture.h"),
        ("src/include/dc_game_entry.h", "runtime/include/dc_game_entry.h"),
        ("src/include/dc_creature_blood.h", "runtime/include/dc_creature_blood.h"),
        ("src/include/dc_fk_policy.h", "runtime/include/dc_fk_policy.h"),
        ("src/include/dc_machine_state.h", "runtime/include/dc_machine_state.h"),
        ("src/runtime/memcard.c", "runtime/src/memcard.c"),
        ("src/runtime/dc_memcard_snapshot.c", "runtime/src/dc_memcard_snapshot.c"),
        ("src/include/dc_memcard_snapshot.h", "runtime/include/dc_memcard_snapshot.h"),
        ("src/runtime/dc_gte_camera.c", "runtime/src/dc_gte_camera.c"),
        ("src/include/dc_gte_camera.h", "runtime/include/dc_gte_camera.h"),
        ("src/runtime/dc_gte_capture.c", "runtime/src/dc_gte_capture.c"),
        ("src/include/dc_gte_capture.h", "runtime/include/dc_gte_capture.h"),
        ("src/runtime/dc_projection_cache.c", "runtime/src/dc_projection_cache.c"),
        ("src/include/dc_projection_cache.h", "runtime/include/dc_projection_cache.h"),
        ("src/runtime/dc_dma_policy.c", "runtime/src/dc_dma_policy.c"),
        ("src/include/dc_dma_policy.h", "runtime/include/dc_dma_policy.h"),
        ("src/runtime/dc_gpu_state.c", "runtime/src/dc_gpu_state.c"),
        ("src/runtime/dc_gpu_snapshot.inc", "runtime/src/dc_gpu_snapshot.inc"),
        ("src/runtime/dc_faces2.inc", "runtime/src/dc_faces2.inc"),
        ("src/include/dc_gpu_state.h", "runtime/include/dc_gpu_state.h"),
        ("src/include/dc_memory_snapshot.h", "runtime/include/dc_memory_snapshot.h"),
        ("src/runtime/dc_kernel_snapshot.inc", "runtime/src/dc_kernel_snapshot.inc"),
        ("src/include/dc_kernel_snapshot.h", "runtime/include/dc_kernel_snapshot.h"),
        ("src/runtime/dc_save_files.cpp", "runtime/src/dc_save_files.cpp"),
        ("src/include/dc_save_files.h", "runtime/include/dc_save_files.h"),
        ("src/runtime/dc_continuation_wire.c", "runtime/src/dc_continuation_wire.c"),
        ("src/include/dc_continuation_wire.h", "runtime/include/dc_continuation_wire.h"),
        ("src/runtime/dc_audio_host.cpp", "runtime/src/dc_audio_host.cpp"),
        ("src/include/dc_audio_host.h", "runtime/include/dc_audio_host.h"),
        ("src/runtime/dc_audio_output.c", "runtime/src/dc_audio_output.c"),
        ("src/include/dc_audio_output.h", "runtime/include/dc_audio_output.h"),
        ("src/runtime/dc_spu_policy.c", "runtime/src/dc_spu_policy.c"),
        ("src/include/dc_spu_policy.h", "runtime/include/dc_spu_policy.h"),
        ("src/runtime/dc_spu_effects.c", "runtime/src/dc_spu_effects.c"),
        ("src/include/dc_spu_effects.h", "runtime/include/dc_spu_effects.h"),
        ("src/runtime/dc_spu_music.c", "runtime/src/dc_spu_music.c"),
        ("src/include/dc_spu_music.h", "runtime/include/dc_spu_music.h"),
        ("src/runtime/dc_mods.h", "runtime/include/dc_mods.h"),
        ("src/runtime/dc_spu_pages.c", "runtime/src/dc_spu_pages.c"),
        ("src/include/dc_spu_pages.h", "runtime/include/dc_spu_pages.h"),
        ("src/runtime/dc_memory_policy.c", "runtime/src/dc_memory_policy.c"),
        ("src/runtime/dc_memory_extra.c", "runtime/src/dc_memory_extra.c"),
        ("src/include/dc_memory_policy.h", "runtime/include/dc_memory_policy.h"),
        ("src/runtime/bios_hle.c", "runtime/src/dc_kernel.c"),
        ("src/runtime/bios_hle.h", "runtime/include/dc_kernel.h"),
        ("src/runtime/dc_kernel_backend.c", "runtime/src/dc_kernel_backend.c"),
        ("src/runtime/dc_kernel_events.c", "runtime/src/dc_kernel_events.c"),
        ("src/include/dc_kernel_events.h", "runtime/include/dc_kernel_events.h"),
        ("src/runtime/pgxp_addr.c", "runtime/src/pgxp_addr.c"),
        ("src/include/pgxp_cells.h", "runtime/include/pgxp_cells.h"),
        ("src/include/dc_pgxp.h", "runtime/include/dc_pgxp.h"),
        ("src/include/dc_register_precision.h", "runtime/include/dc_register_precision.h"),
        ("src/include/dc_generated.h", "runtime/include/dc_generated.h"),
        ("src/include/dc_call_state.h", "runtime/include/dc_call_state.h"),
        ("src/include/dc_draw_pose.h", "runtime/include/dc_draw_pose.h"),
        ("src/include/dc_sound_swap.h", "runtime/include/dc_sound_swap.h"),
        ("src/include/dc_mesh_runs.h", "runtime/include/dc_mesh_runs.h"),
        ("src/include/dc_guest_registers.h", "runtime/include/dc_guest_registers.h"),
        ("src/include/dc_shot.h", "runtime/include/dc_shot.h"),
        ("src/runtime/dc_shot.c", "runtime/src/dc_shot.c"),
        ("src/runtime/dc_shot_calls.c", "runtime/src/dc_shot_calls.c"),
        ("src/runtime/dc_guest_registers.c", "runtime/src/dc_guest_registers.c"),
        ("src/runtime/dc_grenade_calls.c", "runtime/src/dc_grenade_calls.c"),
        ("src/runtime/dc_mesh_calls.c", "runtime/src/dc_mesh_calls.c"),
        ("src/runtime/dc_sound_swap.c", "runtime/src/dc_sound_swap.c"),
        ("src/runtime/dc_sound_calls.c", "runtime/src/dc_sound_calls.c"),
        ("src/runtime/dc_pose_calls.c", "runtime/src/dc_pose_calls.c"),
        ("src/runtime/dc_call_state.c", "runtime/src/dc_call_state.c"),
        ("src/runtime/dc_anim_calls.c", "runtime/src/dc_anim_calls.c"),
        ("src/runtime/dc_blood_calls.c", "runtime/src/dc_blood_calls.c"),
    ):
        shutil.copyfile(mod / source, output / destination)
    posdiff.apply(str(kernel_patch), str(output))

    affected = [
        "runtime/src/dc_machine_state.c", "runtime/include/dc_machine_state.h",
        "runtime/src/dc_fk_policy.c", "runtime/include/dc_fk_policy.h",
        "runtime/src/dc_creature_blood.c", "runtime/include/dc_creature_blood.h",
        "runtime/src/dc_background.c", "runtime/include/dc_background.h",
        "runtime/src/dc_game_entry.c", "runtime/include/dc_game_entry.h",
        "runtime/src/dc_screen_capture.cpp", "runtime/include/dc_screen_capture.h",
        "runtime/src/crash_trace.c", "runtime/src/overlay_loader.c", "runtime/src/psx_selfcheck.c", "runtime/src/gpu_hd_textures.cpp", "runtime/src/text_xlate.cpp", "runtime/src/mod_runtime.cpp",
        "runtime/src/memcard.c",
        "runtime/src/dc_save_session.cpp", "runtime/include/dc_save_session.h",
        "runtime/src/dc_memcard_snapshot.c", "runtime/include/dc_memcard_snapshot.h",
        "runtime/src/dc_gte_camera.c", "runtime/include/dc_gte_camera.h",
        "runtime/src/dc_gte_capture.c", "runtime/include/dc_gte_capture.h",
        "runtime/src/dc_projection_cache.c", "runtime/include/dc_projection_cache.h",
        "runtime/src/dma.c", "runtime/include/dma.h", "runtime/src/dc_dma_policy.c", "runtime/include/dc_dma_policy.h",
        "runtime/src/debug_server.c", "runtime/src/fntrace.c", "runtime/include/fntrace.h", "runtime/src/psx_bios_backend.c", "runtime/src/dirty_ram_interp.c", "runtime/src/iso_reader.cpp", "runtime/src/cdrom.c", "runtime/include/cdrom.h", "runtime/src/boot_state.c", "runtime/include/boot_state.h",
        "runtime/include/guest_cycle_profile.h", "runtime/src/guest_cycle_profile.c", "runtime/include/cpu_state.h",
        "runtime/src/dc_gpu_state.c", "runtime/src/dc_gpu_snapshot.inc",
        "runtime/src/dc_faces2.inc", "runtime/include/dc_gpu_state.h",
        "runtime/include/dc_memory_snapshot.h",
        "runtime/src/dc_kernel_snapshot.inc", "runtime/include/dc_kernel_snapshot.h",
        "runtime/src/dc_save_files.cpp", "runtime/include/dc_save_files.h",
        "runtime/src/dc_continuation_wire.c", "runtime/include/dc_continuation_wire.h",
        "runtime/src/dc_audio_host.cpp", "runtime/include/dc_audio_host.h",
        "runtime/src/dc_audio_output.c", "runtime/include/dc_audio_output.h",
        "runtime/src/dc_spu_policy.c", "runtime/include/dc_spu_policy.h",
        "runtime/src/dc_spu_effects.c", "runtime/include/dc_spu_effects.h",
        "runtime/src/dc_spu_music.c", "runtime/include/dc_spu_music.h", "runtime/include/dc_mods.h",
        "runtime/src/spu.c", "runtime/include/spu.h", "runtime/src/dc_spu_pages.c", "runtime/include/dc_spu_pages.h",
        "runtime/src/memory.c", "runtime/include/psx_memory.h",
        "runtime/include/psx_cyc.h", "runtime/src/dc_memory_policy.c",
        "runtime/include/mod_memory.h",
        "runtime/src/dc_memory_extra.c",
        "runtime/include/dc_memory_policy.h",
        "runtime/src/traps.c", "runtime/src/interrupts.c",
        "runtime/include/psx_scheduler.h", "runtime/src/dc_kernel.c",
        "runtime/include/dc_kernel.h", "runtime/src/dc_kernel_backend.c",
        "runtime/src/dc_kernel_events.c", "runtime/include/dc_kernel_events.h",
        "runtime/src/pgxp.cpp", "runtime/src/pgxp_addr.c",
        "runtime/include/pgxp_hooks.h",
        "runtime/include/pgxp_cells.h", "runtime/include/dc_pgxp.h",
        "runtime/include/dc_register_precision.h",
        "runtime/src/gte.cpp",
        "runtime/include/dc_generated.h",
        "runtime/include/dc_call_state.h", "runtime/src/dc_call_state.c",
        "runtime/src/dc_anim_calls.c",
        "runtime/src/dc_blood_calls.c",
        "runtime/include/dc_draw_pose.h", "runtime/src/dc_pose_calls.c",
        "runtime/include/dc_sound_swap.h", "runtime/src/dc_sound_swap.c",
        "runtime/src/dc_sound_calls.c",
        "runtime/include/dc_mesh_runs.h", "runtime/src/dc_mesh_calls.c",
        "runtime/include/dc_guest_registers.h", "runtime/src/dc_guest_registers.c",
        "runtime/src/dc_grenade_calls.c",
        "runtime/include/dc_shot.h", "runtime/src/dc_shot.c", "runtime/src/dc_shot_calls.c",
        "runtime/src/pgxp_session.cpp", "runtime/include/pgxp_session.h",
        "runtime/src/starvation_ring.c", "runtime/include/starvation_ring.h",
    ]
    affected.extend(["runtime/src/psx_cycles.c", "runtime/include/psx_cycles.h"])
    affected.append("runtime/src/host_sampler.c")
    patches = [core_patch, idle_patch, host_profile_patch, kernel_patch, pgxp_patch, pgxp_session_patch, starvation_ring_patch, gte_patch, memory_patch, spu_patch, dma_patch, cdrom_patch, debug_patch, crash_patch, overlay_patch, selfcheck_patch, hd_patch, translation_patch, mod_runtime_patch, fntrace_patch, bios_backend_patch, interpreter_patch, iso_patch, boot_patch, gcp_pc_patch, input_debug_patch, audio_debug_patch]
    if frontend_base is not None:
        gpu_patch = mod / "patches/dc-gpu-tcb.dcpatch"
        if sha(frontend_base / "runtime/src/gpu.c") != "ae239fe2fb7dbe3ef18cad8f00953f83dd3b4514aa9ed876c5a2b12afe0c99b8":
            raise ValueError("custom GPU dependency hash mismatch")
        shutil.copyfile(frontend_base / "runtime/src/gpu.c", output / "runtime/src/dc_gpu.c")
        posdiff.apply(str(gpu_patch), str(output))
        affected.append("runtime/src/dc_gpu.c")
        patches.append(gpu_patch)
        renderer_patch = mod / "patches/dc-renderer-private.dcpatch"
        for relative, destination, expected in (
            ("runtime/src/gpu_sw_renderer.c", "runtime/src/dc_gpu_sw_renderer.c",
             "e6d9e52f702a96603455af0fcb38dc4a3ad1d5a8f1513e338397ba6622ea3b9a"),
            ("runtime/include/gpu_sw_renderer.h", "runtime/include/dc_gpu_sw_renderer.h",
             "8c7db53da789d2a5e78b956818a29f9119922a93ce6021fed01bf949b0f57a4f"),
        ):
            if sha(frontend_base / relative) != expected:
                raise ValueError("custom renderer dependency hash mismatch")
            shutil.copyfile(frontend_base / relative, output / destination)
            affected.append(destination)
        posdiff.apply(str(renderer_patch), str(output))
        patches.append(renderer_patch)
        frontend_patch = mod / "patches/dc-frontend-tcb.dcpatch"
        font_header = frontend_base / "runtime/deps/stb/stb_truetype.h"
        if sha(font_header) != "ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab":
            raise ValueError("stb_truetype v1.26 dependency hash mismatch")
        font_destination = output / "runtime/deps/stb/stb_truetype.h"
        font_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(font_header, font_destination)
        affected.append("runtime/deps/stb/stb_truetype.h")
        shutil.copyfile(frontend_base / "runtime/src/main.cpp",
                        output / "runtime/src/dc_frontend.cpp")
        posdiff.apply(str(frontend_patch), str(output))
        affected.append("runtime/src/dc_frontend.cpp")
        patches.append(frontend_patch)

    manifest = {
        "schema": 1,
        "framework_revision": REVISION,
        "patches": {patch.name: sha(patch) for patch in patches},
        "outputs": {relative: sha(output / relative) for relative in affected},
        "scope": "Execution core only; product hooks, devices and frontend require integration.",
    }
    (output / "dc_execution_core.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frontend-base", type=Path,
                        help="Exact patched product framework used for frontend reconstruction")
    arguments = parser.parse_args()
    try:
        prepare(arguments.framework, arguments.output, arguments.frontend_base)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"runtime assembly: {error}\n")
    print("Assembled the pinned execution core; see dc_execution_core.json.")


if __name__ == "__main__":
    main()
