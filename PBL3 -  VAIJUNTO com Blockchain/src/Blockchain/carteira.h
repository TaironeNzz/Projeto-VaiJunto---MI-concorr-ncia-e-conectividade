#ifndef CARTEIRA_H
#define CARTEIRA_H

#include <sodium.h>

typedef struct {
    unsigned char chave_publica[crypto_sign_PUBLICKEYBYTES];
    unsigned char chave_privada[crypto_sign_SECRETKEYBYTES];

    char endereco[crypto_hash_sha256_BYTES * 2 + 1];

} carteira;


// Cria e retorna uma nova carteira
carteira* criar_carteira(void);

// Inicializa uma carteira existente
int inicializar_carteira(carteira *c);

// Limpa os dados da carteira
void limpar_carteira(carteira *c);

// Limpa e libera uma carteira alocada dinamicamente
void destruir_carteira(carteira *c);

// Assina uma mensagem usando a chave privada da carteira
int carteira_assinar(const unsigned char *chave_privada, const unsigned char *mensagem, size_t tamanho_mensagem, unsigned char *assinatura, size_t capacidade_assinatura);

// Verifica a assinatura de uma mensagem usando a chave pública da carteira
int carteira_verificar_assinatura(const unsigned char *chave_publica, const unsigned char *mensagem, size_t tamanho_mensagem, const unsigned char *assinatura, size_t tamanho_assinatura);

#endif