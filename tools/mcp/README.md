# Appletini MCP server

Exposes a live Apple II (via the Appletini card) as MCP tools:

    MCP client -> appletini_mcp.py -> serial UART -> ARM firmware
               -> FPGA fabric -> Apple II bus

## Tools

| Tool | What it does |
| --- | --- |
| `apple2_screen_text` | The live text screen as plain text (40/80 col auto). Reads the DDR write-mirror shadow -- zero bus impact, works under graphics modes (where error messages hide). |
| `apple2_peek` | Hex dump of Apple main RAM (from the shadow; mirrors writes since power-on). |
| `apple2_soft_switches` | Decoded //e soft-switch state + RamWorks bank. |
| `apple2_machine_status` | Machine id, aux-provide, RamWorks, physical-aux-card probe, firmware status. |
| `apple2_health` | Serve-path counters (write-queue drops / deadline misses / lost cycles) -- all must be zero. |
| `apple2_bus_trace` | Recent bus cycles from the trace ring, with history offset. |
| `apple2_menu_key` | Drive the Appletini config menu remotely. |
| `apple2_console` | Escape hatch: any firmware console command (`help` lists them). |

## Setup

```
pip install mcp pyserial
```

The firmware console is a single shared channel: close any terminal
program (PuTTY, TeraTerm) holding the COM port first.

Register with Claude Code (adjust the port):

```
claude mcp add appletini -e APPLETINI_PORT=COM5 -- python tools/mcp/appletini_mcp.py
```

Or in an `.mcp.json`:

```json
{
  "mcpServers": {
    "appletini": {
      "command": "python",
      "args": ["tools/mcp/appletini_mcp.py"],
      "env": { "APPLETINI_PORT": "COM5" }
    }
  }
}
```

## Notes and limits

- `reboot`/`reset` console commands restart the Appletini firmware,
  not the Apple II.
- `apple2_menu_key` drives only the Appletini configuration menu; the MCP
  server does not inject Apple keyboard input.
- The memory shadows mirror *writes* since power-on: ROM regions and
  never-written RAM read as zero in `apple2_peek`.
