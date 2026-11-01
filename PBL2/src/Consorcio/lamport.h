#ifndef LAMPORT_H
#define LAMPORT_H
#include <stdint.h>

/*
 * Relógio lógico de Lamport (Lamport, 1978).
 *
 * Regras:
 *   - evento local / envio de mensagem : C = C + 1  (lamportTick), o valor vai na mensagem;
 *   - recebimento de mensagem com C_m  : C = max(C, C_m) + 1  (lamportReceive).
 *
 * Isto garante: se a -> b (a aconteceu antes de b), então C(a) < C(b).
 * Não depende de relógios físicos sincronizados nem do atraso das mensagens.
 *
 * Persistência: para o relógio nunca "voltar no tempo" depois de uma queda do servidor,
 * grava-se em disco um LIMITE SUPERIOR (marca d'água) a cada LAMPORT_LOTE incrementos.
 * Na partida o relógio recomeça desse limite, que é >= qualquer valor já emitido.
 */
#define LAMPORT_LOTE 1000

void     lamportIniciar(const char *arquivo); /* lê a marca d'água do disco (se existir) */
uint64_t lamportTick(void);                   /* evento local ou envio */
uint64_t lamportReceive(uint64_t remoto);     /* recebimento */
uint64_t lamportAtual(void);

/* Prioridade total de uma transação: (timestamp de Lamport, id da empresa coordenadora).
 * O id da empresa desempata timestamps iguais gerados por empresas diferentes. */
typedef struct {
    uint64_t ts;
    int      empresa;
} Prioridade;

/* retorna <0 se a tem precedência (é mais "velha") que b, >0 se b tem, 0 se iguais */
int prioridadeComparar(Prioridade a, Prioridade b);

#endif
