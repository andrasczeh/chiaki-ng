#!/usr/bin/env bash
# Build and install the chiaki-ng Flatpak from THIS checkout (instead of GitHub main).
# Meant to be run natively on an aarch64 device such as the Steam Frame (Desktop Mode),
# but works on x86_64 too.
#
#   ./scripts/flatpak/build-local.sh
#
# Afterwards: flatpak run io.github.andrasczeh.ChiakiNgVR
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$repo_root/.flatpak-local"
manifest="io.github.andrasczeh.ChiakiNgVR.yaml"
mkdir -p "$work"

# Patches and module files referenced relatively by the manifest must sit next to it.
cp "$repo_root"/scripts/flatpak/*.patch "$repo_root"/scripts/flatpak/*.yaml "$work"/

# Point the chiaki-ng module at the local checkout instead of GitHub main.
python3 - "$work/$manifest" "$repo_root" <<'PYEOF'
import sys
path, root = sys.argv[1:]
s = open(path).read()
old = """      - type: git
        url: https://github.com/chiaki-ng/chiaki-ng.git
        branch: main"""
if old not in s:
    sys.exit("could not find the chiaki-ng git source in the manifest")
s = s.replace(old, f"""      - type: dir
        path: {root}
        skip:
          - .flatpak-local
          - .git""")
open(path, "w").write(s)
PYEOF

if [ "${1:-}" = "--prepare-only" ]; then
    echo "Prepared $work/$manifest"
    exit 0
fi

if ! flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    flatpak install --user -y flathub org.flatpak.Builder
fi

cd "$work"
flatpak run org.flatpak.Builder \
    --user --install --force-clean \
    --install-deps-from=flathub \
    --disable-rofiles-fuse \
    --state-dir="$work/.flatpak-builder" \
    build-dir "$manifest"

echo
echo "Installed. Run with: flatpak run io.github.andrasczeh.ChiakiNgVR"
echo "Then pick 'v4l2m2m' (or 'auto') under Settings -> Video -> Hardware Decoder."
