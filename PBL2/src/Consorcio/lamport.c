#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "lamport.h"

static uint64_t relogio = 0;
static uint64_t limiteGravado = 0;
static char caminhoArquivo[256] = "";
static pthread_mutex_t relogioMutex = PTHREAD_MUTEX_INITIALIZER;

/* grava a marca d'água (chamar com o mutex travado) */
static void gravarLimite(uint64_t novoLimite) {
    if (caminhoArquivo[0] == '\0') return;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", caminhoArquivo);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)novoLimite);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, caminhoArquivo) == 0) limiteGravado = novoLimite;
}

void lamportIniciar(const char *arquivo) {
    pthread_mutex_lock(&relogioMutex);
    snprintf(caminhoArquivo, sizeof(caminhoArquivo), "%s", arquivo);
    unsigned long long lido = 0;
    FILE *f = fopen(arquivo, "r");
    if (f) {
        if (fscanf(f, "%llu", &lido) != 1) lido = 0;
        fclose(f);
    }
    relogio = lido;
    gravarLimite(relogio + LAMPORT_LOTE);
    pthread_mutex_unlock(&relogioMutex);
}

/* garante que o limite em disco sempre cobre o valor atual */
static void garantirLimite(void) {
    if (relogio >= limiteGravado) gravarLimite(relogio + LAMPORT_LOTE);
}

uint64_t lamportTick(void) {
    pthread_mutex_lock(&relogioMutex);
    relogio++;
    garantirLimite();
    uint64_t v = relogio;
    pthread_mutex_unlock(&relogioMutex);
    return v;
}

uint64_t lamportReceive(uint64_t remoto) {
    pthread_mutex_lock(&relogioMutex);
    if (remoto > relogio) relogio = remoto;
    relogio++;
    garantirLimite();
    uint64_t v = relogio;
    pthread_mutex_unlock(&relogioMutex);
    return v;
}

uint64_t lamportAtual(void) {
    pthread_mutex_lock(&relogioMutex);
    uint64_t v = relogio;
    pthread_mutex_unlock(&relogioMutex);
    return v;
}

int prioridadeComparar(Prioridade a, Prioridade b) {
    if (a.ts != b.ts) return a.ts < b.ts ? -1 : 1;
    if (a.empresa != b.empresa) return a.empresa < b.empresa ? -1 : 1;
    return 0;
}
