set -euo pipefail
OUT="dist/${PKG_OS}"
mkdir -p "$OUT"
cp "target/${TARGET}/release/mlc" "$OUT/"
for f in README.md README.zh-CN.md LICENSE THIRD-PARTY-NOTICES.md; do
  [ -f "$f" ] && cp "$f" "$OUT/"
done
if [ -f .github/assets/README-INSTALL-unix.txt ]; then
  cp .github/assets/README-INSTALL-unix.txt "$OUT/README-INSTALL.txt"
fi
tar -cJf "dist/mlc-${PKG_OS}.tar.xz" -C dist "${PKG_OS}"
ls -lh "dist/mlc-${PKG_OS}.tar.xz"
