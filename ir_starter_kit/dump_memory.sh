#!/bin/bash
# dump_memory_linux.sh
# Linux のメモリ保全キット。AVML でメモリを取得し、同じ場に
# 「解析に必要なシンボル材料」も確保する（LiME は使わない）。
# ★ ここでは情報を集めるだけ。ISF(JSON) は作らない（dwarf2json は呼ばない）。
#   持ち帰ってから create_isf_linux.sh で ISF を生成する。
#   理由: dwarf2json の Linux-ARM64 版が無く、ARM64 ホストではその場生成できない。
#         生成は OS を問わない作業なので、解析側（mac/x64）に分けた方が確実。
#
# なぜシンボルを取得時に確保するか:
#   Volatility 3 は、対象カーネルに一致する ISF が無いと pslist すら動かない。
#   ローリングリリースではカーネル更新後に dbgsym がリポジトリから消え、
#   後から材料を入手できなくなる。だからメモリと同時に、DWARF 付き vmlinux /
#   System.map / kallsyms / config / BTF を押さえる。――「メモリだけでは半分の証拠」。
#
# 使い方:  sudo ./dump_memory.sh <CASE_NO> <EVIDENCE_ID>
#   例:    cd /mnt/ssd && sudo /path/to/kit/dump_memory.sh C26015 E001
#   出力:  カレントに CASES/<CASE_NO>/<EVIDENCE_ID>/ を作り、その下に
#            memory/   … メモリイメージ(.lime)
#            symbols/  … シンボル材料(kernel.release/vmlinux/System.map/kallsyms/config/BTF)
#            logs/     … システム情報・manifest(SHA-256)
#   arch は uname -m で自動判定し、キット内の avml を選ぶ:
#            avml/avml（x86_64） / avml/avml-aarch64（ARM64）
#
# 方針: ローカルの速い媒体で実行（cd してから起動）。root 必須。
#       収集物はすべて SHA-256 を記録し logs/manifest.txt に残す（CoC）。

set -u

# ---- 設定・ユーティリティ -------------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
HOST=$(hostname 2>/dev/null || echo unknown)
KREL=$(uname -r)
ARCH=$(uname -m)
TS=$(date +%Y%m%d-%H%M%S)

CASE="${1:-}"
EVID="${2:-}"

say(){  printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
warn(){ printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
err(){  printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; }

sha256_of(){
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else echo "NO_SHA_TOOL"; fi
}

MANIFEST=""
record(){
  f="$1"
  [ -f "$f" ] || return 0
  sz=$(wc -c < "$f" 2>/dev/null | tr -d ' ')
  h=$(sha256_of "$f")
  printf '%s  %s bytes  %s\n' "$h" "$sz" "$f" >> "$MANIFEST"
  say "  記録: $(basename "$f")  ($sz bytes)"
}

# ---- 事前チェック ---------------------------------------------------------
if [ -z "$CASE" ] || [ -z "$EVID" ]; then
  err "usage: sudo $0 <CASE_NO> <EVIDENCE_ID>   例: $0 C26015 E001"; exit 1
fi
case "$CASE" in ''|*[!A-Za-z0-9._-]*) err "CASE_NO は A-Za-z0-9._- のみ"; exit 1;; esac
case "$EVID" in ''|*[!A-Za-z0-9._-]*) err "EVIDENCE_ID は A-Za-z0-9._- のみ"; exit 1;; esac

if [ "$(id -u)" != "0" ]; then
  err "root で実行すること（メモリ取得・kallsyms の実アドレスに必要）。 sudo $0 $CASE"
  exit 1
fi

# arch で AVML を選ぶ（x86_64 / aarch64 のみ）
case "$ARCH" in
  x86_64|amd64)  AVML="$SCRIPT_DIR/tools/avml/avml" ;;
  aarch64|arm64) AVML="$SCRIPT_DIR/tools/avml/avml-aarch64" ;;
  *)             AVML="" ;;
esac
if [ -z "$AVML" ]; then
  err "未対応のアーキテクチャ: $ARCH（x86_64 / aarch64 のみ）"; exit 1
fi
if [ ! -x "$AVML" ]; then
  err "AVML が見つからない/実行不可: $AVML"
  err "キットの avml/ に avml（x86_64）と avml-aarch64（ARM64）を置き、chmod +x すること。"
  err "入手: github.com/microsoft/avml のリリース"
  exit 1
fi

# 証拠のフォルダ（カレントに作る＝持ち帰れる。他スクリプトと同じ CASES/<CASE>/<EVID>/ に統一）
CASE_ROOT="./CASES/$CASE/$EVID"
MEM_DIR="$CASE_ROOT/memory"
SYM_DIR="$CASE_ROOT/symbols"
LOG_DIR="$CASE_ROOT/logs"
mkdir -p "$MEM_DIR" "$SYM_DIR" "$LOG_DIR" || { err "出力先を作れない: $CASE_ROOT"; exit 1; }
MANIFEST="$LOG_DIR/manifest.txt"
: > "$MANIFEST"

say "case : $CASE  evidence: $EVID  ->  $CASE_ROOT/"
say "host : $HOST"
say "kernel: $KREL ($ARCH)"
say "avml : $AVML"
say "注意: カレントはローカルの速い媒体を推奨（ネットワーク共有は遅延・切断の恐れ）。"

# ---- 1) システム情報・バナー ---------------------------------------------
say "== 1) システム情報・バナー =="
{
  echo "=== date (UTC) ==="; date -u
  echo "=== uname -a ===";   uname -a
  echo "=== /proc/version ==="; cat /proc/version 2>/dev/null
  echo "=== /etc/os-release ==="; cat /etc/os-release 2>/dev/null
  echo "=== uptime ==="; uptime 2>/dev/null
} > "$LOG_DIR/sysinfo.txt" 2>&1
record "$LOG_DIR/sysinfo.txt"

# ISF スクリプトが対象カーネルを知るための覚え（release と arch）＝symbols/ へ
printf '%s\n' "$KREL" > "$SYM_DIR/kernel.release"; record "$SYM_DIR/kernel.release"
printf '%s\n' "$ARCH" > "$SYM_DIR/kernel.arch";    record "$SYM_DIR/kernel.arch"
cp -f /proc/version "$SYM_DIR/proc_version.txt" 2>/dev/null && record "$SYM_DIR/proc_version.txt"

# ---- 2) メモリ取得（AVML。LiME 形式で出力＝LiME モジュールは使わない） ----
say "== 2) メモリ取得（AVML） =="
IMG="$MEM_DIR/${EVID}_memory_${HOST}_${TS}.lime"
say "AVML 実行中… （$IMG）"
if "$AVML" acquire "$IMG" ; then
  say "メモリ取得 完了"
else
  err "AVML が失敗した。カーネルのメモリ保護（kernel.yama 等）や権限を確認。"
  # シンボルだけでも残す価値があるので続行する
fi
record "$IMG"

# ---- 3) シンボル材料を確保（取得時にしか取れないことがある） --------------
say "== 3) シンボル材料の確保（symbols/ へ） =="
# kallsyms（root でないと実アドレスが 0 に伏せられる）
cp -f /proc/kallsyms "$SYM_DIR/kallsyms.txt" 2>/dev/null && record "$SYM_DIR/kallsyms.txt"
# System.map / config
for f in "/boot/System.map-$KREL" "/boot/config-$KREL"; do
  [ -e "$f" ] || continue
  cp -f "$f" "$SYM_DIR/$(basename "$f")" 2>/dev/null && record "$SYM_DIR/$(basename "$f")"
done
# BTF（名前が vmlinux なので DWARF 版と区別して btf_vmlinux に）
if [ -e /sys/kernel/btf/vmlinux ]; then
  cp -f /sys/kernel/btf/vmlinux "$SYM_DIR/btf_vmlinux" 2>/dev/null && record "$SYM_DIR/btf_vmlinux"
fi

# DWARF 付き vmlinux（dbgsym / debuginfo を入れていれば置かれている）＝ISF の主材料
VMLINUX_DBG=""
for p in \
  "/usr/lib/debug/boot/vmlinux-$KREL" \
  "/usr/lib/debug/lib/modules/$KREL/vmlinux" \
  "/usr/lib/debug/vmlinux-$KREL" ; do
  if [ -f "$p" ]; then VMLINUX_DBG="$p"; break; fi
done
if [ -n "$VMLINUX_DBG" ]; then
  cp -f "$VMLINUX_DBG" "$SYM_DIR/vmlinux-$KREL" 2>/dev/null && record "$SYM_DIR/vmlinux-$KREL"
  say "DWARF 付き vmlinux を確保: $VMLINUX_DBG"
else
  warn "DWARF 付き vmlinux が見つからない（dbgsym/debuginfo 未導入の可能性）。"
  warn "  Ubuntu/Debian 例: linux-image-$KREL-dbgsym（ddeb）を導入すると"
  warn "  /usr/lib/debug/boot/vmlinux-$KREL が置かれる。ISF はこれが要る。"
  warn "  ★ いま入れておかないと、後でカーネルが更新されると入手できなくなる。"
fi

# ---- 4) 仕上げ：要約（ISF はここでは作らない） ---------------------------
say "== 完了（情報収集のみ。ISF は作っていない） =="
say "収集物とハッシュ: $MANIFEST （manifest の SHA-256: $(sha256_of "$MANIFEST")）"
say "次の手順:"
say "  1) この CASES/$CASE/$EVID フォルダを解析側（mac か x86_64 Linux）へ持ち帰ってコピーする"
say "  2) そこで  create_isf_linux.sh $CASE $EVID  を実行して ISF を生成する"
say "  3) ISF を Volatility の symbols/linux/ に置き、"
say "     vol -f $IMG banners.Banners で版を確認 → mem_triage.sh $IMG linux"
say "イメージ: $IMG"
say "材料   : $SYM_DIR/"
