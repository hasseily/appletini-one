"""Read-only DOS 3.3 catalog and Applesoft extraction for profiling inputs."""
from pathlib import Path
import hashlib
import json
import re

SOURCE = Path(r"C:/Users/hasse/Downloads/short-programs.dsk")
OUT = Path(__file__).resolve().parent
TOKENS = [
    "END", "FOR", "NEXT", "DATA", "INPUT", "DEL", "DIM", "READ", "GR", "TEXT", "PR#", "IN#", "CALL", "PLOT", "HLIN", "VLIN",
    "HGR2", "HGR", "HCOLOR=", "HPLOT", "DRAW", "XDRAW", "HTAB", "HOME", "ROT=", "SCALE=", "SHLOAD", "TRACE", "NOTRACE", "NORMAL", "INVERSE", "FLASH",
    "COLOR=", "POP", "VTAB", "HIMEM:", "LOMEM:", "ONERR", "RESUME", "RECALL", "STORE", "SPEED=", "LET", "GOTO", "RUN", "IF", "RESTORE", "&",
    "GOSUB", "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "DEF", "POKE", "PRINT", "CONT", "LIST", "CLEAR", "GET", "NEW",
    "TAB(", "TO", "FN", "SPC(", "THEN", "AT", "NOT", "STEP", "+", "-", "*", "/", "^", "AND", "OR", ">", "=", "<",
    "SGN", "INT", "ABS", "USR", "FRE", "SCRN(", "PDL", "POS", "SQR", "RND", "LOG", "EXP", "COS", "SIN", "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$",
]


def word(b, offset):
    return b[offset] | (b[offset + 1] << 8)


def detokenize(payload):
    result = []
    offset = 0
    while offset + 2 <= len(payload):
        next_line = word(payload, offset)
        if next_line == 0:
            break
        if offset + 4 >= len(payload):
            raise ValueError("Truncated BASIC line")
        number = word(payload, offset + 2)
        end = payload.index(0, offset + 4)
        body = payload[offset + 4:end]
        rendered = []
        in_quote = False
        in_rem = False
        in_data = False
        for byte in body:
            if byte == 34 and not in_rem:
                in_quote = not in_quote
            if byte == 58 and not in_quote and not in_rem:
                in_data = False
            if byte >= 128 and not in_quote and not in_rem and not in_data:
                token = TOKENS[byte - 128] if byte - 128 < len(TOKENS) else f"<TOKEN:{byte:02X}>"
                rendered.append(f" {token} ")
                in_rem = token == "REM"
                in_data = token == "DATA"
            elif 32 <= byte <= 126:
                rendered.append(chr(byte))
            else:
                rendered.append(f"<${byte:02X}>")
        result.append({"line": number, "next_address": next_line, "body_hex": body.hex(), "text": "".join(rendered).strip()})
        offset = end + 1
    return result


def main():
    disk = SOURCE.read_bytes()

    def sector(track, sec):
        start = (track * 16 + sec) * 256
        data = disk[start:start + 256]
        if len(data) != 256:
            raise ValueError((track, sec))
        return data

    vtoc = sector(17, 0)
    track, sec = vtoc[1:3]
    files = []
    visited = set()
    while track:
        if (track, sec) in visited:
            raise ValueError("Catalog cycle")
        visited.add((track, sec))
        cat = sector(track, sec)
        for entry_offset in range(11, 256, 35):
            entry = cat[entry_offset:entry_offset + 35]
            if len(entry) != 35 or entry[0] in (0, 255):
                continue
            name = bytes(c & 127 for c in entry[3:33]).decode("ascii").rstrip()
            typ = entry[2] & 127
            ts_track, ts_sec = entry[:2]
            blocks = {}
            ts_seen = set()
            while ts_track:
                if (ts_track, ts_sec) in ts_seen:
                    raise ValueError("T/S cycle")
                ts_seen.add((ts_track, ts_sec))
                ts = sector(ts_track, ts_sec)
                first = word(ts, 5)
                for i in range(122):
                    data_track, data_sec = ts[12 + i * 2:14 + i * 2]
                    if data_track:
                        blocks[first + i] = sector(data_track, data_sec)
                ts_track, ts_sec = ts[1:3]
            content = b"".join(blocks.get(i, bytes(256)) for i in range(max(blocks, default=-1) + 1))
            safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
            item = {"name": name, "type_code": typ, "type": {0: "TEXT", 1: "INTEGER BASIC", 2: "APPLESOFT", 4: "BINARY"}.get(typ, "OTHER"), "locked": bool(entry[2] & 128), "catalog_sectors": word(entry, 33), "run_command": "RUN " + name if typ == 2 else None}
            if typ == 2:
                length = word(content, 0)
                payload = content[2:2 + length]
                lines = detokenize(payload)
                text_path = OUT / f"{safe_name}.bas.txt"
                text_path.write_text("\n".join(f"{ln['line']} {ln['text']}" for ln in lines) + "\n", encoding="utf-8")
                (OUT / f"{safe_name}.tokens.bin").write_bytes(payload)
                item.update({"length_bytes": length, "lines": lines, "listing": text_path.name, "tokenized_sha256": hashlib.sha256(payload).hexdigest()})
            else:
                (OUT / f"{safe_name}.raw.bin").write_bytes(content)
            files.append(item)
        track, sec = cat[1:3]
    result = {"source": str(SOURCE), "size_bytes": len(disk), "sha256": hashlib.sha256(disk).hexdigest(), "format": "DOS 3.3, 35 tracks, 16 sectors, DOS sector order", "dos_release": vtoc[3], "volume": vtoc[6], "files": files}
    (OUT / "inventory.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for item in files:
        print(f"{item['type']:10} {item['catalog_sectors']:3} {item['name']:25} {item.get('length_bytes', '')}")
        for ln in item.get("lines", []):
            print(f"  {ln['line']} {ln['text']}")


if __name__ == "__main__":
    main()
