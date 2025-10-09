#!/usr/bin/env bash
set -euo pipefail

# medicion_speedup.sh
# Uso: ./medicion_speedup.sh [REPS] [PROCS_LIST]
# Ej: ./medicion_speedup.sh 5 "1 2 4 8"
# Defaults: REPS=5, PROCS_LIST="1 2 4 8"

ROOT="$(pwd)"
REPS="${1:-5}"
PROCS_LIST="${2:-1 2 4 8}"
KW="UNIQUEKEY_2025_DBK"

echo "Starting medicion_speedup.sh in $ROOT"
echo "REPS=$REPS"
echo "PROCS_LIST=[$PROCS_LIST]"
echo "Keyword used (must match input.txt): $KW"
echo

# compile
echo "Compiling sources..."
mpicc -O2 -Wno-deprecated-declarations bruteforce.c -o bruteforce -lssl -lcrypto
mpicc -O2 -Wno-deprecated-declarations encrypt_file.c -o encrypt_file -lssl -lcrypto
chmod +x bruteforce encrypt_file

# choose scale (BITS)
BITS=20
END=$((1<<BITS))

# compute keys (scaled analogues)
K_EASY=$(( END/2 + 1 ))
K_MED=$(( END/2 + END/8 ))
K_HARD=$(( (END + 6)/7 + (END + 12)/13 ))

echo "Using BITS=$BITS (END=$END)"
echo "Keys: EASY=$K_EASY MED=$K_MED HARD=$K_HARD"
echo

# generate cipher files (note: encrypt_file expects key first, then infile, then outfile)
echo "Generating cipher files..."
./encrypt_file "$K_EASY"   input.txt cipher_easy.bin  "$KW"
./encrypt_file "$K_MED"    input.txt cipher_med.bin   "$KW"
./encrypt_file "$K_HARD"   input.txt cipher_hard.bin  "$KW"

ls -l cipher_*.bin

# create outdir for outputs
OUTDIR="$ROOT/results_medicion_speedup_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"
echo "Outputs will be in $OUTDIR"
echo

# helper to run a case (static)
run_static() {
  local bin=$1
  local label=$2
  for P in $PROCS_LIST; do
    out="$OUTDIR/${label}_np${P}_static.txt"
    echo "Running static: $label  P=$P => $out"
    mpirun -np $P ./bruteforce "$bin" "$KW" $END $REPS | tee "$out"
    sleep 1
  done
}

# helper to run a case (dynamic)
run_dynamic() {
  local bin=$1
  local label=$2
  for P in $PROCS_LIST; do
    out="$OUTDIR/${label}_np${P}_dynamic.txt"
    echo "Running dynamic: $label  P=$P => $out"
    mpirun -np $P ./bruteforce "$bin" "$KW" --mode=dynamic --chunk=1000 --start=0 --end=$((END-1)) --reps=$REPS | tee "$out"
    sleep 1
  done
}

# Run all cases (static and dynamic)
CASES=( "easy:cipher_easy.bin" "med:cipher_med.bin" "hard:cipher_hard.bin")

for case in "${CASES[@]}"; do
  label="${case%%:*}"
  bin="${case#*:}"
  echo "=== CASE $label (static) ==="
  run_static "$bin" "$label"
  echo "=== CASE $label (dynamic) ==="
  run_dynamic "$bin" "$label"
done

echo
echo "All experiments finished. Raw outputs in $OUTDIR"
echo "Now run: python3 parse_results.py '$OUTDIR' > summary_medicion_speedup.csv"
echo "Then optionally: python3 plot_results.py summary_medicion_speedup.csv"
