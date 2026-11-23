#include <stdio.h>
#include <string.h>

#include "transacao.h"

int main(void) {

    unsigned char buffer32[4];
    unsigned char buffer64[8];

    const unsigned char esperado32[] = {
        0x01, 0x02, 0x03, 0x04
    };

    const unsigned char esperado64[] = {
        0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08
    };


    // Teste de 32 bits
    if (gravar_u32_be(
            buffer32,
            sizeof(buffer32),
            0x01020304
        ) != 0) {

        printf("[ERRO] Falha na conversao de 32 bits.\n");
        return 1;
    }

    if (memcmp(buffer32, esperado32, sizeof(buffer32)) != 0) {
        printf("[ERRO] Resultado de 32 bits incorreto.\n");
        return 1;
    }

    printf("[OK] Conversao de 32 bits.\n");


    // Teste de 64 bits
    if (gravar_u64_be(
            buffer64,
            sizeof(buffer64),
            UINT64_C(0x0102030405060708)
        ) != 0) {

        printf("[ERRO] Falha na conversao de 64 bits.\n");
        return 1;
    }

    if (memcmp(buffer64, esperado64, sizeof(buffer64)) != 0) {
        printf("[ERRO] Resultado de 64 bits incorreto.\n");
        return 1;
    }

    printf("[OK] Conversao de 64 bits.\n");


    // Teste de capacidade insuficiente
    if (gravar_u32_be(buffer32, 3, 0x01020304) == 0) {
        printf("[ERRO] Buffer insuficiente foi aceito.\n");
        return 1;
    }

    printf("[OK] Buffer insuficiente rejeitado.\n");


    // Teste de ponteiro nulo
    if (gravar_u64_be(NULL, 8, 123) == 0) {
        printf("[ERRO] Ponteiro nulo foi aceito.\n");
        return 1;
    }

    printf("[OK] Ponteiro nulo rejeitado.\n");


    printf("\nTODOS OS TESTES PASSARAM!\n");

    return 0;
}