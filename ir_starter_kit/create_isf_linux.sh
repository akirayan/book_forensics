#!/bin/bash
# create_isf_linux.sh
# dump_memory_linux.sh が集めた材料から、Volatility 3 用の ISF(JSON) を作る。
# 解析側（持ち帰った後）で実行する。OS（macOS / Linux）を自動判別して、
# キット内の対応する dwarf2json を呼ぶ。
#   macOS(Darwin) -> dwarf2json/dwarf2json-darwin-amd64
#   Linux         -> dwarf2json/dwarf2json-linux-amd64
# ※ dwarf2json は「対象と同じ arch」で動かす必要はない（確保した vmlinux を
#    解析するだけ）。だから ARM64 で取得した材料も、mac/x64 でここで ISF にできる。
#
# 使い方:  ./create_isf_linux.sh <CASE_ID>
#   例:    cd /path/to/evidence && /path/to/kit/create_isf_linux.sh C26015
#   前提:  カレントに <CASE_ID>/symbols/ があり、dump_memory_linux.sh の材料が入っている
#          （kernel.release, vmlinux-<rel>, System.map-<rel> など）。
#   出力:  <CASE_ID>/symbols/<rel>.json  … これを Volatility の symbols/linux/ へ置く。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CASE="${1:-}"

say(){  printf '\033[1;34m[*]\033[0m %s\n' "$*" >&2; }
warn(){ printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
err(){  printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; }

sha256_of(){
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else echo "NO_SHA_TOOL"; fi
}

[ -n "$CASE" ] || { err "usage: $0 <CASE_ID>   例: $0 C26015"; exit 1; }

SYM_DIR="./$CASE/symbols"
LOG_DIR="./$CASE/logs"
[ -d "$SYM_DIR" ] || { err "材料フォルダが無い: $SYM_DIR（カレントに $CASE/ があるか確認）"; exit 1; }
mkdir -p "$LOG_DIR" 2>/dev/null || true

# 対象カーネルの release を材料から読む
if [ -f "$SYM_DIR/kernel.release" ]; then
  KREL=$(head -n1 "$SYM_DIR/kernel.release" | tr -d '\r')
else
  err "$SYM_DIR/kernel.release が無い。dump_memory_linux.sh の出力か確認。"; exit 1
fi

VMLINUX="$SYM_DIR/vmlinux-$KREL"
SYSMAP="$SYM_DIR/System.map-$KREL"
ISF="$SYM_DIR/$KREL.json"

# DWARF 付き vmlinux が無いと ISF は作れない
if [ ! -f "$VMLINUX" ]; then
  err "DWARF 付き vmlinux が無い: $VMLINUX"
  err "取得時に dbgsym/debuginfo が未導入だった可能性。対象の dbgsym を入手して"
  err "vmlinux を $SYM_DIR/vmlinux-$KREL に置けば、本スクリプトで ISF を作れる。"
  exit 1
fi

# OS を自動判別して dwarf2json を選ぶ
OSNAME=$(uname -s)
case "$OSNAME" in
  Darwin) DW="$SCRIPT_DIR/tools/dwarf2json/dwarf2json-darwin-amd64" ;;
  Linux)  DW="$SCRIPT_DIR/tools/dwarf2json/dwarf2json-linux-amd64" ;;
  *)      err "未対応の OS: $OSNAME（macOS / Linux のみ）"; exit 1 ;;
esac
if [ ! -x "$DW" ]; then
  err "dwarf2json が見つからない/実行不可: $DW"
  err "キットの dwarf2json/ に darwin-amd64 / linux-amd64 を置き、chmod +x すること。"
  err "入手: github.com/volatilityfoundation/dwarf2json のリリース"
  exit 1
fi

say "case   : $CASE"
say "kernel : $KREL"
say "host OS: $OSNAME  ->  $(basename "$DW")"
say "vmlinux: $VMLINUX"
[ -f "$SYSMAP" ] && say "sysmap : $SYSMAP" || warn "System.map が無い（--system-map 無しで作る。通常は動くが補助に使うと堅牢）。"

# macOS(Apple Silicon): 未署名＋ダウンロード検疫だと x86_64 バイナリが Killed:9 になる。
# Rosetta 下で走らせるため、実行前に検疫解除＋アドホック署名を試す（best-effort・冪等）。
if [ "$OSNAME" = Darwin ]; then
  xattr -c "$DW" 2>/dev/null || true
  codesign --sign - --force "$DW" >/dev/null 2>&1 || true
fi

# ISF 生成（System.map があれば併用。大きな vmlinux では時間・メモリを食う）
say "dwarf2json 実行中…（$ISF）"
if [ -f "$SYSMAP" ]; then
  "$DW" linux --elf "$VMLINUX" --system-map "$SYSMAP" > "$ISF" 2> "$LOG_DIR/dwarf2json.err"
else
  "$DW" linux --elf "$VMLINUX" > "$ISF" 2> "$LOG_DIR/dwarf2json.err"
fi

if [ -s "$ISF" ]; then
  say "ISF を生成した: $ISF"
  say "  SHA-256: $(sha256_of "$ISF")"
  say "次の手順:"
  say "  1) ISF を Volatility の symbols/linux/ に置く（または VOLATILITY_SYMBOL_DIRS に追加）"
  say "  2) vol -f <CASE>/memory/*.lime banners.Banners  で版が一致するか確認"
  say "  3) mem_triage.sh <CASE>/memory/*.lime linux  で一括トリアージ"
else
  err "ISF の生成に失敗（$LOG_DIR/dwarf2json.err を確認）。"
  rm -f "$ISF"
  # macOS/Apple Silicon の典型：x86_64 バイナリが「Killed: 9」になる
  if [ "$OSNAME" = Darwin ]; then
    warn "macOS で dwarf2json が『Killed: 9』になる場合（未署名＋ダウンロード検疫）:"
    warn "    xattr -c '$DW'"
    warn "    codesign --sign - --force '$DW'"
    warn "  さらに Apple Silicon では Rosetta も要る:"
    warn "    softwareupdate --install-rosetta --agree-to-license"
  fi
  exit 1
fi
