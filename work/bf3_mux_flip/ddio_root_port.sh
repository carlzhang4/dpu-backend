#!/usr/bin/env bash
set -euo pipefail

action=${1:-status}
root_port=0000:14:00.0
tool=/home/pimnic/ziyu/ddio-modify/build/ddio_modify

case "${action}" in
status)
	printf 'PERFCTRLSTS_0='
	sudo -n setpci -s "${root_port}" 180.l
	;;
off)
	sudo -n "${tool}" --enable=false --nic_bus=0x14
	printf 'PERFCTRLSTS_0='
	sudo -n setpci -s "${root_port}" 180.l
	;;
on)
	sudo -n "${tool}" --enable=true --nic_bus=0x14
	printf 'PERFCTRLSTS_0='
	sudo -n setpci -s "${root_port}" 180.l
	;;
*)
	echo "usage: $0 status|off|on" >&2
	exit 2
	;;
esac
