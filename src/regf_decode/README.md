# regf_decode — Windows レジストリハイブ(REGF)の自作デコーダ

Windows のレジストリハイブ（`SYSTEM` / `SOFTWARE` / `SAM` / `SECURITY` / `NTUSER.DAT` / `Amcache.hve` など）を、
ライブラリに頼らず **C で一から**デコードする学習・調査用ツール。
ファイル署名 `regf` から始めて、ハイブビン（`hbin`）→セル→**キー(`nk`)のツリー**と**値(`vk`)**を
バイト単位で解き、`キーパス|KEY|時刻` と `キーパス/値名|型|値` の形で出力する。

設計は姉妹ツール **evtx_decode** に倣った層分け（file→hbin→cell→keys→value）。目的は
「ツールの出力を信じる」ことではなく、**レジストリの構造を自分の手で解いて理解する**こと
（`~/src/wheel_reinvention/` の方針＝車輪の再発明）。

## できること

- `regf` ファイルヘッダ（ルートセルの位置・hive bins サイズ）の検証・表示
- 全 `hbin`・全セルの走査（`-v`）
- **キーツリーの再帰列挙**：`nk`（キー）と、サブキーリスト `lf` / `lh` / `ri` / `li` の全分岐
- **値の解読**：`REG_SZ` / `REG_EXPAND_SZ` / `REG_LINK` / `REG_DWORD` / `REG_DWORD_BE` / `REG_QWORD` /
  `REG_MULTI_SZ` / `REG_BINARY` / `REG_NONE`、インライン値（4バイト）と別セル値の両方
- UTF-16LE → UTF-8（日本語などの値名・文字列も正しく表示）
- フルパスでのフィルタ（`-p`）と、値のフルダンプ（`-d`、regedit 形式・ワイルドカード対応）
- `REG_SZ` を名乗るバイナリ（SAM の SID 等）を検出して `[binary-as-SZ]` と注記

## 処理の流れ

```
main.c            ── CLI 解釈、ハイブを丸ごとメモリに読み込み、regf 署名を確認
  └ regf_file     ── regf ヘッダ検証 → ルートセルから列挙開始
      └ regf_hbin     ── 全 hbin の走査（-v の詳細表示）
          └ regf_cell     ── hbin 内の全セル走査（-v）／ 共有 regf_cell_ptr()
              └ regf_keys     ── nk と lf/lh/ri/li を辿り、パス構築・-p フィルタ
                  └ regf_value    ── vk を型ごとに解読して出力（evtx の output 層に相当）
```

## ビルド

```bash
make            # gcc -std=c11、regf_decode を生成
make clean
```

- 依存は C コンパイラと **iconv**（Makefile は `-liconv`）。macOS は標準で通る。Linux で
  `-liconv` のリンクに失敗する場合は glibc が iconv を内蔵しているので `LDFLAGS` から外す。

## 使い方

```
./regf_decode [-v] [-f] [-p PATH] [-d PATTERN] <hivefile>

  -v            詳細表示（regf ヘッダ・hbin・セルを標準エラーへ）
  -f            バイナリ値を省略せず全バイト出力（既定は先頭32バイトで打ち切り）
  -p PATH       指定パスとその子孫だけを出力（前方一致）
  -d PATTERN    値のフルパスがマッチしたら regedit 形式でフルダンプ（* ? 使用可）

出力（既定）:
  キーパス|KEY|最終更新時刻(ISO8601)
  キーパス/値名|型|値
```

### 例

```bash
./regf_decode SYSTEM                                   # 全キー・全値
./regf_decode -p "/ROOT/ControlSet001/Enum/USBSTOR" SYSTEM
./regf_decode -f -p "/ROOT/Select" SYSTEM              # バイナリを省略せず
./regf_decode -d "*PartitionTableCache" SYSTEM         # 該当値を regedit 形式でダンプ
./regf_decode SOFTWARE | grep '|REG_MULTI_SZ|'
```

出力例（抜粋）:

```text
/ROOT/ControlSet001/Control/EarlyStartServices|REG_MULTI_SZ|RpcSs | Power | BrokerInfrastructure | ...
/ROOT/Microsoft/Windows/CurrentVersion/ProgramFilesDir|REG_SZ|C:\Program Files
/ROOT/Adobe/.../DisableMaintenance|REG_DWORD|0x00000001 (1)
```

## 入力の用意（ハイブの取り出し）

解析対象のハイブは、ディスクイメージから取り出すか、オフラインのボリュームからコピーする。

- ディスク上の場所：`C:\Windows\System32\config\{SYSTEM, SOFTWARE, SAM, SECURITY}`、
  各ユーザーの `NTUSER.DAT`、`C:\Windows\AppCompat\Programs\Amcache.hve`。
- The Sleuth Kit で E01 等から取り出す例：

```bash
fls -o <offset> image.e01 | grep -i config         # config ディレクトリの MFT# を探す
icat -o <offset> image.e01 <SYSTEM の MFT#> > SYSTEM
```

稼働中の Windows からは `reg save HKLM\SYSTEM system.hiv` でも取れる（ロック回避）。

## ソース構成

| ファイル | 役割 |
|---|---|
| `main.c` | 引数解釈、ハイブ全体をメモリへ、`regf` 署名確認、`decode_regf_file` 呼び出し |
| `regf_file.c/.h` | `regf` ヘッダ（4096B）の検証・表示、ルートセルから列挙開始 |
| `regf_hbin.c/.h` | 全 `hbin` の走査（`-v` 詳細） |
| `regf_cell.c/.h` | hbin 内セルの走査（`-v`）、共有 `regf_cell_ptr()`（オフセット→ポインタ） |
| `regf_keys.c/.h` | `nk` とサブキーリスト `lf`/`lh`/`ri`/`li` の再帰列挙、パス構築、`-p` フィルタ |
| `regf_value.c/.h` | `vk` の型別解読・出力（`-d` ダンプ含む）。evtx の output 層に相当 |
| `timestamp.c/.h` | FILETIME → ISO8601 |
| `utf16le.c/.h` | UTF-16LE → UTF-8 |
| `regf_main.h` | 構造体（regf/hbin/nk/vk/lf/lh/ri/li）・定数・出力モードのマクロ |

## REGF 構造メモ（デコードの勘所）

- **regf ヘッダ**（先頭 4096B）：署名 `regf`、`root_cell_offset`（ルート `nk` の位置）、
  `hive_bins_data_size`、XOR チェックサム。セルのオフセットは「hive bins data 先頭(`0x1000`)基準」。
- **hbin**：ハイブビン。先頭 32B のヘッダ＋セル群。`hbin_size` だけ進めて次の hbin へ。
- **セル**：先頭 4B が**符号つきサイズ**。**負＝使用中**、正（0 以上）＝解放済み。8 バイト境界。
- **`nk`（キー）**：名前・親・最終更新時刻、サブキー数と `subkey_list_offset`、値数と `value_list_offset`。
- **サブキーリスト**：`lf`/`lh`＝`{nk オフセット, ハッシュ}` の配列、`li`＝nk オフセットの配列、
  `ri`＝**インデックスのインデックス**（`lf`/`lh`/`li` を指す）。大きなキーはこれらが入れ子になる。
- **`vk`（値）**：名前・型・サイズ・データオフセット。サイズに `0x80000000` が立つと
  データは `data_offset` の 4 バイトに**インライン**格納、そうでなければ別セルを指す。

## 方針・制限

- 学習／フォレンジック用であり、完全・汎用パーサではない。
- 既定の出力はパイプ区切りのテキスト（`REG_SZ` 等の整形付き）。`regf_main.h` には CSV/XML 用の
  出力フラグ枠があるが、別形式の出力層は未実装（今後の拡張余地）。
- クラス名・セキュリティ記述子（`sk`）・大容量値（db）の完全展開は扱わない。
- **Fact is Fact**：`REG_SZ` を名乗るバイナリは `[binary-as-SZ]` と明示し、難読化された値（OpenWith の
  command 等）はそのまま忠実に出す。道具が勝手に「それらしく」整えない。
- 出力は決定論的（同じハイブから同じ出力）。実機ハイブ（SYSTEM/SOFTWARE/SECURITY）で検証、
  AddressSanitizer でも境界外アクセスなしを確認済み。
