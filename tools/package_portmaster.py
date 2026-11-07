"""Builds heroeslore.zip for PortMaster from cpp/dist/portmaster and the aarch64 binary.

Usage: python3 tools/package_portmaster.py <heroes_lore aarch64 binary> <output.zip>
"""
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIST = ROOT / "cpp" / "dist" / "portmaster"


def add(archive, source, name, executable=False):
    info = zipfile.ZipInfo(name)
    info.compress_type = zipfile.ZIP_DEFLATED
    # Unix mode in the high bits: launchers and the binary must stay executable on the device.
    info.external_attr = (0o100755 if executable else 0o100644) << 16
    data = source.read_bytes()
    if source.suffix in (".sh", ".md", ".json"):
        data = data.replace(b"\r\n", b"\n")  # the device's bash rejects CRLF scripts
    archive.writestr(info, data)


def main():
    binary, output = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    with zipfile.ZipFile(output, "w") as archive:
        add(archive, DIST / "Heroes Lore Wind of Soltia.sh", "Heroes Lore Wind of Soltia.sh", executable=True)
        for item in sorted((DIST / "heroeslore").iterdir()):
            add(archive, item, "heroeslore/" + item.name)
        add(archive, binary, "heroeslore/heroes_lore.aarch64", executable=True)
    print(output, output.stat().st_size)


if __name__ == "__main__":
    main()
