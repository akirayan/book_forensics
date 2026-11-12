#!/bin/bash
# create_case_dirs.sh — ケースのフォルダ骨格を作る（create_case_dirs.cmd の Linux 版）
#
# CASES は「いまいるフォルダ(カレント)」の下に作る。だから使う前に、証拠を置きたい
# 媒体へ cd しておく。例えば ch9 の現場作業なら、保存SSD を /mnt/ssd にマウントして
# cd /mnt/ssd してから実行する（キット自体は /mnt/fileserver/scripts/ などに置いたまま、
# フルパスで呼ぶ）。
#
# 使い方:  ./create_case_dirs.sh <CASE_ID> [BASE_DIR]
#   例:    cd /mnt/ssd && /mnt/fileserver/scripts/create_case_dirs.sh C26015
#          ./create_case_dirs.sh C26015 /mnt/ssd     # 置き場所を明示する場合
#
# 作る骨格:
#   <BASE>/CASES/<CASE_ID>/
#     acquisition/  … ディスクイメージ(E01 等)
#     memory/       … メモリイメージ(.lime など)
#     symbols/      … ISF の材料(vmlinux/System.map/kallsyms/BTF)と ISF(JSON)
#     logs/         … システム情報・ディスク情報・取得/検証ログ・manifest(SHA-256)
#     reports/      … 報告書・メモ

set -u

say(){ printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
err(){ printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; }

CASE="${1:-}"
BASE="${2:-$PWD}"

[ -n "$CASE" ] || { err "usage: $0 <CASE_ID> [BASE_DIR]   例: cd /mnt/ssd && $0 C26015"; exit 1; }

ROOT="$BASE/CASES/$CASE"
for d in acquisition memory symbols logs reports; do
  mkdir -p "$ROOT/$d" || { err "作れない: $ROOT/$d"; exit 1; }
done

say "CASE_ROOT = $ROOT"
for d in acquisition memory symbols logs reports; do
  say "  $d/"
done
