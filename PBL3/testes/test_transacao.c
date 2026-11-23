/*
gcc -std=c11 -Wall -Wextra -Wpedantic \
-I src/Blockchain \
testes/test_transacao.c \
src/Blockchain/transacao.c \
src/Blockchain/carteira.c \
-lsodium -o bin/test_transacao

./bin/test_transacao

*/


#include <stdio.h>
#include <string.h>
#include <sodium.h>

#include "carteira.h"
#include "transacao.h"

int testes_passaram = 0;
int testes_falharam = 0;

#define TESTAR(nome, condicao) do {                  \
    if (condicao) {                                  \
        printf("[OK] %s\n", nome);                  \
        testes_passaram++;                          \
    } else {                                         \
        printf("[FALHOU] %s\n", nome);              \
        testes_falharam++;                           \
    }                                                \
} while (0)

int main(void) {

    printf("====================================\n");
    printf(" TESTES DO MODULO DE TRANSACOES\n");
    printf("====================================\n\n");

    // 1. Criar duas carteiras
    carteira *alice = criar_carteira();
    carteira *bob = criar_carteira();

    TESTAR("Criacao da carteira de Alice", alice != NULL);
    TESTAR("Criacao da carteira de Bob", bob != NULL);

    if (alice == NULL || bob == NULL) {
        destruir_carteira(alice);
        destruir_carteira(bob);
        return 1;
    }

    // 2. Preparar uma transacao
    Transacao tx = {0};

    tx.versao = TRANSACAO_VERSAO_ATUAL;
    tx.chain_id = 1;
    tx.valor = 100;
    tx.nonce = 1;
    tx.tipo = TOKEN_TRANSFERENCIA;

    memcpy(tx.remetente, alice->endereco, TAM_ENDERECO);
    memcpy(tx.destinatario, bob->endereco, TAM_ENDERECO);

    // 3. Assinar a transacao
    int resultado_assinatura = transacao_assinar(
        &tx,
        alice->chave_publica,
        sizeof(alice->chave_publica),
        alice->chave_privada,
        sizeof(alice->chave_privada)
    );

    TESTAR("Assinatura de uma transacao valida",
           resultado_assinatura == 0);

    // 4. Verificar a transacao original
    TESTAR("Verificacao da transacao original",
           transacao_verificar(&tx, 1) == 0);

    // 5. Alterar o valor
    Transacao alterada = tx;
    alterada.valor = 500;

    TESTAR("Rejeicao de alteracao do valor",
           transacao_verificar(&alterada, 1) != 0);

    // 6. Alterar o destinatario
    alterada = tx;
    memcpy(alterada.destinatario, alice->endereco, TAM_ENDERECO);

    TESTAR("Rejeicao de alteracao do destinatario",
           transacao_verificar(&alterada, 1) != 0);

    // 7. Alterar o nonce
    alterada = tx;
    alterada.nonce = 2;

    TESTAR("Rejeicao de alteracao do nonce",
           transacao_verificar(&alterada, 1) != 0);

    // 8. Alterar o chain_id
    alterada = tx;
    alterada.chain_id = 2;

    TESTAR("Rejeicao de alteracao do chain_id",
           transacao_verificar(&alterada, 2) != 0);

    // 9. Usar uma rede diferente na verificacao
    TESTAR("Rejeicao de uma rede inesperada",
           transacao_verificar(&tx, 2) != 0);

    // 10. Alterar a assinatura
    alterada = tx;
    alterada.assinatura[0] ^= 0x01;

    TESTAR("Rejeicao de assinatura adulterada",
           transacao_verificar(&alterada, 1) != 0);

    // 11. Alterar o ID
    alterada = tx;
    alterada.id[0] = (alterada.id[0] == '0') ? '1' : '0';

    TESTAR("Rejeicao de ID adulterado",
           transacao_verificar(&alterada, 1) != 0);

    // 12. Tentar assinar usando a carteira errada
    Transacao tx_errada = tx;

    int resultado_chave_errada = transacao_assinar(
        &tx_errada,
        bob->chave_publica,
        sizeof(bob->chave_publica),
        bob->chave_privada,
        sizeof(bob->chave_privada)
    );

    TESTAR("Rejeicao de chave que nao pertence ao remetente",
           resultado_chave_errada != 0);

    // 13. Tentar assinar com tamanho incorreto de chave
    Transacao tx_tamanho = tx;

    int resultado_tamanho = transacao_assinar(
        &tx_tamanho,
        alice->chave_publica,
        sizeof(alice->chave_publica) - 1,
        alice->chave_privada,
        sizeof(alice->chave_privada)
    );

    TESTAR("Rejeicao de tamanho incorreto da chave publica",
           resultado_tamanho != 0);

    // 14. Tentar criar uma transacao com valor zero
    Transacao tx_zero = tx;
    tx_zero.valor = 0;

    int resultado_zero = transacao_assinar(
        &tx_zero,
        alice->chave_publica,
        sizeof(alice->chave_publica),
        alice->chave_privada,
        sizeof(alice->chave_privada)
    );

    TESTAR("Rejeicao de transacao com valor zero",
           resultado_zero != 0);

    // 15. Tentar usar um tipo ainda nao implementado
    Transacao tx_tipo = tx;
    tx_tipo.tipo = TOKEN_BLOQUEIO;

    int resultado_tipo = transacao_assinar(
        &tx_tipo,
        alice->chave_publica,
        sizeof(alice->chave_publica),
        alice->chave_privada,
        sizeof(alice->chave_privada)
    );

    TESTAR("Rejeicao de tipo de transacao nao implementado",
           resultado_tipo != 0);

    // 16. Resultado final
    printf("\n====================================\n");
    printf(" RESULTADO DOS TESTES\n");
    printf("====================================\n");
    printf("Passaram: %d\n", testes_passaram);
    printf("Falharam: %d\n", testes_falharam);
    printf("Total:    %d\n", testes_passaram + testes_falharam);

    // Liberar as carteiras
    destruir_carteira(alice);
    destruir_carteira(bob);

    return (testes_falharam == 0) ? 0 : 1;
}
