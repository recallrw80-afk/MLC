set -euo pipefail
OUT="dist/${PKG_OS}"
mkdir -p "$OUT"
cp "target/${TARGET}/release/mlc.exe" "$OUT/"
for f in README.md README.zh-CN.md LICENSE THIRD-PARTY-NOTICES.md; do
  [ -f "$f" ] && cp "$f" "$OUT/"
done
if [ -f .github/assets/README-INSTALL-windows.txt ]; then
  cp .github/assets/README-INSTALL-windows.txt "$OUT/README-INSTALL.txt"
fi
cd dist && 7z a "../mlc-${PKG_OS}.zip" "${PKG_OS}" > /dev/null && cd ..
ls -lh "mlc-${PKG_OS}.zip"
