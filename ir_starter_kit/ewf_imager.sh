#!/bin/bash
# ewf_imager.sh — Linux 版ディスク保全（ewf_imager.cmd と対。libewf / ewftools 使用）
#
# 使い方:
#   ./ewf_imager.sh CASE_NO EVIDENCE_ID DESCRIPTION EXAMINER TARGET
#     EVIDENCE_ID: 証拠(PC/サーバ/HDD)ごとの ID（例 E001）
#     TARGET: ブロックデバイス名（例 sdb, nvme0n1）または絶対パス（/dev/sdb）
#
# 例:
#   ./ewf_imager.sh C26010 E001 "Dell Latitude 5540, disk0" endoaki sdb
#
# 出力は キット直下 CASES/<CASE_NO>/<EVIDENCE_ID>/ に保存する:
#   acquisition/<CASE_NO>_<EVIDENCE_ID>_<dev>.E01        … イメージ
#   acquisition/<CASE_NO>_<EVIDENCE_ID>_<dev>.E01.txt    … FTK Imager 風の要約（E01 の隣）
#   logs/<dev>_acquisition.log / _verify.log / _info.log … 詳細ログ
#
# 先に ./create_case_dirs.sh CASE_NO EVIDENCE_ID で骨格を作っておくこと。
#
# NOTE: 判断はしない。どのデバイスを取得するかは調査担当者が決める。
#       保全先（この SSD）を誤って指定しないよう、先に ./list_disk.sh で確認すること。

set -u

# CASES は「いまいるフォルダ(カレント)」の下に作る。証拠を置きたい媒体へ cd してから使う。
# 例: 保存SSD を /mnt/ssd にマウントし、cd /mnt/ssd してから
#     /mnt/fileserver/scripts/ewf_imager.sh を実行する（キットはフルパスで呼ぶ）。
CASE_BASE="$PWD"

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 CASE_NO EVIDENCE_ID DESCRIPTION EXAMINER TARGET"
    echo "  EVIDENCE_ID: evidence (PC/server/HDD) id, e.g. E001"
    echo "  TARGET: block device name (sdb, nvme0n1) or absolute path (/dev/sdb)"
    exit 1
fi

CASE_NO="$1"
EVID="$2"
DESCRIPTION="$3"
EXAMINER="$4"
TARGET="$5"

# CASE_NO / EVIDENCE_ID はフォルダ名になる。安全な文字だけ許す。
case "$CASE_NO" in ''|*[!A-Za-z0-9._-]*) echo "ERROR: CASE_NO は A-Za-z0-9._- のみ: '$CASE_NO'"; exit 1;; esac
case "$EVID"    in ''|*[!A-Za-z0-9._-]*) echo "ERROR: EVIDENCE_ID は A-Za-z0-9._- のみ: '$EVID'"; exit 1;; esac

# TARGET を /dev/ 絶対パスへ正規化
case "$TARGET" in
    /dev/*) SOURCE="$TARGET" ;;
    *)      SOURCE="/dev/$TARGET" ;;
esac
DEVBASE="$(basename "$SOURCE")"

CASE_ROOT="$CASE_BASE/CASES/$CASE_NO/$EVID"
ACQ_DIR="$CASE_ROOT/acquisition"
LOG_DIR="$CASE_ROOT/logs"
IMAGE_BASE="${CASE_NO}_${EVID}_${DEVBASE}"
TARGET_BASE="$ACQ_DIR/$IMAGE_BASE"
E01_FILE="${TARGET_BASE}.E01"
E01_TXT="${TARGET_BASE}.E01.txt"           # FTK Imager 風の要約（E01 の隣）
ACQ_LOG="$LOG_DIR/${DEVBASE}_acquisition.log"
VERIFY_LOG="$LOG_DIR/${DEVBASE}_verify.log"
INFO_LOG="$LOG_DIR/${DEVBASE}_info.log"

if [ ! -d "$ACQ_DIR" ]; then
    echo "ERROR: $ACQ_DIR がない。先に  ./create_case_dirs.sh $CASE_NO $EVID  を実行する。"
    exit 1
fi
if [ ! -b "$SOURCE" ]; then
    echo "ERROR: $SOURCE はブロックデバイスではない。 ./list_disk.sh で確認する。"
    exit 1
fi
if ! command -v ewfacquire >/dev/null 2>&1; then
    echo "ERROR: ewfacquire が無い（apt install ewf-tools / libewf-tools）。"
    exit 1
fi

echo "Source     : $SOURCE"
echo "Image Base : $IMAGE_BASE"
echo

ACQ_START="$(date '+%Y-%m-%d %H:%M:%S')"
{
  echo "==========================================================="
  echo "Acquisition Log"
  echo "==========================================================="
  echo "Case Number : $CASE_NO"
  echo "Evidence ID : $EVID"
  echo "Examiner    : $EXAMINER"
  echo "Description : $DESCRIPTION"
  echo "Source Disk : $SOURCE"
  echo "Output File : $E01_FILE"
  echo "Start Time  : $ACQ_START"
  echo "==========================================================="
  echo
} > "$ACQ_LOG"

# ewfacquire オプション:
#   -u        無人モード（対話プロンプトなし）
#   -c fast   高速圧縮
#   -f encase6 EnCase6(E01) 形式
#   -S 4G     セグメント最大 4GiB（FAT32 な保全先でも扱える大きさに分割）
#   -d sha1   MD5 に加えて SHA-1 も計算
#   -g 64     読み取りエラー時に 64 セクタ単位でスキップして続行
#   -C/-D/-e  ケース番号 / 説明 / 取得者を E01 メタデータに記録
sudo ewfacquire -u \
    -c fast \
    -f encase6 \
    -S 4G \
    -d sha1 \
    -g 64 \
    -C "$CASE_NO" \
    -D "$DESCRIPTION" \
    -e "$EXAMINER" \
    -t "$TARGET_BASE" \
    "$SOURCE" 2>&1 | tee -a "$ACQ_LOG"
RC=${PIPESTATUS[0]}
ACQ_END="$(date '+%Y-%m-%d %H:%M:%S')"

{ echo; echo "Finish Time : $ACQ_END"; echo "Return Code : $RC"; } >> "$ACQ_LOG"

if [ "$RC" -ne 0 ]; then
    echo "ERROR: Acquisition failed. See $ACQ_LOG"
    exit "$RC"
fi

echo
echo "Acquisition completed. Verifying..."
VERIFY_START="$(date '+%Y-%m-%d %H:%M:%S')"
{
  echo "==========================================================="
  echo "Verify Log"
  echo "==========================================================="
  echo "Case Number : $CASE_NO"
  echo "Image File  : $E01_FILE"
  echo "Start Time  : $VERIFY_START"
  echo "==========================================================="
  echo
} > "$VERIFY_LOG"
ewfverify -d sha1 "$E01_FILE" 2>&1 | tee -a "$VERIFY_LOG"
VRC=${PIPESTATUS[0]}
VERIFY_END="$(date '+%Y-%m-%d %H:%M:%S')"
{ echo; echo "Finish Time : $VERIFY_END"; echo "Return Code : $VRC"; } >> "$VERIFY_LOG"

# ewfinfo（メタデータ・ハッシュの記録）
{
  echo "==========================================================="
  echo "EWF Info Log"
  echo "==========================================================="
  echo "Case Number : $CASE_NO"
  echo "Image File  : $E01_FILE"
  echo "==========================================================="
  echo
} > "$INFO_LOG"
ewfinfo "$E01_FILE" >> "$INFO_LOG" 2>&1

# --- FTK Imager 風の要約（<image>.E01.txt を E01 と同じフォルダに置く）---
# ハッシュと検証結果は verify ログから拾う（stored と calculated の両方が出る）。
pick() { grep -m1 "$1" "$VERIFY_LOG" 2>/dev/null | sed 's/.*:[[:space:]]*//'; }
MD5_STORED="$(pick 'MD5 hash stored in file:')"
MD5_CALC="$(pick 'MD5 hash calculated over data:')"
SHA1_STORED="$(pick 'SHA1 hash stored in file:')"
SHA1_CALC="$(pick 'SHA1 hash calculated over data:')"
VERIFY_RESULT="$(grep -m1 '^ewfverify:' "$VERIFY_LOG" 2>/dev/null | sed 's/^ewfverify:[[:space:]]*//')"
[ -n "$VERIFY_RESULT" ] || VERIFY_RESULT=$([ "$VRC" -eq 0 ] && echo SUCCESS || echo FAILED)

{
  echo "Created By ewf_imager.sh (libewf / ewftools)"
  echo
  echo "Case Information:"
  echo "  Case Number:         $CASE_NO"
  echo "  Evidence ID:         $EVID"
  echo "  Unique Description:  $DESCRIPTION"
  echo "  Examiner:            $EXAMINER"
  echo
  echo "--------------------------------------------------------------"
  echo "Information for $TARGET_BASE:"
  echo
  echo "  Source:              $SOURCE"
  echo "  Image Type:          EnCase6 (E01), compression=fast, segment=4GiB"
  echo "  Acquisition started:  $ACQ_START"
  echo "  Acquisition finished: $ACQ_END"
  echo "  Verification started: $VERIFY_START"
  echo "  Verification finished: $VERIFY_END"
  echo
  echo "--------------------------------------------------------------"
  echo "Image Verification Results:"
  echo "  Verification:        $VERIFY_RESULT"
  echo "  MD5 stored:          ${MD5_STORED:-N/A}"
  echo "  MD5 calculated:      ${MD5_CALC:-N/A}"
  echo "  SHA1 stored:         ${SHA1_STORED:-N/A}"
  echo "  SHA1 calculated:     ${SHA1_CALC:-N/A}"
  echo
  echo "--------------------------------------------------------------"
  echo "Image Segments:"
  for seg in "$ACQ_DIR/$IMAGE_BASE".E??; do
    [ -e "$seg" ] && printf '  %s\n' "$seg"
  done
} > "$E01_TXT"

echo
if [ "$VRC" -eq 0 ]; then
    echo "Verification SUCCESS."
else
    echo "Verification FAILED. See $VERIFY_LOG"
fi
echo "Summary : $E01_TXT"
echo "Logs    : $LOG_DIR"
exit "$VRC"
