# case_rdp — RDP（リモートデスクトップ）正常ログオンの Security ログサンプル

書籍『デジタル・フォレンジックの原理』の「RDP ログオンの真実——Type=3 と Type=10 が語ること」章（第6部）のハンズオン用に用意した、**Windows 11 への正常な RDP ログオンを観察したときの Security イベントログ**です。

検証用 Windows 11 VM（`WIN11JA114`、172.16.28.114）へ、**3 つの異なるクライアント**から RDP 接続したものを記録しています。実在の個人情報・資格情報は含まれていません。

- `WIN11EN121`（172.16.28.121、Windows の `mstsc`）からの RDP
- macOS の RDP クライアント（172.16.28.1）からの RDP
- Kali Linux（172.16.28.81、`kali081`）の `xfreerdp`（FreeRDP）からの RDP ※別ファイル

各セッションは切断（disconnect）ではなく**サインアウト**で閉じ、毎回まっさらな新規セッションとして記録しています。最後にコンソールで入り直し、`wevtutil epl` でエクスポートしました。

## ファイル

| ファイル                     | 内容                                                      |
|------------------------------|-----------------------------------------------------------|
| `Security_rdp_normal.evtx`    | Windows（`mstsc`）と macOS からの RDP（clean、198 件） |
| `Security_rdp_linux.evtx`     | Kali Linux の `xfreerdp` からの RDP（clean、80 件） |
| `Security_rdp_takeover2.evtx` | 乗っ取り検証（コンソール→ロック/解除→RDP 乗っ取り→取り戻し）。**前半は監査オフ、後半は「Other Logon/Logoff Events」監査オンで同じ操作を再現**＝監査の有無の before/after が 1 本に（clean、343 件） |

## 完全性の検証

| 対象                          | SHA-256 |
|-------------------------------|---------|
| `Security_rdp_normal.evtx`    | `f24601b77ee66a06006170162ff6f2034908c7f6eed0cc9060252132c1953756` |
| `Security_rdp_linux.evtx`     | `9d4936755c6f120ffe4f54f749a477a83fbc6ba4a3b4583fcd154006ec4b5398` |
| `Security_rdp_takeover2.evtx` | `37d6eb549907cf6f963e7ba53d3baa472a3b64a07d83b5f8c43aa117db275b54` |

```bash
shasum -a 256 Security_rdp_normal.evtx Security_rdp_linux.evtx Security_rdp_takeover2.evtx
```

### `Security_rdp_takeover2.evtx` の読みどころ（通説の訂正＋監査の before/after）

「ロック解除や既存セッションへの RDP 再接続は `Type=7`（Unlock）になる」という通説を実データで検証したもの。現在の Windows 11 では**そうならなかった**。**前半（04:xx、監査オフ）**と**後半（05:xx、監査オン）**が 1 本に入っている。まず前半（監査オフ）。

| 時刻(UTC) | 操作 | 記録された 4624 |
|---|---|---|
| 04:20:53 | コンソールログイン | **Type=2** ×2 |
| 04:30:18 | ロック→**アンロック** | **Type=2** ×2（Type=7 ではない） |
| 04:33:38 | `kali081` から RDP で乗っ取り | **Type=3 ＋ Type=10 ×2**（通常の RDP と同じ） |
| 04:35:25 | コンソールへ取り戻し | **Type=2** ×2 |

- **Type=7 は 1 件も出ない**（Logon 監査は有効なのにアンロックが Type=2 になる、という挙動）。
- `4778`/`4779`（再接続/切断）・`4800`/`4801`（ロック/解除）も出ていないが、これは監査サブカテゴリ **「その他のログオン/ログオフ イベント（Other Logon/Logoff Events）」が未有効**だったため（存在しないのではない）。`auditpol /set /subcategory:"Other Logon/Logoff Events" /success:enable /failure:enable` を入れれば記録される。
- 「乗っ取り」は特別なイベント ID ではなく、**ログオフを挟まず Type=2 と Type=3/10 が交互に積み上がる時系列**（＋デスクトップの継続）として現れる。
- 教訓：痕跡が「無い」と結論する前に、そのイベントが監査対象だったか（`auditpol /get /category:*`）を確かめる。

#### `Security_rdp_takeover2.evtx`（監査オンで同じ操作）

「Other Logon/Logoff Events」を有効化して v1 と同じ乗っ取りを再現したもの。欠けていた痕跡が名指しで残る。

| 時刻(UTC) | 記録 |
|---|---|
| 05:11:17 / 05:12:07 | `4800`（ロック）/ `4801`（アンロック） LogonId=`0x16156b4` |
| 05:14:14 | `4779` 切断 `Session=Console` LogonId=`0x16156b4` |
| 05:14:15 | `4778` 再接続 `Session=RDP-Tcp#0` **Client=`kali081`/`172.16.28.81`** LogonId=`0x16156b4` |
| 05:15:21 / :22 | `4779`（RDP-Tcp#0 切断）→ `4778`（Console 再接続） |

- **同一 `LogonId 0x16156b4`（＝セッション ID 5）が Console↔RDP-Tcp#0 を渡り歩く**＝乗っ取りの明示。`4778` が攻撃側の `ClientName`/`ClientAddress` を名指しする。
- それでも **`Type=7` は v2 でも 0 件**（アンロックは `4801`＋`Type=2`）。増えたのは `4778/4779/4800/4801` であって通説の `Type=7` ではない。
- `auditpol` 変更自体が `4719`（監査ポリシー変更）として残る＝設定をいじれば痕跡になる。
- v1（監査オフ）と v2（監査オン）の差＝**同じ乗っ取りでも残る痕跡は監査設定しだい**。

## 読みどころ（この章の核心）

RDP の 4624 には **2 種類の LogonType** が現れる。時刻は UTC。

| 時刻(UTC) | Type | WorkstationName | IpAddress | 意味 |
|---|---|---|---|---|
| 08:01:47 / :49 | **3** | `WIN11EN121`（大文字） | 172.16.28.121 | en121（Windows `mstsc`）の NLA 認証。**2 件**（クライアント実装） |
| 08:01:51 | **10** ×2 | `WIN11JA114` | 172.16.28.121 | RDP 対話セッション（UAC ペア。WS は接続先＝自分） |
| 08:08:12 | **3** | `-`（空） | 172.16.28.1 | macOS の NLA 認証。**1 件**・WS は空 |
| 08:08:13 | **10** ×2 | `WIN11JA114` | 172.16.28.1 | RDP 対話セッション（UAC ペア） |
| 03:04:45 | **3** | `kali081`（小文字） | 172.16.28.81 | `xfreerdp` の NLA 認証（`Security_rdp_linux.evtx`）。**1 件**・実ホスト名 |
| 03:04:46 | **10** ×2 | `WIN11JA114` | 172.16.28.81 | RDP 対話セッション（UAC ペア） |

- **Type=10 の `WorkstationName` は接続先自身（`WIN11JA114`）で固定**——接続元の手がかりにならない（罠）。
- **接続元を確実に指すのは `IpAddress`**。`WorkstationName` は Type=3 にクライアント次第で入る——Windows は実ホスト名（大文字）、Linux/FreeRDP は実ホスト名（小文字）、macOS は空。しかも `/client-hostname:` で**詐称可能**なので、名乗りは鵜呑みにしない。
- **UAC の標準／昇格ペアになるのは Type=10 のみ**（`TargetLinkedLogonId` で相互参照）。Type=3（NLA）はペアではない。
- 本人のサインアウト `4647` は Type=10 の標準トークン側に出る。
- 古い `rdesktop` は NLA（CredSSP）非対応のため接続できず、痕跡自体が残らない。NLA を話せる `xfreerdp` は上記のログを残す。

## 読み解きの入口（Linux / macOS）

書籍で自作した `evtx_decode` での例。

```bash
# endoaki の 4624 を Type / WorkstationName / IP で一覧
evtx_decode -c -e 4624 Security_rdp_normal.evtx | \
  awk -F',' '{split($10,f,"|"); if(f[6]=="endoaki") \
    printf "%s  Type=%-2s  WS=%-12s IP=%s\n", \
    substr($3,12,8), f[9], (f[12]==""?"-":f[12]), f[19]}'
```

標準の `wevtutil` でも読める。

```powershell
wevtutil qe Security_rdp_normal.evtx /lf:true /f:text /q:"*[System[(EventID=4624)]]"
```
