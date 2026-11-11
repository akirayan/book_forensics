# evtx_decode — Windows イベントログ(EVTX)の自作デコーダ

Windows の `.evtx`（イベントログ）を、ライブラリに頼らず **C で一から**デコードする学習・調査用ツール。
ファイル署名 `ElfFile` から始めて、チャンク（`ElfChnk`）→レコード→**BinXML（バイナリ XML）**をバイト単位で解き、
Windows Event Viewer が見せるのと同じ XML（`<System>` / `<EventData>`）を自力で組み立てて出力する。

目的は「ツールの出力を信じる」ことではなく、**EVTX の構造を自分の手で解いて理解する**ことにある
（`~/src/wheel_reinvention/` の方針＝車輪の再発明）。既存の `python-evtx` / `libevtx` の代替ではなく、
「なぜその痕跡がそう残るか」を構造から言えるようにするための実装である。

## できること

- EVTX ファイルヘッダ（`ElfFile`、チャンク数、`clean`/`dirty`/`full` フラグ）の検証・表示
- 各チャンク（64KB、`ElfChnk`）の走査
- **共通文字列テーブル**（チャンク内の要素名の使い回し）と**テンプレート定義**の解決
- 各レコード（署名 `0x2a2a`、Record ID、FILETIME タイムスタンプ）の取り出し
- **BinXML トークンの解釈**とテンプレート展開 → XML ツリーの構築
- UTF-16LE → UTF-8 変換、FILETIME → ISO8601 への変換
- 出力：既定サマリ / XML / CSV / TXT / デバッグ（hex ダンプ）
- EventID によるフィルタ

## 処理の流れ

```
main.c            ── CLI 解釈、ファイルを開く
  └ evtx_file     ── ElfFile ヘッダ検証 → chunk_count 回ループ
      └ evtx_chunk    ── ElfChnk ヘッダ、共通文字列配列(64)・テンプレート配列(32)
          └ evtx_record   ── レコードヘッダ(0x2a2a / Record ID / FILETIME)
              └ evtx_binxml   ── BinXML トークンを解釈し XML ツリーへ
                  └ evtx_xmltree  ── 要素/属性のツリーを構築
                      └ evtx_output  ── EventID 一致時に、指定形式で出力
```

## ビルド

```bash
make            # gcc -std=c11、evtx_decode を生成
make clean
```

- 依存：C コンパイラ（gcc）と **iconv**（UTF-16LE→UTF-8 変換に使用。Makefile は `-liconv`）。
- macOS では iconv が標準で入っているのでそのまま通る。
  Linux で `-liconv` のリンクに失敗する場合は glibc が iconv を内蔵しているので、Makefile の `LDFLAGS` から `-liconv` を外す。

## 使い方

```
./evtx_decode [options] <evtxfile>

Output options (組み合わせ可):
  -c, --csv     CSV 出力
  -t, --txt     テキスト出力
  -x, --xml     XML 出力
  -d, --debug   デバッグ出力（構造の hex ダンプ）

Filter options:
  -e <EventID>  EventID で絞り込み（例: 4624）

  -h, --help    使い方

出力形式を何も指定しなければ、既定のサマリ出力になる。
```

### 例

```bash
./evtx_decode Security.evtx              # 既定サマリ（ヘッダ＋レコード要約）
./evtx_decode -x Security.evtx           # Event Viewer 相当の XML
./evtx_decode -x -e 4624 Security.evtx   # EventID 4624（ログオン成功）だけ XML で
./evtx_decode -c Security.evtx > out.csv # CSV に書き出し
./evtx_decode -d setup.evtx              # 構造を hex ダンプしながら（学習・検証用）
```

`-x` の出力は、Windows の Event Viewer が「XML 表示」で見せるものと同じ構造になる（下は 4624 の例）。

```xml
<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
  <System>
    <Provider Name="Microsoft-Windows-Security-Auditing" Guid="{54849625-...}" />
    <EventID>4624</EventID>
    ...
    <TimeCreated SystemTime="2026-02-06T08:19:17.346571900Z" />
    <Computer>win11ja111</Computer>
  </System>
  <EventData>
    <Data Name="TargetUserName">SYSTEM</Data>
    ...
  </EventData>
</Event>
```

## ソース構成

| ファイル | 役割 |
|---|---|
| `main.c` | 引数解釈（`check_cmd_argv`）、ファイルを開いて `decode_evtx_file` を呼ぶ |
| `evtx_file.c/.h` | `ElfFile` ファイルヘッダ（4096B）の検証、チャンクのループ |
| `evtx_chunk.c/.h` | `ElfChnk` チャンクヘッダ、共通文字列テーブル、テンプレート定義 |
| `evtx_record.c/.h` | レコードヘッダ（`0x2a2a`・Record ID・FILETIME）、8バイト境界 |
| `evtx_binxml.c/.h` | **BinXML トークンの解釈**（本体）、テンプレート展開、値型の判定 |
| `evtx_xmltree.c/.h` | 汎用 XML ツリー（要素・属性、カーソル式）と各形式のダンプ |
| `evtx_output.c/.h` | 出力モード（下位16bit=形式フラグ / 上位16bit=EventID）と出力の振り分け |
| `utf16le.c/.h` | UTF-16LE → UTF-8（iconv） |
| `timestamp.c/.h` | FILETIME → ISO8601 |
| `hex_dump.c/.h` | hex ダンプ（デバッグ出力） |
| `stack.c/.h` | ツリー走査用スタック |
| `evtx_msgs.c/.h` | メッセージ文字列 |

### 出力モードの表現（`output_mode`, uint32_t）

```
[31 .............. 16][15 .............. 0]
      EventID               Output flags
```

下位16bitに形式フラグ（CSV/TXT/XML/DEBUG）、上位16bitに EventID フィルタ（0=フィルタなし）を詰める。
`evtx_output.h` のマクロ（`SET_OUTMODE` / `GET_EVTID` 等）で読み書きする。

## EVTX 構造メモ（デコードの勘所）

- **ElfFile ヘッダ**（先頭 4096B）：署名 `ElfFile`、`chunk_count`、`flags`（`0x01` dirty / `0x02` full）、CRC32。
- **ElfChnk チャンク**（各 64KB）：ヘッダに、チャンク内で要素名を使い回すための**共通文字列オフセット配列（64）**と、
  繰り返し構造を圧縮する**テンプレート定義オフセット配列（32）**を持つ。ここを解けないと BinXML は読めない。
- **レコード**：署名 `**`（`0x00002a2a`）、`record_size`、`record_identifier`、`timestamp`（FILETIME）。
  末尾に `record_size` のコピーがあり、8バイト境界で整列する。
- **BinXML**：レコード本体は XML そのものではなく、トークン化された**バイナリ XML**。要素・属性・値・
  テンプレート参照・置換配列（Substitution）を順に解釈して、初めて `<Event>...</Event>` が組み上がる。
  EVTX 解析の難所はここで、本ツールの中心も `evtx_binxml.c`。

## 方針・制限

- 学習／フォレンジック用の実装であり、`libevtx` のような完全・汎用ライブラリではない。
- XML ツリーは**要素と属性のみ**（コメント・CDATA・処理命令などは扱わない）。EVTX の表現には十分。
- ヘッダの一部フィールドは用途不明（`unknown`）のまま残している。わかった範囲を正直に書く方針。
- 出力は決定論的（同じ入力から同じ出力）になるよう作ってある。

## 同梱ファイル（開発・検証用）

- `security_4624.xml` / `setup_rec01.xml` / `evt4624.xml` など … 期待される XML 出力の参照。
- `record_0001.txt` ほか … レコードの hex ダンプ例。
- `*.evtx.txt`（`security_full` / `system` / `setup` 等）… 実際の EVTX から取った出力の控え（サイズ大）。
- `test_xml.c` / `test_output.c` … 部分モジュールの単体確認用。
