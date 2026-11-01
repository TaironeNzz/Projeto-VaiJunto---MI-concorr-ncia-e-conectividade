#ifndef CONSORCIO_H
#define CONSORCIO_H
#include <stddef.h>
#include <microhttpd.h>
#include "cJSON.h"

/* Ids de trechos de OUTRAS empresas são vistos pelo cliente como  empresaId * ID_STRIDE + idLocal.
 * Ids < ID_STRIDE são trechos da própria empresa (compatível com o cliente do Problema 1). */
#define ID_STRIDE 1000000

/* códigos de resultado de uma confirmação de itinerário */
#define ITIN_OK                     1
#define ITIN_INVALIDO               0
#define ITIN_NAO_ENCONTRADO        -1
#define ITIN_SEM_ASSENTO           -2
#define ITIN_DUPLICADO             -3
#define ITIN_ERRO                  -4
#define ITIN_EMPRESA_INDISPONIVEL  -5

int  consorcioPortaClientes(void);
int  consorcioPortaRest(void);
int  consorcioEmpresaId(void);

/* lê a configuração (variáveis de ambiente), recupera o estado do disco (participante e coordenador)
 * e inicia a thread de manutenção. Chamar ANTES de abrir a API REST e o socket dos clientes. */
int  consorcioIniciar(void);

/* Coordena o 2PC de um itinerário (possivelmente com trechos de várias empresas). */
int  consorcioConfirmarItinerario(cJSON *req, int *idItinerario, char *idTransacao, size_t tamTx);

/* Trechos disponíveis em TODAS as empresas alcançáveis (local + pares). Cada item: id, empresaId,
 * nomeMotorista, origem, destino, data, hora, capacidade, preco. Empresas fora do ar são ignoradas.
 * Quem chama libera com cJSON_Delete. */
cJSON *listarTrechosConsorcio(const char *data);

/* API REST entre servidores (libmicrohttpd) */
enum MHD_Result consorcioHandlerRest(void *cls, struct MHD_Connection *connection,
    const char *url, const char *method, const char *version,
    const char *upload_data, size_t *upload_data_size, void **con_cls);
void consorcioLiberarRequisicao(void *cls, struct MHD_Connection *connection,
    void **con_cls, enum MHD_RequestTerminationCode toe);

/* injeção de falhas: se CRASH_AT=<nome>, o processo morre (_exit) ao passar por esse ponto */
void pontoDeFalha(const char *nome);

#endif
