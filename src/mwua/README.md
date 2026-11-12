# MWUA — Make Windows Unix-like Again

フォレンジック本『デジタル・フォレンジックの原理』第2章で使う、Windows 11 VM を
Unix 的な解析足場に整えるためのセットアップスクリプト（管理者権限で実行）。

| スクリプト | 役割 |
|---|---|
| `mwua_setup_gitbash.cmd` | Git Bash（bash/vim）・`gcc`・`jq`・`mount_fileserver` を用意 |
| `mwua_setup_ssh.cmd` | OpenSSH サーバー（ポート22開放。ホストから SSH ログイン） |
| `mwua_setup_ip.cmd HOSTNAME IP GATEWAY` | ホスト名変更＋静的IP/GW/DNS |

例中の IP（`172.16.28.x`）やホスト名は著者のラボの値。各自の環境に読み替えること。
使い方・考え方は本書第2章「MWUA ― Windows を Unix 的に整える」を参照。
