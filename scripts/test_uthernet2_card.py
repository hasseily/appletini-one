from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_text(path):
    return (ROOT / path).read_text(encoding="utf-8")


def retromate_slot_from_prodos_path(pathname):
    """Replicate RetroMate platA2net.c slot selection.

    ProDOS stores a length byte at $280 and the pathname from $281 onward.
    RetroMate reads the final pathname byte and passes that byte & 7 to
    ip65_init().  Therefore a runtime pathname ending in RMATE1 probes slot 1,
    while the upstream default runtime pathname ending in RMATE3 probes slot 3.
    """
    path_bytes = pathname.encode("ascii")
    prodos_path = bytes([len(path_bytes)]) + path_bytes
    last_path_byte = prodos_path[1 + prodos_path[0] - 1]
    return last_path_byte & 0x07


def uthernet_ip65_registers(slot):
    """IP65's Uthernet II driver starts from C084/C085/C087 and slot-fixes it."""
    base = 0xC080 + (slot << 4)
    return {
        "mode": base + 0x04,
        "addr_hi": base + 0x05,
        "addr_lo": base + 0x06,
        "data": base + 0x07,
    }


def ip65_w5100_probe(slot, available_slot):
    """Replicate the first W5100 detection gate in IP65 drivers/w5100.s.

    IP65 enables indirect mode + address auto-increment, sets the W5100
    address register to $0017, and expects the retry timer defaults $07,$D0.
    The assembly checks this as: ($07 ^ $D0) ^ read($0017) ^ read($0018) == 0.
    """
    regs = uthernet_ip65_registers(slot)
    expected_regs = uthernet_ip65_registers(available_slot)
    if regs != expected_regs:
        return False

    retry_timer_hi = 0x07
    retry_timer_lo = 0xD0
    return (0xD7 ^ retry_timer_hi ^ retry_timer_lo) == 0


def contiki_w5100_probe(slot, available_slot):
    """Contiki's w5100.eth init uses the same first hardware gate as IP65."""
    return ip65_w5100_probe(slot, available_slot)


def contiki_macraw_accesses():
    """Contiki w5100.eth opens socket 0 in MACRAW mode and uses W5100 windows."""
    s0_base = 0x0400
    return {
        "s0_mr": s0_base + 0x00,
        "s0_cr": s0_base + 0x01,
        "s0_sr": s0_base + 0x03,
        "s0_tx_fsr": s0_base + 0x20,
        "s0_tx_wr": s0_base + 0x24,
        "s0_rx_rsr": s0_base + 0x26,
        "s0_rx_rd": s0_base + 0x28,
        "tx_base": 0x4000,
        "rx_base": 0x6000,
    }


def w5100s_physical_addr(addr):
    """Mirror the HDL's W5100/Uthernet II address visibility."""
    return addr & 0x7FFF


def main():
    card = read_text("hdl/apple/uthernet2_card.sv")
    top = read_text("hdl/apple/apple_top.sv")
    wrapper = read_text("hdl/appletini_yarz_top.sv")
    xdc = read_text("hdl/constraints/appletini_yarz.xdc")
    hdl_sources = read_text("hdl/hdl_sources.txt")
    config = read_text("ps_sources/frontend/config_menu.c")
    config_h = read_text("ps_sources/frontend/config_menu.h")
    config_tabs = read_text("ps_sources/frontend/config_menu_device_tabs.c")
    frontend = read_text("ps_sources/frontend/main.c")
    regs = read_text("ps_sources/frontend/card_control_regs.h")
    eth_control = read_text("ps_sources/frontend/uthernet2_control.c")
    sim = read_text("hdl/sim/tb_uthernet2_card.sv")
    vitis = read_text("scripts/create_vitis_workspace.py")

    require("apple/uthernet2_card.sv" in hdl_sources,
            "Uthernet II card source must be in hdl_sources.txt")
    require("module uthernet2_card" in card,
            "Uthernet II HDL module missing")
    require("ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign})" in card,
            "Uthernet II must decode the selected C0nX slot I/O page")
    require("wire [1:0] apple_io_reg = ab_read.addr[1:0];" in card,
            "Uthernet II must alias C0n0-C0nF through A0/A1 only")
    require("slot_rom" not in card and "slot_access" not in card,
            "Uthernet II implementation should not add a slot ROM")
    require("function automatic logic [7:0] next_mode_value" in card,
            "Uthernet II must centralize W5100 shadow mode update behavior")
    require("mode_q <= next_mode_value(ab_read.data);" in card,
            "Uthernet II mode register must be Apple-visible shadow state")
    require("function automatic logic [7:0] w5100s_physical_mode_value" in card and
            "W5100S_MR_FIXED_INDIRECT" in card and
            "w5100s_physical_mode_value(ab_read.data)" in card,
            "W5100 IND/AI mode bits must be shadow-only and masked before W5100S MR writes")
    require("apple_local_read_data(reg_sel, mode_q, data_addr_q)" in card,
            "Uthernet II mode/address reads must come from the local W5100 shadow")
    require("if (data_phys_addr == 16'h0000)" in card and
            "read_data_q <= mode_q;" in card,
            "Indirect data-port reads of W5100 MR must return the Uthernet shadow")
    require("mode_q <= next_mode_value(ab_read.data);" in card and
            "w5100s_physical_mode_value(ab_read.data)" in card,
            "Indirect data-port writes of W5100 MR must update the Uthernet shadow")
    require("common_regs_q" not in card and "reset_common_regs" not in card,
            "W5100 common registers must not be shadowed by the FPGA")
    require("BUS_KIND_DATA_READ" in card and
            "read_data_q <= eth_d_i;" in card,
            "W5100 common/socket/buffer reads must come from the physical W5100S")
    require("BUS_KIND_DATA_WRITE" in card and
            "eth_wr_n <= 1'b0;" in card,
            "W5100 common/socket/buffer writes must go to the physical W5100S")
    require("host_req" in card and
            "host_write" in card and
            "host_ready" in card and
            "host_done" in card and
            "host_rdata" in card,
            "Uthernet II must expose a PS host register access path")
    require("BUS_KIND_HOST_READ" in card and
            "BUS_KIND_HOST_WRITE" in card and
            "ETH_HOST_ADDR_RELOAD_READ" in card and
            "ETH_HOST_ADDR_RELOAD_WRITE" in card and
            "ETH_HOST_ADDR_LO_THEN_READ" in card and
            "ETH_HOST_ADDR_LO_THEN_WRITE" in card and
            "ETH_HOST_DATA_READ_START" in card and
            "ETH_HOST_DATA_WRITE_START" in card,
            "Host accesses must share the physical W5100S bus operation states")
    require("start_host_mode_setup" in card and
            "w5100s_physical_mode_value(mode_q)" in card and
            "host_write ? ETH_HOST_ADDR_RELOAD_WRITE :\n"
            "                                         ETH_HOST_ADDR_RELOAD_READ" in card,
            "Host commands must force W5100S indirect+AI mode without clobbering the W5100 mode shadow")
    require("wire apple_io_write_addr_start" in card and
            "apple_write_reserved_q" in card and
            "wire host_can_start =\n"
            "        reset_done_q && (eth_state_q == ETH_IDLE) && !apple_waiting &&\n"
            "        !host_pending_q;" in card,
            "Host commands must not start inside an Apple write cycle")
    require("if (host_req && !host_can_start && !host_pending_q) begin" in card and
            "host_pending_q <= 1'b1;" in card and
            "host_active_q <= 1'b1;" in card,
            "A host request colliding with an Apple IO cycle must be latched and "
            "served at the next free idle cycle, never dropped (a lost one-cycle "
            "pulse wedges apple_top's busy flag forever)")
    require("apple_read_pending_q" in card and
            "task automatic fail_host_access" in card and
            "host_error_q <= 1'b1;" in card,
            "Apple accesses must preempt an in-flight host command cleanly")
    require("wire [15:0] host_phys_addr = host_addr & 16'h7FFF;" in card,
            "Host commands must fold high-mirror W5100 addresses the same way Apple data accesses do")
    require("start_addr_reload(access_addr_q, ETH_HOST_ADDR_LO_THEN_READ);" in card and
            "start_addr_reload(access_addr_q, ETH_HOST_ADDR_LO_THEN_WRITE);" in card and
            "ETH_HOST_DATA_READ_START" in card and
            "ETH_HOST_DATA_WRITE_START" in card,
            "Host register accesses must reload both W5100S indirect address bytes before data access")
    require("addr_synced_q <= 1'b0;" in card and
            "host_done_q <= 1'b1;" in card,
            "Host register accesses must invalidate pointer sync and signal completion")
    require("wire [15:0] data_phys_addr = w5100_physical_addr(data_addr_q);" in card and
            "if (data_phys_addr == 16'h0000)" in card,
            "W5100 MR compatibility must apply after high-mirror address folding")
    require("function automatic logic w5100_reserved_addr" in card and
            "w5100_reserved_addr(data_phys_addr)" in card,
            "W5100S-only common extension registers must be hidden from W5100 software")
    require("RESET_READY_WAIT_US" in card and
            "RESET_READY_WAIT_COUNT" in card and
            "reset_released_q" in card,
            "W5100S hardware reset release must wait for the post-reset stable interval")
    require("W5100S_PHYSR   = 16'h003C" in card and
            "W5100S_PHYLCKR = 16'h0072" in card and
            "W5100S_PHYCR0  = 16'h0046" in card and
            "W5100S_PHYCR1  = 16'h0047" in card and
            "W5100S_VERR    = 16'h0080" in card,
            "W5100S PHY control and identity register definitions must be present for PHY bring-up")
    # The PHY bring-up / identity registers are deliberately punched through the
    # reserved-address block so the PHY can be brought up and the chip identity
    # can be checked from Apple software via the proven normal data-port path.
    # Every other W5100S-only register in the gap stays hidden from W5100 software.
    require("function automatic logic w5100_phy_open_addr" in card and
            "addr == W5100S_PHYSR" in card and
            "addr == W5100S_PHYCR0" in card and
            "addr == W5100S_PHYCR1" in card and
            "addr == W5100S_PHYLCKR" in card and
            "addr == W5100S_VERR" in card and
            "!w5100_phy_open_addr(addr)" in card,
            "PHY bring-up and identity registers must be exposed to Apple software while other extension regs stay hidden")
    require("ETH_INIT" not in card and "set_init_access" not in card,
            "Uthernet II must not carry a disabled PHY-init sequencer")
    require("if (mode_q & W5100_MR_AI)" in card,
            "Uthernet II auto-increment must follow the Apple-visible AI bit")
    require("16'h5FFF: w5100_auto_inc_addr = 16'h4000;" in card,
            "Uthernet II TX buffer auto-increment must wrap $5FFF->$4000")
    require("16'h7FFF: w5100_auto_inc_addr = 16'h6000;" in card,
            "Uthernet II RX buffer auto-increment must wrap $7FFF->$6000")
    require("16'hFFFF: w5100_auto_inc_addr = 16'hE000;" in card,
            "Uthernet II high mirror auto-increment must wrap $FFFF->$E000")
    require("w5100_physical_addr = addr & 16'h7FFF;" in card,
            "Uthernet II high mirror data accesses must first map to the W5100 lower 15-bit space")
    require("TX remains at $4000" in card and "RX remains at $6000" in card,
            "W5100S accesses must preserve the W5100-compatible TX/RX memory map")
    require("16'b010?_????_????_????" not in card and
            "16'b011?_????_????_????" not in card,
            "W5100S TX/RX accesses must not be remapped to non-W5100 windows")
    require("wire [15:0] data_phys_next_addr = access_addr_q + 16'h0001;" in card,
            "W5100S physical pointer tracking must use native physical auto-increment")
    require("addr_synced_q" in card and "phys_addr_q" in card,
            "Uthernet II must track whether the W5100S indirect pointer matches the Apple shadow")
    require("logic [7:0] payload_data_q;" in card and
            "payload_data_q <= ab_read.data;" in card and
            "payload_data_q <= host_wdata;" in card and
            "start_data_write(payload_data_q);" in card and
            "start_host_write(payload_data_q);" in card,
            "Reloaded W5100S writes must preserve the pending data byte across address setup")
    require("U2_REG_ARH" in card and "BUS_KIND_ADDR_HI" in card,
            "Apple writes to the address-high register must mirror into W5100S immediately")
    require("U2_REG_ARL" in card and "BUS_KIND_ADDR_LO" in card,
            "Apple writes to the address-low register must mirror into W5100S immediately")
    require("addr_synced_q && (phys_addr_q == data_phys_addr)" in card,
            "Synced W5100S data accesses must avoid an in-cycle pointer reload")
    require("ETH_ADDR_LO_THEN_READ" in card and
            "ETH_ADDR_LO_THEN_WRITE" in card and
            "start_addr_reload(data_phys_addr" in card,
            "Unsynced W5100S data accesses must reload address high and low before data")
    require("task automatic begin_bus_op" in card and "ETH_BUS_OP" in card,
            "W5100S bus accesses should use the shared physical bus operation path")
    require("BUS_SETUP_CYCLES = 2" in card and
            "READ_DONE_CYCLES" in card and
            "WRITE_DONE_CYCLES" in card and
            "BUS_SETUP_CYCLES + READ_STROBE_CYCLES" in card and
            "BUS_SETUP_CYCLES + WRITE_STROBE_CYCLES" in card and
            "ETH_OP_COUNTER_W" in card and
            "logic [ETH_OP_COUNTER_W-1:0] op_cycle_q" in card,
            "W5100S physical bus cycles must include setup and a non-truncated strobe counter")
    require("read_pending_q" in card and "eth_rd_n <= 1'b0" in card,
            "Uthernet II reads must drive the W5100S RD# path")
    require("apple_io_write_start" in card and "eth_wr_n <= 1'b0" in card,
            "Uthernet II writes must drive the W5100S WR# path")

    require(retromate_slot_from_prodos_path("RMATE1") == 1,
            "RetroMate RMATE1 must select Ethernet slot 1")
    require(retromate_slot_from_prodos_path("RMATE3") == 3,
            "RetroMate's upstream default RMATE3 selects slot 3")
    require(uthernet_ip65_registers(1) == {
                "mode": 0xC094,
                "addr_hi": 0xC095,
                "addr_lo": 0xC096,
                "data": 0xC097,
            },
            "RetroMate/IP65 slot-1 probe must hit C094/C095/C096/C097")
    require(uthernet_ip65_registers(3) == {
                "mode": 0xC0B4,
                "addr_hi": 0xC0B5,
                "addr_lo": 0xC0B6,
                "data": 0xC0B7,
            },
            "RetroMate/IP65 default slot-3 probe hits C0B4/C0B5/C0B6/C0B7")
    require(ip65_w5100_probe(retromate_slot_from_prodos_path("RMATE1"), 1),
            "RetroMate RMATE1 must pass the IP65 W5100 retry-timer detection probe")
    require(not ip65_w5100_probe(retromate_slot_from_prodos_path("RMATE3"), 1),
            "RetroMate RMATE3 must fail when the only Ethernet card is in slot 1")
    require(contiki_w5100_probe(1, 1),
            "Contiki w5100.eth slot-1 init must pass the W5100 retry-timer detection probe")
    require(not contiki_w5100_probe(3, 1),
            "Contiki w5100.eth slot-3 init must fail when the only Ethernet card is in slot 1")
    accesses = contiki_macraw_accesses()
    require(accesses["s0_mr"] == 0x0400 and accesses["s0_cr"] == 0x0401,
            "Contiki MACRAW setup must target W5100 socket-0 mode/control registers")
    require(accesses["s0_tx_fsr"] == 0x0420 and accesses["s0_rx_rsr"] == 0x0426,
            "Contiki send/receive polling must target W5100 socket-0 size registers")
    require(accesses["tx_base"] == 0x4000 and accesses["rx_base"] == 0x6000,
            "Contiki packet buffers must use the W5100 TX/RX memory windows")
    for name, addr in accesses.items():
        require(w5100s_physical_addr(addr) == addr,
                f"Contiki {name} access must not be remapped away from W5100-compatible address ${addr:04X}")
    require(w5100s_physical_addr(0x0000) == 0x0000,
            "W5100 common registers must stay in the low physical window")
    require(w5100s_physical_addr(0x03FF) == 0x03FF,
            "W5100 socket registers must stay in the low physical window")
    require(w5100s_physical_addr(0x4000) == 0x4000,
            "W5100 TX base must remain at the W5100S-compatible TX base")
    require(w5100s_physical_addr(0x5FFF) == 0x5FFF,
            "W5100 TX end must remain at the W5100S-compatible TX end")
    require(w5100s_physical_addr(0x6000) == 0x6000,
            "W5100 RX base must remain at the W5100S-compatible RX base")
    require(w5100s_physical_addr(0x7FFF) == 0x7FFF,
            "W5100 RX end must remain at the W5100S-compatible RX end")
    require(w5100s_physical_addr(0xC000) == 0x4000,
            "High mirror W5100 TX base must mirror to W5100 TX base")
    require(w5100s_physical_addr(0xE000) == 0x6000,
            "High mirror W5100 RX base must mirror to W5100 RX base")
    require(w5100s_physical_addr(0xFFFF) == 0x7FFF,
            "High mirror W5100 RX end must mirror to W5100 RX end")

    require("wire card_slot1_enable = card_slot_enable_mask_q[1];" in top,
            "Apple top must expose slot 1 enable")
    require(".ab_read(gate_ab(ab_read, card_slot1_enable))" in top and
            ".slot_assign(3'h1)" in top,
            "Uthernet II card must be gated by slot 1 enable and assigned to slot 1")
    require("apple_bus_write_arbiter #(.NUM_CLIENTS(11))" in top and
            "uthernet_ab_write" in top,
            "Apple bus arbiter must include the Uthernet writer")

    for signal in [
        "eth_d", "eth_a", "eth_rd_n", "eth_wr_n",
        "eth_cs_n", "eth_rst_n", "eth_int_n",
    ]:
        require(signal in wrapper, f"Top-level wrapper missing {signal}")
        require(signal in xdc, f"Constraints missing {signal}")

    require("#define CARD_CTRL_SLOT_ETHERNET    1U" in regs,
            "PS register header must name Ethernet slot 1")
    require("CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_ETHERNET)" in regs,
            "Ethernet slot 1 should be part of the default enabled slot mask")
    require("#define CARD_CTRL_ETH_ADDR_REG" in regs and
            "#define CARD_CTRL_ETH_DATA_REG" in regs and
            "#define CARD_CTRL_ETH_CMD_REG" in regs and
            "#define CARD_CTRL_ETH_STATUS_REG" in regs and
            "CARD_CTRL_ETH_CMD_GO" in regs and
            "CARD_CTRL_ETH_STATUS_RDATA_SHIFT" in regs,
            "PS register header must expose Uthernet II host command registers")
    require("#define ETHERNET_CONTROL_SLOT 1U" in config,
            "Config menu must use Ethernet slot 1")
    require("#define CONFIG_DEFAULT_ETHERNET_SLOT1_ENABLED 1U" in config,
            "Ethernet slot 1 should be enabled by default for Uthernet II")
    require("ETHERNET_CONTROL_SLOT,\n                                        menu->ethernet_slot1_enabled" in config,
            "Config menu runtime apply must enable/disable slot 1")
    require("ethernet_read_config" in config_h and
            "ethernet_write_config" in config_h and
            "ethernet_test" in config_h and
            "ethernet_dhcp_acquire" in config_h,
            "Config menu platform must expose Ethernet read/write/test/DHCP hooks")
    require("CONFIG_ETHERNET_ITEM_COUNT" in config and
            "return CONFIG_ETHERNET_ITEM_COUNT;" in config,
            "Ethernet tab must expose the full network configuration control surface")
    require("ethernet.config.enabled" in config and
            "ethernet.address.mode" in config and
            "ethernet.mac" in config and
            "ethernet.ip" in config and
            "ethernet.subnet" in config and
            "ethernet.gateway" in config,
            "Config menu must persist Uthernet II network fields")
    require("config_menu_ethernet_read_from_card" in config and
            "config_menu_ethernet_write_to_card" in config and
            "config_menu_ethernet_dhcp" in config and
            "config_menu_ethernet_test" in config,
            "Ethernet tab must implement read/write/DHCP/test actions")
    require("static uint8_t config_menu_ethernet_card_access" in config and
            config.count("config_menu_ethernet_card_access(menu) == 0U") >= 5,
            "Every config-menu W5100 operation must enforce boot-menu ownership")
    toggle_start = config.find("static void config_menu_ethernet_toggle_saved_config")
    toggle_end = config.find("static void config_menu_ethernet_toggle_mode", toggle_start)
    require(toggle_start >= 0 and toggle_end > toggle_start and
            "config_menu_apply_ethernet_config" not in config[toggle_start:toggle_end],
            "Toggling saved config must not touch the card")
    require("menu->ethernet_address_mode == CONFIG_MENU_ETHERNET_ADDRESS_DHCP" in config and
            "return config_menu_acquire_ethernet_dhcp(menu, report);" in config and
            "menu->ethernet_config_enabled = 1U;" in config and
            "report != 0U && menu->session_only == 0U" in config,
            "Saved DHCP mode must negotiate a fresh lease during boot apply")
    dhcp_menu_start = config.find("static uint8_t config_menu_acquire_ethernet_dhcp")
    dhcp_menu_end = config.find("static uint8_t config_menu_apply_ethernet_config",
                                dhcp_menu_start)
    dhcp_menu = config[dhcp_menu_start:dhcp_menu_end]
    require(dhcp_menu_start >= 0 and dhcp_menu_end > dhcp_menu_start and
            dhcp_menu.find("ethernet_write_config(menu->platform.ctx") <
            dhcp_menu.find("ethernet_dhcp_acquire(menu->platform.ctx"),
            "DHCP must seed the card with saved config before preserving its fallback")
    require("uint8_t parsed[UTHERNET2_IPV4_LEN];" in config and
            "memcpy(ip, parsed, sizeof(parsed));" in config and
            "uint8_t parsed[UTHERNET2_MAC_LEN];" in config and
            "memcpy(mac, parsed, sizeof(parsed));" in config,
            "Invalid Ethernet fields must not partially modify saved values")
    require("config_menu_retry_settings_if_needed" in config and
            "config_menu_load_profile_settings" in config and
            "config_menu_apply_ethernet_config(menu, 0U)" in
            config[config.find("void config_menu_bind_platform"):
                   config.find("uint8_t config_menu_is_active")] and
            "config_menu_apply_ethernet_config(menu, 0U)" in
            config[config.find("void config_menu_retry_settings_if_needed"):
                   config.find("static uint8_t config_menu_days_in_month")] and
            "config_menu_apply_ethernet_config(menu, 0U)" not in
            config[config.find("uint8_t config_menu_load_profile_settings"):
                   config.find("uint8_t config_menu_save_profile_settings")],
            "Saved Ethernet config must apply after either initial or deferred boot settings load")
    require("static uint8_t last_ready = 0xFFU;" in frontend and
            "if (ready == last_ready)" in frontend and
            "last_ready == 0xFFU ||" not in frontend,
            "Boot-attached SD media must trigger the deferred settings load")
    test_start = config.find("static void config_menu_ethernet_test")
    test_end = config.find("static uint8_t config_menu_adjust_focused_value", test_start)
    require(test_start >= 0 and test_end > test_start and
            "menu->ethernet_address_mode == CONFIG_MENU_ETHERNET_ADDRESS_STATIC" in
            config[test_start:test_end] and
            "menu->platform.ethernet_write_config(menu->platform.ctx,\n"
            "                                                 &menu->ethernet_config)" in
            config[test_start:test_end] and
            "menu->platform.ethernet_test(menu->platform.ctx, &result)" in
            config[test_start:test_end] and
            config[test_start:test_end].find("ethernet_write_config") <
            config[test_start:test_end].find("ethernet_test(menu->platform.ctx"),
            "Test link must apply visible static config before probing the card")
    require("Configure network at boot" in config_tabs and
            "Get IP from network (DHCP)" in config_tabs and
            "Test link" in config_tabs,
            "Ethernet tab must draw saved-config, DHCP, and test controls")
    require(re.search(r"#define\s+W5100_REG_GAR\s+0x0001U", eth_control) and
            re.search(r"#define\s+W5100_REG_SUBR\s+0x0005U", eth_control) and
            re.search(r"#define\s+W5100_REG_SHAR\s+0x0009U", eth_control) and
            re.search(r"#define\s+W5100_REG_SIPR\s+0x000FU", eth_control),
            "Uthernet II control must use W5100 network configuration registers")
    parse_mac_start = config.find("static uint8_t config_menu_parse_mac")
    parse_mac_end = config.find("static void config_menu_coerce_ethernet",
                                parse_mac_start)
    require(parse_mac_start >= 0 and parse_mac_end > parse_mac_start and
            "uint8_t uthernet2_mac_is_valid" in eth_control and
            "(mac[0] & 0x01U) != 0U" in eth_control and
            "DHCP INVALID SOURCE MAC" in eth_control and
            "uthernet2_mac_is_valid(parsed) == 0U" in
            config[parse_mac_start:parse_mac_end],
            "Saved config and DHCP must reject multicast source MAC addresses")
    require("W5100S_REG_PHYSR" in eth_control and
            "W5100S_REG_VERR" in eth_control and
            "W5100S_PHYSR_LINK" in eth_control,
            "Uthernet II test must read W5100S identity and link state")
    require("DHCP_DISCOVER" in eth_control and
            "DHCP_REQUEST" in eth_control and
            "DHCP_ACK" in eth_control and
            "W5100_S0_MR_MACRAW_MF" in eth_control and
            "w5100_raw_open" in eth_control and
            "uthernet2_dhcp_acquire" in eth_control,
            "Uthernet II control must implement DHCP over MACRAW")
    wait_link = eth_control.find("static int w5100_wait_link")
    dhcp_start = eth_control.find("int uthernet2_dhcp_acquire")
    require(wait_link >= 0 and dhcp_start > wait_link and
            "if (w5100_wait_link() != 0)" in
            eth_control[dhcp_start:] and
            'dhcp_fail(&previous, detail, detail_len, "DHCP LINK DOWN")' in
            eth_control[dhcp_start:],
            "Boot DHCP must wait for PHY link before sending")
    require("static int w5100_reset" in eth_control and
            "if (w5100_reset() != 0)" in eth_control[dhcp_start:] and
            eth_control[dhcp_start:].find("w5100_reset()") <
            eth_control[dhcp_start:].find("w5100_raw_open(mac)"),
            "DHCP must reset the W5100S before switching to MACRAW")
    require("dhcp_build_frame" in eth_control and
            "dhcp_frame_payload" in eth_control and
            "put_be16(&frame[12], 0x0800U)" in eth_control and
            "ip[9] = 17U;" in eth_control,
            "DHCP must frame Ethernet, IPv4, and UDP in software")
    require("w5100_udp_open" not in eth_control and
            "W5100_SN_MR_UDP" not in eth_control,
            "DHCP must not use the W5100 hardware UDP engine")
    dhcp_body = eth_control[dhcp_start:]
    require("W5100_REG_GAR" not in dhcp_body and
            "W5100_REG_SUBR" not in dhcp_body and
            "W5100_REG_SIPR" not in dhcp_body.split(
                "uthernet2_write_network_config(lease)", 1)[0],
            "A failed DHCP request must not erase the active card configuration")
    require("W5100_SOCKET_MEM_4_2_1_1 0x06U" in eth_control and
            "state != W5100_S0_SR_MACRAW" in eth_control,
            "MACRAW must use a valid 8KB Contiki-compatible map and verify OPEN state")
    require("w5100_read16_stable" in eth_control and
            "w5100_read16_stable(W5100_S0_RX_RSR, &received_size)" in eth_control,
            "MACRAW receive polling must use a stable W5100 byte-count read")
    require("DHCP_PACKET_MIN" not in eth_control and
            "DHCP_OPTION(61U" not in eth_control and
            "DHCP_OPTION(55U, 3U)" in eth_control and
            "packet[pos++] = 6U;" in eth_control,
            "DHCP discover options must match Contiki without extra padding")
    require("uthernet2_read_network_config(&previous)" in eth_control[dhcp_start:] and
            "w5100_write_network_config(previous)" in eth_control and
            "DHCP OFFER NO RX" in eth_control and
            "DHCP OFFER INVALID RX" in eth_control,
            "DHCP failure must restore config and distinguish no RX from invalid RX")
    require("w5100_read16_stable(W5100_S0_TX_FSR, &free_size)" in eth_control and
            "if (free_size >= len)" in eth_control and
            "if (free_size < len ||" in eth_control,
            "MACRAW transmit must wait for stable W5100 TX space before SEND")
    read_start = config.find("static void config_menu_ethernet_read_from_card")
    read_end = config.find("static void config_menu_ethernet_write_to_card",
                           read_start)
    read_body = config[read_start:read_end]
    empty_check = read_body.find("config.ip[0] | config.ip[1] |")
    assign = read_body.find("menu->ethernet_config = config;")
    require(empty_check >= 0 and assign > empty_check and
            "UTHERNET II CARD CONFIG IS EMPTY" in read_body,
            "Reading an empty card config must preserve the saved menu values")
    require("uthernet2_mac_is_valid(config.mac) == 0U" in read_body and
            "UTHERNET II CARD MAC IS INVALID" in read_body,
            "Reading an invalid card MAC must preserve the saved menu values")
    require('"../../../ps_sources/frontend/uthernet2_control.c"' in vitis,
            "Uthernet II control source must be registered in Vitis")
    require("eth_host_error_q <= eth_host_error_q | eth_host_error;" in top and
            ".host_error(eth_host_error)" in top,
            "Host collision and overrun errors must remain sticky until the next GO")
    require("UTHERNET2 ARBITRATION PASS" in sim and
            "colliding Apple read was lost" in sim and
            "host started inside Apple write" in sim,
            "Behavioral simulation must cover host/Apple arbitration collisions")
if __name__ == "__main__":
    main()
