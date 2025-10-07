// bruteforce.c (modificado para leer cipher file y medir varias repeticiones)
// Compilar: mpicc bruteforce.c -o bruteforce -lssl -lcrypto

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <unistd.h>
#include <openssl/des.h>

#define TAG_REQ   1001
#define TAG_ASSIGN 1002
#define TAG_FOUND 1003
#define MAX_KEY_56 ((1ULL<<56) - 1ULL)

// Reloj simple con MPI_Wtime
static inline double wall(){
    return MPI_Wtime();
}

// Partición por bloques homogénea
static void block_range(unsigned long long gstart, unsigned long long gend,
                        int np, int rank, unsigned long long *start, unsigned long long *end){
    unsigned long long total = (gend >= gstart)? (gend - gstart + 1ULL) : 0ULL;
    unsigned long long base  = total / np;
    unsigned long long rem   = total % np;
    unsigned long long sidx  = base*rank + (rank < rem ? rank : rem);
    unsigned long long cnt   = base + (rank < rem ? 1ULL : 0ULL);
    *start = gstart + sidx;
    *end   = cnt? (*start + cnt - 1ULL) : (gstart - 1ULL);
}

void des_ecb_do(unsigned long long key, unsigned char *buf, int len, int do_encrypt) {
    DES_cblock kblock;
    unsigned long long k = 0ULL;
    unsigned long long tmp = key;
    for (int i = 0; i < 8; ++i) {
        unsigned char byte = (unsigned char)((tmp >> (8*i)) & 0xFF);
        k |= ((unsigned long long)(byte) << (8*i));
    }
    memcpy(&kblock, &k, sizeof(DES_cblock));
    DES_set_odd_parity(&kblock);
    DES_key_schedule ks;
    if (DES_set_key_checked(&kblock, &ks) != 0) {
        return;
    }
    for (int off = 0; off < len; off += 8) {
        DES_cblock in, out;
        memcpy(&in, buf + off, 8);
        DES_ecb_encrypt(&in, &out, &ks, do_encrypt ? DES_ENCRYPT : DES_DECRYPT);
        memcpy(buf + off, &out, 8);
    }
}

void decrypt(unsigned long long key, unsigned char *ciph, int len) {
    des_ecb_do(key, ciph, len, 0);
}

char *search_str = NULL;

int tryKey(unsigned long long key, unsigned char *ciph, int len){
    if (search_str == NULL) return 0;
    unsigned char temp[4096];
    if (len > (int)sizeof(temp)-1) return 0;
    memcpy(temp, ciph, len);
    temp[len] = 0;
    decrypt(key, temp, len);
    return strstr((char *)temp, search_str) != NULL;
}

unsigned char *cipher = NULL;
int cipher_len = 0;

// Ejecutor dynamic master/worker
static int run_dynamic(unsigned char *cipher_buf, int cipher_len,
                       unsigned long long start_k, unsigned long long end_k,
                       unsigned long long chunk,
                       unsigned long long *out_found_key,
                       double *out_parallel_time,
                       unsigned long long *out_iterations) {

    int rank, np;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &np);

    *out_found_key = 0ULL;
    *out_iterations = 0ULL;
    *out_parallel_time = 0.0;

    if (np <= 1) {
        // fallback secuencial si solo hay 1 proceso
        double t0 = wall();
        unsigned long long iters = 0ULL;
        for (unsigned long long k = start_k; k <= end_k; ++k) {
            ++iters;
            if (tryKey(k, cipher_buf, cipher_len)) {
                *out_found_key = k;
                break;
            }
        }
        *out_iterations = iters;
        *out_parallel_time = wall() - t0;
        return 0;
    }

    if (rank == 0) {
        // MASTER
        unsigned long long next_key = start_k;
        const int active_workers = np - 1;

        int summaries = 0;
        int found_flag = 0;
        unsigned long long reported_key = 0ULL;

        double max_elapsed = 0.0;
        unsigned long long tot_iters = 0ULL;
        unsigned long long any_found = 0ULL;

        // Procesamos mensajes hasta recibir el resumen de TODOS los workers
        while (summaries < active_workers) {
            MPI_Status st;
            MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &st);

            if (st.MPI_TAG == TAG_REQ) {
                // petición de trabajo
                MPI_Recv(NULL, 0, MPI_BYTE, st.MPI_SOURCE, TAG_REQ, MPI_COMM_WORLD, &st);

                if (found_flag || next_key > end_k) {
                    unsigned long long reply[2] = {0ULL, 0ULL};
                    MPI_Send(reply, 2, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, TAG_ASSIGN, MPI_COMM_WORLD);
                } else {
                    unsigned long long remain = end_k - next_key + 1ULL;
                    unsigned long long cnt = (remain < chunk) ? remain : chunk;
                    unsigned long long reply[2] = { next_key, cnt };
                    next_key += cnt;
                    MPI_Send(reply, 2, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, TAG_ASSIGN, MPI_COMM_WORLD);
                }

            } else if (st.MPI_TAG == TAG_FOUND) {
                // algún worker encontró la llave
                unsigned long long k;
                MPI_Recv(&k, 1, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, TAG_FOUND, MPI_COMM_WORLD, &st);
                if (!found_flag) {
                    found_flag = 1;
                    reported_key = k;
                    // avisar inmediatamente a todos los workers
                    for (int p = 1; p < np; ++p) {
                        MPI_Send(&reported_key, 1, MPI_UNSIGNED_LONG_LONG, p, TAG_FOUND, MPI_COMM_WORLD);
                    }
                }

            } else if (st.MPI_TAG == 0) {
                // resumen final de un worker
                struct { double t; unsigned long long it; unsigned long long f; } pkt;
                MPI_Recv(&pkt, sizeof(pkt), MPI_BYTE, st.MPI_SOURCE, 0, MPI_COMM_WORLD, &st);

                if (pkt.t > max_elapsed) max_elapsed = pkt.t;
                tot_iters += pkt.it;
                if (pkt.f) any_found = pkt.f;
                summaries++;  // contamos este resumen

            } else {
                // Mensaje inesperado
                MPI_Recv(NULL, 0, MPI_BYTE, st.MPI_SOURCE, st.MPI_TAG, MPI_COMM_WORLD, &st);
            }
        }

        if (found_flag) *out_found_key = reported_key;
        if (any_found)  *out_found_key = any_found;
        *out_parallel_time = max_elapsed;
        *out_iterations = tot_iters;
        return 0;

    } else {
        // WORKER
        double t0 = wall();
        unsigned long long my_iters = 0ULL;
        unsigned long long my_found = 0ULL;
        int done = 0;

        while (!done) {
            // pedir trabajo
            MPI_Send(NULL, 0, MPI_BYTE, 0, TAG_REQ, MPI_COMM_WORLD);

            // recibir asignación
            unsigned long long reply[2] = {0ULL, 0ULL};
            MPI_Status st;
            MPI_Recv(reply, 2, MPI_UNSIGNED_LONG_LONG, 0, TAG_ASSIGN, MPI_COMM_WORLD, &st);

            unsigned long long asign_start = reply[0], asign_count = reply[1];
            if (asign_count == 0ULL) {
                // maestro indicó que no hay más trabajo
                break;
            }

            unsigned long long asign_end = asign_start + asign_count - 1ULL;
            for (unsigned long long k = asign_start; k <= asign_end; ++k) {
                ++my_iters;
                if (tryKey(k, cipher_buf, cipher_len)) {
                    my_found = k;
                    // informar al master y salir
                    MPI_Send(&my_found, 1, MPI_UNSIGNED_LONG_LONG, 0, TAG_FOUND, MPI_COMM_WORLD);
                    done = 1;
                    break;
                }
                // chequeo si master ya avisó que otro encontró la llave
                int flag = 0;
                MPI_Status st2;
                MPI_Iprobe(0, TAG_FOUND, MPI_COMM_WORLD, &flag, &st2);
                if (flag) {
                    unsigned long long k2;
                    MPI_Recv(&k2, 1, MPI_UNSIGNED_LONG_LONG, 0, TAG_FOUND, MPI_COMM_WORLD, &st2);
                    my_found = k2;
                    done = 1;
                    break;
                }
            }
        }

        double t1 = wall();
        struct { double t; unsigned long long it; unsigned long long f; } pkt;
        pkt.t = t1 - t0;
        pkt.it = my_iters;
        pkt.f = my_found;
        // enviar resumen al master
        MPI_Send(&pkt, sizeof(pkt), MPI_BYTE, 0, 0, MPI_COMM_WORLD);

        if (my_found) *out_found_key = my_found;
        *out_parallel_time = pkt.t;
        *out_iterations = my_iters;
        return 0;
    }
}

int main(int argc, char *argv[]){
    MPI_Init(NULL, NULL);
    int N, id;
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &N);
    MPI_Comm_rank(comm, &id);

    if (argc < 3) {
        if (id==0) fprintf(stderr, "Uso: %s <cipherfile> <keyword> [end_k] [reps]\n", argv[0]);
        MPI_Finalize();
        return 1;
    }
    const char *cipherfile = argv[1];
    const char *keyword_path = argv[2];

    #ifndef MAX_KEY_56
    #define MAX_KEY_56 ((1ULL<<56) - 1ULL)
    #endif

    char mode[32] = "block";
    unsigned long long chunk   = 10000;
    unsigned long long start_k = 0ULL;
    unsigned long long end_k   = MAX_KEY_56;
    int reps = 1;

    for (int i = 3; i < argc; i++) {
        if      (strncmp(argv[i], "--mode=",   7) == 0) strncpy(mode, argv[i] + 7, sizeof(mode) - 1);
        else if (strncmp(argv[i], "--chunk=",  8) == 0) chunk   = strtoull(argv[i] + 8,  NULL, 10);
        else if (strncmp(argv[i], "--start=",  8) == 0) start_k = strtoull(argv[i] + 8,  NULL, 10);
        else if (strncmp(argv[i], "--end=",    6) == 0) end_k   = strtoull(argv[i] + 6,  NULL, 10);
        else if (strncmp(argv[i], "--reps=",   7) == 0) reps    = atoi    (argv[i] + 7);
    }

    //search_str = argv[2];
    //unsigned long long end_k = (1ULL << 24); // por defecto, pequeño para pruebas; cambiar si quieres
    //int reps = 5;
    //if (argc > 3) end_k = strtoull(argv[3], NULL, 10);
    //if (argc > 4) reps = atoi(argv[4]);

    // leer fichero cifrado
    if (id == 0) printf("Proceso %d leyendo %s\n", id, cipherfile);
    FILE *f = fopen(cipherfile, "rb");
    if (!f) {
        perror("fopen cipherfile");
        MPI_Finalize();
        return 1;
    }
    fseek(f, 0, SEEK_END);
    cipher_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    cipher = malloc(cipher_len);
    if (!cipher) { perror("malloc"); fclose(f); MPI_Finalize(); return 1; }
    if (fread(cipher, 1, cipher_len, f) != (size_t)cipher_len) { perror("fread"); free(cipher); fclose(f); MPI_Finalize(); return 1; }
    fclose(f);

    // Cargar keyword desde keyword_path y apuntar search_str 
    {
        FILE *fk = fopen(keyword_path, "rb");
        if (!fk) {
            if (id == 0) perror("fopen keyword");
            free(cipher);
            MPI_Finalize();
            return 1;
        }
        static char keyword_buf[512];
        size_t kwlen = fread(keyword_buf, 1, sizeof(keyword_buf)-1, fk);
        fclose(fk);
        keyword_buf[kwlen] = '\0';
        // recortar CR/LF finales
        while (kwlen > 0 && (keyword_buf[kwlen-1] == '\n' || keyword_buf[kwlen-1] == '\r')) {
            keyword_buf[--kwlen] = '\0';
        }
        if (kwlen == 0) {
            if (id == 0) fprintf(stderr, "keyword vacío en %s\n", keyword_path);
            free(cipher);
            MPI_Finalize();
            return 1;
        }
        search_str = keyword_buf; // tryKey usa esta variable global
        if (id == 0) printf("Keyword: '%s' (len %zu)\n", search_str, kwlen);
    }

    unsigned long long total_range = (end_k >= start_k) ? (end_k - start_k + 1ULL) : 0ULL;
    unsigned long long range_per_node = (N > 0) ? (total_range / (unsigned long long)N) : 0ULL;
    double times_sum = 0.0;
    unsigned long long found_key = 0ULL;
    unsigned long long global_end_excl = end_k + 1ULL;

    if (strncmp(mode, "dynamic", 7) == 0) {
        unsigned long long found = 0ULL;
        double par_time = 0.0;
        unsigned long long iterations = 0ULL;

        run_dynamic(cipher, cipher_len, start_k, end_k, chunk, &found, &par_time, &iterations);

        if (id == 0) {
            printf("DYNAMIC result: N=%d, chunk=%llu, time=%.6f s, iterations=%llu, found_key=%llu\n",
                   N, (unsigned long long)chunk, par_time, iterations, found);
        }

        free(cipher);
        MPI_Finalize();
        return 0;
    }

    for (int rep = 0; rep < reps; ++rep) {
        unsigned long long mylower = start_k + range_per_node * (unsigned long long)id;
        unsigned long long myupper = start_k + range_per_node * (unsigned long long)(id + 1);
        if (id == N - 1) myupper = global_end_excl;

        unsigned long long found = 0ULL;
        MPI_Request req;
        MPI_Irecv(&found, 1, MPI_UNSIGNED_LONG_LONG, MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &req);

        MPI_Barrier(comm); // sincronizar antes de medir
        double t0 = MPI_Wtime();

        for (unsigned long long i = mylower; i < myupper && (found == 0ULL); ++i) {
            if (tryKey(i, cipher, cipher_len)) {
                found = i;
                // notificar a todos que encontraron
                for (int node = 0; node < N; ++node) {
                    MPI_Send(&found, 1, MPI_UNSIGNED_LONG_LONG, node, 0, MPI_COMM_WORLD);
                }
                break;
            }
            // chequeo no bloqueante si el buffer recibido cambió
            int flag = 0;
            MPI_Test(&req, &flag, MPI_STATUS_IGNORE);
            if (flag) break;
        }

        // asegurar que todos conozcan la llave (si algún proceso ya la conocía)
        unsigned long long global_found = 0ULL;
        MPI_Allreduce(&found, &global_found, 1, MPI_UNSIGNED_LONG_LONG, MPI_MAX, comm);

        // obtener tiempo máximo del paralelo (el wall time relevante)
        double t1_local = MPI_Wtime();
        double elapsed_local = t1_local - t0;
        double elapsed_max = 0.0;
        MPI_Reduce(&elapsed_local, &elapsed_max, 1, MPI_DOUBLE, MPI_MAX, 0, comm);

        if (id == 0) {
            if (global_found) found_key = global_found;
            printf("Rep %d: elapsed = %.6f s, found_key = %llu\n", rep + 1, elapsed_max, (unsigned long long)global_found);
            times_sum += elapsed_max;
        }

        // asegurar que todos finalicen la iteración antes de la siguiente
        MPI_Barrier(comm);

        // limpieza de Irecv pendiente
        int completed = 0;
        MPI_Test(&req, &completed, MPI_STATUS_IGNORE);

        // Solo manipular si el request sigue siendo válido
        if (req != MPI_REQUEST_NULL) {
            if (!completed) {
                MPI_Cancel(&req);
                MPI_Wait(&req, MPI_STATUS_IGNORE); // garantiza terminación/cancelación
            }
            if (req != MPI_REQUEST_NULL) {
                MPI_Request_free(&req);
            }
        }
    }

    if (id == 0) {
        double avg = (reps > 0) ? (times_sum / (double)reps) : 0.0;
        printf("Average elapsed over %d runs: %.6f s\n", reps, avg);
        if (found_key) {
            unsigned char outbuf[4096];
            if (cipher_len <= (int)sizeof(outbuf)) {
                memcpy(outbuf, cipher, cipher_len);
                decrypt(found_key, outbuf, cipher_len);
                outbuf[cipher_len] = 0;
                printf("FOUND: %llu -> %s\n", found_key, outbuf);
            } else {
                printf("FOUND: %llu (cipher too big to display)\n", found_key);
            }
        } else {
            printf("No se encontró la llave en el rango proporcionado.\n");
        }
    }

    free(cipher);
    MPI_Finalize();
    return 0;
}
