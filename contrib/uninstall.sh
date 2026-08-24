#!/bin/sh
# amberwm uninstaller: removes session integration, keeps your config.
set -eu
systemctl --user stop amberwm 2>/dev/null || true
systemctl --user disable amberwm 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/amberwm.service"
systemctl --user daemon-reload
sudo rm -f /usr/share/wayland-sessions/amberwm.desktop
sudo rm -f /usr/local/bin/amberwm
echo "amberwm removed. Config kept in ~/.config/amberwm."
