// encrypt_file.c
// Compilar: mpicc encrypt_file.c -o encrypt_file -lssl -lcrypto

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/des.h>

void des_ecb_do(unsigned long long key, unsigned char *buf, int len, int do_encrypt) {
    DES_cblock kblock;
    unsigned long long k = 0ULL;
    unsigned long long tmp = key;
    // Construimos k con bits cada byte (manteniendo compatibilidad simple)
    for (int i = 0; i < 8; ++i) {
        unsigned char byte = (unsigned char)((tmp >> (8*i)) & 0xFF);
        k |= ((unsigned long long)(byte) << (8*i));
    }
    memcpy(&kblock, &k, sizeof(DES_cblock));
    DES_set_odd_parity(&kblock);
    DES_key_schedule ks;
    if (DES_set_key_checked(&kblock, &ks) != 0) {
        fprintf(stderr, "Clave DES inválida (paridad).\\n");
        exit(1);
    }
    for (int off = 0; off < len; off += 8) {
        DES_cblock in, out;
        memcpy(&in, buf + off, 8);
        DES_ecb_encrypt(&in, &out, &ks, do_encrypt ? DES_ENCRYPT : DES_DECRYPT);
        memcpy(buf + off, &out, 8);
    }
}

void encrypt(unsigned long long key, unsigned char *buf, int len) {
    des_ecb_do(key, buf, len, 1);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <key_decimal> <input.txt> <output_cipher.bin> [keyword]\n", argv[0]);
        return 1;
    }
    unsigned long long key = strtoull(argv[1], NULL, 10);
    const char *infile = argv[2];
    const char *outfile = argv[3];
    const char *keyword = (argc >= 5) ? argv[4] : "UNIQUEKEYWORD_12345";

    FILE *f = fopen(infile, "rb");
    if (!f) { perror("fopen input"); return 1; }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) { perror("ftell"); fclose(f); return 1; }
    if (flen > 350 * 10) { // seguridad: evitar textos enormes
        fprintf(stderr, "Archivo demasiado grande (limitado por seguridad)\\n");
        fclose(f);
        return 1;
    }
    int padded_len = (int)((flen + 7) / 8 * 8);
    unsigned char *buf = calloc(padded_len, 1);
    if (!buf) { perror("calloc"); fclose(f); return 1; }
    if (fread(buf, 1, flen, f) != (size_t)flen) { perror("fread"); free(buf); fclose(f); return 1; }
    fclose(f);

    // padding simple con ceros ya hecho por calloc
    encrypt(key, buf, padded_len);

    FILE *fo = fopen(outfile, "wb");
    if (!fo) { perror("fopen output"); free(buf); return 1; }
    if (fwrite(buf, 1, padded_len, fo) != (size_t)padded_len) { perror("fwrite"); free(buf); fclose(fo); return 1; }
    fclose(fo);
    free(buf);

    printf("Encrypted %s -> %s (key=%llu). Keyword: %s\n", infile, outfile, key, keyword);
    return 0;
}
