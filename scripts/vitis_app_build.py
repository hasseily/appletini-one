"""Verify Vitis app builds; bypass only the known Windows link-shell failure."""
from pathlib import Path
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timezone
import hashlib
import io
import json
import os
import re
import shlex
import subprocess
import time


def validate_bsp_sources(bsp: Path):
    """Check the driver source graph that the generated BSP CMake consumes."""
    for name in ("bsp.yaml", "CMakeLists.txt", "cortexa9_toolchain.cmake", "Findcommon.cmake",
                 "Findcommonmeta.cmake", "libsrc/CMakeLists.txt", "libsrc/DRVLISTConfig.cmake"):
        if not (bsp / name).is_file():
            raise RuntimeError(f"Incomplete regenerated BSP: missing {bsp / name}")
    root_cmake = (bsp / "CMakeLists.txt").read_text()
    driver_cmake = (bsp / "libsrc/CMakeLists.txt").read_text()
    drivers = (bsp / "libsrc/DRVLISTConfig.cmake").read_text()
    subdirs = re.search(r"set\s*\(\s*BSP_LIBSRC_SUBDIRS\s+([^)]*)\)", root_cmake, re.I)
    driver_list = re.search(r"set\s*\(\s*DRIVER_LIST\s+([^)]*)\)", drivers, re.I)
    if not subdirs or not driver_list or "add_subdirectory(${DRIVER}/src)" not in driver_cmake:
        raise RuntimeError("Unrecognized generated BSP source graph")
    required = re.split(r"[;\s]+", (subdirs[1] + ";" + driver_list[1]).strip().replace('"', ''))
    for driver in set(required) - {"", "libsrc"}:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", driver):
            raise RuntimeError(f"Unrecognized generated BSP driver: {driver!r}")
        path = bsp / "libsrc" / driver.replace("-", "_") / "src/CMakeLists.txt"
        if not path.is_file():
            raise RuntimeError(f"Incomplete regenerated BSP driver: {path}")


def regenerate_bsp_checked(domain, bsp: Path):
    """The SDK can print an error and still return None; verify its output."""
    for attempt in range(2):
        captured = io.StringIO()
        failure = None
        try:
            with redirect_stdout(captured), redirect_stderr(captured):
                status = domain.regenerate()
            if status is False or (status is not None and status is not True and status != 0):
                raise RuntimeError(f"SDK regeneration returned {status!r}")
        except Exception as exc:
            failure = exc
        output = captured.getvalue()
        print(output, end="", flush=True)
        if re.search(r"Traceback|Error in regenerating|\[ERROR\]|Failed to execute script", output, re.I):
            failure = RuntimeError("SDK reported a BSP regeneration error")
        if failure is None:
            try:
                validate_bsp_sources(bsp)
            except Exception as exc:
                failure = exc
        if failure is None:
            return
        if attempt == 0:
            print(f"BSP regeneration incomplete ({failure}); retrying once after 1 second.", flush=True)
            time.sleep(1.0)
        else:
            raise RuntimeError(f"BSP regeneration failed after two attempts: {bsp}: {failure}") from failure


def workspace_receipt_paths(workspace: Path):
    return {"source_xsa": workspace.parent / "project/appletini_yarz_top.xsa",
            "platform_source_xsa": workspace / "appletini_platform/hw/appletini_yarz_top.xsa",
            "platform_export_xsa": workspace / "appletini_platform/export/appletini_platform/hw/appletini_yarz_top.xsa",
            "fsbl": workspace / "appletini_platform/export/appletini_platform/sw/boot/fsbl.elf",
            "frontend": workspace / "frontend/build/frontend.elf",
            "core1": workspace / "frontend_core1/build/frontend_core1.elf",
            "core1_blob": workspace / "frontend/src/core1_blob.bin"}


def write_workspace_receipt(workspace: Path):
    """Called only at the successful end of the normal workspace workflow."""
    paths = workspace_receipt_paths(workspace)
    entries = {name: {"path": path.resolve().relative_to(workspace.parent.resolve()).as_posix(),
                      "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for name, path in paths.items()}
    if len({entries[n]["sha256"] for n in entries if n.endswith("xsa")}) != 1:
        raise RuntimeError("Completed platform XSAs differ from the source XSA")
    receipt = {"schema_version": 1, "status": "PASS", "completed_utc": datetime.now(timezone.utc).isoformat(),
               "build_token": os.environ.get("APPLETINI_VITIS_BUILD_TOKEN", ""), "inputs": entries}
    with (workspace / "BUILD_COMPLETE.json").open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(receipt, indent=2) + "\n")


def verify_workspace_receipt(workspace: Path, expected_xsa: str, build_token: str):
    path = workspace / "BUILD_COMPLETE.json"
    if not path.is_file():
        raise RuntimeError("Vitis exited without a successful workspace completion receipt; see VITIS_BUILD.log")
    receipt = json.loads(path.read_text())
    if receipt.get("status") != "PASS" or receipt.get("schema_version") != 1 or not build_token or receipt.get("build_token") != build_token:
        raise RuntimeError("Missing or stale Vitis workspace completion receipt")
    for name, source in workspace_receipt_paths(workspace).items():
        expected = {"path": source.resolve().relative_to(workspace.parent.resolve()).as_posix(),
                    "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
        if receipt["inputs"].get(name) != expected or (name.endswith("xsa") and expected["sha256"] != expected_xsa):
            raise RuntimeError(f"Vitis completion receipt input mismatch: {name}")
    return receipt


def native_link_command(build: Path, name: str) -> list[str]:
    """Preserve generated GCC arguments; permit only the known size-only hook."""
    ninja = (build / "build.ninja").read_text()
    target = name + ".elf"
    edge = re.search(r"^build " + re.escape(target) + r": (?P<rule>\S+).*?(?=\n\n)", ninja, re.M | re.S)
    if not edge:
        raise RuntimeError(f"Missing generated link edge for {target}")
    values = dict(re.findall(r"^  (\w+) = (.*)$", edge.group(), re.M))
    rules = (build / "CMakeFiles/rules.ninja").read_text()
    rule = re.search(r"^rule " + re.escape(edge['rule']) + r"\n(.*?)(?=\n\n)", rules, re.M | re.S)
    command = re.search(r"^  command = (.*)$", rule[1], re.M)[1] if rule else ""
    match = re.fullmatch(r'cmd\.exe /C "\$PRE_LINK && (.*?)\s+\$FLAGS(.*?)\s+&& \$POST_BUILD"', command)
    if not match:
        raise RuntimeError("Unrecognized generated link wrapper; refusing to guess")
    compiler = match[1].strip('"')
    if not compiler.lower().endswith("arm-none-eabi-gcc.exe"):
        raise RuntimeError("Unexpected generated linker")
    if values.get("PRE_LINK") != "cd .":
        raise RuntimeError("Cannot bypass a nontrivial generated PRE_LINK")
    expected_post = (f'cmd.exe /C "cd /D {build.resolve()} && arm-none-eabi-size --format=berkeley {target} '
                     f'&& arm-none-eabi-size --format=berkeley {target} > {build.resolve().as_posix()}/{target}.size"')
    if values.get("POST_BUILD") != expected_post:
        raise RuntimeError("Cannot bypass an unrecognized generated POST_BUILD")
    rsp = values.get("RSP_FILE", "").replace("\\", "/")
    if not rsp or not (build / rsp).is_file():
        raise RuntimeError("The failed Ninja link did not preserve its response file")
    tail = "$FLAGS" + match[2]
    for key in ("FLAGS", "LINK_FLAGS", "RSP_FILE", "TARGET_FILE"):
        value = values.get(key, "")
        if key in ("RSP_FILE", "TARGET_FILE"):
            value = shlex.quote(rsp if key == "RSP_FILE" else value)
        tail = tail.replace("$" + key, value)
    if "$" in tail:
        raise RuntimeError("Unexpanded generated link variable")
    return [compiler, *shlex.split(tail)]


def known_link_wrapper_failure(log: str, name: str) -> bool:
    failed = [line.strip() for line in re.findall(r"^FAILED: ([^\r\n]+)", log, re.M)]
    return failed == [name + ".elf"] and "The system cannot execute the specified program." in log


def build_app_component(component, name: str, workspace: Path = Path("vitis_workspace")):
    """Let Vitis generate/build, then require a successful native build result."""
    build = workspace / name / "build"
    api = {}
    try:
        api["status"] = str(component.build())
    except Exception as exc:
        api["exception"] = f"{type(exc).__name__}: {exc}"
    if not (build / "build.ninja").is_file():
        raise RuntimeError(f"Vitis did not generate {name}: {api}")
    (build / "VITIS_API_BUILD.json").write_text(json.dumps(api, indent=2) + "\n")
    cache = (build / "CMakeCache.txt").read_text()
    cmake = re.search(r"^CMAKE_COMMAND:INTERNAL=(.+)$", cache, re.M)[1]
    env = dict(os.environ)
    tools = Path(env["XILINX_VITIS"]) / "gnu/aarch32/nt/gcc-arm-none-eabi/bin"
    env["PATH"] = str(tools) + os.pathsep + env.get("PATH", "")
    normal = subprocess.run([cmake, "--build", str(build.resolve()), "--parallel", "8"],
                            env=env, capture_output=True, text=True, errors="replace")
    log = normal.stdout + normal.stderr
    (build / "CMAKE_VERIFY.log").write_text(log, encoding="utf-8")
    print(log, end="", flush=True)
    if normal.returncode:
        if not known_link_wrapper_failure(log, name):
            raise RuntimeError(f"{name} build failed; see {build / 'CMAKE_VERIFY.log'}")
        command = native_link_command(build, name)
        (build / "DIRECT_LINK_COMMAND.json").write_text(json.dumps(command, indent=2) + "\n")
        linked = subprocess.run(command, cwd=build, env=env, capture_output=True, text=True, errors="replace")
        (build / "DIRECT_LINK.log").write_text(linked.stdout + linked.stderr, encoding="utf-8")
        if linked.returncode:
            raise RuntimeError(f"Native {name} link failed; see {build / 'DIRECT_LINK.log'}")
        sized = subprocess.run([str(tools / "arm-none-eabi-size.exe"), "--format=berkeley", name + ".elf"],
                               cwd=build, env=env, capture_output=True, text=True, errors="replace")
        (build / (name + ".elf.size")).write_text(sized.stdout + sized.stderr, encoding="utf-8")
        if sized.returncode:
            raise RuntimeError(f"Native {name} size check failed")
    elf = build / (name + ".elf")
    if not elf.is_file() or elf.read_bytes()[:4] != b"\x7fELF":
        raise RuntimeError(f"Missing valid ELF after {name} build")
    return 0
