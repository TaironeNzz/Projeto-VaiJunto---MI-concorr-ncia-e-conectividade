#ifndef GRAFOMAPA_H
#define GRAFOMAPA_H

#define MAX_CIDADES 100

typedef struct Vizinho {
    int idDestino;
    struct Vizinho* prox;
} Vizinho;

typedef struct {
    int id;
    char nome[50];
    Vizinho* listaAdj;
} Cidade;

typedef struct {
    Cidade cidades[MAX_CIDADES];
    int totalCidades;
} Grafo;

Grafo* criarGrafo();
void adicionarVizinho(Grafo* g, int origId, int destId);
Grafo* carregarGrafoDeArquivo(const char* nomeArquivo);
void imprimirGrafo(Grafo* g);
int buscarIdPorNome(Grafo* g, const char* nome);
int existeCaminhoBFSPorNome(Grafo* g, const char* nomeOrigem, const char* nomeDestino);

#endif