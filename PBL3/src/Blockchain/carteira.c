#include <stdio.h>
#include <stdlib.h>
#include "carteira.h"

// Cria e retorna uma nova carteira
carteira* criar_carteira() {
    carteira *nova_carteira = calloc(1, sizeof(carteira));

    if (nova_carteira == NULL) {
        fprintf(stderr, "Falha ao alocar memória para a carteira.\n");
        return NULL;
    }

    if (sodium_init() < 0) {
        // Falha ao inicializar a biblioteca
        fprintf(stderr, "Falha ao inicializar a biblioteca libsodium.\n");
        free(nova_carteira);
        return NULL;
    }

    if (inicializar_carteira(nova_carteira) != 0) {
        fprintf(stderr, "Falha ao inicializar a carteira.\n");
        sodium_memzero(nova_carteira, sizeof(carteira));
        free(nova_carteira);
        return NULL;
    }

    return nova_carteira;
}

// Inicializa uma carteira existente
int inicializar_carteira(carteira *c) {

    if (c == NULL) {
        fprintf(stderr, "Carteira inválida.\n");
        return -1;
    }

    if (crypto_sign_keypair(c->chave_publica, c->chave_privada) != 0) {
        fprintf(stderr, "Falha ao gerar o par de chaves.\n");
        sodium_memzero(c, sizeof(carteira));
        return -1;
    }
    unsigned char hash[crypto_hash_sha256_BYTES] = {0};
    if (crypto_hash_sha256(hash, c->chave_publica, crypto_sign_PUBLICKEYBYTES) != 0) {
        fprintf(stderr, "Falha ao gerar o endereço.\n");
        sodium_memzero(c, sizeof(carteira));
        return -1;
    }
    if (sodium_bin2hex(c->endereco, sizeof(c->endereco), hash, crypto_hash_sha256_BYTES) == NULL) {
        fprintf(stderr, "Falha ao converter o hash para hexadecimal.\n");
        sodium_memzero(c, sizeof(carteira));
        sodium_memzero(hash, sizeof(hash));
        return -1;
    }
    sodium_memzero(hash, sizeof(hash));
    return 0;
}

// Limpa os dados da carteira
void limpar_carteira(carteira *c) {
    if (c != NULL) {
        sodium_memzero(c, sizeof(carteira));
    }
}

// Limpa e libera uma carteira alocada dinamicamente
void destruir_carteira(carteira *c) {
    if (c != NULL) {
        limpar_carteira(c);
        free(c);
    }
}

// Assina uma mensagem usando a chave privada da carteira
int carteira_assinar(const unsigned char *chave_privada, const unsigned char *mensagem, size_t tamanho_mensagem, unsigned char *assinatura, size_t capacidade_assinatura) {
    if (chave_privada == NULL || mensagem == NULL || assinatura == NULL || capacidade_assinatura < crypto_sign_BYTES) {
        return -1;
    }

    if (crypto_sign_detached(assinatura, NULL, mensagem, tamanho_mensagem, chave_privada) != 0) {
        return -1;
    }
    return 0;
}

// Verifica a assinatura de uma mensagem usando a chave pública da carteira
int carteira_verificar_assinatura(const unsigned char *chave_publica, const unsigned char *mensagem, size_t tamanho_mensagem, const unsigned char *assinatura, size_t tamanho_assinatura) {
    if (chave_publica == NULL || mensagem == NULL || assinatura == NULL || tamanho_assinatura != crypto_sign_BYTES) {
        return -1;
    }

    if (crypto_sign_verify_detached(assinatura, mensagem, tamanho_mensagem, chave_publica) != 0) {
        return -1;
    }
    // Assinatura válida
    return 0;
}