#!/usr/bin/env bash
# Builds dist/$PORT_SLUG-windows-x86_64.zip after `bash build.sh` in an MSYS2 CLANG64 shell:
# "$PORT_NAME.exe" (the launcher, frozen with PyInstaller so players need no Python),
# "Play $PORT_NAME.exe" (starts the game with the saved settings), out/tlg-probe.exe with
# the CLANG64 DLLs it loads, the preparation scripts and the player README. No game or console
# file may ship. Freezing uses a Windows Python 3.10+ (python.org or actions/setup-python;
# WINPYTHON overrides) and a private venv in out/pyenv.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# shellcheck source=../port.env
source packaging/port.env
source ./msys2-env.sh
[[ -f out/tlg-probe.exe && -f out/tlg-gpu-capabilities.exe && -f out/tlg-play.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
name=${TLG_PACKAGE_NAME:-$PORT_SLUG-windows-x86_64}

python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe \
                     /c/hostedtoolcache/windows/Python/3*/x64/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller iced-x86==1.21.0
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request runpy ctypes.wintypes msvcrt iced_x86; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name "$PORT_NAME" --icon "$(cygpath -w "$PWD/launcher/$PORT_ICON.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" \
    --paths "$(cygpath -w "$PWD/launcher")" "${hidden[@]}" "$(cygpath -w "$PWD/launcher/$PORT_LAUNCHER")"

dest=out/stage/$name
rm -rf -- out/stage
mkdir -p "$dest/out" "$dest/launcher" "$dest/system_lib"
cp -r "out/pyi-dist/$PORT_NAME/." "$dest/"
llvm-strip -o "$dest/Play $PORT_NAME.exe" out/tlg-play.exe
cp launcher/$PORT_ICON.ico launcher/$PORT_ICON.png "$dest/launcher/"
# The executables without debug information, where tlg_run.py looks for them (out/).
for exe in tlg-probe.exe tlg-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/out/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/out/tlg-probe.exe" "$dest/out/tlg-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/out/"
    done
cp -r scripts "$dest/"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
cp LICENSE packaging/README-player.md "$dest/"
printf '%s\r\n' 'Este jogo não precisa de módulos do firmware do console: a pasta fica vazia.' \
    'This game needs no console firmware modules: leave this folder empty.' > "$dest/system_lib/LEIA-ME.txt"
bash packaging/check_package.sh "$dest"
mkdir -p dist
rm -f "dist/$name.zip"
(cd out/stage && powershell -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '../../dist/$name.zip'")
du -sh "$dest" "dist/$name.zip"
