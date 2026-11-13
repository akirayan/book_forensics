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
├── ewf_imager.cmd             ewf_imager.sh         … ディスク保全(E01)＋verify＋info＋E01.txt
├── dump_memory.cmd            dump_memory.sh        … メモリ取得
├── ewf_report.cmd             ewf_report.sh         … ログから Markdown レポート生成
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

ケースの中身はこうなる（`create_case_dirs` が作る）。実案件では 1 つの事件番号
（CASE_NO）に複数の PC・サーバ・HDD が関わるので、**CASE_NO の下に証拠ごとの
フォルダ（EVIDENCE_ID）**を切り、そこへデータを保存する。

```text
CASES/<CASE_NO>/                      … 事件番号
└── <EVIDENCE_ID>/                    … 証拠(PC/サーバ/HDD)ごと。例 E001, E002, …
    ├── acquisition/  … ディスクイメージ  <CASE_NO>_<EVIDENCE_ID>_<dev>.E01
    ├── memory/       … メモリイメージ(.lime など)
    ├── symbols/      … ISF の材料(vmlinux/System.map/BTF)と ISF(JSON)
    ├── logs/         … システム情報・ディスク情報・取得/検証ログ
    └── reports/      … 報告書・メモ
```

例：事件 C26010 に PC 2 台と外付け HDD 1 台 → `CASES/C26010/{E001,E002,E003}/`。

---

## 現場ワークフロー

各スクリプトは **第1引数に CASE_NO、第2引数に EVIDENCE_ID** を取る（証拠ごとに実行する）。

1. **対象の確認** — `list_disk`（.cmd/.sh）で、どれが対象ディスクで、どれが保全先
   （この SSD）かを見分ける。**取り違えないこと。**（`.sh` は引数なしで一覧だけも可）
2. **ケース作成** — `create_case_dirs <CASE_NO> <EVIDENCE_ID>` で骨格を作る。
3. **システム情報** — `get_system_info <CASE_NO> <EVIDENCE_ID>` でライブ状態を記録。
4. **ディスク保全** — `ewf_imager <CASE_NO> <EVIDENCE_ID> "<説明>" <取得者> <対象>` で E01 取得。
   取得後に verify と ewfinfo が自動で走る。
5. **メモリ保全（必要時）** — `dump_memory <CASE_NO> <EVIDENCE_ID>`。
6. **レポート（任意）** — `ewf_report <CASE_NO> <EVIDENCE_ID> <TARGET>` で、ログから Markdown の
   保全レポートを `CASES/<CASE_NO>/<EVIDENCE_ID>/reports/` に生成する（TARGET は取得時のログ接頭辞）。
7. **持ち帰り・移送** — `CASES/<CASE_NO>/` 一式を解析環境の fileserver へ移す。
   E01 のハッシュ（取得ログ / ewfinfo / E01.txt）で、移送後も完全性を確認できる。

> [!NOTE]
> **`CASES/` は「いまいるフォルダ（カレント）」の下に作られる**（Windows の `.cmd` も Linux の
> `.sh` も同じ）。だから、証拠を落としたい媒体（保存用 SSD など）へ `cd` してから、キットを
> 呼ぶ。こうすると保全データ（`CASES/`）だけが保存先に集まり、**キット本体はどこに置いても
> よい**――キットと保存先を分けられる。Live Windows のように「キットも保存先も同じ SSD」なら、
> その SSD（またはキットのフォルダ）へ `cd` してから実行すればよい。Windows の `.cmd` は、
> 既定の `System32` で誤って `CASES` を作らないようガードを入れてある。

---

## 使い方（例）

### Windows（管理者 CMD）

まず証拠を置く場所へ `cd` する。「キットも保存先も同じ SSD」ならキットのフォルダへ、
「保存先を別にする」なら保存先ドライブへ移動し、キットはフルパスで呼ぶ。

```cmd
rem 例) キット兼保存用 SSD(E:) の中で作業する場合
cd /d E:\ir_starter_kit
list_disk.cmd            C26010 E001
create_case_dirs.cmd     C26010 E001
get_system_info.cmd      C26010 E001
ewf_imager.cmd   C26010 E001 "Live Windows11 Arm64, disk0" endoaki 0
ewf_report.cmd   C26010 E001 disk0

rem 例) 保存先を別ドライブ(D:)にする場合はそこへ cd し、キットはフルパスで
D:
cd \
E:\ir_starter_kit\create_case_dirs.cmd  C26010 E001
E:\ir_starter_kit\ewf_imager.cmd  C26010 E001 "..." endoaki 0
```

`ewf_imager.cmd` の TARGET:

- 数字 `0,1,…` → `\\.\PhysicalDriveN`（物理ディスク全体。BitLocker は暗号化のまま取得）
- 英字 `C,D,…` → `\\.\C:`（論理ドライブ。稼働 OS が復号＝平文。鍵不要）

### Linux（root / sudo）

同じく、証拠を置く媒体へ `cd` してから、キットをフルパスで呼ぶ。

```bash
$ cd /mnt/ssd                               # 保存先の媒体へ移動
$ /mnt/fileserver/scripts/list_disk.sh      # 一覧だけ見る（引数なし可）
$ /mnt/fileserver/scripts/create_case_dirs.sh  C26010 E001
$ /mnt/fileserver/scripts/get_system_info.sh   C26010 E001
$ /mnt/fileserver/scripts/ewf_imager.sh  C26010 E001 "Dell Latitude 5540, disk0" endoaki sdb
$ /mnt/fileserver/scripts/ewf_report.sh  C26010 E001 sdb
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
