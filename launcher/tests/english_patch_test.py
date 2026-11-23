"""English patch test for CI (no game files): makes a fake extracted game, a VCDIFF (xdelta) patch
for one of its files inside a .zip, puts the .zip into the "English Patch" folder and checks that
optroy_launcher_tool unpacks it (with the system's tar) and applies, removes and re-applies it.

    python english_patch_test.py <optroy_launcher_tool> <empty work folder>
"""
import os
import subprocess
import sys
import zipfile


def varint(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def vcdiff(source, target_extra):
    """RFC 3284 patch: target = whole source, then target_extra (one COPY, one ADD)."""
    data = target_extra                         # ADD data
    inst = bytes([19]) + varint(len(source))    # COPY, size follows, mode 0 (VCD_SELF)
    inst += bytes([1]) + varint(len(target_extra))  # ADD, size follows
    addr = varint(0)                            # COPY from source offset 0
    target_size = len(source) + len(target_extra)
    body = varint(target_size) + bytes([0]) + varint(len(data)) + varint(len(inst)) + varint(len(addr))
    body += data + inst + addr
    window = bytes([0x01]) + varint(len(source)) + varint(0) + varint(len(body)) + body
    return bytes([0xD6, 0xC3, 0xC4, 0x00, 0x00]) + window


def run(tool, folder, what):
    r = subprocess.run([tool, "english", folder, what], capture_output=True, text=True)
    print(r.stdout + r.stderr)
    return r.returncode


def main():
    tool, work = sys.argv[1], os.path.abspath(sys.argv[2])
    game = os.path.join(work, "RecompData", "source", "game")
    xui = os.path.join(game, "Contents", "textures", "XUI")
    os.makedirs(xui)
    with open(os.path.join(game, ".optroy-extracted"), "w") as f:
        f.write("files=1\n")
    with open(os.path.join(game, "default.xex"), "w") as f:  # placeholder text, not a real game file
        f.write("placeholder\n")
    original = os.urandom(50000)
    with open(os.path.join(xui, "MenuTop.xzp"), "wb") as f:
        f.write(original)
    english = original + b"ENGLISH" * 100
    patch_dir = os.path.join(work, "English Patch")
    os.makedirs(patch_dir)
    with zipfile.ZipFile(os.path.join(patch_dir, "MSGOT_English.zip"), "w") as z:
        z.writestr("MSGOT/XUI/MenuTop.xzp.xdelta", vcdiff(original, b"ENGLISH" * 100))

    def content():
        with open(os.path.join(xui, "MenuTop.xzp"), "rb") as f:
            return f.read()

    ok = True
    ok &= run(tool, work, "apply") == 0 and content() == english
    print("apply (from .zip):", "OK" if ok else "FAILED")
    ok &= run(tool, work, "remove") == 0 and content() == original
    print("remove:", "OK" if ok else "FAILED")
    ok &= run(tool, work, "apply") == 0 and content() == english
    with open(os.path.join(xui, "MenuTop.xzp"), "wb") as f:  # extracted again
        f.write(original)
    ok &= run(tool, work, "extracted") == 0 and content() == english
    print("re-applied after extracting:", "OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
