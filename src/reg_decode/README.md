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
| `reg_poladtev` | C | `SECURITY\Policy\PolAdtEv`(REG_NONE) — 監査ポリシーのバイナリ。カテゴリ/サブカテゴリ毎に 監査なし/成功/失敗/両方 を解く。死活前(dead disk)で `auditpol /get` の代わりに「何が記録される設定だったか」を読む |

(reg_userassist / reg_amcache / reg_shimcache / reg_mounteddevices も順次追加予定)

## ビルド / 実行

C ツールは `make`、Python ツールはそのまま実行する。

```sh
make                 # C ツール全部(reg_ptcache / reg_poladtev)
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

## reg_poladtev の使い方

`regf_decode` で `SECURITY` ハイブの PolAdtEv 値をフル hex で出し、パイプで渡す。
単一の値なので `-f -p`(その値だけを 2 行で出す)を使うのが素直。

```sh
regf_decode -f -p "/ROOT/Policy/PolAdtEv" SECURITY | reg_poladtev        # 日本語名
regf_decode -f -p "/ROOT/Policy/PolAdtEv" SECURITY | reg_poladtev --en   # 英語名(auditpol と同じ語彙)
regf_decode -f -p "/ROOT/Policy/PolAdtEv" SECURITY | reg_poladtev --csv  # CSV
```

`--en` は `auditpol /get /category:*` と同じ英語サブカテゴリ名で出力する
(ただし **PolAdtEv のバイト順**で並ぶ。auditpol の表示順とは異なる)。入力は
`regf_decode` のパイプ(`…|REG_NONE|…`) / regedit の `.reg`(`"…"=hex:`) /
生の hex 列に対応。

> `regf_decode -d "*PolAdtEv/(default)"` でも渡せる(`reg_poladtev` は `hex:`
> ブロックを拾うので結果は同じ)。ただし `-d` は「どの値をフル hex で**ダンプ**
> するか」を指定するもので**フィルタではない**ため、ハイブのツリー全体も一緒に
> 出力される。単一の値には `-f -p` のほうがきれい。なお切り詰められた入力
> (`…[NN bytes]`)は検出してエラーにするので、`-f`(または `-d`)でフル値を渡すこと。

> 構造はフォーマット仕様書ではなく、実ハイブのバイト列から起こし、
> kazamiya 氏「PolAdtEv_Structure」(rev5) と `auditpol /list /category /v`・
> `auditpol /get /category:*` の実出力で裏取りした。ヘッダ/フッタの未確定
> バイトは意味を付けず unknown のまま表示する(Fact is Fact)。

詳細は本書『デジタル・フォレンジックの原理』第5部「Windows イベントログ」
(監査ポリシー — 何が記録されるか)を参照。
