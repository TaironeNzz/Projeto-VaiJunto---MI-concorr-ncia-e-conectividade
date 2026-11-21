#ifndef SERVIDOR_H
#define SERVIDOR_H
#include <pthread.h>
#include "cJSON.h"

/* Funções de servidor.c usadas pelo módulo do consórcio (consorcio.c) */
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
void log_mensagem(LogLevel level, const char *format, ...);

pthread_mutex_t *obterMutexTrecho(int idTrecho);
cJSON *lerTrechoNoLock(int id);
cJSON *lerTrecho(int id);
int    gravarTrechoNoLock(int id, cJSON *trecho);
int    lerLimiteIdTrechos(void);

int  proximoIdItinerario(void);
void garantirIdItinerarioMinimo(int id);
int  gravarItinerario(int id, cJSON *itinerario);
void cancelarItinerarioCompensado(int id, const char *motivo);
#endif
