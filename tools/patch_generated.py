#!/usr/bin/env python3
"""Prepares the recompiled code for the Switch 2 memory model.

The Switch 2 build keeps all guest memory in one flat arena and folds the Xbox 360 physical
memory aliases in software (rex_s2_fold_delta in the SDK's rex/platform.h). The rexglue code
generator writes the per-platform REX_PHYS_HOST_OFFSET macro into the two *_init.h headers
without that case, so it is added here. Safe to run several times.

usage: python3 tools/patch_generated.py [path/to/skate3recomp/generated]
"""
import pathlib
import sys

ARENA_CASE = (
    "#if REX_ARENA_MEMORY\n"
    "#define REX_PHYS_HOST_OFFSET(addr) rex_s2_fold_delta((u32)(addr))\n"
    "#elif "
)
FIRST_CASE = "#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n#define REX_PHYS_HOST_OFFSET"


def patch(header: pathlib.Path) -> str:
    text = header.read_text()
    if "#if REX_ARENA_MEMORY\n#define REX_PHYS_HOST_OFFSET" in text:
        return "already patched"
    if text.count(FIRST_CASE) != 1:
        raise SystemExit(f"{header}: REX_PHYS_HOST_OFFSET block not found (codegen template changed?)")
    text = text.replace(FIRST_CASE, ARENA_CASE + FIRST_CASE[len("#if "):])
    header.write_text(text)
    return "patched"


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    generated = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "third_party/skate3recomp/generated"
    headers = [generated / "skate3_init.h", generated / "eawebkit" / "eawebkit_init.h"]
    for header in headers:
        if not header.is_file():
            raise SystemExit(f"{header} not found: run the code generation first (README.md)")
        print(f"{header}: {patch(header)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
