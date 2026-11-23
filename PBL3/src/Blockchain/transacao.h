#ifndef TRANSACAO_H
#define TRANSACAO_H

typedef enum {
    TOKEN_TRANSFERENCIA,
    TOKEN_BLOQUEIO,
    TOKEN_LIBERACAO,
    TOKEN_ESTORNO,
    AVALIACAO,
    DENUNCIA,
    AUSENCIA,
    CANCELAMENTO
} TipoTransacao;

typedef struct {
    char id[65];

    char remetente[128];
    char destinatario[128];

    double valor;

    TipoTransacao tipo;

    unsigned long nonce;

    char assinatura[256];

} Transacao;

#endif