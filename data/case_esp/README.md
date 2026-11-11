# case_esp — EFI System Partition (ESP) sample

書籍『デジタル・フォレンジックの原理』のハンズオン用に用意した、**EFI システムパーティション（ESP）だけを切り出したサンプルイメージ**です。

検証用に作成した Windows 11（UEFI ブート）環境のディスクから、ESP（FAT32）の領域のみを抽出しています。ユーザーデータや資格情報を含む領域（Windows 本体・SAM/SYSTEM ハイブ等）は**含まれていません**。中身は UEFI のブートファイル（`EFI/Boot/`, `EFI/Microsoft/`）です。

## ファイル

| ファイル        | 内容                                   |
|-----------------|----------------------------------------|
| `esp.img.gz`    | ESP を `gzip` 圧縮した raw イメージ（約 18 MB） |

展開すると約 200 MB の FAT32 raw イメージ（`esp.img`）になります。

## 完全性の検証

| 対象                       | SHA-256 |
|----------------------------|---------|
| 展開後 `esp.img`           | `dd8399c64d103b56926047989c327c7967659b7ad4bcdf7b3cff172d4f7d2249` |

```bash
gzip -dc esp.img.gz | sha256sum
# -> dd8399c64d103b56926047989c327c7967659b7ad4bcdf7b3cff172d4f7d2249
```

## 使い方（Linux）

展開して、ループバックで読み取り専用マウントする。

```bash
# 1. 展開
gunzip -k esp.img.gz          # esp.img が作られる（-k で .gz を残す）

# 2. 読み取り専用でマウント
mkdir -p /mnt/esp
sudo mount -o ro,loop esp.img /mnt/esp

# 3. 中身を確認
ls -lR /mnt/esp/EFI

# 4. 後始末
sudo umount /mnt/esp
```

このイメージは ESP パーティション単体なので、GPT のオフセット計算は不要で、そのまま FAT32 としてマウントできる。

## 注意

ブート構成情報 `EFI/Microsoft/Boot/BCD` はレジストリ（REGF）ハイブである。ブート設定（ブートローダのパスやデバイス GUID）が含まれるが、ユーザーの資格情報は含まない。
