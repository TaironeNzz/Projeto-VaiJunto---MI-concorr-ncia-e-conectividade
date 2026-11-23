#include <stdio.h>
#include <string.h>
#include <sodium.h>
#include "carteira.h"

int main(void) {

    // Criar carteiras de Alice e Bob
    carteira *alice = criar_carteira();
    carteira *bob = criar_carteira();

    if (alice == NULL || bob == NULL) {
        printf("[ERRO] Falha ao criar carteiras.\n");

        destruir_carteira(alice);
        destruir_carteira(bob);

        return 1;
    }

    printf("[OK] Carteiras criadas.\n");


    // Mensagem que Alice deseja assinar
    const unsigned char mensagem[] =
        "Alice autoriza transferir 50 tokens para Bob.";

    // Buffer para armazenar a assinatura
    unsigned char assinatura[crypto_sign_BYTES];


    // TESTE 1: Assinatura da mensagem
    if (carteira_assinar(
            alice->chave_privada,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura,
            sizeof(assinatura)
        ) != 0) {

        printf("[ERRO] Falha ao assinar mensagem.\n");
        goto finalizar;
    }

    printf("[OK] Mensagem assinada.\n");


    // TESTE 2: Verificar assinatura com a chave de Alice
    if (carteira_verificar_assinatura(
            alice->chave_publica,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura,
            sizeof(assinatura)
        ) != 0) {

        printf("[ERRO] Assinatura de Alice deveria ser válida.\n");
        goto finalizar;
    }

    printf("[OK] Assinatura de Alice validada.\n");


    // TESTE 3: Verificar assinatura com a chave de Bob
    if (carteira_verificar_assinatura(
            bob->chave_publica,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura,
            sizeof(assinatura)
        ) == 0) {

        printf("[ERRO] Bob nao deveria validar essa assinatura.\n");
        goto finalizar;
    }

    printf("[OK] Chave de Bob rejeitada.\n");


    // TESTE 4: Alterar a mensagem
    unsigned char mensagem_alterada[sizeof(mensagem)];

    memcpy(mensagem_alterada, mensagem, sizeof(mensagem));

    mensagem_alterada[0] ^= 1;

    if (carteira_verificar_assinatura(
            alice->chave_publica,
            mensagem_alterada,
            sizeof(mensagem_alterada) - 1,
            assinatura,
            sizeof(assinatura)
        ) == 0) {

        printf("[ERRO] Mensagem adulterada foi aceita.\n");
        goto finalizar;
    }

    printf("[OK] Mensagem adulterada rejeitada.\n");


    // TESTE 5: Alterar a assinatura
    unsigned char assinatura_alterada[crypto_sign_BYTES];

    memcpy(assinatura_alterada, assinatura, sizeof(assinatura));

    assinatura_alterada[0] ^= 1;

    if (carteira_verificar_assinatura(
            alice->chave_publica,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura_alterada,
            sizeof(assinatura_alterada)
        ) == 0) {

        printf("[ERRO] Assinatura adulterada foi aceita.\n");
        goto finalizar;
    }

    printf("[OK] Assinatura adulterada rejeitada.\n");


    // TESTE 6: Assinatura com tamanho incorreto
    if (carteira_verificar_assinatura(
            alice->chave_publica,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura,
            crypto_sign_BYTES - 1
        ) == 0) {

        printf("[ERRO] Assinatura de tamanho incorreto foi aceita.\n");
        goto finalizar;
    }

    printf("[OK] Tamanho incorreto rejeitado.\n");


    // TESTE 7: Buffer de saída insuficiente
    unsigned char assinatura_pequena[crypto_sign_BYTES - 1];

    if (carteira_assinar(
            alice->chave_privada,
            mensagem,
            sizeof(mensagem) - 1,
            assinatura_pequena,
            sizeof(assinatura_pequena)
        ) == 0) {

        printf("[ERRO] Buffer insuficiente foi aceito.\n");
        goto finalizar;
    }

    printf("[OK] Buffer insuficiente rejeitado.\n");


    printf("\nTODOS OS TESTES PASSARAM!\n");

    destruir_carteira(alice);
    destruir_carteira(bob);

    return 0;


finalizar:

    destruir_carteira(alice);
    destruir_carteira(bob);

    return 1;
}