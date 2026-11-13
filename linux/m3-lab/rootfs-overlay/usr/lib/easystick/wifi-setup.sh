# Sourced by /etc/profile in a root console shell.  S40network creates
# /run/.es-wifi-setup (its content is the reason) when it could not join the
# saved Wi-Fi network.  The function asks for another SSID and password on
# this console, writes them to /boot/config.txt and waits for the result.
# easystick-bootsync saves config.txt to flash and reloads the network, so the
# new network is also used at the next boot.
#
# Only shell builtins plus cat/ifconfig/grep are used: on NOMMU every other
# exec costs a ~0.5 MB copy of BusyBox and free RAM is small.  No subshells.

ES_WIFI_FLAG=${ES_WIFI_FLAG:-/run/.es-wifi-setup}
ES_WIFI_TMP=${ES_WIFI_TMP:-/run/.es-wifi-cfg-new}
ES_WIFI_CFG=${ES_WIFI_CFG:-/boot/config.txt}
ES_WIFI_OPER=${ES_WIFI_OPER:-/sys/class/net/wlan0/operstate}

es_wifi_up() {
	es_o=""
	read -r es_o 2>/dev/null < "$ES_WIFI_OPER" || return 1
	[ "$es_o" = up ]
}

# Print the saved values of wifi_ssid / wifi_psk from $1 into es_old_ssid /
# es_old_psk (surrounding blanks and CR removed).
es_wifi_saved() {
	es_old_ssid=""
	es_old_psk=""
	es_cr=$(printf '\r')
	while IFS= read -r es_l || [ -n "$es_l" ]; do
		es_l=${es_l%"$es_cr"}
		es_t=${es_l#"${es_l%%[![:blank:]]*}"}
		case "$es_t" in
			wifi_ssid*=*) es_v=${es_t#*=}; es_v=${es_v#"${es_v%%[![:blank:]]*}"}; es_old_ssid=${es_v#\"}; es_old_ssid=${es_old_ssid%\"} ;;
			wifi_psk*=*) es_v=${es_t#*=}; es_v=${es_v#"${es_v%%[![:blank:]]*}"}; es_old_psk=${es_v#\"}; es_old_psk=${es_old_psk%\"} ;;
		esac
	done < "$1"
}

# Write the new settings into config.txt: every old wifi_ssid/wifi_psk/wifi_mode
# line is dropped (config parsing takes the first match), the other lines stay.
es_wifi_write() {
	es_cfg=$ES_WIFI_CFG
	: > "$ES_WIFI_TMP" || return 1
	if [ -f "$es_cfg" ]; then
		while IFS= read -r es_l || [ -n "$es_l" ]; do
			es_t=${es_l#"${es_l%%[![:blank:]]*}"}
			case "$es_t" in
				wifi_ssid*|wifi_psk*|wifi_mode*) ;;
				*) printf '%s\n' "$es_l" >> "$ES_WIFI_TMP" ;;
			esac
		done < "$es_cfg"
	fi
	printf 'wifi_mode=sta\nwifi_ssid=%s\n' "$1" >> "$ES_WIFI_TMP"
	[ -n "$2" ] && printf 'wifi_psk=%s\n' "$2" >> "$ES_WIFI_TMP"
	cat "$ES_WIFI_TMP" > "$es_cfg"
	es_rc=$?
	rm -f "$ES_WIFI_TMP"
	return "$es_rc"
}

es_wifi_setup() {
	[ -f "$ES_WIFI_FLAG" ] || return 0
	while [ -f "$ES_WIFI_FLAG" ]; do
		if es_wifi_up; then
			echo "Wi-Fi is already connected."
			rm -f "$ES_WIFI_FLAG"
			return 0
		fi
		es_why=""
		read -r es_why < "$ES_WIFI_FLAG" || true
		es_wifi_saved "$ES_WIFI_CFG"
		echo
		echo "=== Wi-Fi is NOT connected (${es_why:-unknown}) ==="
		echo "Saved network: ${es_old_ssid:-none}"
		echo "Type the SSID and password of the network to use."
		echo "Empty SSID = keep the saved network and keep waiting for it."
		printf 'SSID: '
		IFS= read -r es_ssid || return 0
		if [ -z "$es_ssid" ]; then
			echo "Keeping the saved network."
			return 0
		fi
		printf 'Password (empty = open network): '
		IFS= read -r es_psk || return 0
		case "$es_ssid$es_psk" in
			*'"'*|*'\'*)
				echo "ERROR: SSID and password must not contain \" or \\"
				continue ;;
		esac
		if [ "${#es_ssid}" -gt 32 ]; then
			echo "ERROR: SSID is longer than 32 characters"
			continue
		fi
		if [ -n "$es_psk" ] && { [ "${#es_psk}" -lt 8 ] || [ "${#es_psk}" -gt 63 ]; }; then
			echo "ERROR: password must be 8-63 characters (you typed ${#es_psk})"
			continue
		fi
		echo "SSID=[$es_ssid]  password=[$es_psk]"
		printf 'Save and connect? [Y/n] '
		read -r es_ok || return 0
		case "$es_ok" in
			''|y|Y|yes|YES) ;;
			*) echo "Cancelled."; continue ;;
		esac
		if ! es_wifi_write "$es_ssid" "$es_psk"; then
			echo "ERROR: could not write $ES_WIFI_CFG"
			continue
		fi
		echo "Saved. Waiting for the network (up to 2.5 minutes)..."
		# bootsync reloads the network about 3 s after the write, but only if
		# the settings changed; identical settings need a reload by hand.
		if [ "$es_ssid" = "$es_old_ssid" ] && [ "$es_psk" = "$es_old_psk" ]; then
			/etc/init.d/S40network reload >/dev/null 2>&1 &
		fi
		es_n=0
		es_gone=0
		while [ "$es_n" -lt 150 ]; do
			sleep 1
			es_n=$((es_n + 1))
			if [ ! -f "$ES_WIFI_FLAG" ]; then
				es_gone=1
				if es_wifi_up; then
					echo "Connected:"
					ifconfig wlan0 | grep 'inet addr'
					return 0
				fi
			elif [ "$es_gone" -eq 1 ]; then
				break
			fi
		done
		echo "Not connected. Try again."
		[ -f "$ES_WIFI_FLAG" ] || echo "unknown" > "$ES_WIFI_FLAG"
	done
}
