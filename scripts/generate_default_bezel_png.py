from pathlib import Path
import struct


def main() -> None:
    png_path = Path("Assets") / "bezel_default.png"
    data = png_path.read_bytes()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"{png_path} is not a PNG")

    width, height = struct.unpack(">II", data[16:24])
    if width != 1920 or height == 0 or height > 1080:
        raise RuntimeError(
            f"{png_path} must be 1920 pixels wide and <=1080 high, got {width}x{height}")

    out_dir = Path("ps_sources") / "frontend"
    h_path = out_dir / "bezel_default_png.h"
    c_path = out_dir / "bezel_default_png.c"

    h_path.write_text(
        "#ifndef BEZEL_DEFAULT_PNG_H\n"
        "#define BEZEL_DEFAULT_PNG_H\n\n"
        "#include <stddef.h>\n\n"
        "extern const unsigned char bezel_default_png[];\n"
        "extern const size_t bezel_default_png_len;\n"
        "extern const unsigned bezel_default_png_width;\n"
        "extern const unsigned bezel_default_png_height;\n\n"
        "#endif\n")

    rows = []
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        rows.append("    " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")

    c_path.write_text(
        "#include \"bezel_default_png.h\"\n\n"
        f"const unsigned bezel_default_png_width = {width}U;\n"
        f"const unsigned bezel_default_png_height = {height}U;\n"
        f"const size_t bezel_default_png_len = {len(data)}U;\n"
        "const unsigned char bezel_default_png[] = {\n"
        + "\n".join(rows) +
        "\n};\n")

    print(f"generated {c_path} from {png_path} ({width}x{height}, {len(data)} bytes)")


if __name__ == "__main__":
    main()
