#ifndef TRANSACAO_H
#define TRANSACAO_H

#include <stdint.h>
#include <stddef.h>
#include <sodium.h>

// Tamanho do endereço em hexadecimal, sem o terminador
#define TAM_ENDERECO_HEX (crypto_hash_sha256_BYTES * 2)

// Tamanho total do endereço, incluindo '\0'
#define TAM_ENDERECO (TAM_ENDERECO_HEX + 1)

// Identificação do formato das transações
#define TRANSACAO_DOMINIO "VAIJUNTO:TX:"
#define TRANSACAO_DOMINIO_TAMANHO (sizeof(TRANSACAO_DOMINIO) - 1)

// Versão atual do formato
#define TRANSACAO_VERSAO_ATUAL UINT32_C(1)

// Tamanho dos dados serializados, sem assinatura
#define TRANSACAO_SERIALIZADA_BYTES ( \
    TRANSACAO_DOMINIO_TAMANHO + \
    (3 * sizeof(uint32_t)) + \
    (2 * TAM_ENDERECO_HEX) + \
    (2 * sizeof(uint64_t)) + \
    crypto_sign_PUBLICKEYBYTES \
)

// Tamanho dos dados serializados junto da assinatura
#define TRANSACAO_SERIALIZADA_COM_ASSINATURA_BYTES ( \
    TRANSACAO_SERIALIZADA_BYTES + crypto_sign_BYTES \
)

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

    uint32_t versao;
    uint32_t chain_id;

    uint64_t valor;
    uint64_t nonce;

    TipoTransacao tipo;

    unsigned char assinatura[crypto_sign_BYTES];
    unsigned char chave_publica[crypto_sign_PUBLICKEYBYTES];

} Transacao;


// Converte um inteiro de 32 bits para big-endian
int gravar_u32_be(
    unsigned char *destino,
    size_t capacidade,
    uint32_t valor
);

// Converte um inteiro de 64 bits para big-endian
int gravar_u64_be(
    unsigned char *destino,
    size_t capacidade,
    uint64_t valor
);

// Validação
int transacao_validar_campos(const Transacao *transacao);


// Serialização
int transacao_serializar_assinatura(
    const Transacao *transacao,
    unsigned char *destino,
    size_t capacidade,
    size_t *bytes_serializados
);

//Assinatura para a transação
int transacao_assinar(
    Transacao *transacao,
    const unsigned char *chave_publica,
    size_t tamanho_chave_publica,
    const unsigned char *chave_privada,
    size_t tamanho_chave_privada
);

// Cálculo do identificador
int transacao_calcular_id(
    const Transacao *transacao,
    char *id_saida,
    size_t capacidade
);

// Verificação completa
int transacao_verificar(
    const Transacao *transacao,
    uint32_t chain_id_esperado
);

#endif