#!/bin/bash
# setup_vol3_linux.sh
# Volatility 3 を Linux（Debian/Kali 系）に常設する。
# 方針: PEP 668 を避けるため専用 venv に入れ、PATH 上のラッパーから呼ぶ（activate 不要）。
# 冪等（再実行可）。全段の成功は保証しない――失敗は警告して継続し、最後に動作検証する。
#
# 使い方:  bash setup_vol3_linux.sh
# 検証記録: verification/memory_analysis_verify_log.md

set -u

VENV="$HOME/bin/venv_vol3"       # 専用 venv（macOS と同じ。ラッパーと並べて見える場所に置く）
BINDIR="$HOME/bin"                      # ラッパー設置先（~/.local/bin は見えず忘れるので使わない。Debian/Kali は ~/.profile が ~/bin を PATH に足す）
WRAPPERS="vol volshell"
OPT="capstone yara-python pycryptodome"

say(){ printf '\033[1;34m[*]\033[0m %s\n' "$*"; }
warn(){ printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
err(){ printf '\033[1;31m[x]\033[0m %s\n' "$*"; }

# --- 1. python3 ---
if ! command -v python3 >/dev/null 2>&1; then
  err "python3 が見つかりません。 sudo apt install python3 python3-venv python3-full"; exit 1
fi
say "python3: $(python3 --version 2>&1) ($(command -v python3))"

# --- 2. venv（python3-venv が無いと失敗する）---
mkdir -p "$BINDIR"
if [ -x "$VENV/bin/python" ]; then
  say "既存 venv を再利用: $VENV"
else
  say "venv を作成: $VENV"
  if ! python3 -m venv "$VENV" 2>/tmp/vol3_venv_err.$$; then
    err "venv 作成に失敗。多くは python3-venv 未導入:  sudo apt install python3-venv python3-full"
    sed 's/^/    /' /tmp/vol3_venv_err.$$ 2>/dev/null; rm -f /tmp/vol3_venv_err.$$
    exit 1
  fi
  rm -f /tmp/vol3_venv_err.$$
fi

# --- 3. pip / volatility3 / 任意依存 ---
say "pip を更新"
"$VENV/bin/python" -m pip install --upgrade pip >/dev/null 2>&1 || warn "pip 更新に失敗（継続）"
say "volatility3 を導入"
if ! "$VENV/bin/python" -m pip install volatility3; then
  err "volatility3 の導入に失敗（ネットワーク/プロキシを確認）"; exit 1
fi
say "任意依存を導入（capstone / yara-python / pycryptodome）"
"$VENV/bin/python" -m pip install $OPT >/dev/null 2>&1 \
  || warn "任意依存の一部が入らなかった（build に gcc/python3-dev が要ることがある。本体は動く）"

# --- 3b. BitLocker FVEK プラグイン（コミュニティ製・任意）---
# 標準の volatility3 には bitlocker プラグインが無い。lorelyai/volatility3-bitlocker を
# venv 内の volatility3/plugins/windows/ に置けば、--plugin-dirs 無しで
# windows.bitlocker.BitlockerFVEKScan が使える（メモリから BitLocker FVEK を回収）。
# 取得できなければ警告して継続（本体は動く）。
BL_REPO="https://github.com/lorelyai/volatility3-bitlocker"
say "BitLocker FVEK プラグインを導入（${BL_REPO##*/}）"
PLUGDIR="$("$VENV/bin/python" -c 'import os,volatility3; print(os.path.join(os.path.dirname(volatility3.__file__),"plugins","windows"))' 2>/dev/null)"
if command -v git >/dev/null 2>&1 && [ -n "${PLUGDIR:-}" ] && [ -d "$PLUGDIR" ]; then
  bltmp="$(mktemp -d)"
  if git clone --depth 1 "$BL_REPO" "$bltmp/vb" >/dev/null 2>&1 && [ -f "$bltmp/vb/bitlocker.py" ]; then
    cp "$bltmp/vb/bitlocker.py" "$PLUGDIR/bitlocker.py" \
      && say "配置: $PLUGDIR/bitlocker.py" \
      || warn "bitlocker.py の配置に失敗"
  else
    warn "bitlocker プラグインの取得に失敗（git/ネットワーク）。後で --plugin-dirs で手動投入も可"
  fi
  rm -rf "$bltmp"
else
  warn "git が無い、または plugins/windows 不明のため bitlocker を配置できなかった"
fi

# --- 4. ラッパー作成 ---
say "ラッパーを作成: $BINDIR"
made=0
for t in $WRAPPERS; do
  if [ ! -x "$VENV/bin/$t" ]; then warn "venv に無いので skip: $t"; continue; fi
  if [ -e "$BINDIR/$t" ] && ! grep -q "venv_vol3" "$BINDIR/$t" 2>/dev/null; then
    warn "既存ファイルがある（当スクリプト製でない）ので skip: $t"; continue
  fi
  printf '#!/bin/bash\nexec "%s/bin/%s" "$@"\n' "$VENV" "$t" > "$BINDIR/$t" \
    && chmod +x "$BINDIR/$t" && made=$((made+1))
done
say "ラッパー作成数: $made"

# --- 5. PATH 確認 ---
case ":$PATH:" in
  *":$BINDIR:"*) : ;;
  *) warn "$BINDIR が PATH にありません。"
     warn "Debian/Kali は ~/.profile が '~/bin があれば PATH に追加' する。"
     warn "いま有効化するには:  export PATH=\"$BINDIR:\$PATH\"（恒久化は再ログイン or .profile 読み直し）";;
esac

# --- 6. 動作検証 ---
say "動作検証"
ver="$("$VENV/bin/pip" show volatility3 2>/dev/null | awk -F': ' '/^Version/{print $2}')"
say "volatility3: ${ver:-不明}"
if "$VENV/bin/vol" --help >/dev/null 2>&1; then
  say "vol 実行OK: $("$VENV/bin/vol" --help 2>&1 | head -1)"
  n=$("$VENV/bin/vol" --help 2>&1 | grep -oE 'windows\.[a-zA-Z0-9_.]+' | sort -u | wc -l | tr -d ' ')
  say "windows プラグイン数: ${n}"
else
  err "vol が実行できません"
fi
"$VENV/bin/python" -c "import capstone" 2>/dev/null && say "任意 capstone OK（malfind 逆アセンブル）" || warn "任意 capstone なし"
"$VENV/bin/python" -c "import yara"     2>/dev/null && say "任意 yara OK（yarascan）"               || warn "任意 yara なし"
"$VENV/bin/python" -c "import Crypto"   2>/dev/null && say "任意 pycryptodome OK"                   || warn "任意 pycryptodome なし"
"$VENV/bin/vol" --help 2>&1 | grep -qi 'windows\.bitlocker' && say "任意 bitlocker プラグイン OK（windows.bitlocker.BitlockerFVEKScan）" || warn "bitlocker プラグイン未導入（FVEK 抽出を使うなら手動投入）"
say "完了。使い方:  vol -f <image> linux.pslist   または  windows.info"
