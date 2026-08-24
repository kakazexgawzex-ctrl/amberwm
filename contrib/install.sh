#!/bin/sh
# amberwm installer: deps, build, session integration.
# Package names assume Arch Linux; on other distros install the
# equivalents of wlroots-0.20, scenefx-0.5, wayland, libinput,
# pixman, xkbcommon and a C compiler, then re-run.
set -eu
cd "$(dirname "$0")/.."

say() { printf '\n\033[1;33m==>\033[0m %s\n' "$*"; }

say "Checking dependencies"
missing=""
pkg-config --exists wlroots-0.20 || missing="$missing wlroots0.20"
pkg-config --exists scenefx-0.5 || missing="$missing scenefx-0.5"
pkg-config --exists wayland-server || missing="$missing wayland"
pkg-config --exists xkbcommon || missing="$missing libxkbcommon"
command -v cc >/dev/null || missing="$missing base-devel"
if [ -n "$missing" ]; then
	say "Installing:$missing"
	if command -v pacman >/dev/null; then
		sudo pacman -S --needed $missing
	else
		say "Install the packages above manually, then re-run."
		exit 1
	fi
fi

say "Building amberwm"
make

say "Installing binary to /usr/local/bin (needs sudo)"
sudo install -Dm755 amberwm /usr/local/bin/amberwm

say "Installing systemd user unit"
install -Dm644 contrib/amberwm-user.service \
	"$HOME/.config/systemd/user/amberwm.service"
systemctl --user daemon-reload

say "Installing login-manager entry (needs sudo)"
printf '%s\n' \
'[Desktop Entry]' \
'Name=amberwm' \
'Comment=AmberWM tiling compositor' \
'Exec=systemctl --user --wait start amberwm' \
'Type=Application' \
'DesktopNames=amberwm' \
| sudo tee /usr/share/wayland-sessions/amberwm.desktop >/dev/null

if [ ! -f "$HOME/.config/amberwm/amberwm.cfg" ]; then
	mkdir -p "$HOME/.config/amberwm"
	printf '# amberwm config\n# animations=yes\n# gaps=8\n' \
		> "$HOME/.config/amberwm/amberwm.cfg"
fi

say "Done. Select 'amberwm' at your login manager and log out."
say "Manual start: systemctl --user --wait start amberwm"
say "Remove again with: contrib/uninstall.sh"
