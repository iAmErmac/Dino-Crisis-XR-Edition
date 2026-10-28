import subprocess
import sys

for width in (1, 2, 4):
    result = subprocess.run([sys.argv[1], str(width)], capture_output=True, timeout=10)
    if result.returncode != 85:
        raise SystemExit(f"Device read width {width} reached an unexpected path: {result.returncode}")
print("PASS: all device read widths reject unsupported execution context before device access")
