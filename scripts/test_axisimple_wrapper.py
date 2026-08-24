#!/usr/bin/env python3
"""Build and run focused AxiSimple wrapper regressions."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "axisimple_wrapper_sim"
WRAPPER = ROOT / "hdl" / "axisimple" / "axisimple_wrapper.sv"
SKIDBUFFER = ROOT / "hdl" / "axisimple" / "skidbuffer.v"
TOP = ROOT / "hdl" / "appletini_yarz_top.sv"
APPLE_TOP = ROOT / "hdl" / "apple" / "apple_top.sv"
MMIO_H = ROOT / "ps_sources" / "lib" / "axisimple_mmio.h"
FRONTEND_MAIN = ROOT / "ps_sources" / "frontend" / "main.c"
CORE1_MAIN = ROOT / "ps_sources" / "frontend_core1" / "main.c"


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}"
        )
    return completed.stdout


def require(pattern: str, source: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def check_registered_wskid() -> None:
    source = WRAPPER.read_text(encoding="utf-8")
    require(
        r"wire\s+\[36:0\]\s+wskid_payload\s*;",
        source,
        "axisimple_wrapper must retain one 37-bit W payload",
    )
    require(
        r"assign\s+S_AXI_WREADY\s*=\s*"
        r"S_AXI_ARESETN\s*&&\s*wskid_input_ready\s*;",
        source,
        "wrapper WREADY must use the skid input-ready signal and reset gate",
    )
    require(
        r"skidbuffer\s*#\s*\("
        r"(?=[^;]*\.OPT_LOWPOWER\s*\(\s*1'b1\s*\))"
        r"(?=[^;]*\.OPT_OUTREG\s*\(\s*1'b1\s*\))"
        r"(?=[^;]*\.DW\s*\(\s*37\s*\))"
        r"[^;]*\)\s*wchannel_skid_i\s*\(",
        source,
        "wrapper must instantiate a registered 37-bit low-power skid",
    )
    require(
        r"wchannel_skid_i\s*\("
        r"(?=[^;]*\.i_clk\s*\(\s*S_AXI_ACLK\s*\))"
        r"(?=[^;]*\.i_reset\s*\(\s*!S_AXI_ARESETN\s*\))"
        r"(?=[^;]*\.i_valid\s*\(\s*S_AXI_WVALID\s*\))"
        r"(?=[^;]*\.o_ready\s*\(\s*wskid_input_ready\s*\))"
        r"(?=[^;]*\.i_data\s*\(\s*\{\s*S_AXI_WLAST\s*,\s*"
        r"S_AXI_WSTRB\s*,\s*S_AXI_WDATA\s*\}\s*\))"
        r"(?=[^;]*\.o_valid\s*\(\s*wskid_valid\s*\))"
        r"(?=[^;]*\.i_ready\s*\(\s*axid_wready\s*\))"
        r"(?=[^;]*\.o_data\s*\(\s*wskid_payload\s*\))",
        source,
        "registered skid ports or W payload packing changed",
    )
    require(
        r"\.S_AXI_WVALID\s*\(\s*wskid_valid\s*\)\s*,\s*"
        r"\.S_AXI_WREADY\s*\(\s*axid_wready\s*\)\s*,\s*"
        r"\.S_AXI_WDATA\s*\(\s*wskid_payload\[31:0\]\s*\)\s*,\s*"
        r"\.S_AXI_WSTRB\s*\(\s*wskid_payload\[35:32\]\s*\)\s*,\s*"
        r"\.S_AXI_WLAST\s*\(\s*wskid_payload\[36\]\s*\)",
        source,
        "axidouble must consume valid and all payload bits from the W skid",
    )


def check_masked_wskid_copy() -> None:
    wrapper_source = WRAPPER.read_text(encoding="utf-8")
    skid_source = SKIDBUFFER.read_text(encoding="utf-8")
    top_source = TOP.read_text(encoding="utf-8")
    apple_source = APPLE_TOP.read_text(encoding="utf-8")

    require(
        r"parameter\s+\[0:0\]\s+OPT_INITIAL\s*=\s*1'b1\s*,\s*"
        r"parameter\s+\[DW-1:0\]\s+OPT_COPY_MASK\s*=\s*"
        r"\{\s*DW\s*\{\s*1'b0\s*\}\s*\}",
        skid_source,
        "copy mask must append after OPT_INITIAL and default to no copied bits",
    )
    require(
        r"output\s+(?:wire\s+)?\[DW-1:0\]\s+o_data_copy",
        skid_source,
        "skidbuffer must expose its same-stage masked copy",
    )
    require(
        r"if\s*\(\s*OPT_PASSTHROUGH\s*\)\s*"
        r"begin\s*:\s*PASSTHROUGH.*?"
        r"assign\s+o_data_copy\s*=\s*o_data\s*;.*?"
        r"end\s+else\s+begin\s*:\s*LOGIC",
        skid_source,
        "passthrough mode must alias o_data_copy to o_data",
    )
    require(
        r"if\s*\(\s*!OPT_OUTREG\s*\)\s*"
        r"begin\s*:\s*NET_OUTPUT.*?"
        r"assign\s+o_data_copy\s*=\s*o_data\s*;.*?"
        r"end\s+else\s+begin\s*:\s*REG_OUTPUT",
        skid_source,
        "unregistered output mode must alias o_data_copy to o_data",
    )
    require(
        r"for\s*\(\s*copy_index\s*=\s*0\s*;\s*"
        r"copy_index\s*<\s*DW\s*;\s*copy_index\s*=\s*copy_index\s*\+\s*1\s*\)"
        r"\s*begin\s*:\s*COPY_BITS.*?"
        r"if\s*\(\s*OPT_COPY_MASK\[copy_index\]\s*\)\s*begin\s*:\s*ENABLED.*?"
        r"\breg\s+copy_q\s*;",
        skid_source,
        "skidbuffer must create copy flops only for selected mask bits",
    )
    require(
        r"begin\s*:\s*ENABLED.*?\breg\s+copy_q\s*;\s*"
        r"initial\s+if\s*\(\s*OPT_INITIAL\s*\)\s*copy_q\s*=\s*0\s*;",
        skid_source,
        "selected copy flops must follow the existing OPT_INITIAL policy",
    )
    require(
        r"always\s*@\s*\(\s*posedge\s+i_clk\s*\)\s*"
        r"if\s*\(\s*OPT_LOWPOWER\s*&&\s*i_reset\s*\)\s*"
        r"copy_q\s*<=\s*(?:1'b)?0\s*;\s*"
        r"else\s+if\s*\(\s*!o_valid\s*\|\|\s*i_ready\s*\)\s*begin\s*"
        r"if\s*\(\s*r_valid\s*\)\s*"
        r"copy_q\s*<=\s*r_data\[copy_index\]\s*;\s*"
        r"else\s+if\s*\(\s*!OPT_LOWPOWER\s*\|\|\s*i_valid\s*\)\s*"
        r"copy_q\s*<=\s*i_data\[copy_index\]\s*;\s*"
        r"else\s+copy_q\s*<=\s*(?:1'b)?0\s*;\s*end",
        skid_source,
        "copy flops must use the registered skid output's exact update priority",
    )
    if re.search(r"\bcopy_q\s*<=\s*o_data(?:\b|\s*\[)", skid_source):
        raise AssertionError(
            "masked skid copy must not add an o_data-to-copy register stage"
        )
    require(
        r"assign\s+o_data_copy\[copy_index\]\s*=\s*copy_q\s*;",
        skid_source,
        "selected copy output bits must come from copy_q",
    )
    require(
        r"else\s+begin\s*:\s*DISABLED.*?"
        r"assign\s+o_data_copy\[copy_index\]\s*=\s*o_data\[copy_index\]\s*;",
        skid_source,
        "unselected copy output bits must remain canonical combinational aliases",
    )

    require(
        r"output\s+(?:wire|logic)?\s*\[31:0\]\s+as_vtw_phasor_wdata",
        wrapper_source,
        "wrapper must expose the shared vTW/Phasor WDATA copy",
    )
    require(
        r"output\s+(?:wire|logic)?\s*\[3:0\]\s+as_vtw_phasor_wstrb",
        wrapper_source,
        "wrapper must expose the shared vTW/Phasor WSTRB copy",
    )
    require(
        r"wire\s+\[36:0\]\s+wskid_payload_copy\s*;",
        wrapper_source,
        "wrapper must retain the complete copied W payload",
    )
    require(
        r"wchannel_skid_i\s*\("
        r"(?=[^;]*\.o_data\s*\(\s*wskid_payload\s*\))"
        r"(?=[^;]*\.o_data_copy\s*\(\s*wskid_payload_copy\s*\))",
        wrapper_source,
        "canonical and copied payloads must come from the same skid instance",
    )
    require(
        r"\.OPT_COPY_MASK\s*\(\s*"
        r"\{\s*1'b0\s*,\s*4'b1111\s*,\s*32'h0000_2526\s*\}\s*\)",
        wrapper_source,
        "copy mask must select WDATA bits 1, 2, 5, 8, 10, 13 and WSTRB bits 0-3",
    )
    require(
        r"assign\s+as_vtw_phasor_wdata\s*=\s*"
        r"wskid_payload_copy\[31:0\]\s*;",
        wrapper_source,
        "shared WDATA output must use the copied payload",
    )
    require(
        r"assign\s+as_vtw_phasor_wstrb\s*=\s*"
        r"wskid_payload_copy\[35:32\]\s*;",
        wrapper_source,
        "shared WSTRB output must use the copied payload",
    )
    if re.search(r"MAX_FANOUT", wrapper_source):
        raise AssertionError("masked trial must not restore broad MAX_FANOUT copying")

    for signal, width in (
        ("as_vtw_phasor_wdata", r"31:0"),
        ("as_vtw_phasor_wstrb", r"3:0"),
    ):
        require(
            rf"(?:wire|logic)\s+\[{width}\]\s+{signal}\s*;",
            top_source,
            f"top must declare {signal}",
        )
        connections = re.findall(
            rf"\.{signal}\s*\(\s*{signal}\s*\)", top_source
        )
        if len(connections) != 2:
            raise AssertionError(
                f"top must thread {signal} once from wrapper and once to apple_top"
            )
        require(
            rf"input\s+(?:wire|logic)?\s*\[{width}\]\s+{signal}",
            apple_source,
            f"apple_top must accept {signal}",
        )

    allowed_patterns = [
        r"input\s+(?:wire|logic)?\s*\[31:0\]\s+as_vtw_phasor_wdata",
        r"input\s+(?:wire|logic)?\s*\[3:0\]\s+as_vtw_phasor_wstrb",
        r"wire\s+vtw_sh_addr_set\s*=.*?;",
        r"wire\s+vtw_sh_byte_write\s*=.*?;",
        r"wire\s+vtw_sh_word_write\s*=.*?;",
        r"wire\s+vtw_sh_word_read\s*=.*?;",
        r"vtw_shadow_host_port\s+vtw_shadow_host_port_i\s*\(.*?\);",
        r"CARD_CTRL_REG_PHASOR_PAN_LO\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_PHASOR_PAN_HI\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_PHASOR_AUDIO\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_NSC_TIME_LO\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_NSC_TIME_HI\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_VTW_SYNC_CMD\s*:\s*begin.*?end",
        r"CARD_CTRL_REG_VTW_POST_PUSH\s*:\s*begin.*?end",
        r"always_comb\s+begin\s*mouse_as_common\s*=\s*as_common\s*;.*?end",
    ]
    allowed_spans: list[tuple[int, int]] = []
    for pattern in allowed_patterns:
        match = re.search(pattern, apple_source, re.MULTILINE | re.DOTALL)
        if not match:
            raise AssertionError(
                f"apple_top is missing an approved copy consumer: {pattern}"
            )
        allowed_spans.append(match.span())
    for match in re.finditer(r"\bas_vtw_phasor_w(?:data|strb)\b", apple_source):
        if not any(start <= match.start() < end for start, end in allowed_spans):
            line = apple_source.count("\n", 0, match.start()) + 1
            raise AssertionError(
                f"shared skid copy reached an unapproved apple_top consumer at line {line}"
            )

    for pattern, message in (
        (
            r"wire\s+vtw_sh_addr_set\s*=.*?"
            r"as_vtw_phasor_wstrb\s*!=\s*4'b0000\s*\)\s*;",
            "vTW address command must use copied WSTRB",
        ),
        (
            r"wire\s+vtw_sh_byte_write\s*=.*?"
            r"as_vtw_phasor_wstrb\[0\]\s*;",
            "vTW byte command must use copied WSTRB",
        ),
        (
            r"wire\s+vtw_sh_word_write\s*=.*?"
            r"as_vtw_phasor_wstrb\s*==\s*4'b1111\s*\)\s*;",
            "vTW word command must use copied WSTRB",
        ),
        (
            r"wire\s+vtw_sh_word_read\s*=.*?"
            r"as_vtw_phasor_wstrb\[0\].*?as_vtw_phasor_wdata\[0\]\s*;",
            "vTW word-read command must use both copied buses",
        ),
        (
            r"\.addr_value\s*\(\s*as_vtw_phasor_wdata\[17:0\]\s*\).*?"
            r"\.byte_wdata\s*\(\s*as_vtw_phasor_wdata\[7:0\]\s*\).*?"
            r"\.word_wdata\s*\(\s*as_vtw_phasor_wdata\s*\)",
            "vTW shadow payload ports must use copied WDATA",
        ),
    ):
        require(pattern, apple_source, message)

    for target, pattern in (
        ("vtw_sh_addr_set", r"wire\s+vtw_sh_addr_set\s*=.*?;"),
        ("vtw_sh_byte_write", r"wire\s+vtw_sh_byte_write\s*=.*?;"),
        ("vtw_sh_word_write", r"wire\s+vtw_sh_word_write\s*=.*?;"),
        ("vtw_sh_word_read", r"wire\s+vtw_sh_word_read\s*=.*?;"),
        (
            "vtw_shadow_host_port_i",
            r"vtw_shadow_host_port\s+vtw_shadow_host_port_i\s*\(.*?\);",
        ),
    ):
        match = re.search(pattern, apple_source, re.MULTILINE | re.DOTALL)
        if not match:
            raise AssertionError(f"apple_top is missing {target}")
        if re.search(r"as_common\.w(?:data|strb)", match.group(0)):
            raise AssertionError(
                f"{target} must not retain canonical WDATA/WSTRB loads"
            )

    for register in ("PHASOR_PAN_LO", "PHASOR_PAN_HI", "PHASOR_AUDIO"):
        match = re.search(
            rf"CARD_CTRL_REG_{register}\s*:\s*begin(.*?)end",
            apple_source,
            re.MULTILINE | re.DOTALL,
        )
        if not match or "as_vtw_phasor_wdata" not in match.group(1) or \
                "as_vtw_phasor_wstrb" not in match.group(1):
            raise AssertionError(
                f"{register} must use both shared copied buses"
            )
        if re.search(r"as_common\.w(?:data|strb)", match.group(1)):
            raise AssertionError(
                f"{register} must not retain a canonical WDATA/WSTRB load"
            )

    for register in ("NSC_TIME_LO", "NSC_TIME_HI"):
        match = re.search(
            rf"CARD_CTRL_REG_{register}\s*:\s*begin(.*?)end",
            apple_source,
            re.MULTILINE | re.DOTALL,
        )
        if not match or "as_vtw_phasor_wdata" not in match.group(1) or \
                "as_vtw_phasor_wstrb" not in match.group(1):
            raise AssertionError(
                f"{register} must use both shared copied buses"
            )
        if re.search(r"as_common\.w(?:data|strb)", match.group(1)):
            raise AssertionError(
                f"{register} must not retain a canonical WDATA/WSTRB load"
            )

    for register in ("VTW_SYNC_CMD", "VTW_POST_PUSH"):
        match = re.search(
            rf"CARD_CTRL_REG_{register}\s*:\s*begin(.*?)end",
            apple_source,
            re.MULTILINE | re.DOTALL,
        )
        if not match or "as_vtw_phasor_wdata" not in match.group(1):
            raise AssertionError(
                f"{register} must use the east-side copied WDATA"
            )
        if "as_common.wdata" in match.group(1):
            raise AssertionError(
                f"{register} must not retain a canonical WDATA load"
            )

    require(
        r"globals::AxiSimple_common\s+mouse_as_common\s*;.*?"
        r"mouse_as_common\.wdata\s*=\s*as_vtw_phasor_wdata\s*;.*?"
        r"mouse_as_common\.wstrb\s*=\s*as_vtw_phasor_wstrb\s*;.*?"
        r"mouse_card\s+mouse_card_i\s*\(.*?"
        r"\.as_common\s*\(\s*mouse_as_common\s*\)",
        apple_source,
        "mouse AXI writes must use the local same-stage WDATA/WSTRB copy",
    )


def check_no_exclusive_mmio_contract() -> None:
    wrapper_source = WRAPPER.read_text(encoding="utf-8")
    mmio_source = MMIO_H.read_text(encoding="utf-8")
    frontend_source = FRONTEND_MAIN.read_text(encoding="utf-8")
    core1_source = CORE1_MAIN.read_text(encoding="utf-8")
    exclusive_settings = re.findall(
        r"\.OPT_EXCLUSIVE_ACCESS\s*\(\s*([^)]*?)\s*\)",
        wrapper_source,
    )
    if exclusive_settings != ["1'b0"]:
        raise AssertionError(
            "axisimple_wrapper must disable its one exclusive monitor instance"
        )
    require(
        r"axisimple_mmio_mmu_init\s*\([^)]*\)\s*\{.*?"
        r"Xil_SetTlbAttributes\s*\(\s*0x40000000U\s*,\s*"
        r"DEVICE_MEMORY\s*\)\s*;",
        mmio_source,
        "both cores must share the Device-memory AxiSimple mapping helper",
    )
    if mmio_source.count("Xil_SetTlbAttributes") != 1:
        raise AssertionError("AxiSimple MMIO helper must map exactly one section")
    if re.search(r"NORM_NONCACHE|STRONG_ORDERED", mmio_source):
        raise AssertionError("AxiSimple MMIO mapping must fail closed to Device")
    for source, first_pl_use, name in (
        (frontend_source, "apple_fb_handoff_init();", "CPU0"),
        (core1_source, "apple_cycle_egress_amp_secondary_init();", "CPU1"),
    ):
        main_source = source[source.index("int main(void)") :]
        if main_source.count("axisimple_mmio_mmu_init();") != 1:
            raise AssertionError(f"{name} must map AxiSimple MMIO exactly once")
        call = main_source.find("axisimple_mmio_mmu_init();")
        first_use = main_source.find(first_pl_use)
        if call < 0 or first_use < 0 or call > first_use:
            raise AssertionError(
                f"{name} must map GP0 as Device memory before its first PL use"
            )

    # This is deliberately broad. Any new atomic use needs review before it
    # can coexist with a PL register plane that has no exclusive monitor.
    forbidden = re.compile(
        r"(?:__atomic|__sync|__ldrex|__strex|\bldrex(?:b|h|d)?\b|"
        r"\bstrex(?:b|h|d)?\b|\bclrex\b|\bswpb?\b|"
        r"Xil_InitializeSpinLock|\bstdatomic\.h\b)",
        re.IGNORECASE,
    )
    violations: list[str] = []
    for path in sorted((ROOT / "ps_sources").rglob("*")):
        if path.suffix.lower() not in {".c", ".h", ".s"}:
            continue
        source = path.read_text(encoding="utf-8", errors="ignore")
        if forbidden.search(source):
            violations.append(str(path.relative_to(ROOT)))
    if violations:
        raise AssertionError(
            "GP0 MMIO firmware must not introduce atomic/exclusive operations: "
            + ", ".join(violations)
        )


def main() -> int:
    check_registered_wskid()
    check_masked_wskid_copy()
    check_no_exclusive_mmio_contract()
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    axisimple = ROOT / "hdl" / "axisimple"
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "globals.sv"),
            str(axisimple / "skidbuffer.v"),
            str(axisimple / "sfifo.v"),
            str(axisimple / "addrdecode.v"),
            str(axisimple / "axi_addr.v"),
            str(axisimple / "axidouble.v"),
            str(WRAPPER),
            str(ROOT / "hdl" / "apple" / "disk2_card.sv"),
            str(ROOT / "hdl" / "sim" / "tb_axisimple_wrapper_wskid.sv"),
            str(ROOT / "hdl" / "sim" / "tb_axisimple_disk2_base.sv"),
        ],
        "xvlog.log",
    )
    shutil.copy2(
        ROOT / "hdl" / "apple" / "disk2_slot6.mem",
        OUT_DIR / "disk2_slot6.mem",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_axisimple_wrapper_wskid",
            "-s",
            "tb_axisimple_wrapper_wskid_snap",
        ],
        "xelab.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_axisimple_wrapper_wskid_snap",
            "--runall",
        ],
        "xsim.log",
    )
    if "AXISIMPLE WRAPPER WSKID PASS" not in output:
        print(output)
        raise RuntimeError("AXI write-data skid bench did not report success")
    run(
        [
            vivado_tool("xelab"),
            "tb_axisimple_disk2_base",
            "-s",
            "tb_axisimple_disk2_base_snap",
        ],
        "xelab_disk2.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_axisimple_disk2_base_snap",
            "--runall",
        ],
        "xsim_disk2.log",
    )
    if "AXISIMPLE DISK2 BASE PASS" not in output:
        print(output)
        raise RuntimeError("AXI Disk II BASE bench did not report success")
    print("AxiSimple wrapper regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
