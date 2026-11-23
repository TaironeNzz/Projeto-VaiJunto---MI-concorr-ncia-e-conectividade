#include <stdio.h>
#include <string.h>
#include <sodium.h>

#include "carteira.h"
#include "transacao.h"

int main(void) {

    if (sodium_init() < 0) {
        printf("[ERRO] Falha ao inicializar libsodium.\n");
        return 1;
    }

    // Criar carteiras
    carteira *alice = criar_carteira();
    carteira *bob = criar_carteira();
    carteira *carol = criar_carteira();

    if (!alice || !bob || !carol) {
        printf("[ERRO] Falha ao criar carteiras.\n");

        destruir_carteira(alice);
        destruir_carteira(bob);
        destruir_carteira(carol);

        return 1;
    }

    printf("[OK] Carteiras criadas.\n");

    // Criar transação de teste
    Transacao tx = {0};

    tx.versao = TRANSACAO_VERSAO_ATUAL;
    tx.chain_id = 1;
    tx.tipo = TOKEN_TRANSFERENCIA;
    tx.valor = 5000;
    tx.nonce = 1;

    memcpy(tx.remetente, alice->endereco, TAM_ENDERECO);
    memcpy(tx.destinatario, bob->endereco, TAM_ENDERECO);

    memcpy(
        tx.chave_publica,
        alice->chave_publica,
        crypto_sign_PUBLICKEYBYTES
    );

    unsigned char buffer[TRANSACAO_SERIALIZADA_BYTES];
    unsigned char buffer2[TRANSACAO_SERIALIZADA_BYTES];

    size_t tamanho = 0;
    size_t tamanho2 = 0;

    // TESTE 1: Serialização
    if (transacao_serializar_assinatura(
            &tx,
            buffer,
            sizeof(buffer),
            &tamanho
        ) != 0) {

        printf("[ERRO] Falha na serializacao.\n");
        goto erro;
    }

    if (tamanho != TRANSACAO_SERIALIZADA_BYTES) {
        printf("[ERRO] Tamanho serializado incorreto.\n");
        goto erro;
    }

    printf("[OK] Serializacao realizada: %zu bytes.\n", tamanho);


    // TESTE 2: Conferir domínio
    if (memcmp(
            buffer,
            TRANSACAO_DOMINIO,
            TRANSACAO_DOMINIO_TAMANHO
        ) != 0) {

        printf("[ERRO] Dominio incorreto.\n");
        goto erro;
    }

    printf("[OK] Dominio correto.\n");


    // TESTE 3: Conferir remetente
    if (memcmp(
            buffer + 24,
            alice->endereco,
            TAM_ENDERECO_HEX
        ) != 0) {

        printf("[ERRO] Remetente serializado incorretamente.\n");
        goto erro;
    }

    printf("[OK] Remetente correto.\n");


    // TESTE 4: Conferir destinatário
    if (memcmp(
            buffer + 88,
            bob->endereco,
            TAM_ENDERECO_HEX
        ) != 0) {

        printf("[ERRO] Destinatario serializado incorretamente.\n");
        goto erro;
    }

    printf("[OK] Destinatario correto.\n");


    // TESTE 5: Conferir valor em big-endian
    const unsigned char valor_esperado[8] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x13, 0x88
    };

    if (memcmp(buffer + 152, valor_esperado, 8) != 0) {
        printf("[ERRO] Valor serializado incorretamente.\n");
        goto erro;
    }

    printf("[OK] Valor em big-endian.\n");


    // TESTE 6: Conferir nonce
    const unsigned char nonce_esperado[8] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01
    };

    if (memcmp(buffer + 160, nonce_esperado, 8) != 0) {
        printf("[ERRO] Nonce serializado incorretamente.\n");
        goto erro;
    }

    printf("[OK] Nonce correto.\n");


    // TESTE 7: Conferir chave pública
    if (memcmp(
            buffer + 168,
            alice->chave_publica,
            crypto_sign_PUBLICKEYBYTES
        ) != 0) {

        printf("[ERRO] Chave publica incorreta.\n");
        goto erro;
    }

    printf("[OK] Chave publica correta.\n");


    // TESTE 8: Determinismo
    if (transacao_serializar_assinatura(
            &tx,
            buffer2,
            sizeof(buffer2),
            &tamanho2
        ) != 0) {

        printf("[ERRO] Segunda serializacao falhou.\n");
        goto erro;
    }

    if (tamanho != tamanho2 ||
        memcmp(buffer, buffer2, tamanho) != 0) {

        printf("[ERRO] Serializacao nao deterministica.\n");
        goto erro;
    }

    printf("[OK] Serializacao deterministica.\n");


    // TESTE 9: Alteração do valor
    tx.valor++;

    if (transacao_serializar_assinatura(
            &tx,
            buffer2,
            sizeof(buffer2),
            &tamanho2
        ) != 0) {

        printf("[ERRO] Falha ao serializar valor alterado.\n");
        goto erro;
    }

    if (memcmp(buffer, buffer2, tamanho) == 0) {
        printf("[ERRO] Alteracao do valor nao foi detectada.\n");
        goto erro;
    }

    tx.valor--;

    printf("[OK] Alteracao do valor detectada.\n");


    // TESTE 10: Alteração do destinatário
    memcpy(tx.destinatario, carol->endereco, TAM_ENDERECO);

    if (transacao_serializar_assinatura(
            &tx,
            buffer2,
            sizeof(buffer2),
            &tamanho2
        ) != 0) {

        printf("[ERRO] Falha ao serializar destinatario alterado.\n");
        goto erro;
    }

    if (memcmp(buffer, buffer2, tamanho) == 0) {
        printf("[ERRO] Alteracao do destinatario nao detectada.\n");
        goto erro;
    }

    printf("[OK] Alteracao do destinatario detectada.\n");


    // TESTE 11: Buffer insuficiente
    tamanho2 = 999;

    if (transacao_serializar_assinatura(
            &tx,
            buffer2,
            sizeof(buffer2) - 1,
            &tamanho2
        ) == 0) {

        printf("[ERRO] Buffer insuficiente foi aceito.\n");
        goto erro;
    }

    if (tamanho2 != 0) {
        printf("[ERRO] Tamanho de saida deveria ser zero.\n");
        goto erro;
    }

    printf("[OK] Buffer insuficiente rejeitado.\n");


    // TESTE 12: Endereço inválido
    tx.destinatario[0] = 'g';

    if (transacao_serializar_assinatura(
            &tx,
            buffer2,
            sizeof(buffer2),
            &tamanho2
        ) == 0) {

        printf("[ERRO] Endereco invalido foi aceito.\n");
        goto erro;
    }

    printf("[OK] Endereco invalido rejeitado.\n");


    printf("\nTODOS OS TESTES PASSARAM!\n");

    destruir_carteira(alice);
    destruir_carteira(bob);
    destruir_carteira(carol);

    return 0;


erro:

    destruir_carteira(alice);
    destruir_carteira(bob);
    destruir_carteira(carol);

    return 1;
}