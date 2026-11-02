#!/usr/bin/env bash
# Build the TrailSafe firmware: upstream Meshtastic at UPSTREAM_REF + overlay/ + patches/. Used locally and in CI.
set -euo pipefail
shopt -s nullglob
cd "$(dirname "$0")"
REF=$(<UPSTREAM_REF)
ENV=tracker-t1000-e-trailsafe

if [ "$(git -C upstream describe --tags --exact-match 2>/dev/null || true)" != "$REF" ]; then
    rm -rf upstream
    git clone -q --depth 1 --branch "$REF" https://github.com/meshtastic/firmware upstream
fi
# drop the previous overlay and patches; reset --hard because apply --3way stages into the index. .pio/ survives.
git -C upstream reset -q --hard && git -C upstream clean -qfd
cp -r overlay/. upstream/
for p in patches/*.patch; do
    git -C upstream apply --3way "../$p"
done

[ -x .venv/bin/pio ] || { python3 -m venv .venv && .venv/bin/pip -q install platformio==6.2.0; }
.venv/bin/pio run -d upstream -e "$ENV"

mkdir -p dist
rm -f dist/*.uf2
for f in upstream/.pio/build/$ENV/firmware-*.uf2; do
    cp "$f" "dist/trailsafe-$(git rev-parse --short HEAD 2>/dev/null || echo dev)-$(basename "$f")"
done
ls -l dist/
