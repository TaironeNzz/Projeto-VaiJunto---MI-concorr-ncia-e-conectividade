#ifndef GRAFOMAPA_H
#define GRAFOMAPA_H
#include <stdio.h>
#include <stdbool.h>
#include "cJSON.h"
#include <pthread.h>

#define MAX_CIDADES 100

typedef struct Vizinho {
    int idDestino;
    struct Vizinho* prox;
} Vizinho;

/* idTrecho = id visto pelo cliente (empresaId * ID_STRIDE + idLocal para trechos de OUTRAS empresas) */
typedef struct Carona {
    int idTrecho;
    int idDestino;
    int empresaId;
    char nomeMotorista[50];
    char data[16];
    char hora[8];
    double preco;
    struct Carona* prox;
} Carona;

typedef struct {
    int idTrecho;
    int idOrigem;
    int idDestino;
    int empresaId;
    char nomeMotorista[50];
    char data[16];
    char hora[8];
    double preco;
} Trecho;

typedef struct {
    int id;
    char nome[50];
    Vizinho* listaAdj;
    Carona* listaCaronas;
} Cidade;

typedef struct {
    Cidade cidades[MAX_CIDADES];
    int totalCidades;
} Grafo;

Grafo* criarGrafo(void);
void liberarGrafo(Grafo* g);
void adicionarVizinho(Grafo* g, int origId, int destId);
void oferecerCarona(Grafo* g, int idTrecho, int origId, int destId, const char* nomeMotorista);
/* versão completa: guarda também empresa, data, hora e preço para devolver ao cliente */
void oferecerCaronaCompleta(Grafo* g, int idTrecho, int empresaId, int origId, int destId,
                            const char* nomeMotorista, const char* data, const char* hora, double preco);
Grafo* carregarGrafoDeArquivo(const char* nomeArquivo);
int buscarIdPorNome(Grafo* g, const char* nome);
bool existeCaminhoBFSPorNome(Grafo* g, const char* origem, const char* destino);
/* Itinerários (cadeias de trechos) entre origem e destino usando as caronas de TODAS as empresas do consórcio
 * (obtidas por listarTrechosConsorcio). Limites: MAX_TRECHOS_ROTA trechos por itinerário e MAX_ROTAS_RESPOSTA rotas. */
#define MAX_TRECHOS_ROTA   6
#define MAX_ROTAS_RESPOSTA 200
cJSON* buscar_rotas_no_grafo(cJSON *jsonLogin);

#endif