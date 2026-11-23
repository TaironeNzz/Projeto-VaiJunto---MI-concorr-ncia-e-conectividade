#ifndef TRANSACAO_H
#define TRANSACAO_H

#include <stdint.h>
#include <sodium.h>

#define TAM_ENDERECO (crypto_hash_sha256_BYTES * 2 + 1)

typedef enum {
    TOKEN_TRANSFERENCIA = 1,
    TOKEN_BLOQUEIO = 2,
    TOKEN_LIBERACAO = 3,
    TOKEN_ESTORNO = 4,
    AVALIACAO = 5,
    DENUNCIA = 6,
    AUSENCIA = 7,
    CANCELAMENTO = 8
} TipoTransacao;

typedef struct {
    char id[TAM_ENDERECO];

    char remetente[TAM_ENDERECO];
    char destinatario[TAM_ENDERECO];

    uint64_t valor;
    uint64_t nonce;

    TipoTransacao tipo;

    unsigned char assinatura[crypto_sign_BYTES];
    unsigned char chave_publica[crypto_sign_PUBLICKEYBYTES];

} Transacao;

#endif