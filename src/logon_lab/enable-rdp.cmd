@echo off
rem ============================================================
rem  enable-rdp.cmd  --  RDP ハンズオン用のラボ有効化スクリプト
rem
rem  デジタル・フォレンジックの原理（第5部 イベントログ / RDP ログオンの章）
rem  の検証用に、ターゲット VM で RDP を受け付けられるようにする。
rem
rem  実運用のホストでは実行しないこと。隔離したラボ VM 専用。
rem  管理者（Administrator）権限のコマンドプロンプトから実行する。
rem ============================================================

rem --- 管理者権限の確認 ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 管理者権限が必要です。管理者 CMD から実行してください。
    exit /b 1
)

rem --- ターミナルサービス（RDP 受け入れ）を有効化 ---
rem  fDenyTSConnections = 0  ... RDP 接続を拒否しない（＝許可）
reg add "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Terminal Server" ^
    /v fDenyTSConnections /t REG_DWORD /d 0 /f

rem --- ファイアウォールで 3389/tcp の受信を許可 ---
netsh advfirewall firewall add rule name="enable rdp" ^
    dir=in localport=3389 protocol=tcp action=allow

echo.
echo [*] RDP を有効化しました（3389/tcp 受信許可）。
echo [*] 無効に戻すには fDenyTSConnections を 1 にし、"enable rdp" ルールを削除する。
