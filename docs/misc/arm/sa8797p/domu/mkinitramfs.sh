#!/bin/bash
# mkinitramfs.sh <busybox> <libdir> <out.cpio.gz>: the guest initramfs used
# by the domU configurations here: an arm64 BusyBox, the shared libraries it
# needs (taken from <libdir>, e.g. the dom0 rootfs's /lib), init and
# poweroff from this directory. Needs no root: the console device nodes are
# appended as a second cpio archive.
set -euo pipefail
BB=${1:?busybox} LIB=${2:?libdir} OUT=$(readlink -f "${3:?out}")
D=$(dirname "$(readlink -f "$0")")
READELF=${READELF:-aarch64-linux-gnu-readelf}
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT
mkdir -p "$R"/bin "$R"/sbin "$R"/proc "$R"/sys "$R"/dev "$R"/lib "$R"/usr
ln -sfn lib "$R"/lib64
ln -sfn ../lib "$R"/usr/lib
ln -sfn ../lib "$R"/usr/lib64
cp -L "$BB" "$R"/bin/busybox
for l in $("$READELF" -d "$BB" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p') ld-linux-aarch64.so.1; do
	cp -L "$LIB/$l" "$R"/lib/
done
install -m 755 "$D"/init "$R"/init
install -m 755 "$D"/poweroff "$R"/sbin/poweroff
(cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet | gzip -9) > "$OUT"
python3 -I - "$OUT" <<'PY'
import gzip, sys

def ent(name, mode, rdev=(0, 0)):
    n = name.encode() + b"\0"
    h = "070701" + "".join("%08X" % v for v in
                           (0, mode, 0, 0, 1, 0, 0, 0, 0, rdev[0], rdev[1], len(n), 0))
    b = h.encode() + n
    return b + b"\0" * ((4 - len(b) % 4) % 4)

d = ent("dev/console", 0o020620, (5, 1)) + ent("dev/hvc0", 0o020620, (229, 0))
d += ent("TRAILER!!!", 0)
d += b"\0" * ((512 - len(d) % 512) % 512)
open(sys.argv[1], "ab").write(gzip.compress(d))
PY
