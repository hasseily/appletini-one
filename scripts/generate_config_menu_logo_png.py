from pathlib import Path
import struct


def main() -> None:
    png_path = Path("Assets") / "config_menu_logo.png"
    data = png_path.read_bytes()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"{png_path} is not a PNG")

    width, height = struct.unpack(">II", data[16:24])
    if width == 0 or height == 0 or width > 640 or height > 96:
        raise RuntimeError(
            f"{png_path} must fit the config header, got {width}x{height}")

    out_dir = Path("ps_sources") / "frontend"
    h_path = out_dir / "config_menu_logo_png.h"
    c_path = out_dir / "config_menu_logo_png.c"

    h_path.write_text(
        "#ifndef CONFIG_MENU_LOGO_PNG_H\n"
        "#define CONFIG_MENU_LOGO_PNG_H\n\n"
        "#include <stddef.h>\n\n"
        "extern const unsigned char config_menu_logo_png[];\n"
        "extern const size_t config_menu_logo_png_len;\n"
        "extern const unsigned config_menu_logo_png_width;\n"
        "extern const unsigned config_menu_logo_png_height;\n\n"
        "#endif\n")

    rows = []
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        rows.append("    " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")

    c_path.write_text(
        "#include \"config_menu_logo_png.h\"\n\n"
        f"const unsigned config_menu_logo_png_width = {width}U;\n"
        f"const unsigned config_menu_logo_png_height = {height}U;\n"
        f"const size_t config_menu_logo_png_len = {len(data)}U;\n"
        "const unsigned char config_menu_logo_png[] = {\n"
        + "\n".join(rows) +
        "\n};\n")

    print(f"generated {c_path} from {png_path} ({width}x{height}, {len(data)} bytes)")


if __name__ == "__main__":
    main()
