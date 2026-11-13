#!/bin/bash
# create_case_dirs.sh — 証拠(Evidence)単位のフォルダ骨格を作る（create_case_dirs.cmd の Linux 版）
#
# 実案件では 1 つの事件番号(CASE)に複数の PC・サーバ・HDD が関わる。そこで
# CASE の下に「証拠 ID(EVIDENCE_ID)」のフォルダを切り、その中にデータを保存する。
#
# CASES は「いまいるフォルダ(カレント)」の下に作る。だから使う前に、証拠を置きたい
# 媒体へ cd しておく。例えば ch9 の現場作業なら、保存SSD を /mnt/ssd にマウントして
# cd /mnt/ssd してから実行する（キット自体は /mnt/fileserver/scripts/ などに置いたまま、
# フルパスで呼ぶ）。
#
# 使い方:  ./create_case_dirs.sh <CASE_NO> <EVIDENCE_ID> [BASE_DIR]
#   例:    cd /mnt/ssd && /mnt/fileserver/scripts/create_case_dirs.sh C26015 E001
#          ./create_case_dirs.sh C26015 E001 /mnt/ssd    # 置き場所を明示する場合
#
# 作る骨格:
#   <BASE>/CASES/<CASE_NO>/<EVIDENCE_ID>/
#     acquisition/  … ディスクイメージ(E01 等)
#     memory/       … メモリイメージ(.lime など)
#     symbols/      … ISF の材料(vmlinux/System.map/kallsyms/BTF)と ISF(JSON)
#     logs/         … システム情報・ディスク情報・取得/検証ログ・manifest(SHA-256)
#     reports/      … 報告書・メモ

set -u

say(){ printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
err(){ printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; }

# CASE_NO / EVIDENCE_ID はフォルダ名になる。安全な文字だけ許す。
valid_id(){ case "$1" in ''|*[!A-Za-z0-9._-]*) return 1;; *) return 0;; esac; }

CASE="${1:-}"
EVID="${2:-}"
BASE="${3:-$PWD}"

if [ -z "$CASE" ] || [ -z "$EVID" ]; then
  err "usage: $0 <CASE_NO> <EVIDENCE_ID> [BASE_DIR]   例: cd /mnt/ssd && $0 C26015 E001"
  exit 1
fi
valid_id "$CASE" || { err "CASE_NO に使えない文字: '$CASE'（A-Za-z0-9._- のみ）"; exit 1; }
valid_id "$EVID" || { err "EVIDENCE_ID に使えない文字: '$EVID'（A-Za-z0-9._- のみ）"; exit 1; }

ROOT="$BASE/CASES/$CASE/$EVID"
for d in acquisition memory symbols logs reports; do
  mkdir -p "$ROOT/$d" || { err "作れない: $ROOT/$d"; exit 1; }
done

say "CASE=$CASE  EVIDENCE=$EVID"
say "EVIDENCE_ROOT = $ROOT"
for d in acquisition memory symbols logs reports; do
  say "  $d/"
done
