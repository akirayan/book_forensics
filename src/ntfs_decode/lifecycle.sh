#!/bin/bash
# ============================================================
# lifecycle.sh v05
#
# 指定した MFT# のファイル/フォルダのライフサイクルを調査する
#
# 使い方:
#   bash lifecycle.sh <MFT#>
#   例: bash lifecycle.sh 55
#
# v02 からの修正:
#   [Fix1] ファイル名フィルタ：ターゲットと無関係な MFT 再利用を識別
#   [Fix2] Section 4 の窓重複修正：USN 消滅 seq はスキップ
#   [Fix3] Section 6 にサマリーテーブルを追加
#   [Fix4] Section 2 の Timestomp 判定を tm_judge の verdict.csv から取得
#          （mft_tm_decode を decode 専用に分離し判定列を廃止したため）
#   [Fix5] Section 2 の存在チェック awk の exit が if の外にあり、先頭データ
#          行しか見ずに常に「記録なし」になっていた不具合を修正（exit を if 内へ）
#   [Fix6] Section 5 のトランザクション完了判定を CommitTransaction から
#          ForgetTransaction に修正。実機の $LogFile では通常のファイル操作に
#          CommitTransaction は現れず、完了は ForgetTransaction で示されるため
#          （「$LogFile の構造と解析」章の実測による）
# ============================================================

# -- 設定 ----------------------------------------------------
MFT_NUMBER=$1
MFT_DECODE_CSV="undelete/drive_D_file_list.csv"
TIMESTAMP_CSV="undelete/drive_D_file_timestamps.csv"
VERDICT_CSV="undelete/drive_D_file_verdict.csv"   # tm_judge の段階判定（mft_ref,filename,score,tier,flags）
USNJOURNAL_CSV="usnjournal/D_usnjournal.bin_decoded.txt"
LOGFILE_CSV="undelete/drive_D_logfile.bin_decoded.csv"

# -- 引数チェック ---------------------------------------------
if [ -z "$MFT_NUMBER" ]; then
    echo "使い方: $0 <MFT#>"
    echo "  例:   $0 7966"
    exit 1
fi

# -- ファイル存在チェック --------------------------------------
for f in "$MFT_DECODE_CSV" "$TIMESTAMP_CSV" "$USNJOURNAL_CSV" "$LOGFILE_CSV"; do
    if [ ! -f "$f" ]; then
        echo "警告: ファイルが見つかりません: $f"
    fi
done

echo "============================================================"
echo " lifecycle.sh v05  MFT# $MFT_NUMBER"
echo "============================================================"

# -- ターゲットファイル名（mft_decode の最新エントリから取得）--
TARGET_FILENAME=$(awk -F',' -v mft="$MFT_NUMBER" '
    NR > 1 {
        split($1, a, ":")
        if (a[1] == mft) { gsub(/"/, "", $8); print $8; exit }
    }
' "$MFT_DECODE_CSV")
echo "  ターゲットファイル名: [$TARGET_FILENAME]"

# -- SEQ_LIST 収集（USN + timestamps + mft_decode から全 seq を網羅）--
SEQ_LIST=$(printf "%s\n%s\n%s\n" \
    "$(awk -F',' -v mft="$MFT_NUMBER" '
        NR > 1 { split($1,a,":"); if (a[1]==mft) print a[2] }
    ' "$MFT_DECODE_CSV")" \
    "$(awk -F',' -v mft="$MFT_NUMBER" '
        NR > 1 { split($1,a,":"); if (a[1]==mft) print a[2] }
    ' "$TIMESTAMP_CSV")" \
    "$(awk -F',' -v mft="$MFT_NUMBER" '
        $1 == mft { print $2 }
    ' "$USNJOURNAL_CSV")" \
    | grep -v '^$' | sort -n | uniq)

SEQ_COUNT=$(echo "$SEQ_LIST" | grep -c .)

# ============================================================
# [Fix1] 各 seq の分類
#
# "target" : USN にターゲットファイル名が含まれる
#            OR USN 記録なしで mft_decode に存在する（最新 seq）
# "other"  : USN にターゲットファイル名が含まれない（別ファイルの MFT 再利用）
# ============================================================
classify_seq() {
    local seq=$1

    # USN にターゲット名が含まれるか
    local match=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" \
        -v tgt="$TARGET_FILENAME" '
        $1 == mft && $2 == seq && $8 == tgt { print "yes"; exit }
    ' "$USNJOURNAL_CSV")
    [ -n "$match" ] && echo "target" && return

    # USN 記録なし → mft_decode に存在すればターゲット
    local has_usn=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq { print "yes"; exit }
    ' "$USNJOURNAL_CSV")
    if [ -z "$has_usn" ]; then
        local in_mft=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
            NR > 1 {
                split($1,a,":")
                if (a[1]==mft && a[2]==seq) { print "yes"; exit }
            }
        ' "$MFT_DECODE_CSV")
        [ -n "$in_mft" ] && echo "target" && return
    fi

    echo "other"
}

# seq ごとの主要ファイル名（$ で始まらない最初のファイル名）
seq_fname() {
    local seq=$1
    local f=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $8 !~ /^\$/ { print $8; exit }
    ' "$USNJOURNAL_CSV")
    [ -z "$f" ] && f=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq { print $8; exit }
    ' "$USNJOURNAL_CSV")
    echo "${f:-(不明)}"
}

# ターゲット seq リスト
TARGET_SEQ_LIST=""
for seq in $SEQ_LIST; do
    [ "$(classify_seq $seq)" = "target" ] && \
        TARGET_SEQ_LIST="$TARGET_SEQ_LIST $seq"
done
TARGET_SEQ_LIST=$(echo $TARGET_SEQ_LIST | tr ' ' '\n' | grep -v '^$')
TARGET_SEQ_ARRAY=($TARGET_SEQ_LIST)
TARGET_SEQ_COUNT=${#TARGET_SEQ_ARRAY[@]}

# ============================================================
# Section 1: Basic Info
# ============================================================
echo ""
echo "============================================================"
echo "  Section 1: Basic Info (mft_decode.csv)"
echo "============================================================"

awk -F',' -v mft="$MFT_NUMBER" '
NR == 1 { next }
{
    split($1, a, ":")
    if (a[1] == mft) {
        printf "  mft_ref    : %s\n",  $1
        printf "  status     : %s\n",  $3
        printf "  type       : %s\n",  $4
        printf "  filename   : %s\n",  $8
        printf "  fullpath   : %s\n",  $9
        printf "  file_size  : %s\n",  $10
        printf "  si_modified: %s\n",  $5
        printf "  si_accessed: %s\n",  $6
        printf "  si_created : %s\n",  $7
        print  "  --------------------------------------------------"
    }
}
' "$MFT_DECODE_CSV"

echo "  全インスタンス数      : $SEQ_COUNT seq"
echo "  全 seq                : $(echo $SEQ_LIST | tr '\n' ' ')"
echo "  対象 seq [$TARGET_FILENAME] : $(echo $TARGET_SEQ_LIST | tr '\n' ' ')"

# ============================================================
# Section 2: Timestamps per seq（対象 seq のみ）
# ============================================================
echo ""
echo "============================================================"
echo "  Section 2: Timestamps per seq (timestamps.csv)"
echo "============================================================"

for seq in $SEQ_LIST; do
    label=$(classify_seq $seq)
    fname=$(seq_fname $seq)
    echo ""
    if [ "$label" = "other" ]; then
        echo "  [ seq=$seq  ← 別ファイル: ${fname}（スキップ）]"
        continue
    fi
    echo "  [ seq=$seq  ← $TARGET_FILENAME ]"

    FOUND=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        NR > 1 { split($1,a,":"); if (a[1]==mft && a[2]==seq) { print "yes"; exit } }
    ' "$TIMESTAMP_CSV")

    if [ -z "$FOUND" ]; then
        echo "    ※ timestamps.csv に記録なし（上書き済みの可能性）"
        continue
    fi

    # verdict.csv（tm_judge）があれば段階判定を引く。無ければ /dev/null にして "-"
    VFILE="$VERDICT_CSV"; [ -f "$VFILE" ] || VFILE=/dev/null

    awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" -v vf="$VFILE" '
    # 1 ファイル目: verdict.csv（mft_ref,filename,score,tier,flags）を mft_ref で索引
    FILENAME == vf { if (FNR > 1) { tier[$1]=$4; score[$1]=$3; flags[$1]=$5 } next }
    # 2 ファイル目: timestamps.csv
    FNR == 1 { next }
    {
        split($1, a, ":")
        if (a[1] == mft && a[2] == seq) {
            printf "    fn_created     : %s  ← Birth Time\n", $7
            printf "    si_created     : %s\n", $3
            printf "    si_modified    : %s\n", $4
            printf "    si_mft_modified: %s\n", $5
            printf "    si_accessed    : %s\n", $6
            printf "    fn_modified    : %s\n", $8
            printf "    fn_mft_modified: %s\n", $9
            printf "    fn_accessed    : %s\n", $10
            printf "    filename       : %s\n", $11
            t = ($1 in tier) ? tier[$1] : "-"
            printf "    Timestomp      : %s  (tm_judge: score=%s flags=%s)\n", t, score[$1], flags[$1]
            if ($7 == $3)
                printf "    ※ fn_created = si_created → タイムスタンプ書き戻しなし\n"
            else
                printf "    ※ fn_created ≠ si_created → BASIC_INFO_CHANGE で書き戻しあり\n"
        }
    }
    ' "$VFILE" "$TIMESTAMP_CSV"
done

# ============================================================
# Section 3: USN History per seq（全 seq、別ファイルは簡略表示）
# ============================================================
echo ""
echo "============================================================"
echo "  Section 3: USN History per seq (usnjournal.csv)"
echo "============================================================"

for seq in $SEQ_LIST; do
    label=$(classify_seq $seq)
    fname=$(seq_fname $seq)
    echo ""

    USN_EVENTS=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq
    ' "$USNJOURNAL_CSV" | sort -t',' -k5)

    if [ "$label" = "other" ]; then
        echo "  [ seq=$seq  ← 別ファイル: $fname ]"
        if [ -n "$USN_EVENTS" ]; then
            FIRST=$(echo "$USN_EVENTS" | head -1 | awk -F',' '{print $5}')
            LAST=$(echo  "$USN_EVENTS" | tail -1 | awk -F',' '{print $5}')
            echo "    期間: $FIRST 〜 $LAST"
        fi
        continue
    fi

    echo "  [ seq=$seq  ← $TARGET_FILENAME ]"

    if [ -z "$USN_EVENTS" ]; then
        echo "    ※ USN ジャーナルに記録なし（ローテーションで消滅）"
        continue
    fi

    echo "$USN_EVENTS" \
        | awk -F',' '{printf "    %s  %-60s  %s\n", $5, $6, $8}'

    FIRST_CREATE=$(echo "$USN_EVENTS" \
        | awk -F',' '$6 ~ /FILE_CREATE/' | head -1 | awk -F',' '{print $5}')
    HAS_BIC=$(echo "$USN_EVENTS" \
        | awk -F',' '$6 ~ /BASIC_INFO_CHANGE/' | wc -l | tr -d ' ')
    LAST_DELETE=$(echo "$USN_EVENTS" \
        | awk -F',' '$6 ~ /FILE_DELETE/'  | tail -1 | awk -F',' '{print $5}')

    echo ""
    echo "    -- サマリー ------------------------------------------"
    echo "    最初の FILE_CREATE : ${FIRST_CREATE:-(なし)}"
    if [ "$HAS_BIC" -gt 0 ]; then
        echo "    BASIC_INFO_CHANGE  : あり（コピー元タイムスタンプ書き戻し）"
    else
        echo "    BASIC_INFO_CHANGE  : なし"
    fi
    echo "    最後の FILE_DELETE : ${LAST_DELETE:-(なし / IN_USE または USN 消滅)}"
done

# ============================================================
# Section 4: Children（DIR の場合）
#
# [Fix2] 対象 seq のみ処理
#        USN 記録なし seq はスキップ（窓が確定できない）
#        窓の重複を防ぐため end は FILE_DELETE 時刻のみ使用
#        次の対象 seq の start は使わない（USN消滅 seq がある場合に誤る）
# ============================================================
echo ""
echo "============================================================"
echo "  Section 4: Children (Parent_MFT# = $MFT_NUMBER)"
echo "============================================================"

CHILD_ALL=$(awk -F',' -v mft="$MFT_NUMBER" '$3 == mft' "$USNJOURNAL_CSV")
CHILD_CREATE_ALL=$(echo "$CHILD_ALL" | awk -F',' '$6 ~ /FILE_CREATE/')
CHILD_DELETE_ALL=$(echo "$CHILD_ALL" | awk -F',' '$6 ~ /FILE_DELETE/')

C_TOTAL=$(echo "$CHILD_CREATE_ALL" | awk -F',' '{print $1}' \
    | sort -u | grep -c .)
D_TOTAL=$(echo "$CHILD_DELETE_ALL" | awk -F',' '{print $1}' \
    | sort -u | grep -c .)

if [ "$C_TOTAL" -eq 0 ] && [ "$D_TOTAL" -eq 0 ]; then
    echo "  子エントリなし（FILE の場合は正常）"
else
    echo ""
    echo "  全期間の集計（全 seq 合算）"
    echo "  FILE_CREATE ユニーク件数: $C_TOTAL"
    echo "  FILE_DELETE ユニーク件数: $D_TOTAL"
    echo ""
    echo "  -- 対象 seq ごとの子ファイル操作 -------------------------"

    for seq in $TARGET_SEQ_ARRAY; do
        echo ""
        echo "    seq=$seq"

        # USN 記録なし → スキップ
        HAS_USN=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
            $1 == mft && $2 == seq { print "yes"; exit }
        ' "$USNJOURNAL_CSV")
        if [ -z "$HAS_USN" ]; then
            echo "    ※ USN 記録なし → 子ファイル操作の特定不可（スキップ）"
            continue
        fi

        # ウィンドウ start: フォルダ自身の最初の FILE_CREATE
        WIN_START=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
            $1 == mft && $2 == seq && $6 ~ /FILE_CREATE/
        ' "$USNJOURNAL_CSV" | sort -t',' -k5 | head -1 | awk -F',' '{print $5}')

        # ウィンドウ end: フォルダ自身の最後の FILE_DELETE
        WIN_END=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
            $1 == mft && $2 == seq && $6 ~ /FILE_DELETE/
        ' "$USNJOURNAL_CSV" | sort -t',' -k5 | tail -1 | awk -F',' '{print $5}')

        if [ -z "$WIN_START" ]; then
            echo "    ※ 開始時刻不明のためスキップ"
            continue
        fi
        if [ -z "$WIN_END" ]; then
            echo "    ウィンドウ: $WIN_START 〜 （削除記録なし / USN 消滅）"
            echo "    ※ END 不明のためスキップ（他の seq の子と区別できない）"
            continue
        fi

        echo "    ウィンドウ: $WIN_START 〜 $WIN_END"

        C_CNT=$(echo "$CHILD_CREATE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | awk -F',' '{print $1}' | sort -u | grep -c .)
        C_FIRST=$(echo "$CHILD_CREATE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | sort -t',' -k5 | head -1 | awk -F',' '{print $5}')
        C_LAST=$(echo "$CHILD_CREATE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | sort -t',' -k5 | tail -1 | awk -F',' '{print $5}')

        D_CNT=$(echo "$CHILD_DELETE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | awk -F',' '{print $1}' | sort -u | grep -c .)
        D_FIRST=$(echo "$CHILD_DELETE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | sort -t',' -k5 | head -1 | awk -F',' '{print $5}')
        D_LAST=$(echo "$CHILD_DELETE_ALL" \
            | awk -F',' -v s="$WIN_START" -v e="$WIN_END" \
                '$5 >= s && $5 <= e' \
            | sort -t',' -k5 | tail -1 | awk -F',' '{print $5}')

        echo "    FILE_CREATE: $C_CNT 件  最初=$C_FIRST  最後=$C_LAST"
        echo "    FILE_DELETE: $D_CNT 件  最初=$D_FIRST  最後=$D_LAST"
    done
fi

# ============================================================
# Section 5: $LogFile による確証
# ============================================================
echo ""
echo "============================================================"
echo "  Section 5: LogFile verification (logfile.csv)"
echo "============================================================"

LOG_ENTRIES=$(awk -F',' -v mft="$MFT_NUMBER" 'NR > 1 && $7 == mft' "$LOGFILE_CSV")

if [ -z "$LOG_ENTRIES" ]; then
    echo "  ※ LogFile に記録なし（ローテーションで消滅した可能性）"
else
    echo ""
    echo "  -- ファイル作成（InitializeFileRecordSegment）--"
    echo "$LOG_ENTRIES" \
        | awk -F',' '$5 == "InitializeFileRecordSegment" {
            printf "    LSN:%-15s  TransID=%-8s  %s\n", $1, $4, $10 }'

    echo ""
    echo "  -- ファイル削除（DeallocateFileRecordSegment）--"
    echo "$LOG_ENTRIES" \
        | awk -F',' '$5 == "DeallocateFileRecordSegment" {
            printf "    LSN:%-15s  TransID=%-8s\n", $1, $4 }'

    echo ""
    echo "  -- トランザクション完了（ForgetTransaction）--"
    # 実機の $LogFile では通常操作に CommitTransaction は出ず、完了は
    # ForgetTransaction で示される（「$LogFile の構造と解析」章の実測）
    for tid in $(echo "$LOG_ENTRIES" | awk -F',' '{print $4}' | sort -u); do
        DONE=$(awk -F',' -v t="$tid" '
            $4 == t && $5 == "ForgetTransaction"
        ' "$LOGFILE_CSV" | wc -l | tr -d ' ')
        if [ "$DONE" -gt 0 ]; then
            echo "    TransID=$tid → ForgetTransaction あり（完了）"
        else
            echo "    TransID=$tid → ForgetTransaction なし（完了記録なし）"
        fi
    done
fi

# ============================================================
# Section 6: Timeline Summary
# [Fix3] ①全再利用履歴（別ファイルも含む）
#         ②対象ファイルのみのサマリーテーブル
# ============================================================
echo ""
echo "============================================================"
echo "  Section 6: Timeline Summary"
echo "============================================================"
echo ""
echo "  -- MFT# $MFT_NUMBER の全再利用履歴 ------------------------"
echo ""

INSTANCE_NUM=0
for seq in $SEQ_LIST; do
    label=$(classify_seq $seq)
    fname=$(seq_fname $seq)

    # Birth Time: timestamps 優先 → USN の最初の FILE_CREATE
    BIRTH=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        NR > 1 {
            split($1,a,":")
            if (a[1]==mft && a[2]==seq) { gsub(/"/, "", $7); print $7; exit }
        }
    ' "$TIMESTAMP_CSV")
    [ -z "$BIRTH" ] && BIRTH=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /FILE_CREATE/
    ' "$USNJOURNAL_CSV" | sort -t',' -k5 | head -1 | awk -F',' '{print $5}')

    # BASIC_INFO_CHANGE
    HAS_BIC=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /BASIC_INFO_CHANGE/
    ' "$USNJOURNAL_CSV" | wc -l | tr -d ' ')

    # 削除時刻
    DELETE_TIME=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /FILE_DELETE/
    ' "$USNJOURNAL_CSV" | sort -t',' -k5 | tail -1 | awk -F',' '{print $5}')

    # MFT ステータス
    STATUS=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        NR > 1 { split($1,a,":"); if (a[1]==mft && a[2]==seq) { print $3; exit } }
    ' "$MFT_DECODE_CSV")

    [ "$HAS_BIC" -gt 0 ] && BIC_MSG="あり" || BIC_MSG="なし"

    if [ "$label" = "target" ]; then
        INSTANCE_NUM=$((INSTANCE_NUM + 1))
        echo "  ★ 第${INSTANCE_NUM}回  seq=$seq  [$TARGET_FILENAME]"
    else
        echo "  - 別ファイル  seq=$seq  [$fname]（MFT 再利用）"
    fi
    echo "    作成時刻（Birth）  : ${BIRTH:-(不明)}"
    echo "    BASIC_INFO_CHANGE  : $BIC_MSG"
    echo "    削除時刻           : ${DELETE_TIME:-(記録なし / IN_USE または USN 消滅)}"
    echo "    MFT ステータス     : ${STATUS:-(上書き済み)}"
    echo ""
done

# -- 対象ファイルのみのサマリーテーブル --
echo ""
echo "  -- [$TARGET_FILENAME] サマリーテーブル ----------------------"
printf "  %-5s  %-6s  %-30s  %-30s  %-4s\n" \
    "回" "seq" "作成時刻（Birth）" "削除時刻" "BIC"
echo "  $(printf '-%.0s' {1..80})"

INSTANCE_NUM=0
PREV_BIRTH=""
PREV_INSTANCE_NUM=0
for seq in $SEQ_LIST; do
    [ "$(classify_seq $seq)" != "target" ] && continue
    INSTANCE_NUM=$((INSTANCE_NUM + 1))

    BIRTH=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        NR > 1 {
            split($1,a,":")
            if (a[1]==mft && a[2]==seq) { gsub(/"/, "", $7); print $7; exit }
        }
    ' "$TIMESTAMP_CSV")
    [ -z "$BIRTH" ] && BIRTH=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /FILE_CREATE/
    ' "$USNJOURNAL_CSV" | sort -t',' -k5 | head -1 | awk -F',' '{print $5}')

    DELETE_TIME=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /FILE_DELETE/
    ' "$USNJOURNAL_CSV" | sort -t',' -k5 | tail -1 | awk -F',' '{print $5}')

    HAS_USN=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq { print "yes"; exit }
    ' "$USNJOURNAL_CSV")

    HAS_BIC=$(awk -F',' -v mft="$MFT_NUMBER" -v seq="$seq" '
        $1 == mft && $2 == seq && $6 ~ /BASIC_INFO_CHANGE/
    ' "$USNJOURNAL_CSV" | wc -l | tr -d ' ')
    [ "$HAS_BIC" -gt 0 ] && BIC_MSG="あり" || BIC_MSG="なし"

    printf "  %-5s  %-6s  %-30s  %-30s  %-4s\n" \
        "${INSTANCE_NUM}回目" \
        "$seq" \
        "${BIRTH:-(不明)}" \
        "${DELETE_TIME:-(不明/USN消滅)}" \
        "$BIC_MSG"

    # -- 注記の判定 ------------------------------------------

    # 注記1: USN 記録なし（ローテーション消滅）
    if [ -z "$HAS_USN" ]; then
        echo "         ↑ ※ USN 記録なし：ローテーションで消滅。MFT レコードのみ残存"
    fi

    # 注記2: 前の回と Birth 時刻が一致（小数点以下の精度差を吸収して先頭19文字で比較）
    BIRTH_SHORT=$(echo "$BIRTH"       | cut -c1-19)
    PREV_BIRTH_SHORT=$(echo "$PREV_BIRTH" | cut -c1-19)
    if [ -n "$PREV_BIRTH" ] && [ "$BIRTH_SHORT" = "$PREV_BIRTH_SHORT" ]; then
        echo "         ↑ ※ 作成時刻が第${PREV_INSTANCE_NUM}回目と一致"
        echo "              → 独立した操作ではなく同一 Paste 操作で生成された可能性あり"
    fi

    PREV_BIRTH="$BIRTH"
    PREV_INSTANCE_NUM=$INSTANCE_NUM
done

echo ""
echo "============================================================"
echo " 完了  (lifecycle.sh v05)"
echo "============================================================"
