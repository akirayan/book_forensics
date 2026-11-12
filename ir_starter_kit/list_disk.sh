#!/bin/bash
# list_disk.sh — 保全対象を選ぶためのディスク/パーティション一覧（list_disk.cmd の Linux 版）
#
# 使い方:  ./list_disk.sh [CASE_NO]
#   CASE_NO を渡すと CASES/<CASE_NO>/logs/disk-info.log にも保存する。
#
# 判断はしない。どのデバイスを保全するかは調査担当者が決める。
# 保全先（この SSD）を誤って取得しないよう、SIZE / MODEL / SERIAL / TRAN(接続) /
# RM(取り外し可) をよく見て取り違えないこと。

set -u
CASE_NO="${1:-}"   # CASE_NO を渡すと カレントの CASES/<CASE_NO>/logs/ にも保存する

run() {
  echo "===================================="
  echo "Disk Information   $(date '+%Y-%m-%d %H:%M:%S')"
  echo "===================================="
  echo
  echo "[BLOCK DEVICES]  (RM=1 は取り外し可能＝USB等。保全先 SSD もここに出る)"
  lsblk -o NAME,TYPE,SIZE,RO,RM,FSTYPE,LABEL,MOUNTPOINT,MODEL,SERIAL,TRAN
  echo
  echo "[PARTITION TABLES]"
  for d in /dev/sd? /dev/nvme?n? /dev/vd? /dev/mmcblk?; do
    [ -b "$d" ] || continue
    echo "--- $d ---"
    sudo fdisk -l "$d" 2>/dev/null
    echo
  done
  echo "[LVM]"
  sudo pvs 2>/dev/null
  sudo vgs 2>/dev/null
  sudo lvs 2>/dev/null
}

if [ -n "$CASE_NO" ]; then
  LOG="$PWD/CASES/$CASE_NO/logs/disk-info.log"
  if [ ! -d "$(dirname "$LOG")" ]; then
    echo "ERROR: 先に  ./create_case_dirs.sh $CASE_NO  を実行する。"
    exit 1
  fi
  run | tee "$LOG"
  echo "Log: $LOG"
else
  run
fi
