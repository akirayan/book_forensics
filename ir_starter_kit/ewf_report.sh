#!/bin/bash
# ewf_report.sh — CASES/<CASE>/logs のログから Markdown 保全レポートを作る
#                 （ewf_report.cmd の Linux 版）。
#
# 使い方:  ./ewf_report.sh CASE_NO TARGET
#   TARGET: ewf_imager.sh で取得したときのデバイス名（例 sdb）。ログ名の接頭辞になる。
#   例:    cd /mnt/ssd && /mnt/fileserver/scripts/ewf_report.sh C26010 sdb
#
# CASES はカレントフォルダ基準。証拠を置いた媒体へ cd してから使う（他の .sh と同じ）。

set -u
CASE_NO="${1:-}"
TARGET="${2:-}"
if [ -z "$CASE_NO" ] || [ -z "$TARGET" ]; then
    echo "Usage: $0 CASE_NO TARGET   (TARGET = device name used at acquisition, e.g. sdb)"
    exit 1
fi

CASE_ROOT="$PWD/CASES/$CASE_NO"
LOG_DIR="$CASE_ROOT/logs"
ACQ_DIR="$CASE_ROOT/acquisition"
REPORT_DIR="$CASE_ROOT/reports"

DISKINFO="$LOG_DIR/disk-info.log"
SYSINFO="$LOG_DIR/system-info.log"
ACQLOG="$LOG_DIR/${TARGET}_acquisition.log"
VERIFYLOG="$LOG_DIR/${TARGET}_verify.log"
INFOLOG="$LOG_DIR/${TARGET}_info.log"
E01TXT="$ACQ_DIR/${CASE_NO}_${TARGET}.E01.txt"

if [ ! -d "$LOG_DIR" ]; then
    echo "ERROR: $LOG_DIR が無い。証拠を置いた媒体へ cd し、取得後に実行する。"
    exit 1
fi
mkdir -p "$REPORT_DIR"
REPORT="$REPORT_DIR/acquisition_report.md"

{
  echo "# Acquisition Report"
  echo
  echo "## Case Information"
  echo
  echo "| Item | Value |"
  echo "|------|-------|"
  echo "| Case Number | $CASE_NO |"
  echo "| Target | $TARGET |"
  echo "| Generated | $(date '+%Y-%m-%d %H:%M:%S') |"
  echo
  echo "---"
  echo
  echo "## Available Logs"
  echo
  echo '```text'
  for f in "$DISKINFO" "$SYSINFO" "$ACQLOG" "$VERIFYLOG" "$INFOLOG"; do
    [ -e "$f" ] && basename "$f"
  done
  echo '```'
  echo
  echo "---"
  echo
  echo "## Summary (E01.txt)"
  echo
  echo '```text'
  if [ -e "$E01TXT" ]; then cat "$E01TXT"; else echo "(no E01.txt)"; fi
  echo '```'
  echo
  echo "---"
  echo
  echo "## Verification"
  echo
  echo '```text'
  [ -e "$VERIFYLOG" ] && grep -E "hash (stored|calculated)|ewfverify:" "$VERIFYLOG"
  echo '```'
  echo
  echo "---"
  echo
  echo "## EWF Metadata (ewfinfo)"
  echo
  echo '```text'
  [ -e "$INFOLOG" ] && sed -n '1,100p' "$INFOLOG"
  echo '```'
  echo
  echo "---"
  echo
  echo "## Evidence Files"
  echo
  echo '```text'
  ls -1 "$ACQ_DIR" 2>/dev/null
  echo '```'
  echo
  echo "---"
  echo
  echo "## Examiner Notes"
  echo
  echo "- Acquisition completed."
  echo "- Review logs for details."
  echo "- Verify hash values before evidence submission."
} > "$REPORT"

echo "Done."
echo "$REPORT"
