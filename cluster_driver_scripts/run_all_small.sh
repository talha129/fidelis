#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
LOG=/shared/vine-audit/run_all_small_master.log
echo "[master] $(date) START small-scale sweep, all 5 workflows" >> "$LOG"

echo "[master] $(date) === DV5 prep (small samples) ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_prep_small.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_prep_small

echo "[master] $(date) === MapReduce small: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_mr_small_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_mr_small_all4
echo "[master] $(date) === MapReduce small: sync ===" >> "$LOG"
bash /shared/vine-audit/run_mr_small_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_mr_small_sync

echo "[master] $(date) === CTrend small: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_ctrend_small_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_ctrend_small_all4
echo "[master] $(date) === CTrend small: sync ===" >> "$LOG"
bash /shared/vine-audit/run_ctrend_small_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_ctrend_small_sync

echo "[master] $(date) === DConv small: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_dconv_small_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dconv_small_all4
echo "[master] $(date) === DConv small: sync ===" >> "$LOG"
bash /shared/vine-audit/run_dconv_small_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dconv_small_sync

echo "[master] $(date) === RAG small: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_rag_small_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_rag_small_all4
echo "[master] $(date) === RAG small: sync ===" >> "$LOG"
bash /shared/vine-audit/run_rag_small_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_rag_small_sync

echo "[master] $(date) === DV5 small: all4 ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_small_all4.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_small_all4
echo "[master] $(date) === DV5 small: sync ===" >> "$LOG"
bash /shared/vine-audit/run_dv5_small_sync.sh >> "$LOG" 2>&1
touch /shared/vine-audit/DONE_dv5_small_sync

echo "[master] $(date) ALL SMALL-SCALE RUNS DONE" >> "$LOG"
touch /shared/vine-audit/DONE_ALL_SMALL
