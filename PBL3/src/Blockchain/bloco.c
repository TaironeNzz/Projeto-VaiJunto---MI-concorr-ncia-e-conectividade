#include <stdio.h>
#include <sodium.h>
#include <string.h>

typedef struct {
    int indice;

    char hash_anterior[65];
    char hash_atual[65];

    long timestamp;

    char *transacoes;
    int quantidade_transacoes;

} Bloco;

void calcularHash(const char *dados, char *resultado) {

    unsigned char hash[crypto_hash_sha256_BYTES];

    crypto_hash_sha256(
        hash,
        (const unsigned char *)dados,
        strlen(dados)
    );

    sodium_bin2hex(
        resultado,
        65,
        hash,
        crypto_hash_sha256_BYTES
    );
}