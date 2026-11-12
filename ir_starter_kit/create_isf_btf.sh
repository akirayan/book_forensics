#!/bin/bash
# create_isf_btf.sh
# dump_memory_linux.sh が集めた BTF から、Volatility 3 用の ISF(JSON) を作る。
# dbgsym（DWARF 付き vmlinux）が無い環境（Kali/rolling/ARM64 など）向けの経路。
# BTF は /sys/kernel/btf/vmlinux として普通に手に入り、dump 側で symbols/btf_vmlinux
# に確保済み。これを btf2json で ISF に変換する（dwarf2json は BTF 非対応）。
#
# 対比:
#   create_isf_linux.sh … DWARF 付き vmlinux（dbgsym）→ dwarf2json   【dbgsym がある時】
#   create_isf_btf.sh   … BTF（btf_vmlinux）         → btf2json     【dbgsym が無い時】
#
# 使い方:  ./create_isf_btf.sh <CASE_ID>
#   例:    cd /path/to/evidence && /path/to/kit/create_isf_btf.sh C26015
#   前提:  カレントに <CASE_ID>/symbols/ があり、btf_vmlinux と kernel.release が入っている。
#   出力:  <CASE_ID>/symbols/<rel>.json  … これを Volatility の symbols/linux/ へ置く。
#
# btf2json バイナリ（キットの btf2json/ に置く。OS/arch で選ぶ）:
#   Linux x86_64   : btf2json/btf2json-linux-amd64   または btf2json/btf2json
#   Linux aarch64  : btf2json/btf2json-linux-arm64
#   macOS arm64    : btf2json/btf2json-darwin-arm64
#   公開ビルドは Linux x86_64 のみ。他は github.com/vobst/btf2json を
#   `cargo build --release` して作る（依存は pure-Rust、mac/arm でも通る）。
#   ※ 変換はターゲット arch に依存しない。ARM64 の BTF も x86_64 ホストで変換できる。

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

# 対象カーネルの release
if [ -f "$SYM_DIR/kernel.release" ]; then
  KREL=$(head -n1 "$SYM_DIR/kernel.release" | tr -d '\r')
else
  err "$SYM_DIR/kernel.release が無い。dump_memory_linux.sh の出力か確認。"; exit 1
fi

BTF="$SYM_DIR/btf_vmlinux"
ISF="$SYM_DIR/$KREL.json"
[ -f "$BTF" ] || { err "BTF が無い: $BTF（取得時に /sys/kernel/btf/vmlinux が無かった可能性）"; exit 1; }

# シンボルの名前/アドレス源（map）。System.map が小さすぎる（Kali では 91B 等）ことが
# あるので、十分な大きさが無ければ kallsyms.txt を使う。
MAP=""
SYSMAP="$SYM_DIR/System.map-$KREL"
KALLSYMS="$SYM_DIR/kallsyms.txt"
if [ -f "$SYSMAP" ] && [ "$(wc -c < "$SYSMAP" 2>/dev/null | tr -d ' ')" -gt 65536 ]; then
  MAP="$SYSMAP"
elif [ -f "$KALLSYMS" ]; then
  MAP="$KALLSYMS"; say "System.map が小さい/無いので kallsyms.txt を map に使う。"
else
  warn "map（System.map / kallsyms）が無い。--map 無しで試す（シンボル解決が弱くなる）。"
fi

# banner（Volatility のダンプ内バナーと一致させる必要がある）。proc_version をそのまま。
BANNER=""
if [ -f "$SYM_DIR/proc_version.txt" ]; then
  BANNER=$(head -n1 "$SYM_DIR/proc_version.txt" | tr -d '\r')
fi

# btf2json バイナリを OS/arch で選ぶ
OSN=$(uname -s); ARCHM=$(uname -m)
case "$OSN" in Darwin) OSK=darwin;; Linux) OSK=linux;; *) OSK="";; esac
case "$ARCHM" in x86_64|amd64) ARCHK=amd64;; aarch64|arm64) ARCHK=arm64;; *) ARCHK="";; esac
BTF2JSON=""
for c in "$SCRIPT_DIR/btf2json/btf2json-$OSK-$ARCHK" "$SCRIPT_DIR/btf2json/btf2json"; do
  if [ -x "$c" ]; then BTF2JSON="$c"; break; fi
done
if [ -z "$BTF2JSON" ] && command -v btf2json >/dev/null 2>&1; then BTF2JSON="btf2json"; fi
if [ -z "$BTF2JSON" ]; then
  err "btf2json が見つからない。キットの btf2json/ に置くこと（OS/arch 対応版）。"
  err "公開版は Linux x86_64 のみ。mac/arm は github.com/vobst/btf2json を cargo build。"
  exit 1
fi

# macOS で自前ビルドした場合に備え、検疫/署名を自己修復（best-effort）
if [ "$OSN" = Darwin ]; then
  xattr -c "$BTF2JSON" 2>/dev/null || true
  codesign --sign - --force "$BTF2JSON" >/dev/null 2>&1 || true
fi

say "case   : $CASE"
say "kernel : $KREL"
say "host   : $OSN/$ARCHM  ->  $(basename "$BTF2JSON")"
say "btf    : $BTF"
[ -n "$MAP" ]    && say "map    : $MAP"
[ -n "$BANNER" ] && say "banner : $BANNER" || warn "banner 不明（proc_version.txt 無し）。Volatility の照合に失敗したら --banner を手当て。"

# ISF 生成: btf2json --btf <BTF> [--map <MAP>] [--banner <BANNER>] > ISF
say "btf2json 実行中…（$ISF）"
set -- --btf "$BTF"
[ -n "$MAP" ]    && set -- "$@" --map "$MAP"
[ -n "$BANNER" ] && set -- "$@" --banner "$BANNER"
"$BTF2JSON" "$@" > "$ISF" 2> "$LOG_DIR/btf2json.err"

if [ -s "$ISF" ]; then
  say "ISF を生成した: $ISF"
  say "  SHA-256: $(sha256_of "$ISF")"
  say "次の手順:"
  say "  1) ISF を Volatility の symbols/linux/ に置く"
  say "  2) vol -f $CASE/memory/*.lime banners.Banners  で版が一致するか確認"
  say "  3) mem_triage.sh $CASE/memory/*.lime linux  で一括トリアージ"
  say "  ※ banners が一致しない時は、その出力の文字列を --banner にして作り直す。"
else
  err "ISF の生成に失敗（$LOG_DIR/btf2json.err を確認）。"
  rm -f "$ISF"
  # Linux ELF を別 OS で動かそうとした典型
  head -1 "$LOG_DIR/btf2json.err" 2>/dev/null | grep -qiE 'exec format|cannot execute|bad CPU|not permitted' && \
    warn "バイナリの OS/arch 不一致の可能性。x86_64 Linux で動かすか、mac/arm 用に cargo build。"
  if [ "$OSN" = Darwin ]; then
    warn "公開版 btf2json は Linux ELF＝macOS では動かない。macOS で使うなら arm64 版を自前ビルド。"
  fi
  exit 1
fi
