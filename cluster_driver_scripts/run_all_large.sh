#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
LOG=/shared/vine-audit/run_all_large_master.log
echo "[master] $(date) START full-scale (large) sweep, all 5 workflows" >> "$LOG"

echo "[master] $(date) === DV5 prep (large samples) ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_prep_large.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_prep_large

echo "[master] $(date) === MapReduce large: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_mr_large_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_mr_large_all4
echo "[master] $(date) === MapReduce large: sync ===" >> "$LOG"
bash /shared/vine-audit/run_mr_large_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_mr_large_sync

echo "[master] $(date) === CTrend large: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_ctrend_large_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_ctrend_large_all4
echo "[master] $(date) === CTrend large: sync ===" >> "$LOG"
bash /shared/vine-audit/run_ctrend_large_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_ctrend_large_sync

echo "[master] $(date) === DConv large: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_dconv_large_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dconv_large_all4
echo "[master] $(date) === DConv large: sync ===" >> "$LOG"
bash /shared/vine-audit/run_dconv_large_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dconv_large_sync

echo "[master] $(date) === RAG large: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_rag_large_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_rag_large_all4
echo "[master] $(date) === RAG large: sync ===" >> "$LOG"
bash /shared/vine-audit/run_rag_large_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_rag_large_sync

echo "[master] $(date) === DV5 large: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_large_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_large_all4
echo "[master] $(date) === DV5 large: sync ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_large_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_large_sync

echo "[master] $(date) ALL LARGE-SCALE RUNS DONE" >> "$LOG"
touch /shared/vine-audit/DONE_ALL_LARGE
