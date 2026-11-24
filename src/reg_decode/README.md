# reg_decode — 2nd-layer registry/artifact decoders

Windows レジストリ解析の**2層目**のツール置き場。

- **1st layer**: [`regf_decode`](../regf_decode/) が regf ハイブ
  (SYSTEM / SOFTWARE / NTUSER.DAT …) を解析し、
  `/PATH|TYPE|VALUE` 形式のテキストに展開する。
- **2nd layer (ここ)**: その出力(または regedit.exe / evtx_decode の出力)を
  入力に取り、特定のアーティファクトを人間が読める形に解く。

各ツールは独立した単機能ツール(1 ソース = 1 ツール)で、`sb_decode` のような
「複数ソースで 1 ツール」とは設計が異なる。

## 収録ツール

| ツール | 種別 | 解くもの |
|---|---|---|
| `reg_ptcache` | C | `PartitionTableCache`(partmgr) / Event 1006 の `PartitionTable` — 過去に接続した GPT/MBR ディスクの Disk GUID・パーティション GUID・LBA・名前 |
| `pt_timeline` | Python | `evtx_decode -t -e 1006` の出力から、ディスクごとのパーティションテーブル変化(接続/切断/追加/削除/リサイズ)を時系列で可視化 |

(reg_userassist / reg_amcache / reg_shimcache / reg_mounteddevices も順次追加予定)

## ビルド / 実行

C ツールは `make`、Python ツールはそのまま実行する。

```sh
make                 # C ツール全部(現状 reg_ptcache)
make reg_ptcache     # 個別
./pt_timeline ...    # Python スクリプト(shebang 付き・ビルド不要)
```

新しい C ツールは `Makefile` の `TOOLS` に名前を足す。

## reg_ptcache の使い方

`regf_decode -d "*PartitionTableCache"` で全デバイスのキャッシュ値を
`…|REG_BINARY|…` のフル hex で出し、パイプで渡す。

```sh
regf_decode -d "*PartitionTableCache" SYSTEM.hive | reg_ptcache
```

`-c`/`--csv` で CSV 出力。入力は regedit.exe の `.reg`(`"…"=hex:`) /
`regf_decode` のパイプ(`…|REG_BINARY|…`) / `evtx_decode` の
Event ID 1006(Partition/Diagnostic) 出力に対応する。

## pt_timeline の使い方

`evtx_decode -t -e 1006` の**テキスト出力をファイルで**渡す(パイプ不可)。

```sh
evtx_decode -t -e 1006 Microsoft-Windows-Partition%4Diagnostic.evtx > evtx1006.txt
./pt_timeline evtx1006.txt              # 全ディスクのタイムライン + サマリ
./pt_timeline evtx1006.txt --disk 1     # 特定ディスクのみ
./pt_timeline evtx1006.txt --summary    # 変化イベントだけ
./pt_timeline evtx1006.txt --csv        # CSV 出力
```

詳細は本書『デジタル・フォレンジックの原理』第6部「実行と操作の痕跡」
(外部記憶媒体の接続履歴調査)を参照。
