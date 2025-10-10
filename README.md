# Brute-force DES (MPI)

Proyecto de búsqueda por fuerza bruta de la clave DES en modo ECB usando MPI.  
Incluye dos binarios:

- `encrypt_file`: cifra un texto en claro y genera un `.bin` (múltiplo de 8 bytes, con padding cero).
- `bruteforce`: busca la clave que descifra un `.bin` tal que el texto en claro contenga una **palabra clave** dada.

Además, se incluyen instrucciones para mediciones de tiempo, cálculo de **speedup** y **eficiencia**, y un script para automatizar experimentos.

## Requisitos

- GNU/Linux (probado en Ubuntu-like).
- **MPI** (OpenMPI o MPICH).
- **OpenSSL** (librerías `libssl` y `libcrypto` de desarrollo).
- Compilador C (e.g., `gcc`/`mpicc`).

Instalación típica (Ubuntu/Debian):

```bash
sudo apt-get update
sudo apt-get install -y build-essential openmpi-bin libopenmpi-dev libssl-dev
```

## Compilación

Clona el repositorio:

```bash
git clone <url del repo>
cd bruteForce
```

Desde la carpeta del proyecto compila los archios `bruteforce` y `encrypt_file`:

```bash
mpicc -O2 -Wno-deprecated-declarations bruteforce.c -o bruteforce -lssl -lcrypto
mpicc -O2 -Wno-deprecated-declarations encrypt_file.c -o encrypt_file -lssl -lcrypto
```

Comprueba que existen:

```bash
ls -l bruteforce encrypt_file
```

## Archivos del repositorio

- `bruteforce.c` — Búsqueda paralela de clave DES.
- `encrypt_file.c` — Cifra un archivo de texto y genera un `.bin`.
- `input.txt` — Texto de ejemplo.
- `keyword.txt` — Palabra clave a buscar.
- `run_experiments.sh` — Script para automatizar mediciones.


## Uso rápido

### 1) Generar un `.bin` de prueba

Cifra `input.txt` usando una clave decimal (entera sin signo). El binario aplica DES-ECB con padding de ceros hasta múltiplo de 8.

```bash
# Ejemplo: clave = 3100 (cualquiera en rango de 64 bits)
./encrypt_file 3100 input.txt cipher.bin
```

Salida esperada:

```
Encrypted input.txt -> cipher.bin (key=3100). Keyword: UNIQUEKEYWORD_12345
```

> Nota: ese “Keyword: …” es solo informativo (no se escribe en el binario).  
> El contenido real que se busca debe estar **incluido en tu texto en claro**, por ejemplo al final de `input.txt`:
>
> ```
> ...
> UNIQUEKEY_2025_DBK
> ```

### 2) Buscar la clave con `bruteforce`

Sintaxis general:

```
# mpirun -np <N> ./bruteforce <cipherfile> <keyword|keyword_file> [upper] [repetitions]
```

- `<cipherfile>`: archivo `.bin` cifrado.
- `<keyword|keyword_file>`:
  - si es una **ruta existente**, el programa lee la **palabra clave** desde ese archivo;
  - si no existe, se usa como **literal** la cadena proporcionada.
- `[upper]`: **límite superior exclusivo** de la búsqueda de claves (rango `[0 .. upper-1]`).  
  Si no lo das, puedes usar flags como `--start=`, `--end=`, `--mode=` (ver más abajo).
- `[repetitions]`: número de repeticiones de medición para promediar tiempos.

Ejemplos:

```bash
# Ejemplo 1: 4 procesos, keyword literal, rango, 5 repeticiones
mpirun -np 4 ./bruteforce cipher.bin "UNIQUEKEY_2025_DBK" 16777216 5

# Ejemplo 2: keyword desde archivo
mpirun -np 4 ./bruteforce cipher.bin keyword.txt 16777216 5
```

Salida típica:

```
Proceso 0 leyendo cipher.bin
Using keyword literal: 'UNIQUEKEY_2025_DBK' (len 18)
Rep 1: elapsed = 0.017510 s, found_key = 3100
...
Average elapsed over 5 runs: 0.017308 s
FOUND: 3100 -> <texto en claro...>
```

## Parámetros avanzados en `bruteforce`

Además de `[upper] [repetitions]`, puedes usar flags:

- `--mode=block` _(por defecto)_ o `--mode=dynamic`
  - **block**: divide el rango de claves equitativamente entre procesos.
  - **dynamic**: maestro/workers con reparto en **chunks**.
- `--start=<s>` y `--end=<e>`: define el rango `[s, e]` (inclusive).
- `--reps=<R>`: número de repeticiones.
- `--chunk=<C>`: (solo `dynamic`) tamaño del chunk de claves.

Ejemplos:

```bash
# Block mode (rango 0..16777216), 2 reps:
mpirun -np 4 ./bruteforce cipher.bin "UNIQUEKEY_2025_DBK" --mode=block --start=0 --end=16777216 --reps=2

# Dynamic mode con chunk de 1000:
mpirun -np 4 ./bruteforce cipher.bin "UNIQUEKEY_2025_DBK" --mode=dynamic --chunk=1000 --start=0 --end=16777216 --reps=2
```

## Mini-demo “easy / mid / hard”

Para pruebas rápidas sin llegar a `2^56`, usa un espacio de claves pequeño (ej. `2^20` o `2^24`).
El ejemplo usa un script `run_demo.sh` que compila, genera los .bin y ejecuta las pruebas.

1. Hacer ejecutable el script y ejecutarlo

   Coloca el script run_demo.sh en el directorio del proyecto y hazlo ejecutable:
   ```bash
   # dar permiso de ejecución (solo la primera vez)
   chmod +x medicion_speedup.sh
  
   # ejecutar el demo (el script compila, genera los .bin y ejecuta el bruteforce)
   ./medicion_speedup.sh
   ```
2. Qué hace el script
   - recompila bruteforce y encrypt_file
   - define BITS (por defecto el script usa BITS=20, puedes cambiarlo dentro del script)
   - calcula 3 claves representativas (EASY, MID, HARD) dentro del espacio 2^BITS
   - genera cipher_easy.bin, cipher_mid.bin, cipher_hard.bin cifrando el input.txt
   - ejecuta mpirun sobre cada .bin con N procesos (por defecto N=4) y reps repeticiones (por defecto 5)

## Probar binarios de otros grupos

Si recibes un archivo `.bin` y una **keyword** (literal), ejecuta:

```bash
Ejemplo 1:
mpirun -np 4 ./bruteforce encrypted_output.bin "consectetur adipiscing elit" 16777216 5

Ejemplo 2:
mpirun -np 4 ./bruteforce message_secret.bin "Alan" 16777216 5
```

Suele bastar con subir o bajar `np` y repetir para recolectar tiempos.

## Medición de speedup y eficiencia

Definiciones:

- `T1`: tiempo (promedio) con **1 proceso** (`np=1`).
- `Tp`: tiempo (promedio) con **p procesos**.

Cálculos:
- **Speedup**: `S_p = T1 / Tp`
- **Eficiencia**: `E_p = S_p / p = T1 / (p * Tp)`

### Script de automatización

```bash
chmod +x medicion_llaves.sh
./medicion_llaves.sh
```

Obtendrás un CSV por `.bin` en `results/` para analizar en Python/Excel y calcular `T1`, `Tp`, `S_p`, `E_p`.

## Consejos y solución de problemas

- **La keyword no aparece**: confirma que la cadena exacta (incluyendo mayúsculas/minúsculas y sin espacios extra) está **en el texto en claro** usado para cifrar ese `.bin`. Si usas archivo, verifica con `cat keyword.txt`.
- **`found_key = 0`**: suele significar que la clave real está fuera del rango buscado. Aumenta `upper` (o `--end=`) o verifica que el `.bin` y la keyword correspondan.
- **Paridad DES inválida** al cifrar: cambia la clave a otra (ciertos valores pueden no pasar el `DES_set_key_checked` tras ajustar paridad). En espacios de prueba chicos, usa valores como los del ejemplo “easy/mid/hard”.
- **Velocidad muy variable**: es normal —depende de dónde caiga la clave dentro de la partición o de la asignación dinámica (puntos del enunciado sobre speedups “engañosos”).
- **`dynamic` vs `block`**: `dynamic` reduce desequilibrios cuando la clave se encuentra muy temprano/tarde en un rango asignado a un proceso concreto; para medir overhead, compara ambos modos.

## Notas de implementación

- **Cifrado**: DES ECB, bloques de 8 bytes, **padding de ceros**.
- **Construcción de clave DES**: se empaqueta la clave entera en 8 bytes y se ajusta **odd parity** con `DES_set_odd_parity` (mismo proceso en `encrypt_file` y `bruteforce`).
- **Búsqueda**:
  - `block`: partición homogénea del rango y notificación temprana al encontrar clave.
  - `dynamic`: maestro reparte *chunks* (`--chunk=`) bajo demanda; cualquier hallazgo se propaga para detener a todos.
