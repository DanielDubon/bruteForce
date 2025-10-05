//bruteforce.c
//nota: el key usado es bastante pequenio, cuando sea random speedup variara

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
        tmp <<= 1;
        k |= ( (unsigned long long)(tmp & (0xFEULL << (i*8))) );
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

void decrypt(unsigned long long key, unsigned char *ciph, int len){
    des_ecb_do(key, ciph, len, 0);
}

void encrypt(unsigned long long key, unsigned char *ciph, int len){
    des_ecb_do(key, ciph, len, 1);
}

char search_str[] = " the ";

int tryKey(unsigned long long key, unsigned char *ciph, int len){
    unsigned char temp[256];
    if (len > (int)sizeof(temp)-1) return 0;
    memcpy(temp, ciph, len);
    temp[len] = 0;
    decrypt(key, temp, len);
    return strstr((char *)temp, search_str) != NULL;
}

unsigned char cipher[] = {108,245,65,63,125,200,150,66,17,170,207,170,34,31,70,215};


int main(int argc, char *argv[]){ //char **argv
  int N, id;
  unsigned long long upper = (1ULL << 56); //upper bound DES keys 2^56
    // Si se pasa un argumento numérico, se interpreta como upper (valor entero)
    if (argc > 1) {
        unsigned long long v = strtoull(argv[1], NULL, 10);
        if (v > 0) upper = v;
    }
  unsigned long long mylower, myupper;
  MPI_Status st;
  MPI_Request req;
  int flag;
  const int ciphlen = sizeof(cipher);
  MPI_Comm comm = MPI_COMM_WORLD;

  MPI_Init(NULL, NULL);
  MPI_Comm_size(comm, &N);
  MPI_Comm_rank(comm, &id);

  unsigned long long range_per_node = upper / (unsigned long long)N;
    mylower = range_per_node * (unsigned long long)id;
    myupper = range_per_node * (unsigned long long)(id+1);
    if (id == N-1){
        myupper = upper;
    }

    unsigned long long found = 0ULL;

    MPI_Irecv(&found, 1, MPI_UNSIGNED_LONG_LONG, MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &req);

    for (unsigned long long i = mylower; i < myupper && (found == 0ULL); ++i){
        if (tryKey(i, cipher, ciphlen)){
            found = i;
            for (int node=0; node<N; node++){
                MPI_Send(&found, 1, MPI_UNSIGNED_LONG_LONG, node, 0, MPI_COMM_WORLD);
            }
            break;
        }
    }

    if (id == 0){
        MPI_Wait(&req, &st);
        unsigned char outbuf[256];
        if (ciphlen <= (int)sizeof(outbuf)) {
            memcpy(outbuf, cipher, ciphlen);
            decrypt(found, outbuf, ciphlen);
            outbuf[ciphlen] = 0;
            printf("FOUND: %llu -> %s\n", found, outbuf);
        } else {
            printf("FOUND: %llu\n", found);
        }
    }

    MPI_Finalize();
    return 0;
}
