#!/usr/bin/env python3
"""Check that Vitis cannot continue after a failed platform export."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "create_vitis_workspace.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    source = SCRIPT.read_text(encoding="utf-8")
    compile(source, str(SCRIPT), "exec")

    build = source.index(
        'platform_build_status = run_step("Build platform"')
    retry = source.index('"Retry platform build"', build)
    status = source.index('"Verify platform build status"', retry)
    xpfm = source.index('"Verify exported platform XPFM"', status)
    apps = source.index("def create_app_only", xpfm)

    require(
        "if status != 0:" in source and
        "Vitis platform build returned status" in source,
        "numeric Vitis platform failures must raise",
    )
    require(build < retry < status < xpfm < apps,
            "bounded retry, status, and XPFM checks must precede apps")
    require(
        source.count("lambda: platform.build()") == 2,
        "the platform build must have exactly one retry",
    )
    require(
        'Path("vitis_workspace")' in source and
        '"appletini_platform.xpfm"' in source,
        "the exported XPFM must be checked by its real workspace path",
    )

    print("Vitis platform build guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
