#!/usr/bin/env bash
# Ejecuta el protocolo: para cada bin y cada P (incrementos +2),
# realiza N_RUNS corridas, extrae 'Average elapsed' y genera resúmenes CSV con mean/std/speedup/eff.
set -euo pipefail

MODE="static"
# MODE="dynamic"

# Si MODE="dynamic", ajustar CHUNK
DYNAMIC_CHUNK=2000

N_RUNS=10

# Lista de procesos
PROCS=(1 2 4 6 8)

# Definir los bins a probar
BINS=(
  "encrypted_output.bin|consectetur adipiscing elit|16777216"
  "message_secret.bin|Alan|16777216"
)

# Ejecutable (ruta relativa si está en el mismo directorio)
EXEC="./bruteforce"
MPIRUN_EXTRA="--oversubscribe"

# Salida
OUTDIR="results_medicion_llaves_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"
echo "Resultados serán guardados en: $OUTDIR"
echo

# Helpers
# Extrae "Average elapsed over X runs: Y s" de un log (devuelve número o "nan")
extract_avg() {
  awk -F': ' '/Average elapsed over/ {gsub(" s","",$2); print $2+0; exit}' "$1" || echo "nan"
}

# Wrapper que construye y ejecuta mpirun segun modo
run_bruteforce_cmd() {
  local P=$1
  local CIPHER="$2"
  local KEY="$3"
  local ENDK="$4"
  local LOGFILE="$5"

  if [[ "$MODE" == "dynamic" ]]; then
    # modo dinámico: usar --mode=dynamic --chunk=...
    mpirun $MPIRUN_EXTRA -np "$P" "$EXEC" "$CIPHER" "$KEY" \
      --mode=dynamic --chunk="$DYNAMIC_CHUNK" --start=0 --end=$((ENDK-1)) --reps=5 \
      2>&1 | tee "$LOGFILE"
  else
    # modo estático tradicional: argumentos: cipher keyword end_k reps
    mpirun $MPIRUN_EXTRA -np "$P" "$EXEC" "$CIPHER" "$KEY" "$ENDK" 5 | tee "$LOGFILE"
  fi
}

# MAIN
echo "Configuración:"
echo "  MODE = $MODE"
if [[ "$MODE" == "dynamic" ]]; then echo "  DYNAMIC_CHUNK = $DYNAMIC_CHUNK"; fi
echo "  N_RUNS = $N_RUNS"
echo "  PROCS = ${PROCS[*]}"
echo "  BINS = ${#BINS[@]} archivos"
echo "  EXEC = $EXEC"
echo

for binconf in "${BINS[@]}"; do
  IFS='|' read -r CIPHER KEYWORD ENDK <<< "$binconf"
  safe_name=$(basename "$CIPHER" | sed 's/\.[^.]*$//')
  LOGDIR="$OUTDIR/logs_${safe_name}"
  mkdir -p "$LOGDIR"
  echo "========================================"
  echo "Archivo: $CIPHER"
  echo "Keyword: $KEYWORD"
  echo "END_K: $ENDK"
  echo "========================================"
  echo

  # CSV crudo: file,P,run,avg
  RAWCSV="$OUTDIR/raw_${safe_name}.csv"
  echo "file,P,run,avg" > "$RAWCSV"

  for P in "${PROCS[@]}"; do
    echo ">>> Ejecutando P=$P (reps por P = $N_RUNS)"
    for run in $(seq 1 $N_RUNS); do
      logfile="$LOGDIR/${safe_name}_P${P}_run${run}.log"
      echo "--- $safe_name  P=$P  run=$run ---"
      run_bruteforce_cmd "$P" "$CIPHER" "$KEYWORD" "$ENDK" "$logfile"
      # extraer avg y añadir CSV
      avg=$(extract_avg "$logfile")
      echo "${safe_name},${P},${run},${avg}" >> "$RAWCSV"
      sleep 0.3
    done
    echo
  done

  STATS_CSV="$OUTDIR/summary_stats_${safe_name}.csv"
  SPEED_CSV="$OUTDIR/summary_speedup_${safe_name}.csv"

  python3 - <<PY
import csv, math, statistics
raw = "$RAWCSV"
stats_out = "$STATS_CSV"
speed_out = "$SPEED_CSV"

# leer raw
rows = {}
with open(raw, newline='') as f:
    r = csv.DictReader(f)
    for rec in r:
        P = int(rec['P'])
        avg = float(rec['avg']) if rec['avg'] != 'nan' else float('nan')
        rows.setdefault(P, []).append(avg)

# calcular mean/std/count
with open(stats_out, 'w', newline='') as fo:
    w = csv.writer(fo)
    w.writerow(['P','mean','std','count'])
    for P in sorted(rows.keys()):
        vals = [v for v in rows[P] if not math.isnan(v)]
        if len(vals)>0:
            mu = statistics.mean(vals)
            sd = statistics.pstdev(vals) if len(vals)>1 else 0.0
            cnt = len(vals)
        else:
            mu = float('nan'); sd = float('nan'); cnt = 0
        w.writerow([P, "{:.6f}".format(mu), "{:.6f}".format(sd), cnt])

# calcular speedup y efficiency usando P=1 mean como referencia (si existe)
# cargar stats
stats = {}
with open(stats_out, newline='') as f:
    r = csv.DictReader(f)
    for rec in r:
        stats[int(rec['P'])] = float(rec['mean']) if rec['mean'] != '' else float('nan')

T1 = stats.get(1, float('nan'))
with open(speed_out, 'w', newline='') as fo:
    w = csv.writer(fo)
    w.writerow(['P','mean','std','count','speedup','efficiency_percent'])
    for P in sorted(stats.keys()):
        mu = stats[P]
        # find std and count
        # read again for std and count
        # quick approach: reuse rows dict
        vals = rows.get(P, [])
        vals = [v for v in vals if not math.isnan(v)]
        sd = statistics.pstdev(vals) if len(vals)>1 else 0.0
        cnt = len(vals)
        if not math.isnan(T1) and mu>0:
            speedup = T1 / mu
            eff = (speedup / P) * 100.0
        else:
            speedup = float('nan'); eff = float('nan')
        w.writerow([P, "{:.6f}".format(mu), "{:.6f}".format(sd), cnt, "{:.6f}".format(speedup), "{:.2f}".format(eff)])
print("Resumen generado:")
print("  - stats ->", stats_out)
print("  - speed ->", speed_out)
PY

  echo "Resúmenes para $safe_name guardados en:"
  echo "  - $STATS_CSV"
  echo "  - $SPEED_CSV"
  echo
done

echo "=== EXPERIMENTO COMPLETADO ==="
echo "Revisa CSVs en $OUTDIR, y logs en $OUTDIR/logs_*"
