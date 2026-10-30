"""Exercise the frontend overlay gate with interleaved backbuffer holds."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frontend", type=Path)
    parser.add_argument("--compiler", default="cl")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error(f"compiler unavailable: {args.compiler}")
    source = args.frontend.read_text(encoding="utf-8")
    start = source.index("                static int s_last_full_verts =")
    end = source.index("                /*", source.index(
        "else if (!defer_backbuffer) s_ovl_kept_verts = 0;", start))
    block = source[start:end]
    block = block.replace("                int overlay_defer = 0;",
                          "s_last_full_verts = 1356;\n                int overlay_defer = 0;")
    fast_forward = source.index("    if (ff_skip_render) {")
    reset = source.index("dcvk_frame_reset();", fast_forward)
    if "s_ovl_kept_verts = 0;" not in source[fast_forward:reset]:
        raise SystemExit("fast-forward stream reset must invalidate the retained prefix")
    harness = r'''
#include <cassert>
#include <cstdio>
#ifdef NDEBUG
#error Assertions must be enabled
#endif
static int count;
static int dcvk_stream_count() { return count; }
static int dcvk_submit_count() { return 0; }
static const char* SDL_getenv(const char*) { return nullptr; }
static bool step(int vertices, bool backbuffer) {
    static int s_ovl_kept_verts = 0;
    count = vertices;
    int flipped = 0, h = 240, s_vb_held = 0;
    int defer_backbuffer = backbuffer;
''' + block + r'''
    return defer_backbuffer != 0;
}
int main() {
    assert(step(90, true));
    assert(step(168, false));
    assert(step(246, true));
    assert(step(318, false));
    assert(step(408, true));
    assert(step(432, false));
    assert(step(456, false));
    assert(step(480, false));
    assert(!step(504, false));
    assert(step(90, false));
    puts("PASS: retained prefix, hold cap, and stream shrink");
}
'''
    with tempfile.TemporaryDirectory(prefix="frontend-overlay-") as directory:
        work = Path(directory)
        cpp = work / "overlay.cpp"
        cpp.write_text(harness, encoding="utf-8")
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/EHsc", "/O2", "/UNDEBUG",
                       "overlay.cpp", "/Fe:overlay.exe", "/Fo:overlay.obj"]
            executable = work / "overlay.exe"
        else:
            command = [compiler, "-std=c++11", "-O2", "-UNDEBUG", "overlay.cpp", "-o", "overlay"]
            executable = work / "overlay"
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)


if __name__ == "__main__":
    main()
