#!/bin/bash
# get_system_info.sh — ライブ Linux のシステム情報を記録（get_system_info.cmd の Linux 版）
#
# 使い方:  ./get_system_info.sh CASE_NO EVIDENCE_ID
# 出力:    CASES/<CASE_NO>/<EVIDENCE_ID>/logs/system-info.log
#
# ライブ取得なので、OS が動いている間の状態を押さえる。保全の前後で一度ずつ
# 取っておくと、作業中に何が変わったかを後から説明できる。

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # 兄弟スクリプトを呼ぶため（CASES はカレント基準）
CASE_NO="${1:-}"
EVID="${2:-}"
if [ -z "$CASE_NO" ] || [ -z "$EVID" ]; then
  echo "Usage: $0 CASE_NO EVIDENCE_ID"; exit 1
fi

"$SCRIPT_DIR/create_case_dirs.sh" "$CASE_NO" "$EVID" >/dev/null || exit 1
LOG="$PWD/CASES/$CASE_NO/$EVID/logs/system-info.log"
echo "Writing: $LOG"

{
  echo "====================================="
  echo "System Information"
  echo "Case=$CASE_NO   Evidence=$EVID   Date=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "====================================="
  echo; echo "[HOSTNAME]";     hostname
  echo; echo "[WHOAMI/ID]";    id
  echo; echo "[UNAME]";        uname -a
  echo; echo "[OS-RELEASE]";   cat /etc/os-release 2>/dev/null
  echo; echo "[UPTIME]";       uptime
  echo; echo "[TIMEZONE]";     timedatectl 2>/dev/null || cat /etc/timezone 2>/dev/null
  echo; echo "[IP ADDR]";      ip addr
  echo; echo "[IP ROUTE]";     ip route
  echo; echo "[DNS]";          cat /etc/resolv.conf 2>/dev/null
  echo; echo "[BLOCK DEVICES]";lsblk -o NAME,TYPE,SIZE,RO,RM,FSTYPE,MOUNTPOINT,MODEL,SERIAL,TRAN
  echo; echo "[MOUNTS]";       mount
  echo; echo "[LOADED MODULES (head)]"; lsmod 2>/dev/null | head -50
} > "$LOG" 2>&1

echo "Done."
echo "$LOG"
