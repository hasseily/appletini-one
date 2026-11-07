-- Observe the Apple and V30 buses without changing values or device state.
local machine = manager.machine
local apple = assert(machine.devices[":maincpu"], "Apple CPU not found")
local v30 = assert(machine.devices[":sl5:pcxport:v30"], "slot 5 V30 not found")
local output = assert(io.open("bus.tsv", "w"))
local taps = {}
local stop
local last_key
local first_time, last_time, repeats

output:write("first_s\tlast_s\tcount\tbus\tdirection\taddress\tdata\tmask\tpc\n")

local function flush()
    if last_key then
        output:write(string.format("%.9f\t%.9f\t%d\t%s\n", first_time, last_time, repeats, last_key))
    end
end

local function tap(cpu, space_name, bus, first, last, include)
    local space = assert(cpu.spaces[space_name], "address space not found: " .. space_name)
    local pc = assert(cpu.state["PC"], "CPU PC not found")
    local function capture(direction)
        return function(address, data, mask)
            local current_pc = pc.value
            if machine:side_effects_disabled() or (include and not include(address, current_pc, data)) then
                return
            end
            local time = machine.time:as_double()
            local key = string.format("%s\t%s\t%X\t%X\t%X\t%X", bus, direction, address, data, mask, current_pc)
            if key == last_key then
                last_time = time
                repeats = repeats + 1
            else
                flush()
                first_time, last_time, repeats, last_key = time, time, 1, key
            end
        end
    end
    taps[#taps + 1] = space:install_read_tap(first, last, "pct_read_" .. bus, capture("r"))
    taps[#taps + 1] = space:install_write_tap(first, last, "pct_write_" .. bus, capture("w"))
end

tap(apple, "program", "apple-keyboard", 0xc000, 0xc010, function(address, _, data)
    return address == 0xc010 or (address == 0xc000 and data & 0x80 ~= 0)
end)
tap(apple, "program", "apple-slot", 0xc0d0, 0xc7ff, function(address)
    return address <= 0xc0df or (address >= 0xc100 and address % 0x100 == 0)
end)
tap(apple, "program", "apple-c8", 0xc800, 0xcfff, function(address, pc)
    -- Skip the storage card's C8 ROM traffic, except for C8 release accesses.
    return address == 0xcfff or pc < 0xc100 or pc > 0xcffe
end)
tap(v30, "program", "v30-reset", 0xffff0, 0xfffff)
-- The card can reach this address space from both the Apple and the V30.
tap(v30, "io", "pc-io", 0x0000, 0xffff)

stop = emu.add_machine_stop_notifier(function()
    for _, handler in ipairs(taps) do
        handler:remove()
    end
    flush()
    output:close()
    local program = apple.spaces["program"]
    local screen = assert(io.open("apple-text-page.txt", "w"))
    for row = 0, 23 do
        local start = 0x400 + (row % 8) * 0x80 + (row // 8) * 0x28
        local text = {}
        for column = 0, 39 do
            local byte = program:read_u8(start + column) & 0x7f
            if byte < 0x20 then
                byte = byte + 0x40
            elseif byte == 0x7f then
                byte = 0x20
            end
            text[#text + 1] = string.char(byte)
        end
        screen:write(table.concat(text), "\n")
    end
    screen:close()
    local done = assert(io.open("capture.complete", "w"))
    done:write(string.format("%.9f\n%X\n%X\n",
        machine.time:as_double(), apple.state["PC"].value, v30.state["PC"].value))
    done:close()
    stop = nil
end)

local key_time = os.getenv("PCT_TRACE_KEY_TIME")
local keys = os.getenv("PCT_TRACE_KEYS")
if key_time and keys then
    if emu.wait(tonumber(key_time)) then
        emu.keypost(keys)
    end
end
