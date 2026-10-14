#!/usr/bin/env python3
"""Build Appletini multiboot ARM apps against an explicitly selected platform.

Run with Vitis 2025.2: vitis -s scripts/build_multiboot_arm.py
Required environment: APP_MULTIBOOT_PLATFORM (exported .xpfm),
APP_MULTIBOOT_BASELINE (audited firmware directory). Optional environment:
APP_MULTIBOOT_WORKSPACE, APP_MULTIBOOT_OUTPUT, APP_MULTIBOOT_THIRD_PARTY_ROOT.
The workspace and output must be new directories below this checkout.
This script retains the audited FSBL and FPGA image. CPU1 stays unchanged by
default; APP_MULTIBOOT_REBUILD_CORE1=1 rebuilds it from the current source.
APP_MULTIBOOT_HARDWARE_ARCHIVE and APP_MULTIBOOT_HARDWARE_MANIFEST may select
an existing hardware source archive. Every member must match the baseline.
"""

import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRONTEND_STACK_BYTES = 0x00010000
FRONTEND_HEAP_BYTES = 0x04000000
CORE1_DDR_ORIGIN = 0x20000000
CORE1_DDR_LENGTH = 0x10000000
CORE1_SHARED_SOURCES = (
    "apple_cycle_egress.c", "apple_cycle_renderer.c", "apple_fb_handoff.c",
    "apple_pal_video_timing.c", "linear_text_overlay_capture.c",
    "apple2e_video_rom_data.c", "appletini_csbits.c", "appletini_ntsc.c",
    "compositor_layout.c",
)
CORE1_EXCLUDED_LIBS = {
    "amp.c", "lodepng.c", "psdma.c", "golden_self_update.c",
    "multiboot.c", "multiboot_platform.c",
}


def rebuild_core1_requested(environ=None):
    environ = os.environ if environ is None else environ
    value = environ.get("APP_MULTIBOOT_REBUILD_CORE1", "0")
    if value not in ("0", "1"):
        raise RuntimeError("APP_MULTIBOOT_REBUILD_CORE1 must be 0 or 1")
    return value == "1"


def core1_source_plan(root=ROOT):
    """Keep CPU0-only startup/update code out of the renderer core."""
    sources = [path for path in sorted((root / "ps_sources/lib").glob("*.c"))
               if path.name not in CORE1_EXCLUDED_LIBS]
    sources += sorted((root / "ps_sources/frontend_core1").glob("*.c"))
    sources += [root / "ps_sources/frontend" / name for name in CORE1_SHARED_SOURCES]
    for path in sources:
        if not path.is_file():
            raise FileNotFoundError(path)
    if not any(path.name == "main.c" for path in sources):
        raise RuntimeError("CPU1 source plan is missing its entry point")
    return {
        "domain": "standalone_ps7_cortexa9_1",
        "sources": sources,
        "include_dirs": [root / "ps_sources/frontend", root / "ps_sources/lib"],
        "optimization": "-O3", "compiler_flags": ["-mfpu=neon"],
        "link_libraries": ["m"],
        "ddr_origin": CORE1_DDR_ORIGIN, "ddr_length": CORE1_DDR_LENGTH,
    }


def configure_core1(config, linker, plan):
    set_config(config, "USER_COMPILE_SOURCES", [path.as_posix() for path in plan["sources"]])
    set_config(config, "USER_INCLUDE_DIRECTORIES", [path.as_posix() for path in plan["include_dirs"]])
    set_config(config, "USER_COMPILE_OPTIMIZATION_LEVEL", [plan["optimization"]])
    set_config(config, "USER_COMPILE_OTHER_FLAGS", plan["compiler_flags"])
    set_config(config, "USER_LINK_LIBRARIES", plan["link_libraries"])
    text, count = re.subn(
        r"(ps7_ddr_0_memory_0\s*:\s*ORIGIN\s*=\s*)0x[0-9A-Fa-f]+"
        r"(\s*,\s*LENGTH\s*=\s*)0x[0-9A-Fa-f]+",
        lambda match: f"{match.group(1)}0x{plan['ddr_origin']:08X}"
                      f"{match.group(2)}0x{plan['ddr_length']:08X}",
        linker.read_text(),
    )
    if count != 1:
        raise RuntimeError(f"expected one CPU1 DDR region in {linker}, found {count}")
    linker.write_text(text)


def audit_core1_layout(elf):
    """Check the linked image, rather than only the linker request."""
    data = Path(elf).read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise RuntimeError("CPU1 must be a 32-bit little-endian ELF")
    machine = struct.unpack_from("<H", data, 18)[0]
    entry, phoff = struct.unpack_from("<II", data, 24)
    phsize, phcount = struct.unpack_from("<HH", data, 42)
    limit = CORE1_DDR_ORIGIN + CORE1_DDR_LENGTH
    if machine != 40 or not CORE1_DDR_ORIGIN <= entry < limit or phsize < 32:
        raise RuntimeError("CPU1 ELF entry or machine does not match the AMP layout")
    loads = []
    for index in range(phcount):
        offset = phoff + index * phsize
        if offset + 32 > len(data):
            raise RuntimeError("CPU1 ELF program headers are truncated")
        kind, _, vaddr, paddr, _, memsz, _, _ = struct.unpack_from("<8I", data, offset)
        if kind == 1 and memsz:
            if not (CORE1_DDR_ORIGIN <= paddr < limit and paddr + memsz <= limit
                    and paddr == vaddr):
                raise RuntimeError("CPU1 load segment falls outside its upper DDR region")
            loads.append({"address": paddr, "memory_bytes": memsz})
    if not loads or min(item["address"] for item in loads) != CORE1_DDR_ORIGIN:
        raise RuntimeError("CPU1 raw load base must match amp.c's 0x20000000 destination")
    return {"entry": entry, "ddr_origin": CORE1_DDR_ORIGIN,
            "ddr_length": CORE1_DDR_LENGTH, "load_segments": loads}


def audit_frontend_memory(symbols):
    """Check the linked reservation, independently of the requested flags."""
    names = ("_STACK_SIZE", "_stack", "_stack_end", "_HEAP_SIZE", "_heap_start", "_heap_end")
    addresses = {}
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in names:
            addresses[fields[2]] = int(fields[0], 16)
    missing = sorted(set(names) - addresses.keys())
    if missing:
        raise RuntimeError(f"missing linked frontend memory symbols: {missing}")
    stack_span = addresses["_stack"] - addresses["_stack_end"]
    heap_span = addresses["_heap_end"] - addresses["_heap_start"]
    if stack_span != addresses["_STACK_SIZE"] or stack_span < FRONTEND_STACK_BYTES:
        raise RuntimeError(f"frontend stack must reserve at least {FRONTEND_STACK_BYTES} bytes: {addresses}")
    if heap_span != addresses["_HEAP_SIZE"] or heap_span != FRONTEND_HEAP_BYTES:
        raise RuntimeError(f"frontend heap must retain {FRONTEND_HEAP_BYTES} bytes: {addresses}")
    return {"symbols": addresses, "stack_bytes": stack_span,
            "minimum_stack_bytes": FRONTEND_STACK_BYTES, "heap_bytes": heap_span,
            "stack_span_matches": True, "heap_span_matches": True}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def file_record(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def required_env(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must name an explicit existing input")
    path = Path(value).resolve()
    if not path.exists():
        raise FileNotFoundError(path)
    return path


def new_directory(path):
    path = path.resolve()
    if not path.is_relative_to(ROOT) or path == ROOT:
        raise RuntimeError(f"generated output must stay below this checkout: {path}")
    if path.exists():
        raise RuntimeError(f"refusing to reuse or remove existing directory: {path}")
    path.mkdir(parents=True)
    return path


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def set_config(path, name, values):
    text = path.read_text()
    block = f"set({name}\n" + "\n".join(f'"{value}"' for value in values) + "\n)"
    text, count = re.subn(r"set\(" + name + r"\b[^)]*\)", lambda _: block, text, count=1)
    if count != 1:
        raise RuntimeError(f"missing CMake setting {name}: {path}")
    path.write_text(text)


def keep_usb_classes(path):
    text = path.read_text()
    section = """
.usbh_class_info : {
   . = ALIGN(4);
   __usbh_class_info_start__ = .;
   KEEP(*(.usbh_class_info))
   __usbh_class_info_end__ = .;
} > ps7_ddr_0_memory_0
"""
    text, count = re.subn(r"(\.rodata\s*:\s*\{.*?\}\s*>\s*ps7_ddr_0_memory_0\s*)",
                          lambda match: match.group(1) + section, text,
                          count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"missing .rodata output section: {path}")
    path.write_text(text)


SOURCE_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".s", ".asm", ".inc", ".ld"}
HARDWARE_SUFFIXES = SOURCE_SUFFIXES | {".sv", ".svh", ".v", ".vh", ".vhd", ".vhdl",
    ".xdc", ".mem", ".hex", ".coe", ".mif", ".tcl", ".py", ".bat", ".ps1",
    ".md", ".txt", ".json", ".yaml", ".yml", ".csv"}


def legal_or_readme(path):
    return path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "AUTHORS", "CREDITS", "README"))


def selected_hardware_source(name):
    path = Path(name)
    return ((name.startswith(("hdl/", "scripts/")) and
             (path.suffix.lower() in HARDWARE_SUFFIXES or legal_or_readme(path))) or
            (name.startswith("third_party/") and
             (path.suffix.lower() in SOURCE_SUFFIXES or legal_or_readme(path))))


def hardware_archive_inputs(environ=None):
    environ = os.environ if environ is None else environ
    archive = environ.get("APP_MULTIBOOT_HARDWARE_ARCHIVE")
    manifest = environ.get("APP_MULTIBOOT_HARDWARE_MANIFEST")
    if bool(archive) != bool(manifest):
        raise RuntimeError("select both APP_MULTIBOOT_HARDWARE_ARCHIVE and APP_MULTIBOOT_HARDWARE_MANIFEST")
    if not archive:
        return None
    paths = (Path(archive).resolve(), Path(manifest).resolve())
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(path)
    return paths


def validate_hardware_archive(archive_path, manifest_path, retained_hashes, license_path):
    """Validate the complete historical source set without changing it."""
    expected = {name: value for name, value in retained_hashes.items()
                if selected_hardware_source(name)}
    expected["LICENSE"] = retained_hashes.get("LICENSE", digest(license_path))
    records = json.loads(Path(manifest_path).read_text())["files"]
    if set(records) != set(expected):
        raise RuntimeError("hardware archive manifest does not contain the complete baseline source set")
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != set(expected):
            raise RuntimeError("hardware archive members do not match the baseline source set")
        for name, expected_hash in expected.items():
            member = archive.read(name)
            actual = hashlib.sha256(member).hexdigest()
            record = records[name]
            if actual != expected_hash or actual != record["sha256"] or len(member) != record["bytes"]:
                raise RuntimeError(f"retained hardware archive member changed: {name}")
    return records


def validate_core1_platform(platform):
    bsp = platform.parent / "sw/standalone_ps7_cortexa9_1/bsp.yaml"
    if not bsp.is_file():
        raise RuntimeError(f"selected platform has no CPU1 AMP domain: {bsp}")
    text = bsp.read_text()
    if not re.search(r"proc_extra_compiler_flags:.*?\n\s+value:\s*['\"]?-DUSE_AMP=1(?:['\"]?\s|$)",
                     text, flags=re.S):
        raise RuntimeError("selected CPU1 BSP must enable -DUSE_AMP=1")
    return bsp


def source_files(base):
    return sorted(path for path in base.rglob("*") if path.is_file() and
                  ".git" not in path.relative_to(base).parts and
                  (path.suffix.lower() in SOURCE_SUFFIXES or legal_or_readme(path)))


def source_snapshot(third_party):
    members = {}
    for path in source_files(ROOT / "ps_sources"):
        members[f"ps_sources/{path.relative_to(ROOT / 'ps_sources').as_posix()}"] = path
    for directory in ("xtulator_cpu", "z80emu", "CherryUSB/common", "CherryUSB/core",
                      "CherryUSB/class/hid", "CherryUSB/class/hub", "CherryUSB/port/ehci"):
        for path in source_files(third_party / directory):
            members[f"third_party/{path.relative_to(third_party).as_posix()}"] = path
    for path in (third_party / "CherryUSB").iterdir():
        if path.is_file() and legal_or_readme(path):
            members[f"third_party/CherryUSB/{path.name}"] = path
    for name in ("LICENSE", "scripts/build_multiboot_arm.py",
                 "scripts/create_vitis_workspace.py", "scripts/image_manifest.py",
                 "scripts/package_timing_firmware.py", "scripts/test_multiboot_glyph_build.py",
                 "scripts/build_boot_menu_rom.py", "scripts/build_vtw_boot_start_rom.py",
                 "software/boot_menu_slot7.a65"):
        path = ROOT / name
        if path.is_file():
            members[name] = path
    return members


def archive_sources(path, members):
    records = {}
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, source in sorted(members.items()):
            data = source.read_bytes()
            records[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            archive.writestr(name, data)
    return records


def run(args, *, cwd=None):
    print("RUN:", json.dumps([str(arg) for arg in args]), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


def main():
    import vitis
    from image_manifest import append_manifest, verify_image, ROLE_FIRMWARE

    versions = (ROOT / "ps_sources/image_versions.h").read_text()
    golden_match = re.search(r'APPLETINI_BOOT_IMAGE_VERSION_SHORT\s+"([^"]+)"', versions)
    if golden_match is None:
        raise RuntimeError("missing Golden version in source header")
    golden_version = golden_match.group(1)
    firmware_match = re.search(r'APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT\s+"([^"]+)"', versions)
    if firmware_match is None:
        raise RuntimeError("missing firmware version in source header")
    firmware_version = firmware_match.group(1)

    platform = required_env("APP_MULTIBOOT_PLATFORM")
    baseline = required_env("APP_MULTIBOOT_BASELINE")
    rebuild_core1 = rebuild_core1_requested()
    core1_plan = core1_source_plan() if rebuild_core1 else None
    core1_bsp = validate_core1_platform(platform) if rebuild_core1 else None
    hardware_archive = hardware_archive_inputs()
    baseline_root = baseline.parents[1]
    retained_hashes = json.loads((baseline / "source_after.json").read_text())["source_sha256"]
    hardware_archive_records = None
    if hardware_archive:
        validate_hardware_archive(*hardware_archive, retained_hashes, baseline_root / "LICENSE")
        hardware_archive_records = [file_record(path) for path in hardware_archive]
    third_party = Path(os.environ.get("APP_MULTIBOOT_THIRD_PARTY_ROOT", ROOT / "third_party")).resolve()
    vitis_root = required_env("XILINX_VITIS")
    objcopy = vitis_root / "gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-objcopy.exe"
    nm = objcopy.with_name("arm-none-eabi-nm.exe")
    bootgen = vitis_root / "bin/bootgen.bat"
    for path in (objcopy, nm, bootgen, third_party / "CherryUSB/core/usbh_core.c"):
        if not path.is_file():
            raise FileNotFoundError(path)
    audit = json.loads((baseline / "FIRMWARE_AUDIT.json").read_text())
    software = json.loads((baseline / "software_inputs.json").read_text())
    retained = {
        "fsbl": baseline / "inputs/fsbl.elf",
        "cpu1_elf": baseline / "inputs/frontend_core1.elf",
        "cpu1_raw": baseline / "inputs/core1_blob.bin",
        "bitstream": Path(os.environ.get("APP_MULTIBOOT_BITSTREAM", audit["inputs"]["bitstream"]["path"])),
        "xsa": Path(os.environ.get("APP_MULTIBOOT_XSA", audit["inputs"]["xsa"]["path"])),
    }
    expected = {
        "fsbl": software["fsbl.elf"]["sha256"],
        "cpu1_elf": software["frontend_core1.elf"]["sha256"],
        "cpu1_raw": software["core1_blob.bin"]["sha256"],
        "bitstream": audit["inputs"]["bitstream"]["sha256"],
        "xsa": audit["inputs"]["xsa"]["sha256"],
    }
    for name, path in retained.items():
        if digest(path) != expected[name]:
            raise RuntimeError(f"audited retained input changed: {name}: {path}")
    audited_baseline_inputs = {name: file_record(path) for name, path in retained.items()}
    selected_cpu1_elf = retained["cpu1_elf"]
    selected_cpu1_raw = retained["cpu1_raw"]
    core1_layout = None
    exported_xsa = platform.parent / "hw" / retained["xsa"].name
    if digest(exported_xsa) != expected["xsa"]:
        raise RuntimeError("selected platform XSA does not match the audited FPGA input")
    expected_cherry = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD:third_party/CherryUSB"], text=True).strip()
    selected_cherry = subprocess.check_output(
        ["git", "-C", str(third_party / "CherryUSB"), "rev-parse", "HEAD"], text=True).strip()
    if selected_cherry != expected_cherry:
        raise RuntimeError("selected CherryUSB source does not match the checkout's pinned commit")
    if subprocess.check_output(["git", "-C", str(third_party / "CherryUSB"),
                                "diff", "--name-only", "HEAD"], text=True).strip():
        raise RuntimeError("selected CherryUSB checkout contains tracked source edits")

    workspace = new_directory(Path(os.environ.get("APP_MULTIBOOT_WORKSPACE", ROOT / "build/vitis-multiboot")))
    output = new_directory(Path(os.environ.get("APP_MULTIBOOT_OUTPUT", ROOT / f"build/firmware-{firmware_version}")))
    records = output / "build-records"
    inputs = output / "inputs"
    records.mkdir()
    inputs.mkdir()
    members = source_snapshot(third_party)
    before = {name: digest(path) for name, path in members.items()}
    write_json(records / "source-before.json", before)
    client = vitis.create_client()
    client.set_workspace(path=str(workspace))
    client.add_platform_repos(platform=str(platform.parent))
    compiled_sources = {}
    include_dirs = [ROOT / "ps_sources/frontend", ROOT / "ps_sources/lib"] + [third_party / path for path in (
        "xtulator_cpu", "z80emu", "CherryUSB/common", "CherryUSB/core", "CherryUSB/class/hub",
        "CherryUSB/class/hid", "CherryUSB/port/ehci")]
    component_order = ("bootloader", "frontend_core1", "frontend") if rebuild_core1 else ("bootloader", "frontend")
    for name in component_order:
        component = client.create_app_component(name=name, platform=str(platform),
                                                domain=(core1_plan["domain"] if name == "frontend_core1"
                                                        else "standalone_ps7_cortexa9_0"),
                                                template="empty_application")
        src = workspace / name / "src"
        component.import_files(from_loc="", files=[str(ROOT / "ps_sources/lib"), str(ROOT / "ps_sources" / name)],
                               is_skip_copy_sources=True)
        sources = sorted((ROOT / "ps_sources/lib").glob("*.c"))
        if name == "bootloader":
            sources = [path for path in sources if path.name not in ("amp.c", "lodepng.c", "psdma.c")]
        sources += sorted((ROOT / "ps_sources" / name).glob("*.c"))
        if name == "frontend_core1":
            sources = core1_plan["sources"]
        if name == "frontend":
            sources += [third_party / path for path in ("CherryUSB/core/usbh_core.c", "CherryUSB/class/hid/usbh_hid.c",
                        "CherryUSB/port/ehci/usb_hc_ehci.c", "xtulator_cpu/cpu.c", "z80emu/z80emu.c")]
            blob = src / "core1_blob.bin"
            run([objcopy, "-O", "binary", selected_cpu1_elf, blob])
            if digest(blob) != digest(selected_cpu1_raw):
                raise RuntimeError("selected CPU1 ELF does not produce the selected raw binary")
            stub = src / "core1_blob.S"
            stub.write_text('.section .rodata.core1_blob,"a",%progbits\n.global core1_blob_start\n.global core1_blob_end\n.balign 4\n'
                            'core1_blob_start:\n.incbin "' + blob.as_posix() + '"\ncore1_blob_end:\n'
                            '.section .rodata.core1_blob_size,"a",%progbits\n.global core1_blob_size\n.balign 4\n'
                            'core1_blob_size:\n.word core1_blob_end-core1_blob_start\n')
            sources.append(stub)
            shutil.copy2(stub, records / "frontend-core1_blob.S")
            keep_usb_classes(src / "lscript.ld")
        config = src / "UserConfig.cmake"
        set_config(config, "USER_COMPILE_SOURCES", [path.as_posix() for path in sources])
        set_config(config, "USER_INCLUDE_DIRECTORIES", [path.as_posix() for path in include_dirs])
        set_config(config, "USER_COMPILE_OPTIMIZATION_LEVEL", ["-O2"])
        if name == "frontend_core1":
            configure_core1(config, src / "lscript.ld", core1_plan)
        if name == "frontend":
            set_config(config, "USER_COMPILE_OTHER_FLAGS", ["-mfpu=neon"])
            set_config(config, "USER_COMPILE_OPTIMIZATION_OTHER_FLAGS", ["-falign-functions=32"])
            set_config(config, "USER_LINK_LIBRARIES", ["m"])
            # main keeps the menu on its stack, while multiboot verification
            # adds a 4 KiB read buffer and QSPI call frames. The 8 KiB Vitis
            # default cannot hold that call chain. Keep both linker switches
            # in one token because Vitis concatenates this setting's entries.
            set_config(config, "USER_LINK_OTHER_FLAGS",
                       [f"-Wl,--defsym,_HEAP_SIZE=0x{FRONTEND_HEAP_BYTES:08X},"
                        f"--defsym,_STACK_SIZE=0x{FRONTEND_STACK_BYTES:08X}"])
        if name == "bootloader":
            set_config(config, "USER_LINK_OTHER_FLAGS",
                       ["-Wl,--defsym,_STACK_SIZE=0x00010000,--defsym,_HEAP_SIZE=0x00010000"])
        with config.open("a") as stream:
            stream.write('\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)\n')
        compiled_sources[name] = [file_record(path) for path in sources]
        shutil.copy2(config, records / f"{name}-UserConfig.cmake")
        shutil.copy2(src / "lscript.ld", records / f"{name}-lscript.ld")

        print(f"BUILD {name}", flush=True)
        component.build()
        elf = workspace / name / "build" / f"{name}.elf"
        if not elf.is_file() or elf.stat().st_size == 0:
            raise RuntimeError(f"Vitis did not produce {elf}")
        shutil.copy2(elf, inputs / elf.name)
        compile_commands = workspace / name / "build" / "compile_commands.json"
        if not compile_commands.is_file():
            raise RuntimeError(f"missing actual compilation commands: {compile_commands}")
        shutil.copy2(compile_commands, records / f"{name}-compile_commands.json")
        if name == "frontend_core1":
            selected_cpu1_elf = inputs / "frontend_core1.elf"
            selected_cpu1_raw = inputs / "core1_blob.bin"
            core1_layout = audit_core1_layout(selected_cpu1_elf)
            run([objcopy, "-O", "binary", selected_cpu1_elf, selected_cpu1_raw])
            if selected_cpu1_raw.stat().st_size == 0:
                raise RuntimeError("rebuilt CPU1 raw image is empty")
            shutil.copy2(core1_bsp, records / "frontend_core1-bsp.yaml")
            write_json(records / "frontend_core1-layout.json", core1_layout)
    vitis.dispose()

    after = {name: digest(path) for name, path in members.items()}
    write_json(records / "source-after.json", after)
    if before != after:
        changed = [name for name in before if before[name] != after[name]]
        raise RuntimeError(f"source changed during build: {changed}")
    source_records = archive_sources(output / "cpu-sources.zip", members)
    write_json(output / "source-manifest.json", {"files": source_records})
    for name, path in retained.items():
        if digest(path) != expected[name]:
            raise RuntimeError(f"audited baseline input changed during build: {name}: {path}")
        if rebuild_core1 and name in ("cpu1_elf", "cpu1_raw"):
            continue
        target = {"fsbl": "fsbl.elf", "cpu1_elf": "frontend_core1.elf", "cpu1_raw": "core1_blob.bin",
                  "bitstream": "appletini_yarz_top.bit", "xsa": "appletini_yarz_top.xsa"}[name]
        shutil.copy2(path, inputs / target)
    shutil.copy2(platform, records / platform.name)
    symbols = subprocess.check_output([str(nm), str(inputs / "frontend.elf")], text=True)
    frontend_memory = audit_frontend_memory(symbols)
    write_json(records / "frontend-memory.json", frontend_memory)
    addresses = {}
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in ("core1_blob_start", "core1_blob_end", "core1_blob_size"):
            addresses[fields[2]] = int(fields[0], 16)
    raw = records / "frontend.raw.bin"
    run([objcopy, "-O", "binary", inputs / "frontend.elf", raw])
    contents = raw.read_bytes()
    # The Vitis CPU0 DDR entry and lowest load segment remain at 0x00100000.
    first = addresses["core1_blob_start"] - 0x00100000
    last = addresses["core1_blob_end"] - 0x00100000
    size_at = addresses["core1_blob_size"] - 0x00100000
    embedded = contents[first:last]
    if embedded != selected_cpu1_raw.read_bytes() or int.from_bytes(contents[size_at:size_at+4], "little") != len(embedded):
        raise RuntimeError("CPU0 embedded CPU1 payload or size word does not match the selected CPU1 executable")
    bif = records / "firmware.bif"
    bif.write_text('the_ROM_image:\n{\n    [bootloader] "' + (inputs / "fsbl.elf").as_posix() + '"\n    "' +
                   (inputs / "appletini_yarz_top.bit").as_posix() + '"\n    "' +
                   (inputs / "frontend.elf").as_posix() + '"\n}\n')
    firmware_raw = records / "firmware.raw.bin"
    run([bootgen, "-arch", "zynq", "-image", bif, "-o", firmware_raw, "-w", "on"])
    append_manifest(firmware_raw, output / "FIRMWARE.BIN", role=ROLE_FIRMWARE,
                    max_size=0x00600000, product="appletini")
    verify_image(output / "FIRMWARE.BIN", expected_role=ROLE_FIRMWARE,
                 expected_recovery=True, expected_product="appletini")

    # Publish the source which produced the retained FPGA image, independently
    # of the new CPU code. Require every selected file to match the d1 record.
    if hardware_archive:
        for path, record in zip(hardware_archive, hardware_archive_records):
            if digest(path) != record["sha256"]:
                raise RuntimeError(f"selected hardware source archive input changed during build: {path}")
        shutil.copy2(hardware_archive[0], output / "hardware-sources.zip")
        shutil.copy2(hardware_archive[1], output / "hardware-source-manifest.json")
        validate_hardware_archive(output / "hardware-sources.zip", output / "hardware-source-manifest.json",
                                  retained_hashes, baseline_root / "LICENSE")
    else:
        hardware_members = {}
        for name, expected_hash in retained_hashes.items():
            source = baseline_root / name
            if selected_hardware_source(name):
                if digest(source) != expected_hash:
                    raise RuntimeError(f"retained hardware build source changed: {name}")
                hardware_members[name] = source
        hardware_members["LICENSE"] = baseline_root / "LICENSE"
        hardware_records = archive_sources(output / "hardware-sources.zip", hardware_members)
        write_json(output / "hardware-source-manifest.json", {"files": hardware_records})
    retained_evidence = output / "retained-d1"
    retained_evidence.mkdir()
    for name in ("FIRMWARE_AUDIT.json", "software_inputs.json", "BUILD_REPORT.json", "TIMING_MANIFEST.txt", "source_after.json"):
        shutil.copy2(baseline / name, retained_evidence / name)

    # Use the retained independent Bootgen/ELF auditor with this checkout's
    # v2-aware manifest parser. Its explicit inputs never write to the baseline.
    import package_timing_firmware  # Cache the matching parser before loading the auditor.
    baseline_audit = baseline / "audit_firmware.py"
    shutil.copy2(baseline_audit, records / "baseline_audit_firmware.py")
    audit_script = records / "audit_firmware.py"
    audit_text = baseline_audit.read_text()
    old_check = '    need(boot_label in b"".join(s["data"] for s in front_segments), "Wrong loaded boot-version label")'
    if audit_text.count(old_check) != 1:
        raise RuntimeError("retained auditor's boot display contract has changed")
    # Preserve every retained payload/ELF/CRC check. Replace only the obsolete
    # static About label gate with the installed-image display contract.
    new_check = '\n'.join((
        '    need(boot_label not in b"".join(s["data"] for s in front_segments), "Frontend still embeds a misleading static boot label")',
        '    need(b"Golden Boot: %s\\0" in b"".join(s["data"] for s in front_segments), "Missing installed Golden display format")',
        '    need("mb_golden_installed_version" in front_symbols and "golden_self_update_run" in front_symbols, "Missing installed-version/update implementation")',
    ))
    audit_script.write_text(audit_text.replace(old_check, new_check))
    spec = importlib.util.spec_from_file_location("retained_firmware_audit", audit_script)
    auditor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(auditor)
    auditor.OUT = output
    auditor.ROOT = ROOT
    saved_argv = sys.argv
    sys.argv = [str(audit_script), "--firmware", str(output / "FIRMWARE.BIN"),
                "--fsbl", str(inputs / "fsbl.elf"), "--frontend", str(inputs / "frontend.elf"),
                "--bitstream", str(inputs / "appletini_yarz_top.bit"),
                "--xsa", str(inputs / "appletini_yarz_top.xsa"),
                "--run-manifest", str(baseline / "TIMING_MANIFEST.txt"),
                "--platform-xsa", str(exported_xsa),
                "--core1-elf", str(inputs / "frontend_core1.elf"),
                "--core1-blob", str(inputs / "core1_blob.bin"),
                "--version", firmware_version, "--boot-version", golden_version,
                "--output", str(output / "FIRMWARE_AUDIT.json")]
    try:
        auditor.main()
    finally:
        sys.argv = saved_argv

    report = {
        "built_utc": datetime.now(timezone.utc).isoformat(),
        "version": firmware_version, "golden_version": golden_version, "tool_version": "Vitis 2025.2",
        "git_commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
        "git_status": subprocess.check_output(["git", "-C", str(ROOT), "status", "--short"], text=True),
        "platform": file_record(platform), "platform_xsa": file_record(exported_xsa),
        "inputs": {path.name: file_record(path) for path in inputs.iterdir() if path.is_file()},
        "firmware": file_record(output / "FIRMWARE.BIN"), "compiled_sources": compiled_sources,
        "source_snapshot": {"before_equals_after": before == after, "files": before},
        "cpu_source_archive": file_record(output / "cpu-sources.zip"),
        "hardware_source_archive": file_record(output / "hardware-sources.zip"),
        "audited_baseline_inputs": audited_baseline_inputs,
        "hardware_source_inputs": hardware_archive_records,
        "cpu1": {"mode": "rebuilt" if rebuild_core1 else "retained",
                 "retained_baseline": None if rebuild_core1 else str(baseline),
                 "elf_sha256": digest(selected_cpu1_elf),
                 "raw_sha256": digest(selected_cpu1_raw), "embedded_sha256": hashlib.sha256(embedded).hexdigest(),
                 "embedded_bytes": len(embedded), "size_word_matches": True, "elf_raw_matches": True},
        "frontend_memory": frontend_memory,
        "golden_version_display": "installed-image",
        "pinned_cherryusb": selected_cherry, "hardware_flashed": False, "hardware_tested": False,
    }
    if rebuild_core1:
        report["cpu1"]["build_configuration"] = {
            "domain": core1_plan["domain"], "bsp": file_record(core1_bsp),
            "optimization": core1_plan["optimization"], "compiler_flags": core1_plan["compiler_flags"],
            "link_libraries": core1_plan["link_libraries"],
            "include_dirs": [str(path) for path in core1_plan["include_dirs"]],
            "layout": core1_layout,
        }
    write_json(output / "ARM_BUILD.json", report)
    files = {path.relative_to(output).as_posix(): {"bytes": path.stat().st_size, "sha256": digest(path)}
             for path in sorted(output.rglob("*")) if path.is_file()}
    write_json(output / "manifest.json", {"files": files})
    (output / "SHA256SUMS.txt").write_text("".join(f'{value["sha256"]}  {name}\n' for name, value in files.items()))
    print(f"BUILT: {output}", flush=True)


if __name__ == "__main__":
    main()
