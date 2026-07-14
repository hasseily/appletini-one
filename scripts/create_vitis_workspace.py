# This script generates the Vitis workspace and imports the source files for both the bootloader and frontend components.
# The workspace is generated in the `vitis_workspace` directory.
# The source files are imported from their original location in the repository and not copied to the workspace.
# The workspace should not be in git.

# NOTE: To run this script: `vitis -s create_vitis_workspace.py`

# FINAL NOTE:   If you can't delete the workspace because of a permission error, close any Vitis instances and kill any java process in the task manager.
#               When you quit the `vitis` command, the java process is not killed automatically. Kill the fucker with a vengeance.

import json
import os
import sys
from pathlib import Path

import vitis
import subprocess
import shutil
import stat


def fatal(step, exc):
    print(f"\nERROR at step: {step}", file=sys.stderr, flush=True)
    print(f"{type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
    try:
        vitis.dispose()
    except Exception:
        pass
    raise SystemExit(1)


def run_step(step, fn):
    print(f"\n==> {step}", flush=True)
    try:
        return fn()
    except Exception as exc:
        fatal(step, exc)


def require_path(step, p: Path):
    if not p.exists():
        raise FileNotFoundError(f"{p} does not exist")
    return p


def generate_default_bezel_png_sources():
    subprocess.run([sys.executable, "scripts/generate_default_bezel_png.py"], check=True)


def generate_config_menu_logo_png_sources():
    subprocess.run([sys.executable, "scripts/generate_config_menu_logo_png.py"], check=True)


def generate_disk2_sound_sample_sources():
    subprocess.run([sys.executable, "scripts/build_disk2_sound_assets.py"], check=True)


def _ensure_userconfig_block_entry(component_name: str,
                                   block_name: str,
                                   entry: str):
    """Append `entry` to a UserConfig.cmake `set(<block_name> ... )`
    block. Tolerant of whitespace variations Vitis might emit (extra
    spaces after the block name, blank-line-only body, etc.). Verifies
    the entry actually landed in the file before returning. """
    import re
    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()

    # Match: set(<NAME>[any-ws]<body>) -- body may be empty, may contain
    # newlines and arbitrary whitespace, may already have entries.
    pattern = re.compile(
        r"set\(\s*" + re.escape(block_name) + r"\b(?P<body>[^)]*)\)",
        re.DOTALL,
    )
    m = pattern.search(text)
    if m is None:
        raise RuntimeError(
            f"Could not find set({block_name} ...) block in {user_config}.\n"
            f"First 200 chars: {text[:200]!r}")

    body = m.group("body")
    # Idempotency: word-boundary check inside the matched block, not
    # the whole file. Avoids false positives from comments or other
    # blocks that happen to mention the same string.
    if re.search(r"(?:^|\s)" + re.escape(entry) + r"(?:$|\s)", body):
        return

    body_stripped = body.rstrip()
    if body_stripped.strip() == "":
        # Empty block: just put the entry on its own line.
        new_body = f"\n{entry}\n"
    else:
        # Populated block: append on a new line, preserving existing entries.
        new_body = f"{body_stripped}\n{entry}\n"

    new_text = text[:m.start()] + f"set({block_name}{new_body})" + text[m.end():]
    user_config.write_text(new_text)

    # Verify the entry actually landed -- otherwise downstream "build"
    # silently uses the unpatched file.
    verify = user_config.read_text()
    verify_m = pattern.search(verify)
    if verify_m is None or not re.search(
            r"(?:^|\s)" + re.escape(entry) + r"(?:$|\s)", verify_m.group("body")):
        raise RuntimeError(
            f"Patched {user_config} but '{entry}' is not in the "
            f"{block_name} block afterwards. Block now reads:\n"
            f"{verify_m.group(0) if verify_m else '(block not found)'}")


def ensure_userconfig_flag(component_name: str, flag: str):
    """Append `flag` to USER_LINK_OTHER_FLAGS. These are *extra*
    linker flags, placed before the object files / library group on
    the gcc command line. Use for pure linker switches (-Wl,..., -T,
    etc.) -- NOT for libraries: -lm here gets dropped because gcc
    walks left-to-right and there are no unresolved symbols when -lm
    is seen. Use ensure_userconfig_library() for libraries. """
    _ensure_userconfig_block_entry(component_name,
                                   "USER_LINK_OTHER_FLAGS",
                                   f"\"{flag}\"")


def ensure_userconfig_library(component_name: str, libname: str):
    """Append `libname` to USER_LINK_LIBRARIES. CMake places these
    inside the link library group at the end of the gcc command line,
    where they can resolve forward references from the objects.
    `libname` is the library name without the -l prefix (e.g. "m" for
    libm). """
    _ensure_userconfig_block_entry(component_name,
                                   "USER_LINK_LIBRARIES",
                                   libname)


def ensure_userconfig_include_dirs(component_name: str, include_dirs: list[str]):
    for include_dir in include_dirs:
        _ensure_userconfig_block_entry(component_name,
                                       "USER_INCLUDE_DIRECTORIES",
                                       f"\"{include_dir}\"")


def set_userconfig_optimization(component_name: str, level: str = "-O2"):
    """Patch the USER_COMPILE_OPTIMIZATION_LEVEL line in the
    component's UserConfig.cmake. Vitis defaults this to -O0 for
    debug builds; we want -O2 for release-grade performance,
    especially on the AMP CPU1 firmware which has to keep up with
    the ~1 MHz Apple bus rate. """
    import re
    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()
    pattern = re.compile(
        r"(set\(USER_COMPILE_OPTIMIZATION_LEVEL\s+)-O[0123sg](\s*\))",
    )
    new_text, count = pattern.subn(rf"\g<1>{level}\g<2>", text)
    if count == 0:
        raise RuntimeError(
            f"Could not find USER_COMPILE_OPTIMIZATION_LEVEL line in {user_config}")
    if new_text != text:
        user_config.write_text(new_text)
        print(f"  Set {user_config} optimization to {level}", flush=True)


def set_userconfig_optimization_other_flag(component_name: str, flag: str):
    """Set USER_COMPILE_OPTIMIZATION_OTHER_FLAGS to a single flag token.

    The Vitis CMake consumer concatenates multiple tokens inside one
    USER_COMPILE_* variable without separators, so each variable must carry
    exactly one token.

    -falign-functions=32 aligns functions to Cortex-A9 I-cache lines and keeps
    hot-loop placement stable across builds. """
    import re
    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()
    pattern = re.compile(
        r"set\(USER_COMPILE_OPTIMIZATION_OTHER_FLAGS\s*[^)]*\)")
    m = pattern.search(text)
    if m is None:
        raise RuntimeError(
            f"Could not find USER_COMPILE_OPTIMIZATION_OTHER_FLAGS in {user_config}")
    replacement = f"set(USER_COMPILE_OPTIMIZATION_OTHER_FLAGS {flag})"
    if m.group(0) == replacement:
        return
    text = text[:m.start()] + replacement + text[m.end():]
    user_config.write_text(text)
    print(f"  Set optimization other flags to {flag}", flush=True)


def set_userconfig_mfpu(component_name: str, fpu: str):
    """Patch the -mfpu= token in USER_COMPILE_OTHER_FLAGS.

    USER_COMPILE_OTHER_FLAGS lands at the end of the per-target compile
    command, after the BSP toolchain's CMAKE_C_FLAGS, so an -mfpu= flag here
    overrides the BSP default. Preserve any unrelated flags in the generated
    UserConfig.cmake rather than replacing the whole line.

    The frontend uses -mfpu=neon so the compositor's NEON blit path compiles with
    intrinsics (__ARM_NEON). -mfpu=neon is a strict superset of vfpv3 on
    the Cortex-A9 (same -mfloat-abi=hard ABI): all existing VFP code is
    unaffected; it just additionally enables NEON. """
    import re
    if not fpu.startswith("-mfpu="):
        raise ValueError(f"FPU flag must start with -mfpu=: {fpu}")

    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()
    pattern = re.compile(
        r"(set\(USER_COMPILE_OTHER_FLAGS\s*)([^)]*?)(\s*\))",
        re.DOTALL,
    )

    def replace_mfpu(match):
        prefix, body, suffix = match.groups()
        tokens = [token for token in body.split()
                  if not token.startswith("-mfpu=")]
        tokens.append(fpu)
        return prefix + " ".join(tokens) + suffix

    new_text, count = pattern.subn(replace_mfpu, text, count=1)
    if count == 0:
        raise RuntimeError(
            f"Could not find USER_COMPILE_OTHER_FLAGS line in {user_config}")
    if new_text != text:
        user_config.write_text(new_text)
        print(f"  Set {user_config} FPU flag to {fpu}", flush=True)


def ensure_userconfig_sources(component_name: str, sources: list[str]):
    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()

    missing = [src for src in sources if f"\"{src}\"" not in text]
    if not missing:
        return

    marker = "set(USER_COMPILE_SOURCES\n"
    start = text.find(marker)
    if start == -1:
        raise RuntimeError(f"Could not find USER_COMPILE_SOURCES block in {user_config}")

    end = text.find("\n)", start)
    if end == -1:
        raise RuntimeError(f"Could not find end of USER_COMPILE_SOURCES block in {user_config}")

    insertion = "".join(f"\"{src}\"\n" for src in missing)
    text = text[: end + 1] + insertion + text[end + 1 :]
    user_config.write_text(text)


def ensure_userconfig_sources_absent(component_name: str, sources: list[str]):
    user_config = Path("vitis_workspace") / component_name / "src" / "UserConfig.cmake"
    text = require_path(f"Find UserConfig.cmake for {component_name}", user_config).read_text()
    original_text = text

    for src in sources:
        text = text.replace(f"\"{src}\"\n", "")

    if text != original_text:
        user_config.write_text(text)


def enable_fatfs_lfn(domain_bsp: Path):
    """Enable FatFs long filename support for the frontend BSP domain."""
    patches = {
        Path("include") / "xilffs_config.h": [
            ("/* #undef FILE_SYSTEM_USE_LFN */", "#define FILE_SYSTEM_USE_LFN 1"),
        ],
        Path("libsrc") / "build_configs" / "gen_bsp" / "include" / "xilffs_config.h": [
            ("/* #undef FILE_SYSTEM_USE_LFN */", "#define FILE_SYSTEM_USE_LFN 1"),
        ],
        Path("libsrc") / "build_configs" / "gen_bsp" / "CMakeCache.txt": [
            ("XILFFS_use_lfn:STRING=0", "XILFFS_use_lfn:STRING=1"),
        ],
        Path("libsrc") / "build_configs" / "gen_bsp" / "cmake_lib_configs.txt": [
            ("XILFFS_use_lfn:STRING=0", "XILFFS_use_lfn:STRING=1"),
        ],
    }

    for rel_path, replacements in patches.items():
        path = require_path(f"Find {rel_path}", domain_bsp / rel_path)
        text = path.read_text()
        updated = text
        for old, new in replacements:
            updated = updated.replace(old, new)
        if updated != text:
            path.write_text(updated)
            print(f"  Patched {path}", flush=True)

    bsp_yaml = require_path("Find BSP yaml", domain_bsp / "bsp.yaml")
    lines = bsp_yaml.read_text().splitlines(keepends=True)
    in_lfn_option = False
    changed = False
    for i, line in enumerate(lines):
        if line.startswith("    XILFFS_use_lfn:"):
            in_lfn_option = True
            continue
        if in_lfn_option and line.startswith("    ") and not line.startswith("      "):
            in_lfn_option = False
        if in_lfn_option and line.strip() == "value: '0'":
            lines[i] = line.replace("value: '0'", "value: '1'", 1)
            changed = True
            break
    if changed:
        bsp_yaml.write_text("".join(lines))
        print(f"  Patched {bsp_yaml}", flush=True)


def enable_fatfs_timestamp_hook(domain_bsp: Path):
    """Patch generated FatFs get_fattime() to allow a frontend override."""
    path = require_path("Find FatFs diskio.c",
                        domain_bsp / "libsrc" / "xilffs" / "src" / "diskio.c")
    text = path.read_text()
    if "appletini_fatfs_get_fattime" in text:
        return

    old = (
        "DWORD get_fattime (void)\n"
        "{\n"
        "\treturn\t((DWORD)(2010U - 1980U) << 25U)\t/* Fixed to Jan. 1, 2010 */\n"
        "\t\t| ((DWORD)1 << 21)\n"
        "\t\t| ((DWORD)1 << 16)\n"
        "\t\t| ((DWORD)0 << 11)\n"
        "\t\t| ((DWORD)0 << 5)\n"
        "\t\t| ((DWORD)0 >> 1);\n"
        "}\n"
    )
    new = (
        "__attribute__((weak)) DWORD appletini_fatfs_get_fattime (void)\n"
        "{\n"
        "\treturn\t((DWORD)(2010U - 1980U) << 25U)\t/* Fixed to Jan. 1, 2010 */\n"
        "\t\t| ((DWORD)1 << 21)\n"
        "\t\t| ((DWORD)1 << 16)\n"
        "\t\t| ((DWORD)0 << 11)\n"
        "\t\t| ((DWORD)0 << 5)\n"
        "\t\t| ((DWORD)0 >> 1);\n"
        "}\n\n"
        "DWORD get_fattime (void)\n"
        "{\n"
        "\treturn appletini_fatfs_get_fattime();\n"
        "}\n"
    )

    if old not in text:
        raise RuntimeError(f"Could not find generated get_fattime() in {path}")

    path.write_text(text.replace(old, new, 1))
    print(f"  Patched {path}", flush=True)


def patch_fatfs_no_card_hang(domain_bsp: Path):
    """Bound card detection in the generated FatFs diskio.c.

    disk_status() must report STA_NODISK immediately because frontend callers
    already retry attachment on their own schedule. disk_initialize() gets a
    one-second stable-card timeout so a marginal detect signal cannot block the
    main loop.
    """
    path = require_path("Find FatFs diskio.c",
                        domain_bsp / "libsrc" / "xilffs" / "src" / "diskio.c")
    text = path.read_text()
    if "Appletini no-card fail-fast" in text:
        return

    replacements = [
        # disk_status(): remove the insertion-poll counter.
        (
            "\tu32 StatusReg;\n"
            "\tu32 DelayCount = 0;\n",

            "\tu32 StatusReg;\n",
        ),
        # disk_status(): missing card -> STA_NODISK immediately.
        (
            "\t\t\tif (CardDetect[pdrv]) {\n"
            "\t\t\t\twhile ((StatusReg & XSDPS_PSR_CARD_INSRT_MASK) == 0U) {\n"
            "\t\t\t\t\tif (DelayCount == 500U) {\n"
            "\t\t\t\t\t\ts = STA_NODISK | STA_NOINIT;\n"
            "\t\t\t\t\t\tgoto Label;\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\telse {\n"
            "\t\t\t\t\t\t/* Wait for 10 msec */\n"
            "\t\t\t\t\t\tusleep(SD_CD_DELAY);\n"
            "\t\t\t\t\t\tDelayCount++;\n"
            "\t\t\t\t\t\tStatusReg = XSdPs_GetPresentStatusReg(BaseAddress[pdrv]);\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t}\n"
            "\t\t\t}\n",

            "\t\t\tif (CardDetect[pdrv]) {\n"
            "\t\t\t\t/* Appletini no-card fail-fast: a missing card\n"
            "\t\t\t\t * must report STA_NODISK immediately, not after\n"
            "\t\t\t\t * a 5 s insertion poll that stalls every caller\n"
            "\t\t\t\t * on a cardless boot. */\n"
            "\t\t\t\tif ((StatusReg & XSDPS_PSR_CARD_INSRT_MASK) == 0U) {\n"
            "\t\t\t\t\ts = STA_NODISK | STA_NOINIT;\n"
            "\t\t\t\t\tgoto Label;\n"
            "\t\t\t\t}\n"
            "\t\t\t}\n",
        ),
        # disk_initialize(): bound the unbounded card-stable wait.
        (
            "\t\tif (CardDetect[pdrv]) {\n"
            "\t\t\t/*\n"
            "\t\t\t * Card detection check\n"
            "\t\t\t * If the HC detects the No Card State, power will be cleared\n"
            "\t\t\t */\n"
            "\t\t\twhile (!((XSDPS_PSR_CARD_DPL_MASK |\n"
            "\t\t\t\t  XSDPS_PSR_CARD_STABLE_MASK |\n"
            "\t\t\t\t  XSDPS_PSR_CARD_INSRT_MASK) ==\n"
            "\t\t\t\t (XSdPs_GetPresentStatusReg(BaseAddress[pdrv]) &\n"
            "\t\t\t\t  (XSDPS_PSR_CARD_DPL_MASK |\n"
            "\t\t\t\t   XSDPS_PSR_CARD_STABLE_MASK |\n"
            "\t\t\t\t   XSDPS_PSR_CARD_INSRT_MASK))));\n"
            "\t\t}\n",

            "\t\tif (CardDetect[pdrv]) {\n"
            "\t\t\t/*\n"
            "\t\t\t * Card detection check\n"
            "\t\t\t * If the HC detects the No Card State, power will be cleared\n"
            "\t\t\t * Appletini: bounded wait (max 1 s) instead of the\n"
            "\t\t\t * stock unbounded loop so a missing or marginal card\n"
            "\t\t\t * cannot hang the firmware; report no-disk on timeout.\n"
            "\t\t\t */\n"
            "\t\t\tu32 StableCount = 0U;\n"
            "\t\t\twhile (!((XSDPS_PSR_CARD_DPL_MASK |\n"
            "\t\t\t\t  XSDPS_PSR_CARD_STABLE_MASK |\n"
            "\t\t\t\t  XSDPS_PSR_CARD_INSRT_MASK) ==\n"
            "\t\t\t\t (XSdPs_GetPresentStatusReg(BaseAddress[pdrv]) &\n"
            "\t\t\t\t  (XSDPS_PSR_CARD_DPL_MASK |\n"
            "\t\t\t\t   XSDPS_PSR_CARD_STABLE_MASK |\n"
            "\t\t\t\t   XSDPS_PSR_CARD_INSRT_MASK)))) {\n"
            "\t\t\t\tif (StableCount == 100U) {\n"
            "\t\t\t\t\ts |= STA_NODISK | STA_NOINIT;\n"
            "\t\t\t\t\treturn s;\n"
            "\t\t\t\t}\n"
            "\t\t\t\tusleep(SD_CD_DELAY);\n"
            "\t\t\t\tStableCount++;\n"
            "\t\t\t}\n"
            "\t\t}\n",
        ),
    ]

    for old, new in replacements:
        if old not in text:
            raise RuntimeError(
                f"Could not find expected no-card hang code in {path}:\n{old!r}")
        text = text.replace(old, new, 1)

    path.write_text(text)
    print(f"  Patched {path}", flush=True)


def relocate_lscript_to_upper_ddr(component_name: str,
                                   origin: str = "0x20000000",
                                   length: str = "0x10000000"):
    """Edit the Vitis-generated lscript.ld so the component lives in
    upper PS DDR. Used for the AMP core-1 firmware: core 0 owns
    0x100000-0x1FFFFFFF (lower 511 MB minus FSBL/OCM); core 1 lives
    at 0x20000000-0x2FFFFFFF (upper 256 MB) so the two images do not
    overlap. The RAM32 SmartPort RAM disk reserves
    0x30000000-0x31FFFFFF. The Apple FB slots / cycle ring at
    0x3E000000-0x3F800000 sit above both code regions and are
    accessed by both cores. """
    lscript = Path("vitis_workspace") / component_name / "src" / "lscript.ld"
    text = require_path(f"Find lscript.ld for {component_name}", lscript).read_text()

    import re
    # Match: ps7_ddr_0_memory_0 : ORIGIN = 0xNNN, LENGTH = 0xNNN
    pattern = re.compile(
        r"(ps7_ddr_0_memory_0\s*:\s*ORIGIN\s*=\s*)0x[0-9A-Fa-f]+(\s*,\s*LENGTH\s*=\s*)0x[0-9A-Fa-f]+",
    )
    new_text, count = pattern.subn(rf"\g<1>{origin}\g<2>{length}", text)
    if count == 0:
        raise RuntimeError(
            f"Could not find ps7_ddr_0_memory_0 ORIGIN/LENGTH in {lscript}")
    if new_text != text:
        lscript.write_text(new_text)
        print(f"  Relocated {lscript} ps7_ddr_0_memory_0 -> {origin}/{length}",
              flush=True)


def patch_usbh_class_info_section(component_name: str):
    """Keep CherryUSB's CLASS_INFO_DEFINE records in the frontend ELF.

    CherryUSB's GCC port expects linker-provided
    __usbh_class_info_start__/__usbh_class_info_end__ symbols around the
    .usbh_class_info input sections. Vitis does not generate that section by
    default, so add it after .rodata where const class records belong.
    """
    import re
    lscript = Path("vitis_workspace") / component_name / "src" / "lscript.ld"
    text = require_path(f"Find lscript.ld for {component_name}", lscript).read_text()

    if "__usbh_class_info_start__" in text:
        return

    section = (
        "\n.usbh_class_info : {\n"
        "   . = ALIGN(4);\n"
        "   __usbh_class_info_start__ = .;\n"
        "   KEEP(*(.usbh_class_info))\n"
        "   __usbh_class_info_end__ = .;\n"
        "} > ps7_ddr_0_memory_0\n"
    )

    pattern = re.compile(
        r"(\.rodata\s*:\s*\{.*?\}\s*>\s*ps7_ddr_0_memory_0\s*)",
        re.DOTALL,
    )
    new_text, count = pattern.subn(r"\1" + section, text, count=1)
    if count == 0:
        raise RuntimeError(f"Could not find .rodata section in {lscript}")

    lscript.write_text(new_text)
    print(f"  Patched {lscript} with CherryUSB class-info section", flush=True)


def fix_launch_bitstream(app_name: str):
    """Point the Vitis launch configuration at the impl_1 bitstream output."""
    launch_path = Path("vitis_workspace") / app_name / "_ide" / "launch.json"
    if not launch_path.exists():
        print(f"  {launch_path} not found, skipping.", flush=True)
        return

    data = json.loads(launch_path.read_text(encoding="utf-8"))
    correct_bitstream = r"${workspaceFolder}\..\project\appletini_yarz.runs\impl_1\appletini_yarz_top.bit"
    changed = False

    for cfg in data.get("configurations", []):
        target_setup = cfg.get("targetSetup", {})
        if "bitstreamFile" in target_setup and target_setup["bitstreamFile"] != correct_bitstream:
            target_setup["bitstreamFile"] = correct_bitstream
            changed = True

    if changed:
        launch_path.write_text(json.dumps(data, indent="\t") + "\n", encoding="utf-8")
        print(f"  Patched {launch_path}", flush=True)
    else:
        print(f"  {launch_path} already correct.", flush=True)


import time


def del_rw(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)


def remove_existing_workspace():
    """Remove vitis_workspace/ and verify the deletion.

    Vitis processes (vitis-ide.exe, java.exe) sometimes hold file
    handles inside the workspace even after taskkill returns. shutil's
    rmtree silently ignores per-file errors via onerror=del_rw and
    leaves a partial directory behind, which then makes
    create_platform_component fail with a misleading INVALID_ARGUMENT
    'invalid project location'. Retry a few times with a small delay
    between attempts, then raise loud if the directory still has
    contents. """
    workspace_path = Path("vitis_workspace")
    if not workspace_path.exists():
        print("vitis_workspace does not exist; continuing.", flush=True)
        return

    last_err = None
    for attempt in range(5):
        try:
            shutil.rmtree(workspace_path, onerror=del_rw)
        except Exception as exc:
            last_err = exc
        if not workspace_path.exists():
            return
        # Some children survived (file handles still held). Wait for
        # the OS to release them and try again.
        print(f"  workspace cleanup attempt {attempt + 1} left residue; "
              f"retrying after 1s", flush=True)
        time.sleep(1.0)

    residue = sorted(p.name for p in workspace_path.iterdir())
    raise RuntimeError(
        f"Could not fully remove {workspace_path}; "
        f"residue: {residue}. Close Vitis and kill any java.exe / "
        f"vitis-ide.exe / vitis-server.exe processes manually, then "
        f"re-run." +
        (f" Last rmtree error: {last_err}" if last_err else "")
    )


# kill any existing vitis server. Windows taskkill on the full process
# tree, then give the OS a moment to actually release file handles
# before we try to rmtree the workspace.
run_step("Kill existing Vitis IDE",
         lambda: subprocess.run(["C:\\WINDOWS\\system32\\taskkill.exe", "/F", "/T",
                                 "/IM", "vitis-ide.exe"]))
run_step("Kill existing Vitis server (vitis-server.exe)",
         lambda: subprocess.run(["C:\\WINDOWS\\system32\\taskkill.exe", "/F", "/T",
                                 "/IM", "vitis-server.exe"]))
run_step("Kill stray java.exe (Vitis Java runtime)",
         lambda: subprocess.run(["C:\\WINDOWS\\system32\\taskkill.exe", "/F", "/T",
                                 "/IM", "java.exe"]))
time.sleep(1.0)


# nuke existing workspace directory
run_step("Remove existing vitis_workspace", remove_existing_workspace)

client = run_step("Create Vitis client", lambda: vitis.create_client())
run_step("Set workspace to vitis_workspace", lambda: client.set_workspace(path="vitis_workspace"))

# Create platform component (it includes FSBL)
platform = run_step(
    "Create platform component appletini_platform",
    lambda: client.create_platform_component(
        name="appletini_platform",
        hw_design="$COMPONENT_LOCATION/../../project/appletini_yarz_top.xsa",
        os="standalone",
        cpu="ps7_cortexa9_0",
        domain_name="standalone_ps7_cortexa9_0",
        compiler="gcc",
    ),
)
# Add the Xilinx libraries needed for the bootloader (and maybe the frontend)
domain = run_step(
    "Get standalone_ps7_cortexa9_0 domain",
    lambda: platform.get_domain(name="standalone_ps7_cortexa9_0"),
)

def _get_libsw_path():
    xilinx_vitis = os.environ.get("XILINX_VITIS")
    if not xilinx_vitis:
        raise EnvironmentError("XILINX_VITIS is not set")
    return require_path("Find embeddedsw sw_services library path", Path(xilinx_vitis) / "data" / "embeddedsw" / "lib" / "sw_services")

libsw_path = run_step("Resolve XILINX_VITIS sw_services path", _get_libsw_path)
run_step(
    "Verify required BSP library directories exist",
    lambda: [
        require_path("xilffs path", libsw_path / "xilffs_v5_5"),
        require_path("xilflash path", libsw_path / "xilflash_v4_12"),
        require_path("xilrsa path", libsw_path / "xilrsa_v1_8"),
    ],
)
run_step("Add BSP library xilffs", lambda: domain.set_lib(lib_name="xilffs", path=str(libsw_path / "xilffs_v5_5")))
run_step("Add BSP library xilflash", lambda: domain.set_lib(lib_name="xilflash", path=str(libsw_path / "xilflash_v4_12")))
run_step("Add BSP library xilrsa", lambda: domain.set_lib(lib_name="xilrsa", path=str(libsw_path / "xilrsa_v1_8")))
run_step("Regenerate standalone_ps7_cortexa9_0 BSP", lambda: domain.regenerate())
run_step(
    "Enable frontend FatFs long filenames",
    lambda: enable_fatfs_lfn(
        Path("vitis_workspace")
        / "appletini_platform"
        / "ps7_cortexa9_0"
        / "standalone_ps7_cortexa9_0"
        / "bsp"
    ),
)
run_step(
    "Enable frontend FatFs timestamp hook",
    lambda: enable_fatfs_timestamp_hook(
        Path("vitis_workspace")
        / "appletini_platform"
        / "ps7_cortexa9_0"
        / "standalone_ps7_cortexa9_0"
        / "bsp"
    ),
)
run_step(
    "Patch FatFs no-SD-card boot hang",
    lambda: patch_fatfs_no_card_hang(
        Path("vitis_workspace")
        / "appletini_platform"
        / "ps7_cortexa9_0"
        / "standalone_ps7_cortexa9_0"
        / "bsp"
    ),
)

# AMP: add a standalone domain on the second Cortex-A9 core. The core-1
# firmware (apple_cycle_egress + renderer) gets exclusive use of core 1
# so it can keep up with the ~1 MHz Apple bus record stream without
# fighting the compositor's 1080p draw on core 0.
#
# USE_AMP=1 tells the standalone BSP this image will not initialize
# shared resources (L2 cache, SCU) that core 0 already brought up.
# Without this, core 1's startup races core 0's and locks up.
#
# stdout = uart1 keeps core 1's printf output off the boot UART so
# the two cores don't interleave bytes mid-line.
domain_core1 = run_step(
    "Add standalone_ps7_cortexa9_1 domain (AMP)",
    lambda: platform.add_domain(
        cpu="ps7_cortexa9_1",
        os="standalone",
        name="standalone_ps7_cortexa9_1",
        display_name="standalone_ps7_cortexa9_1",
        compiler="gcc",
    ),
)
run_step(
    "Set USE_AMP=1 on core 1 domain",
    lambda: domain_core1.set_config(
        option="proc",
        param="proc_extra_compiler_flags",
        value="-DUSE_AMP=1",
    ),
)
run_step(
    "Set core 1 stdout = uart1",
    lambda: domain_core1.set_config(
        option="os",
        param="standalone_stdout",
        value="ps7_uart_1",
    ),
)
run_step(
    "Regenerate core 1 BSP",
    lambda: domain_core1.regenerate(),
)

run_step("Build platform", lambda: platform.build())

# Create application components

# We always import the source files from the original location in the repository 
# and skip copying them to the workspace. This way we can edit the source files in-place 
# and build them directly from the workspace without a git issue. The workspace is out of git.

# We also always compile the whole lib directory for both components, even if not all files are needed for each component.
# This is because the lib directory contains common utility functions that are used by both components
# and it is easier to just compile all of them. The `import_files()` method modifies UserConfig.cmake
# in the component's src directory to add the source files to the build.

# NOTE: The default UserConfig.cmake generated by Vitis is a DEBUG version.

platform_xpfm = "$COMPONENT_LOCATION/../appletini_platform/export/appletini_platform/appletini_platform.xpfm"

def create_app_only(name, src_subdir, domain="standalone_ps7_cortexa9_0"):
    """Create the Vitis app component and import sources, but do NOT
    build yet. Used for the frontend app where we need to drop the
    generated core1_blob files into src/ between create and build. """
    comp = run_step(
        f"Create app component {name}",
        lambda: client.create_app_component(
            name=name,
            platform=platform_xpfm,
            domain=domain,
            template="empty_application",
        ),
    )
    run_step(
        f"Import sources for {name}",
        lambda: comp.import_files(
            from_loc="",
            files=["ps_sources/lib", src_subdir],
            is_skip_copy_sources=True,
        ),
    )
    return comp


def create_and_build_app(name, src_subdir, domain="standalone_ps7_cortexa9_0"):
    """Convenience helper for the simple apps (bootloader, frontend_core1).
    The frontend app is handled out-of-line because its build has to
    sandwich the core1-blob generation between create and build."""
    comp = create_app_only(name, src_subdir, domain)
    # Default Vitis-emitted UserConfig is -O0; everything in this
    # project benefits from -O2, and on CPU1 the renderer dispatch
    # absolutely needs it to keep up with the Apple bus rate.
    opt_level = "-O3" if name == "frontend_core1" else "-O2"
    run_step(
        f"Set {name} optimization to {opt_level}",
        lambda: set_userconfig_optimization(name, opt_level),
    )
    # These sources are only valid/useful in the frontend app: amp.c
    # references core1_blob_start/end emitted by core0, and lodepng.c
    # exists only for the frontend bezel loader.
    if name != "frontend":
        run_step(
            f"Remove frontend-only sources from {name}",
            lambda: ensure_userconfig_sources_absent(
                name,
                [
                    "../../../ps_sources/lib/amp.c",
                    "../../../ps_sources/lib/lodepng.c",
                ],
            ),
        )
    if name == "frontend_core1":
        # AMP core-1 firmware lives in upper DDR so it does not
        # overlap the much larger core-0 frontend image.
        run_step(
            "Relocate frontend_core1 lscript to upper DDR",
            lambda: relocate_lscript_to_upper_ddr("frontend_core1"),
        )
        # Pull in the renderer / egress / NTSC core source files
        # from ps_sources/frontend that core 1 needs. import_files
        # only auto-grabs ps_sources/frontend_core1 and
        # ps_sources/lib; these are the cross-app shared sources
        # that are shared by both apps.
        run_step(
            "Register frontend_core1 cross-app sources",
            lambda: ensure_userconfig_sources(
                "frontend_core1",
                [
                    "../../../ps_sources/frontend/apple_cycle_egress.c",
                    "../../../ps_sources/frontend/apple_cycle_renderer.c",
                    "../../../ps_sources/frontend/apple_fb_handoff.c",
                    "../../../ps_sources/frontend/apple_pal_video_timing.c",
                    "../../../ps_sources/frontend/apple2e_video_rom_data.c",
                    "../../../ps_sources/frontend/appletini_csbits.c",
                    "../../../ps_sources/frontend/appletini_ntsc.c",
                    "../../../ps_sources/frontend/compositor_layout.c",
                ],
            ),
        )
        # Like the frontend, the renderer's chroma init uses cos/sin
        # via libm.
        run_step(
            "Link frontend_core1 against libm",
            lambda: ensure_userconfig_library("frontend_core1", "m"),
        )
    run_step(f"Build app {name}", lambda: comp.build())
    return comp


create_and_build_app("bootloader", "ps_sources/bootloader")

# Build core 1 before core 0 so its binary can be embedded in core 0 as
# .rodata. At boot, core 0 copies it to 0x20000000, flushes the destination,
# and wakes CPU1 through the Zynq CPU1 boot-vector slot.
create_and_build_app(
    "frontend_core1",
    "ps_sources/frontend_core1",
    domain="standalone_ps7_cortexa9_1",
)


def generate_core1_blob_and_stub():
    """Convert frontend_core1.elf to a raw .bin and emit a tiny .S
    stub that .incbin's it. Both files land in the frontend component's
    generated source directory before that component is built."""
    xilinx_vitis = os.environ.get("XILINX_VITIS")
    if not xilinx_vitis:
        raise EnvironmentError("XILINX_VITIS is not set")
    objcopy = (Path(xilinx_vitis) / "gnu" / "aarch32" / "nt"
               / "gcc-arm-none-eabi" / "bin" / "arm-none-eabi-objcopy.exe")
    require_path("Find arm-none-eabi-objcopy", objcopy)

    elf = Path("vitis_workspace") / "frontend_core1" / "build" / "frontend_core1.elf"
    require_path("Find frontend_core1.elf (build it first)", elf)

    out_dir = Path("vitis_workspace") / "frontend" / "src"
    require_path("Frontend src dir (created by create_app_component)", out_dir)
    bin_path = out_dir / "core1_blob.bin"
    s_path   = out_dir / "core1_blob.S"

    subprocess.run(
        [str(objcopy), "-O", "binary", str(elf), str(bin_path)],
        check=True,
    )
    print(f"  objcopy -> {bin_path} ({bin_path.stat().st_size} bytes)",
          flush=True)

    # The .S lives in the same directory as the .bin so the bare
    # filename in .incbin resolves correctly: GAS searches the .S
    # file's directory by default for .incbin.
    s_path.write_text(
        "/* Generated by create_vitis_workspace.py -- core 1 ELF\n"
        " * embedded as raw binary. Loaded by core 0 at boot via\n"
        " * amp_release_core1() in lib/amp.c. */\n"
        "    .section .rodata.core1_blob, \"a\", %progbits\n"
        "    .global core1_blob_start\n"
        "    .global core1_blob_end\n"
        "    .balign 4\n"
        "core1_blob_start:\n"
        "    .incbin \"core1_blob.bin\"\n"
        "core1_blob_end:\n"
        "\n"
        "    .section .rodata.core1_blob_size, \"a\", %progbits\n"
        "    .global core1_blob_size\n"
        "    .balign 4\n"
        "core1_blob_size:\n"
        "    .word core1_blob_end - core1_blob_start\n"
    )
    print(f"  wrote {s_path}", flush=True)


# Frontend create + import (no build yet); blob generation; then build.
run_step("Generate default bezel PNG source",
         generate_default_bezel_png_sources)
run_step("Generate config menu logo PNG source",
         generate_config_menu_logo_png_sources)
run_step("Generate Disk II sound sample source",
         generate_disk2_sound_sample_sources)
frontend_comp = create_app_only("frontend", "ps_sources/frontend")
run_step(
    "Set frontend optimization to -O2",
    lambda: set_userconfig_optimization("frontend", "-O2"),
)
run_step(
    "Enable NEON on frontend (compositor blit uses vld1q/vzipq)",
    lambda: set_userconfig_mfpu("frontend", "-mfpu=neon"),
)
run_step(
    "Align frontend functions for icache",
    lambda: set_userconfig_optimization_other_flag("frontend", "-falign-functions=32"),
)
run_step(
    "Set frontend heap for bezel PNG decode",
    lambda: ensure_userconfig_flag("frontend", "-Wl,--defsym,_HEAP_SIZE=0x04000000"),
)
run_step(
    "Link frontend against libm (apple_cycle_renderer chroma init uses cos/sin)",
    lambda: ensure_userconfig_library("frontend", "m"),
)
run_step(
    "Register frontend include directories",
    lambda: ensure_userconfig_include_dirs(
        "frontend",
        [
            "../../../ps_sources/frontend",
            "../../../third_party/z80emu",
            "../../../third_party/CherryUSB/common",
            "../../../third_party/CherryUSB/core",
            "../../../third_party/CherryUSB/class/hub",
            "../../../third_party/CherryUSB/class/hid",
            "../../../third_party/CherryUSB/port/ehci",
        ],
    ),
)
run_step(
    "Patch frontend linker script for CherryUSB classes",
    lambda: patch_usbh_class_info_section("frontend"),
)
# core1_blob.S is generated below by generate_core1_blob_and_stub
# and lives in vitis_workspace/frontend/src/, so the path in
# UserConfig.cmake is just the bare filename.
run_step(
    "Register frontend sources",
    lambda: ensure_userconfig_sources(
        "frontend",
        [
            "../../../ps_sources/frontend/apple_fb_handoff.c",
            "../../../ps_sources/frontend/applicard_rom.c",
            "../../../ps_sources/frontend/applicard_service.c",
            "../../../ps_sources/frontend/applicard_z80.c",
            "../../../ps_sources/frontend/boot_menu_service.c",
            "../../../ps_sources/frontend/bezel_default_png.c",
            "../../../ps_sources/frontend/bezel_loader.c",
            "../../../ps_sources/frontend/config_menu.c",
            "../../../ps_sources/frontend/config_menu_help.c",
            "../../../ps_sources/frontend/config_menu_device_tabs.c",
            "../../../ps_sources/frontend/config_menu_main_tabs.c",
            "../../../ps_sources/frontend/config_menu_phasor.c",
            "../../../ps_sources/frontend/config_menu_profiles.c",
            "../../../ps_sources/frontend/config_menu_logo_png.c",
            "../../../ps_sources/frontend/config_menu_ui.c",
            "../../../ps_sources/frontend/debug_overlay.c",
            "../../../ps_sources/frontend/disk2_service.c",
            "../../../ps_sources/frontend/disk2_sound_samples.c",
            "../../../ps_sources/frontend/no_slot_clock_control.c",
            "../../../ps_sources/frontend/profile_manager.c",
            "../../../ps_sources/frontend/screenshot_service.c",
            "../../../ps_sources/frontend/smartport_service.c",
            "../../../ps_sources/frontend/uthernet2_control.c",
            "../../../ps_sources/frontend/cherryusb_baremetal_osal.c",
            "../../../ps_sources/frontend/cherryusb_usbh_hub_poll.c",
            "../../../ps_sources/frontend/cherryusb_zynq_hc.c",
            "../../../ps_sources/frontend/usb_hid_service.c",
            "../../../ps_sources/frontend/usb_phy_init.c",
            "../../../ps_sources/frontend/usb_storage_service.c",
            "../../../ps_sources/frontend/usb_storage_sd.c",
            "../../../ps_sources/frontend/usb_sdd_service.c",
            "../../../ps_sources/frontend/usb0_personality.c",
            "../../../ps_sources/frontend/xusbps_ch9.c",
            "../../../ps_sources/frontend/xusbps_ch9_storage.c",
            "../../../ps_sources/frontend/xusbps_ch9_sddvendor.c",
            "../../../ps_sources/frontend/xusbps_class_storage.c",
            "../../../ps_sources/lib/crc32.c",
            "../../../ps_sources/lib/fb_ui.c",
            "../../../ps_sources/lib/lodepng.c",
            "../../../third_party/CherryUSB/core/usbh_core.c",
            "../../../third_party/CherryUSB/class/hid/usbh_hid.c",
            "../../../third_party/CherryUSB/port/ehci/usb_hc_ehci.c",
            "../../../third_party/z80emu/z80emu.c",
            "core1_blob.S",
        ],
    ),
)
run_step("Generate core1 blob (objcopy + .S stub)",
         generate_core1_blob_and_stub)
run_step("Build app frontend", lambda: frontend_comp.build())

run_step("Fix frontend launch.json bitstream path", lambda: fix_launch_bitstream("frontend"))
run_step("Fix bootloader launch.json bitstream path", lambda: fix_launch_bitstream("bootloader"))


# NOTE: do NOT add a second downloadElf entry to launch.json.
# Vitis's JTAG flow with resetProcessor=true just downloads + resets
# CPU1 without reliably running it. The blob embedded in core 0's
# image (via lib/core1_blob.S) carries the core-1 binary, and CPU0 starts CPU1.

print("\nWorkspace creation completed successfully.", flush=True)
run_step("Shutdown Vitis client", lambda: vitis.dispose())

# kill lingering vitis server
run_step("Kill Vitis server", lambda: subprocess.run(["C:\\WINDOWS\\system32\\taskkill.exe","/F","/IM","java.exe"]))
