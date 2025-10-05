// bruteforce.c (modificado para leer cipher file y medir varias repeticiones)
// Compilar: mpicc bruteforce.c -o bruteforce -lssl -lcrypto

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <unistd.h>
#include <openssl/des.h>

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
    unsigned char temp[4096];
    if (len > (int)sizeof(temp)-1) return 0;
    memcpy(temp, ciph, len);
    temp[len] = 0;
    decrypt(key, temp, len);
    return strstr((char *)temp, search_str) != NULL;
}

unsigned char *cipher = NULL;
int cipher_len = 0;

int main(int argc, char *argv[]){
    MPI_Init(NULL, NULL);
    int N, id;
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &N);
    MPI_Comm_rank(comm, &id);

    if (argc < 3) {
        if (id==0) fprintf(stderr, "Uso: %s <cipherfile> <keyword> [upper] [repetitions]\n", argv[0]);
        MPI_Finalize();
        return 1;
    }
    const char *cipherfile = argv[1];
    search_str = argv[2];
    unsigned long long upper = (1ULL << 24); // por defecto, pequeño para pruebas; cambiar si quieres
    int repetitions = 5;
    if (argc > 3) upper = strtoull(argv[3], NULL, 10);
    if (argc > 4) repetitions = atoi(argv[4]);

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

    unsigned long long range_per_node = upper / (unsigned long long)N;
    double times_sum = 0.0;
    unsigned long long found_key = 0ULL;

    for (int rep = 0; rep < repetitions; ++rep) {
        unsigned long long mylower = range_per_node * (unsigned long long)id;
        unsigned long long myupper = range_per_node * (unsigned long long)(id+1);
        if (id == N-1) myupper = upper;

        unsigned long long found = 0ULL;
        MPI_Request req;
        MPI_Irecv(&found, 1, MPI_UNSIGNED_LONG_LONG, MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &req);

        MPI_Barrier(comm); // sincronizar antes de medir
        double t0 = MPI_Wtime();

        for (unsigned long long i = mylower; i < myupper && (found == 0ULL); ++i) {
            if (tryKey(i, cipher, cipher_len)) {
                found = i;
                // enviar encontrado a todos
                for (int node=0; node<N; node++){
                    MPI_Send(&found, 1, MPI_UNSIGNED_LONG_LONG, node, 0, MPI_COMM_WORLD);
                }
                break;
            }
            // chequeo no bloqueante si el buffer recibido cambió (puedes usar MPI_Test para ser proactivo)
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
        double elapsed_max;
        MPI_Reduce(&elapsed_local, &elapsed_max, 1, MPI_DOUBLE, MPI_MAX, 0, comm);

        if (id == 0) {
            if (global_found) found_key = global_found;
            printf("Rep %d: elapsed = %.6f s, found_key = %llu\n", rep+1, elapsed_max, (unsigned long long)global_found);
            times_sum += elapsed_max;
        }

        // asegurar que todos finalicen la iteración antes de la siguiente
        MPI_Barrier(comm);
        // rearmar Irecv para la siguiente repetición (si rep+1 < repetitions)
    }

    if (id == 0) {
        double avg = times_sum / (double)repetitions;
        printf("Average elapsed over %d runs: %.6f s\n", repetitions, avg);
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
