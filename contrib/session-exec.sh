#!/bin/sh
# Bind libseat-logind to the user's ACTIVE login session. Without
# XDG_SESSION_ID in its environment, libseat may attach to a stale
# session that never becomes active, and wlroots aborts with
# "Timeout waiting session to become active" after 10s.
sid=$(loginctl list-sessions --no-legend 2>/dev/null |
	awk -v u="$(id -u)" '$2==u {print $1}' |
	while read -r s; do
		[ "$(loginctl show-session "$s" -p Active --value)" = yes ] || continue
		[ "$(loginctl show-session "$s" -p Class --value)" = user ] || continue
		echo "$s"
	done | tail -n1)
[ -n "$sid" ] && export XDG_SESSION_ID="$sid"
exec /home/watrib/Projects/amberwm/amberwm
