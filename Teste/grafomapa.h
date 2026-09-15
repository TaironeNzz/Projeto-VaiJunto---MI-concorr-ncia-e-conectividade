#ifndef GRAFOMAPA_H
#define GRAFOMAPA_H
#include <stdbool.h>
#include "cJSON.h"
#include <pthread.h>

#define MAX_CIDADES 100

typedef struct Vizinho {
    int idDestino;
    struct Vizinho* prox;
} Vizinho;

typedef struct Carona {
    int idTrecho;
    int idDestino;
    char nomeMotorista[50];
    struct Carona* prox;
} Carona;

typedef struct {
    int idTrecho;
    int idOrigem;
    int idDestino;
    char nomeMotorista[50];
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
Grafo* carregarGrafoDeArquivo(const char* nomeArquivo);
int buscarIdPorNome(Grafo* g, const char* nome);
bool existeCaminhoBFSPorNome(Grafo* g, const char* origem, const char* destino);
// AGORA RETORNA cJSON* (Sem o socketCliente)
cJSON* buscar_rotas_no_grafo(cJSON *jsonLogin, FILE *arquivoTrechos);

#endif