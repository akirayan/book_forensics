# reg_decode — 2nd-layer registry decoders

Windows レジストリ解析の**2層目**のツール置き場。

- **1st layer**: [`regf_decode`](../regf_decode/) が regf ハイブ
  (SYSTEM / SOFTWARE / NTUSER.DAT …) を解析し、
  `/PATH|TYPE|VALUE` 形式のテキストに展開する。
- **2nd layer (ここ)**: その出力(または regedit.exe / evtx_decode の出力)を
  入力に取り、特定のアーティファクトを人間が読める形に解く。

## 収録ツール

| ツール | 解くもの |
|---|---|
| `reg_ptcache` | `PartitionTableCache`(partmgr) — 過去に接続した GPT/MBR ディスクの Disk GUID・パーティション GUID・名前 |

(reg_userassist / reg_amcache / reg_shimcache / reg_mounteddevices も順次追加予定)

## ビルド

```sh
make                 # 全ツール
make reg_ptcache     # 個別
```

各ツールは単一 `.c` = 単一バイナリ。新しいツールは `Makefile` の `TOOLS` に名前を足す。

## reg_ptcache の使い方

`regf_decode` で `PartitionTableCache` 値を regedit.exe 形式でダンプし、パイプで渡す。

```sh
regf_decode -p "/ROOT/ControlSet001/Services/partmgr/Parameters" \
            -d "*PartitionTableCache" SYSTEM.hive \
  | reg_ptcache > connected_devices.txt
```

`--csv` で CSV 出力。入力は regedit.exe hex ダンプ / `regf_decode` のパイプ /
`evtx_decode` の Event ID 1006(Partition/Diagnostic) 出力に対応する。

詳細は本書『デジタル・フォレンジックの原理』第4部 第27章を参照。
