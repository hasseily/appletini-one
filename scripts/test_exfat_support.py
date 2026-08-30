#!/usr/bin/env python3
"""Source checks for the shared exFAT BSP and 64-bit FatFs sizes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    build = read("scripts/create_vitis_workspace.py")
    updater = read("ps_sources/bootloader/updater.c")
    serial = read("ps_sources/bootloader/serial_boot.c")
    disk2 = read("ps_sources/frontend/disk2_service.c")
    ftp = read("ps_sources/frontend/ftp_sd_service.c")
    profiles = read("ps_sources/frontend/profile_manager.c")
    smartport = read("ps_sources/frontend/smartport_service.c")
    versions = read("ps_sources/image_versions.h")

    compile(build, "scripts/create_vitis_workspace.py", "exec")
    config = build.index('param="XILFFS_enable_exfat"')
    regenerate = build.index(
        'run_step("Regenerate standalone_ps7_cortexa9_0 BSP"', config
    )
    sync = build.index('"Sync generated FatFs exFAT files"', regenerate)
    patch = build.index('"Patch FatFs media validation"', sync)
    platform = build.index('platform_build_status = run_step("Build platform"', patch)
    require('value="true"' in build[config:regenerate],
            "the shared BSP must enable exFAT")
    require(config < regenerate < sync < patch < platform,
            "exFAT config and FatFs patches must precede the platform build")
    require("def enable_fatfs_exfat" in build and
            "cmake_lib_configs.txt" in build,
            "Vitis stale exported exFAT files must be synced")
    verify = build.index('"Verify shared BSP exFAT configuration"', patch)
    require(patch < verify < platform and
            "def verify_fatfs_exfat_configuration" in build,
            "the build must check Vitis generated exFAT files before compile")
    for generated_marker in (
        "#define FILE_SYSTEM_FS_EXFAT",
        "#define FILE_SYSTEM_USE_LFN 1",
        "XILFFS_enable_exfat:BOOL=ON",
        "value: 'true'",
    ):
        require(generated_marker in build,
                f"missing generated BSP check: {generated_marker}")
    for guard in (
        "#define MIN_EXFAT\\t0x00000100",
        "nclst < MIN_EXFAT || nclst > MAX_EXFAT",
        "fasize >= 0x200000U",
        "si < dj.dir[XDIR_NumLabel] && si < 11U",
    ):
        require(guard in build, f"missing FatFs media guard: {guard}")

    check = updater.index("sd_file_size > (FSIZE_t)layout->firmware.size")
    cast = updater.index("file_size = (uint32_t)sd_file_size", check)
    require("static int file_size_of(const char *path, FSIZE_t *size_out)" in updater,
            "golden updater must keep FatFs file sizes wide")
    require(check < cast,
            "golden updater must reject an oversized image before narrowing")
    require('size=%llu' in serial and
            '(unsigned long long)fi.fsize' in serial,
            "golden UART status must print the full FatFs size")

    helper = disk2.index("static int file_size_u32")
    limit = disk2.index("size > (FSIZE_t)UINT32_MAX", helper)
    helper_cast = disk2.index("*size_out = (uint32_t)size", limit)
    require(limit < helper_cast and disk2.count("file_size_u32(&file") == 3,
            "Disk II must range-check every narrowed FatFs file size")
    require("(uint32_t)f_size(" not in disk2,
            "Disk II must not narrow f_size directly")
    require("file_size > (FSIZE_t)UINT32_MAX" in smartport,
            "SmartPort must reject files above its 32-bit size limit")
    require("file_size > (FSIZE_t)SIZE_MAX" in profiles,
            "profile loads must reject files too large for size_t")
    require('"213 %llu\\r\\n"' in ftp and "%10llu" in ftp,
            "FTP must format FatFs sizes without 32-bit truncation")
    require(ftp.count("(unsigned long long)") >= 2,
            "FTP SIZE and LIST must pass a wide size value")

    require('"B1.2.0"' in versions and '"F1.0.1"' in versions,
            "both image versions must identify this exFAT build")

    print("exFAT support source checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
