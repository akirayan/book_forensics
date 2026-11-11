# exFAT ファイルシステム フォレンジック

## 概要

exFAT (Extended File Allocation Table) は、Microsoft が開発した FAT 系ファイルシステムである。
フラッシュメモリ向けに設計されており、大容量ファイル (4 GB 超) と大容量ストレージの両方に対応する。
SDXC カード・USB メモリ・外付け SSD において広く採用されており、デジタルフォレンジック調査での遭遇頻度は高い。

**主な用途と遭遇場面**

- SDXC / microSDXC カード (デジタルカメラ・スマートフォン・ドライブレコーダー)
- 4 GB 超ファイルを扱う USB メモリ・外付けドライブ
- ゲーム機の外部ストレージ (Nintendo Switch など)
- Windows / macOS / Linux で標準マウント対応

---

## 1. ディスク上の構造

### 1.1 レイアウト概要

```
パーティション先頭
├── VBR (Volume Boot Record)          セクタ 0
│     先頭 512 バイト。ボリュームパラメータを格納。
├── 拡張ブートレコード (EBR)          セクタ 1-8
├── OEM パラメータ                    セクタ 9
├── 予約済み                          セクタ 10
├── ブートチェックサム                セクタ 11
├── バックアップ VBR                  セクタ 12-23
│
├── FAT (File Allocation Table)       VBR.FatOffset セクタ目～
│     クラスタチェーンを格納する 32bit エントリ配列。
│
└── クラスタヒープ                    VBR.ClusterHeapOffset セクタ目～
      クラスタ番号 2 始まり。ファイル・ディレクトリデータを格納。
      クラスタ 0, 1 は予約済みで使用されない。
```

### 1.2 VBR フィールド

| オフセット | サイズ | フィールド | 説明 |
|---|---|---|---|
| 0x00 | 3 | JumpBoot | `EB 76 90` |
| 0x03 | 8 | FileSystemName | `"EXFAT   "` (末尾スペース3個) |
| 0x0B | 53 | MustBeZero | すべて 0x00 でなければ exFAT でない |
| 0x40 | 8 | PartitionOffset | 物理ディスク上のセクタオフセット |
| 0x48 | 8 | VolumeLength | ボリューム長 (セクタ単位) |
| 0x50 | 4 | FatOffset | FAT 先頭位置 (セクタ単位) |
| 0x54 | 4 | FatLength | FAT の長さ (セクタ単位) |
| 0x58 | 4 | ClusterHeapOffset | クラスタヒープ先頭 (セクタ単位) |
| 0x5C | 4 | ClusterCount | クラスタ総数 |
| 0x60 | 4 | RootDirectoryCluster | ルートディレクトリの先頭クラスタ番号 |
| 0x64 | 4 | VolumeSerialNumber | ボリュームシリアル番号 (`XXXX-XXXX` 形式) |
| 0x68 | 2 | FileSystemRevision | `0x0100` = exFAT 1.0 |
| 0x6A | 2 | VolumeFlags | VolumeDirty bit など |
| 0x6C | 1 | BytesPerSectorShift | log₂(セクタサイズ)。通常 9 (= 512 B) |
| 0x6D | 1 | SectorsPerClusterShift | log₂(クラスタあたりセクタ数) |
| 0x6E | 1 | NumberOfFats | 通常 1。TexFAT は 2 |
| 0x6F | 1 | DriveSelect | BIOS ドライブ番号 |
| 0x70 | 1 | PercentInUse | 使用クラスタ率 (0-100, 0xFF=不明) |

**クラスタサイズの計算**

```
BytesPerSector  = 1 << BytesPerSectorShift
SectorsPerClus  = 1 << SectorsPerClusterShift
BytesPerCluster = BytesPerSector × SectorsPerClus
```

実例 (本件 HDD1 p3):
```
BytesPerSectorShift=9  → セクタ 512 B
SectorsPerClusterShift=8 → クラスタあたり 256 セクタ
BytesPerCluster = 512 × 256 = 131,072 B (128 KB)
ClusterHeapOffset: 0xB00000 (セクタ 22528)
RootDirectoryCluster: 6
```

### 1.3 FAT (File Allocation Table)

exFAT の FAT は 32bit エントリの配列である。
インデックスがそのままクラスタ番号に対応する。

| エントリ値 | 意味 |
|---|---|
| `0x00000000` | 空きクラスタ |
| `0x00000002`～`0xFFFFFFF6` | 次のクラスタ番号 (チェーン) |
| `0xFFFFFFF7` | 不良クラスタ |
| `0xFFFFFFF8`～`0xFFFFFFFF` | チェーン末尾 (EOF) |

クラスタ 0, 1 は予約済みであり、エントリは存在するが使用されない。
クラスタ番号は必ず 2 以上の値を取る。

**クラスタ番号からバイトオフセットへの変換**

```
バイトオフセット = ClusterHeapOffset(B) + (ClusterNo - 2) × BytesPerCluster
```

### 1.4 ディレクトリエントリ構造

exFAT のディレクトリは 32 バイト固定長エントリの配列である。
1 ファイル/ディレクトリは **エントリセット** と呼ばれる複数エントリの組で表される。

#### エントリタイプ byte[0]

```
bit 7 : InUse    (1=使用中, 0=削除済み)
bit 6 : Category (0=Primary, 1=Secondary)
bit 5 : TypeImportance
bit 4:0: TypeCode
```

| 値 | 使用中 | 削除済 | 名称 |
|---|---|---|---|
| 0x85 | ✓ | — | File Entry (Primary) |
| 0x05 | — | ✓ | File Entry (削除済) |
| 0xC0 | ✓ | — | Stream Extension (Secondary) |
| 0x40 | — | ✓ | Stream Extension (削除済) |
| 0xC1 | ✓ | — | File Name Extension (Secondary) |
| 0x41 | — | ✓ | File Name Extension (削除済) |
| 0x00 | — | — | ディレクトリ終端 / 未割り当て |

#### File Entry (0x85 / 0x05)

```
Offset  Size  Field
0x00     1    EntryType          (0x85 or 0x05)
0x01     1    SecondaryCount     後続セカンダリエントリ数 (最小 2)
0x02     2    SetChecksum
0x04     2    FileAttributes     ファイル属性 (下表)
0x06     2    Reserved1
0x08     4    CreateTime         作成日時
0x0C     4    LastModifiedTime   最終更新日時
0x10     4    LastAccessedTime   最終アクセス日時
0x14     1    Create10msIncrement    作成時の 10ms 精度補正
0x15     1    LastModified10msIncrement
0x16     1    CreateUtcOffset    UTC オフセット (符号付き 15 分単位)
0x17     1    LastModifiedUtcOffset
0x18     1    LastAccessedUtcOffset
0x19     7    Reserved2
```

FileAttributes の主要ビット:

| bit | マスク | 意味 |
|---|---|---|
| 0 | 0x0001 | ReadOnly |
| 1 | 0x0002 | Hidden |
| 2 | 0x0004 | System |
| 4 | 0x0010 | Directory |
| 5 | 0x0020 | Archive |

#### Stream Extension (0xC0 / 0x40)

```
Offset  Size  Field
0x00     1    EntryType
0x01     1    GeneralSecondaryFlags
             bit0: AllocationPossible (1=クラスタ割り当て済み)
             bit1: NoFatChain (1=連続クラスタ, FAT チェーン不使用)
0x02     1    Reserved1
0x03     1    NameLength         ファイル名の UTF-16LE 文字数 (1-255)
0x04     2    NameHash
0x06     2    Reserved2
0x08     8    ValidDataLength    実データバイト数 (ファイルサイズ)
0x10     4    Reserved3
0x14     4    FirstCluster       先頭クラスタ番号
0x18     8    DataLength         割り当て済みバイト数 (≥ ValidDataLength)
```

**NoFatChain フラグ**
フラッシュ最適化のため、連続クラスタが保証されている場合にこのビットが立つ。
このとき FAT チェーンは信頼できず、`DataLength / ClusterSize` で切り上げた個数分の連続クラスタを直接読む。

#### File Name Extension (0xC1 / 0x41)

```
Offset  Size  Field
0x00     1    EntryType
0x01     1    GeneralSecondaryFlags
0x02    30    FileName           UTF-16LE (1 エントリあたり最大 15 文字)
```

1 エントリに 15 文字まで格納できるため、ファイル名が 16 文字以上の場合は複数の File Name Extension が連なる。
最大 255 文字 → 最大 17 エントリ。

#### エントリセットの全体像

```
[File Entry 0x85]          1 エントリ (32 B)
[Stream Extension 0xC0]    1 エントリ (32 B)
[File Name Extension 0xC1] ceil(NameLength / 15) エントリ

SecondaryCount = 1 (Stream) + ceil(NameLength / 15) (FileName)

エントリセット合計サイズ = (1 + SecondaryCount) × 32 B
```

---

## 2. タイムスタンプ

### 2.1 エンコード形式

exFAT タイムスタンプは 32bit の独自形式であり、FILETIME (NTFS) とは異なる。

```
bit 31:25  Year-1980   (0 = 1980 年)   7 bit
bit 24:21  Month        (1-12)          4 bit
bit 20:16  Day          (1-31)          5 bit
bit 15:11  Hour         (0-23)          5 bit
bit 10: 5  Minute       (0-59)          6 bit
bit  4: 0  DoubleSecond (0-29 → ×2)    5 bit
```

**秒の計算:**
```
Second = DoubleSecond × 2 + (10msIncrement / 100)
Millisecond = (10msIncrement % 100) × 10
```

`10msIncrement` は File Entry の `Create10msIncrement` / `LastModified10msIncrement` フィールドに格納される (0-199)。

### 2.2 タイムゾーン

File Entry の UTC オフセットフィールド (`CreateUtcOffset` など) が示す値:

```
符号付き 8bit。有効範囲: -128 ～ 127
実際の UTC オフセット = 値 × 15 分
例: 0x24 (36) → 36 × 15 = +540 分 → UTC+9 (日本時間)
    0x00        → UTC オフセット不明 (古いドライバが書いた場合)
    0xFF (-1)   → UTC-0:15 (正常な負値として扱われることもある)
```

**フォレンジックでの留意点**

- **exFAT タイムスタンプはローカル時刻で格納される**。NTFS の FILETIME (常に UTC) とは根本的に異なる。
- UTC オフセットフィールドは Windows 10 1903 以降で書き込まれるようになった。それ以前の OS や古いデジタルカメラ等が書いたボリュームでは `0x00` (不明) になっており、タイムゾーンが特定できない。
- UTC オフセットが `0x00` の場合、タイムスタンプが示すタイムゾーンは書き込み機器の設定に依存する。デバイス設定・地域・他の証拠との照合が必要。
- 日本の機器が書いた場合は通常 UTC+9 (JST) が前提となるが、機器の時計設定が誤っている可能性も排除できない。

### 2.3 タイムスタンプの意味

| フィールド | 意味 | 更新タイミング |
|---|---|---|
| CreateTime | 作成日時 | ファイル作成時のみ |
| LastModifiedTime | 最終更新日時 | ファイル内容変更時 |
| LastAccessedTime | 最終アクセス日時 | 読み書き時 (OS 設定依存) |

CreateTime は移動・コピー時の挙動に注意:

- **同一ボリューム内移動**: CreateTime は変わらない
- **別ボリュームへのコピー**: コピー先の CreateTime は **コピー実施時刻** になる (Windows の動作)
- **ごみ箱への移動**: CreateTime は変わらない (削除されたのは LastModifiedTime で追跡)

---

## 3. 削除ファイルの検出

### 3.1 削除マーキングの仕組み

ファイル削除時、OS は以下の処理を行う:

1. File Entry の EntryType を `0x85` → `0x05` に書き換える (InUse bit をクリア)
2. Stream Extension を `0xC0` → `0x40` に書き換える
3. File Name Extension を `0xC1` → `0x41` に書き換える
4. FAT チェーンのエントリを `0x00000000` (空き) に戻す
5. ディレクトリの親エントリのデータは変更しない

クラスタ自体のデータは消去されない。FAT エントリが解放された後、新しいデータが上書きされるまでカービング可能。

### 3.2 削除エントリの読み取り可能範囲

| 情報 | 削除後の取得可否 |
|---|---|
| ファイル名 | ✓ 可能 (File Name Extension が残る) |
| ファイルサイズ | ✓ 可能 (Stream Extension の ValidDataLength が残る) |
| タイムスタンプ | ✓ 可能 (File Entry が残る) |
| 先頭クラスタ番号 | ✓ 可能 (Stream Extension の FirstCluster が残る) |
| FAT チェーン | ✗ 不可 (解放されている) |
| ファイル内容 | △ 先頭クラスタは残存可能性あり。連続クラスタでなければ途中から破損 |

`fat_decode` は削除エントリも出力するため、cluster_no フィールドが先頭クラスタの手がかりになる。

### 3.3 削除ファイルのカービング

先頭クラスタが判明している場合:

```bash
# cluster_no=100940, BytesPerCluster=131072, ClusterHeapOffset=0xB00000
# バイトオフセット = 0xB00000 + (100940 - 2) × 131072

python3 -c "print(hex(0xB00000 + (100940-2)*131072))"
# → 0x7B1FA0000

dd if=/dev/loop0p3 bs=131072 skip=$((100940-2)) \
   iflag=skip_bytes,count_bytes count=131072 \
   of=recovered_cluster.bin
```

ファイル全体の復元は、連続クラスタかどうかに依存する。
NoFatChain フラグが立っていたなら連続クラスタなのでサイズ分まとめて復元できる。

---

## 4. `fat_decode` ツール

### 4.1 ツールの目的

`fat_decode` は FAT32 / exFAT ボリューム上の全ファイル・ディレクトリを走査し、
`mft_decode` と同じ列構造の CSV を出力するフォレンジックツールである。VBR の
シグネチャで FAT32 と exFAT を自動判定するので、同じツールでどちらも扱える。

- 2 パス方式: Pass 1 でディレクトリ構造を把握してからフルパスを構築し、Pass 2 で CSV 出力する
- 削除エントリ (`DELETED`) も出力する
- 商用ツール不要。デバイスへの書き込みゼロ (読み取り専用アクセス)
- マウント中のパーティションでも動作する

### 4.2 コンパイルと実行

```bash
# コンパイル (モジュール構成。make で fat_decode が作られる)
make

# 実行 (パーティションデバイスを直接指定)
sudo ./fat_decode /dev/loop0p3
sudo ./fat_decode /dev/loop0p3 output.csv

# ディスクイメージから loop デバイスを作成する場合
sudo losetup -P /dev/loop0 disk.img        # -P でパーティション自動認識
# → /dev/loop0p1, /dev/loop0p2, /dev/loop0p3 が作成される
sudo ./fat_decode /dev/loop0p3 output.csv
```

**注意**: ディスクデバイス全体 (`/dev/loop0`) ではなく、
パーティションデバイス (`/dev/loop0p3`) を指定すること。
VBR は各パーティションの先頭セクタにある。

### 4.3 CSV 出力列の説明

```
cluster_no, parent_cluster_no, status, type,
modified_tm, accessed_tm, created_tm,
filename, fullpath, file_size
```

| 列 | 内容 |
|---|---|
| `cluster_no` | ファイル/ディレクトリの先頭クラスタ番号 |
| `parent_cluster_no` | 親ディレクトリの先頭クラスタ番号 |
| `status` | `IN_USE` または `DELETED` |
| `type` | `FILE` または `DIR` |
| `modified_tm` | LastModifiedTime + 10ms精度 |
| `accessed_tm` | LastAccessedTime |
| `created_tm` | CreateTime + 10ms精度 |
| `filename` | ファイル名 (UTF-8) |
| `fullpath` | ルートからのフルパス |
| `file_size` | ValidDataLength (DIR は 0) |

### 4.4 動作原理

#### Pass 1: ディレクトリテーブル構築

ルートディレクトリからすべてのサブディレクトリを再帰的に走査し、
`(先頭クラスタ番号 → 親クラスタ番号, ディレクトリ名)` のハッシュテーブルを構築する。
これにより Pass 2 でのフルパス構築が可能になる。

ディレクトリの重複登録はクラスタ番号で検出し、無限ループを防止している。

#### Pass 2: CSV 出力

再度ルートから再帰走査し、全エントリ (使用中・削除済み両方) を CSV 出力する。
フルパスはスタックに積んだパーツを逆順結合して構築する (mft_decode の build_fullpath と同じ方式)。

#### FAT キャッシュ

FAT 全体を起動時にメモリへ読み込んでキャッシュする。
128 KB クラスタ / 120 GB ボリュームのケース (本件 HDD1 p3):
- クラスタ総数: 2,457,512
- FAT サイズ: 2,457,514 × 4 B ≒ 9.4 MB

FAT キャッシュにより、ディレクトリ読み込み時の逐次 FAT アクセスが不要になり、
大規模ボリュームでも高速に走査できる。

#### NoFatChain 対応

Stream Extension の `GeneralSecondaryFlags bit1` が立っているディレクトリは、
FAT チェーンではなく連続クラスタとして読む。
フラッシュメモリ最適化の exFAT ドライバがこの形式を好む。

### 4.5 実行例と出力確認

```
# デバイス              : /dev/loop0p3
# FS 種別               : exFAT 1.00
# シリアル番号          : B4AF-2A48
# セクタサイズ          : 512 B
# クラスタサイズ        : 131072 B
# クラスタ総数          : 2457512
# ルートクラスタ        : 6
# FAT 読み込み中 (9 MB)...
# FAT 読み込み完了
# Pass 1: ディレクトリテーブル構築中...
# ディレクトリテーブル構築完了
# Pass 2: CSV 出力中...
# ─────────────────────────────────
# 処理済みエントリ: 67992
# IN_USE          : 67670
# DELETED         : 322
```

---

## 5. 主要フォレンジックアーティファクト

### 5.1 $RECYCLE.BIN (ごみ箱)

exFAT ボリュームにも NTFS と同様の構造のごみ箱ディレクトリが存在する。
Windows は `$RECYCLE.BIN` ディレクトリをルートに作成し、SID ごとのサブディレクトリに削除ファイルを格納する。

**ごみ箱内のファイル命名規則:**

| プレフィックス | 役割 |
|---|---|
| `$I` + ランダム文字列 + 拡張子 | メタデータファイル。元のパス・削除日時・元のサイズを格納 |
| `$R` + ランダム文字列 + 拡張子 | ファイル本体 (元の内容) |

`$I` と `$R` はランダム部分が同じ文字列でペアを形成する。

**調査の着目点:**

- `$I` が IN_USE で `$R` が DELETED → ごみ箱から「完全削除」された状態
- `$I` が DELETED で `$R` が DELETED → 両方が削除されている (ごみ箱空にした後)
- `$I` に元のフルパスが記録されているため、ファイルの元の場所が判明する

**`$I` ファイルの構造** (バイナリ):

```
Offset  Size  内容
0x00     8    シグネチャ (0x0000000000000001 または 0x0000000000000002)
0x08     8    元のファイルサイズ (バイト)
0x10     8    削除日時 (FILETIME, UTC)
0x18     4    元のパス長 (文字数, バージョン2のみ)
0x1C     可変  元のパス (UTF-16LE)
```

```bash
# $I ファイルを ASCII/UTF-16 で確認
iconv -f UTF-16LE -t UTF-8 '$IXXX.ext' 2>/dev/null | strings

# クラスタ番号から直接読む
dd if=/dev/loop0p3 bs=131072 skip=$((CLUSTER-2)) count=1 | \
  xxd | head -20
```

### 5.2 System Volume Information

Windows が自動的に作成するシステムディレクトリ。exFAT ボリュームにも存在する。

```
System Volume Information/
├── WPSettings.dat          Windows Portable デバイス設定
├── IndexerVolumeGuid       ボリューム GUID (検索インデックス用)
└── EDP/                    Enterprise Data Protection 関連
    └── Recovery/
```

**IndexerVolumeGuid**: このファイルに格納された GUID は、Windows が該当ボリュームを識別するために使用する。
同じボリュームが別の PC に接続された場合でも GUID は変わらないため、
LNK ファイルや JumpList の VolumeID と照合することでデバイスの同一性確認に使える。

```bash
# GUID を確認
cluster=$( grep 'IndexerVolumeGuid' output.csv | cut -d, -f1 )
dd if=/dev/loop0p3 bs=131072 skip=$((cluster-2)) count=1 2>/dev/null | \
  cat | tr -d '\000' | head -c 40
```

### 5.3 タイムスタンプ比較による接続日時の推定

デバイスをパソコンに接続して新規ファイルが作成された場合、
その CreateTime が「接続した時刻の前後」に集中する。

```bash
# ある日時前後に作成されたファイルを抽出 (例: 2021-12-09)
grep 'IN_USE' output.csv | \
  awk -F'"' '{ if ($6 >= "2021-12-09" && $6 < "2021-12-10") print }' | \
  sort -t'"' -k6
```

`System Volume Information` の `WPSettings.dat` や `IndexerVolumeGuid` の
CreateTime は、Windows がそのボリュームを初めてマウントした時刻を示す場合がある。

---

## 6. よく使うコマンド

```bash
# 削除ファイルのみ抽出
grep ',DELETED,' output.csv

# ディレクトリ一覧のみ (フォルダ構造の把握)
grep ',DIR,' output.csv | cut -d, -f9

# 特定の拡張子のファイル (例: .dll)
grep -i '\.dll"' output.csv

# 大きなファイル上位10件
sort -t, -k10 -rn output.csv | head -10

# 更新日時でソート (降順)
sort -t'"' -k2 -r output.csv | head -20

# ごみ箱内のファイル
grep '"\$RECYCLE\.BIN/' output.csv

# ごみ箱の削除済みファイルで $R プレフィックスのもの
grep '"\$RECYCLE\.BIN.*\$R' output.csv | grep ',DELETED,'

# cluster_no 指定でバイナリ抽出
CLUSTER=100940
CLUSTER_SIZE=131072
dd if=/dev/loop0p3 \
   bs=$CLUSTER_SIZE skip=$((CLUSTER-2)) count=1 \
   of=cluster_${CLUSTER}.bin 2>/dev/null

# ファイルサイズ 0 かつ IN_USE のファイル (空ファイル・フォルダ)
awk -F, '$3=="IN_USE" && $4=="FILE" && $NF=="0"' output.csv
```

---

## 7. NTFS との構造上の相違点

調査時に NTFS との混同を避けるための対照表。

| 項目 | NTFS | exFAT |
|---|---|---|
| ファイル管理単位 | MFT レコード (MFT#) | ディレクトリエントリセット |
| エントリ識別子 | MFT# (通し番号) | 先頭クラスタ番号 |
| 削除エントリの残存 | MFT レコードが残る | ディレクトリエントリが残る |
| ファイルサイズ | $DATA 属性の data_size | Stream Extension の ValidDataLength |
| タイムスタンプ形式 | 100ns 単位 FILETIME (UTC) | DoubleSecond + 10ms 補正 (ローカル時刻) |
| タイムゾーン | 常に UTC | ローカル時刻 (UTC オフセット付きは Win10 1903+) |
| ADS (代替データストリーム) | あり ($DATA 複数) | なし |
| ハードリンク | あり (同一 MFT# への参照) | なし |
| シンボリックリンク | あり (Reparse Point) | なし (仕様上未定義) |
| ジャーナル | $LogFile, $UsnJrnl | なし (exFAT 仕様に含まれない) |
| ごみ箱 | $RECYCLE.BIN (Windows が管理) | 同上 |

---

## 8. 注意事項・制限

### 8.1 クラスタ番号の扱い

exFAT ではエントリの一意識別子として「先頭クラスタ番号」を使用するが、
削除されたファイルはその後クラスタが解放・再利用される。
したがって同じ cluster_no に異なるファイルが存在する場合があり、
cluster_no のみで同一性を断定することはできない。

### 8.2 タイムスタンプ精度とタイムゾーン

前述のとおり、UTC オフセットフィールドが `0x00` のエントリは
ローカル時刻のタイムゾーンが不明であることを報告書に明記すること。

### 8.3 マウント中デバイスへのアクセス

マウント中のパーティションに対して `fat_decode` を実行した場合、
OS がディレクトリを変更中のタイミングでは不整合なデータを読む可能性がある。
重要証拠の取得は、デバイスを書き込み禁止アダプタ経由で接続し、
アンマウント状態で行うことが望ましい。

### 8.4 TexFAT

組み込み向けのトランザクション exFAT (TexFAT) は FAT を 2 本持つ。
本ツールは NumberOfFats=1 (通常 exFAT) を前提とする。

---

## 参考

- Microsoft exFAT File System Specification (exFAT-FileSysSpec.docx)
  - [https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-exfat/](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-exfat/)
- `fat_decode` (FAT32/exFAT 対応、本ハンドブック付属ツール)
