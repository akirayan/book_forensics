# IR Starter Kit

現場に持ち込む **データ保全スターターキット**。この 1 本の SSD を調査対象の
Windows / Linux に挿し、**ディスクとメモリを E01 などで保全して `CASES/` に集め、
持ち帰ってから `CASES/` の中身を解析環境の fileserver に移す**――そこまでを一通り
まかなう、コマンドラインのツール集である。

将来は SystemRescue をベースに **ブータブル SSD** 化する予定だが、まずは中身の
ツールを整備する（ブータブル化は後段）。FTK Imager のような GUI には依存せず、
オープンソースの **libewf / ewftools** で E01 を取得する。

本キットは **判断をしない**。どのディスク・どのプロセスを保全するかは調査担当者が
決める。各スクリプトは「決めたことを、記録を残しながら正確に実行する」だけだ。

---

## 構成

Windows 用（`.cmd`）と Linux 用（`.sh`）を、**同じ名前・拡張子だけ違い**で並べてある。
サブフォルダは作らず、キット直下にフラットに置く。

```text
ir_starter_kit/
├── README.md
├── CASES/                     … 証拠の保存先（この下にケースごとのフォルダができる）
│
├── list_disk.cmd              list_disk.sh          … 保全対象の確認
├── get_system_info.cmd        get_system_info.sh    … システム情報の記録
├── create_case_dirs.cmd       create_case_dirs.sh   … ケースの骨格作成
├── ewf_imager.cmd             ewf_imager.sh         … ディスク保全(E01)＋verify＋info
├── dump_memory.cmd            dump_memory.sh        … メモリ取得
├── create_isf_linux.sh / create_isf_btf.sh         … Volatility3 用 ISF 生成
│
└── tools/                     … 同梱バイナリ（フォルダ名だけ版管理、本体は入れない）
    ├── ewftools-x64/          … Windows 版 ewfacquire/ewfverify/ewfinfo
    ├── dumpit/                … Windows メモリ取得（DumpIt、ARM64/x64/x86）
    ├── avml/                  … Linux メモリ取得
    ├── MemProcFS/             … メモリ解析（任意）
    ├── dwarf2json/            … Volatility3 ISF 生成（DWARF）
    └── btf2json/              … Volatility3 ISF 生成（BTF）
```

ケースの中身はこうなる（`create_case_dirs` が作る）。

```text
CASES/<CASE_NO>/
├── acquisition/   … ディスクイメージ(E01 等)
├── memory/        … メモリイメージ(.lime など)
├── symbols/       … ISF の材料(vmlinux/System.map/BTF)と ISF(JSON)
├── logs/          … システム情報・ディスク情報・取得/検証ログ
└── reports/       … 報告書・メモ
```

---

## 現場ワークフロー

1. **対象の確認** — `list_disk`（.cmd/.sh）で、どれが対象ディスクで、どれが保全先
   （この SSD）かを見分ける。**取り違えないこと。**
2. **ケース作成** — `create_case_dirs <CASE_NO>` で骨格を作る。
3. **システム情報** — `get_system_info <CASE_NO>` で、ライブ状態のシステム情報を記録。
4. **ディスク保全** — `ewf_imager <CASE_NO> "<説明>" <取得者> <対象>` で E01 取得。
   取得後に verify と ewfinfo が自動で走る。
5. **メモリ保全（必要時）** — `dump_memory`。
6. **持ち帰り・移送** — `CASES/<CASE_NO>/` 一式を解析環境の fileserver へ移す。
   E01 のハッシュ（取得ログ / ewfinfo）で、移送後も完全性を確認できる。

> [!NOTE]
> USB-SSD への直接 imaging が遅い場合は、`create_case_dirs <CASE_NO> <速いローカル>`
> で一旦ローカルに保全し、後で `CASES/` または fileserver へ移してもよい。

---

## 使い方（例）

### Windows（管理者 CMD）

```cmd
list_disk.cmd            CASE_NO
get_system_info.cmd      CASE_NO
create_case_dirs.cmd     CASE_NO
ewf_imager.cmd  C26010 "Live Windows11 Arm64, disk0" endoaki 0
```

`ewf_imager.cmd` の TARGET:

- 数字 `0,1,…` → `\\.\PhysicalDriveN`（物理ディスク全体。BitLocker は暗号化のまま取得）
- 英字 `C,D,…` → `\\.\C:`（論理ドライブ。稼働 OS が復号＝平文。鍵不要）

### Linux（root / sudo）

```bash
./list_disk.sh           # 一覧だけ見る（CASE_NO 省略可）
./create_case_dirs.sh    C26010
./get_system_info.sh     C26010
./ewf_imager.sh  C26010 "Dell Latitude 5540, disk0" endoaki sdb
```

`ewf_imager.sh` の TARGET はブロックデバイス名（`sdb`, `nvme0n1`）か絶対パス（`/dev/sdb`）。

---

## ディスク保全の中身（.cmd / .sh 共通）

`ewfacquire` を次の設定で呼ぶ（`.cmd` と `.sh` で揃えてある）。

- `-c fast`（高速圧縮）／`-f encase6`（E01）／`-S 4G`（4GiB 分割）
- `-d sha1`（MD5 に加えて SHA-1 も計算）／`-g 64`（エラー時 64 セクタ単位でスキップ）
- `-C`（ケース番号）／`-D`（説明）／`-e`（取得者）を E01 メタデータに記録

取得後に `ewfverify`（保存ハッシュと再計算ハッシュの一致）と `ewfinfo`（メタデータ）を
実行し、`logs/` にそれぞれのログを残す。

> [!WARNING]
> **BitLocker** が有効な物理ディスクを取得すると、中身は**暗号化されたまま**だ。復号には
> Recovery Key / Password / BEK が要る。鍵が無く中身が要るときは、稼働 OS 上で論理
> ドライブ（`C:` など）を取得すれば平文で保全できる（その代わり OS が動いている間の
> 変化を伴う点に注意）。

---

## ツールの入手（同梱バイナリ）

同梱バイナリは容量が大きく、各々ライセンスが異なるので **このリポジトリには含めない**
（`.gitignore` 済み。版管理するのは `tools/<ツール>/` のフォルダ名だけ）。実機キットには、
各ツールを公式配布元から入手して、下表の **`tools/` 配下**のフォルダに配置する。
ライセンスはそれぞれの配布元に従うこと。

| 置き場所 | ツール / 用途 | 入手先 |
|---|---|---|
| `tools/ewftools-x64/` | ewftools（libewf）― ディスク保全 E01（Windows ビルド） | libyal/libewf のリリース（`github.com/libyal/libewf`）。Linux は `apt install ewf-tools` でよい |
| `tools/avml/` | AVML ― Linux メモリ取得 | `github.com/microsoft/avml` のリリース（MIT） |
| `tools/dumpit/` | DumpIt ― Windows メモリ取得（`ARM64/` `x64/` `x86/`） | Magnet Forensics（旧 Comae）の配布ページ |
| `tools/MemProcFS/` | MemProcFS ― メモリ解析 | `github.com/ufrisk/MemProcFS` のリリース |
| `tools/dwarf2json/` | Volatility3 用 ISF（シンボル）生成 | `github.com/volatilityfoundation/dwarf2json` |
| `tools/btf2json/` | BTF から ISF 生成 | Volatility3 エコシステム（`create_isf_btf.sh` 参照） |

---

## 注意・免責

- すべて管理者 / root 権限で実行する。
- ライブ取得中は OS が動作しており、Event Log・Registry・一時ファイル等は変化し得る。
  必要に応じて VSS・メモリ取得・通信キャプチャを併用する。
- 本ツールは教育・研究・正当な調査目的でのみ使用する。対象システムへのアクセス権限が
  あることを前提とする。利用によって生じた損害について作者は責任を負わない。

## Author

遠藤 暁 (Endo Akira) — Windows / Linux Forensics, Incident Response
