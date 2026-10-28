#!/usr/bin/env python3
"""Guard the product frontend's SPU deadline audio integration."""

from pathlib import Path
import re
import sys


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def read(path):
    require(path.is_file(), f"missing source file: {path}")
    return path.read_text(encoding="utf-8")


require(len(sys.argv) == 3, "usage: spu_sample_frontend_test.py <frontend> <runtime-root>")
frontend_path = Path(sys.argv[1])
runtime_root = Path(sys.argv[2])
frontend = read(frontend_path)

require(
    frontend.count("psx_set_midframe_audio_pump(spu_catch_up);") == 1,
    "frontend must register exactly one midframe audio pump",
)
require(
    re.search(
        r"psx_set_midframe_audio_pump\(spu_catch_up\);\s*spu_catch_up\(\);",
        frontend,
    ),
    "frontend must prime the registered pump immediately",
)
setter = frontend.index("psx_set_midframe_audio_pump(spu_catch_up);")
audio_open = frontend.index("SDL_OpenAudioDevice")
scheduler = frontend.index("psx_scheduler_run(&cpu);")
require(setter < audio_open < scheduler, "guest SPU registration must precede host audio setup and scheduler execution")
audio_guard = frontend.rfind("#ifndef PSX_SDL_NO_AUDIO", 0, audio_open)
audio_guard_end = frontend.find("#endif", audio_open)
require(audio_guard >= 0 and audio_guard_end > audio_open, "host audio setup must remain audio-gated")
require(setter < audio_guard, "guest SPU registration must not depend on the host audio guard")
require(
    "dc_audio_host_pump(sdl_audio_device, s_world_hold, g_dc_skip_burst, cdrom_xa_stream_active())"
    in frontend,
    "normal frame path must retain the existing host audio pump",
)
callback_block = re.search(
    r"static void sdl_audio_pump\(void\)\s*\{(?P<body>.*?)\n\}", frontend, re.S
)
require(callback_block and "dc_audio_host_pump" in callback_block.group("body"),
        "normal frame callback must retain host queue refill")
midframe_region = frontend[setter:audio_open]
require("dc_audio_host_pump" not in midframe_region,
        "guest deadline callback must not call the SDL host queue pump")

interrupts = read(runtime_root / "runtime/src/interrupts.c")
cycles = read(runtime_root / "runtime/src/psx_cycles.c")
spu = read(runtime_root / "runtime/src/spu.c")
require("psx_set_midframe_audio_pump" in interrupts, "core callback setter is missing")
require("psx_spu_sample_event_service" in interrupts, "SPU sample service is missing")
require("s_midframe_audio_pump()" in interrupts, "VBlank/sample service does not invoke the callback")
require("psx_spu_sample_event_service()" in cycles, "cycle deadline slicer does not service SPU samples")
require("if (!frames || s_catching_up) return;" in spu,
        "SPU catch-up reentrancy guard is missing")

print("SPU sample frontend audio integration source checks passed")
