# case_rdp — RDP（リモートデスクトップ）正常ログオンの Security ログサンプル

書籍『デジタル・フォレンジックの原理』の「RDP ログオンの真実——Type=3 と Type=10 が語ること」章（第6部）のハンズオン用に用意した、**Windows 11 への正常な RDP ログオンを観察したときの Security イベントログ**です。

検証用 Windows 11 VM（`WIN11JA114`、172.16.28.114）へ、**2 つの異なるクライアント**から RDP 接続したものを記録しています。実在の個人情報・資格情報は含まれていません。

- `WIN11EN121`（172.16.28.121、Windows の `mstsc`）からの RDP
- macOS の RDP クライアント（172.16.28.1）からの RDP

各セッションは切断（disconnect）ではなく**サインアウト**で閉じ、毎回まっさらな新規セッションとして記録しています。最後にコンソールで入り直し、`wevtutil epl` でエクスポートしました。

## ファイル

| ファイル                   | 内容                                                      |
|----------------------------|-----------------------------------------------------------|
| `Security_rdp_normal.evtx` | `wevtutil epl` でエクスポートした Security ログ（clean、198 件） |

## 完全性の検証

| 対象                       | SHA-256 |
|----------------------------|---------|
| `Security_rdp_normal.evtx` | `f24601b77ee66a06006170162ff6f2034908c7f6eed0cc9060252132c1953756` |

```bash
shasum -a 256 Security_rdp_normal.evtx
# -> f24601b77ee66a06006170162ff6f2034908c7f6eed0cc9060252132c1953756
```

## 読みどころ（この章の核心）

RDP の 4624 には **2 種類の LogonType** が現れる。時刻は UTC。

| 時刻(UTC) | Type | WorkstationName | IpAddress | 意味 |
|---|---|---|---|---|
| 08:01:47 / :49 | **3** | `WIN11EN121` | 172.16.28.121 | en121（Windows）の NLA 認証。**2 件**（クライアント実装） |
| 08:01:51 | **10** ×2 | `WIN11JA114` | 172.16.28.121 | RDP 対話セッション（UAC ペア。WS は接続先＝自分） |
| 08:08:12 | **3** | `-`（空） | 172.16.28.1 | macOS の NLA 認証。**1 件**・WS は空 |
| 08:08:13 | **10** ×2 | `WIN11JA114` | 172.16.28.1 | RDP 対話セッション（UAC ペア） |

- **Type=10 の `WorkstationName` は接続先自身（`WIN11JA114`）で固定**——接続元の手がかりにならない（罠）。
- **接続元を確実に指すのは `IpAddress`**。`WorkstationName` は Type=3 に接続元名が入る「ことがある」が、macOS クライアントは空で送る——クライアント実装依存。
- **UAC の標準／昇格ペアになるのは Type=10 のみ**（`TargetLinkedLogonId` で相互参照）。Type=3（NLA）はペアではない。
- 本人のサインアウト `4647` は Type=10 の標準トークン側に出る。

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
