# reg_decode — 2nd-layer registry decoders
#
# 1st layer: regf_decode が regf ハイブ(SYSTEM/SOFTWARE/NTUSER...)を
#            テキスト(/PATH|TYPE|VALUE 形式)に展開する。
# 2nd layer: ここの各ツールが、その出力(や regedit.exe / evtx の出力)を
#            受け取り、個別のレジストリ・アーティファクトを解く。
#
# 各ツールは「単一 .c = 単一バイナリ」。
# ツールを追加するときは TOOLS に名前を足すだけ(reg_ptcache の隣に並べる)。
# (sb_decode のような「複数 .c で1ツール」とは設計が異なる点に注意)

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11

TOOLS   := reg_ptcache reg_poladtev

.PHONY: all clean
all: $(TOOLS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TOOLS)
