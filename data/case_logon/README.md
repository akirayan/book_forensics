# case_logon — コンソールログオンの Security ログサンプル

書籍『デジタル・フォレンジックの原理』の「コンソールログオンを観察する」章（第6部）のハンズオン用に用意した、**Windows 11（日本語）でコンソールログオンを観察したときの Security イベントログ**です。

検証用 Windows 11 VM（`WIN11JA114`）で、Security ログをクリア（Event 1102）してから、わざとログオンを失敗させ、正しくログオン・ログオフし、標準ユーザー `testuser` を作成する——という一連の操作を記録したものです。実在の個人情報・資格情報は含まれていません（ログオン失敗の素材として用いたユーザー名 `root` / `admin` / `kanri` は実在しないアカウントで、Security ログにパスワードは残りません）。

## ファイル

| ファイル                | 内容                                                      |
|-------------------------|-----------------------------------------------------------|
| `Security_console.evtx` | `wevtutil epl` でエクスポートした Security ログ（clean、304 件） |

`wevtutil epl`（エクスポート）で取り出しているため、ヘッダは正規化されており `flags=0x00(clean)` です。

## 完全性の検証

| 対象                    | SHA-256 |
|-------------------------|---------|
| `Security_console.evtx` | `9e8032215ac659f711ff1edfe82eaa9f438f8ce523289cb850b281adcd66be67` |

```bash
shasum -a 256 Security_console.evtx
# -> 9e8032215ac659f711ff1edfe82eaa9f438f8ce523289cb850b281adcd66be67
```

## 含まれる主なイベント

| Event ID | 意味                         | このサンプルでの内容                                   |
|----------|------------------------------|--------------------------------------------------------|
| `1102`   | ログのクリア                 | 先頭。観察を楽にするため最初にクリアした記録           |
| `4625`   | ログオン失敗                 | パスワード違い ×3（SubStatus `0xC000006A`）／ユーザー名違い ×3（`runas` 由来、SubStatus `0xC0000064`） |
| `4624`   | ログオン成功                 | コンソール（LogonType 2）。UAC により標準／昇格の 2 トークンがペアで記録される |
| `4672`   | 特別な権限の割り当て         | 昇格トークン側のログオンにのみ付く（標準ユーザー `testuser` には付かない） |
| `4647`   | 利用者によるサインアウト     | 本人が明示的にログオフした記録                         |
| `4634`   | ログオフ                     | セッション破棄（付随アカウント・切断を含む）           |
| `4720` / `4732` | アカウント作成／グループ追加 | `testuser` を作成し `Users` グループへ追加（標準ユーザー。Administrators には入れていない） |

## 読み解きの入口（Linux / macOS）

書籍で自作した `evtx_decode` での例。EventViewer の「イベント ID でフィルタ」は `-e`、「詳細タブの XML 表示」は `-x` に対応する。

```bash
# 概要（ヘッダ・レコード一覧）
evtx_decode Security_console.evtx

# ログオン失敗だけを XML で（SubStatus の対比を確認）
evtx_decode -x -e 4625 Security_console.evtx

# 件数を Event ID 別に数える
evtx_decode Security_console.evtx | grep -oE 'event_id=[0-9]+' | sort | uniq -c | sort -rn
```

標準の `wevtutil` / PowerShell でも読める。

```powershell
wevtutil qe Security_console.evtx /lf:true /f:text /q:"*[System[(EventID=4625)]]"
```
