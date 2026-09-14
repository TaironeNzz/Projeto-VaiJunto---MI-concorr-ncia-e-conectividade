#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>

#define MAX_CIDADES 100
#define MAX_LINHA 256

// 1. ESTRUTURAS (Declaradas na ordem correta)

// Estrutura para cada trecho oferecido por um motorista
typedef struct Carona {
    int idDestino;
    char nomeMotorista[50];
    struct Carona* prox;
} Carona;

// Estrutura para a Cidade
typedef struct {
    int id;
    char nome[50];
    Carona* listaCaronas; // Lista encadeada de caronas disponíveis
} Cidade;

// Estrutura do Grafo
typedef struct {
    Cidade cidades[MAX_CIDADES];
    int totalCidades;
} Grafo;

// Estrutura auxiliar para rastrear os trechos durante a busca DFS
typedef struct {
    int idOrigem;
    int idDestino;
    char nomeMotorista[50];
} Trecho;

// 2. FUNÇÕES DO GRAFO

Grafo* criarGrafo() {
    Grafo* g = (Grafo*)malloc(sizeof(Grafo));
    g->totalCidades = 0;
    for (int i = 0; i < MAX_CIDADES; i++) {
        g->cidades[i].id = -1;
        g->cidades[i].listaCaronas = NULL;
    }
    return g;
}

// Adiciona uma oferta de carona entre duas cidades
void oferecerCarona(Grafo* g, int origId, int destId, const char* nomeMotorista) {
    Carona* nova = (Carona*)malloc(sizeof(Carona));
    nova->idDestino = destId;
    strcpy(nova->nomeMotorista, nomeMotorista);
    
    nova->prox = g->cidades[origId].listaCaronas;
    g->cidades[origId].listaCaronas = nova;
}

// Carrega cidades e cria ofertas "padrão" a partir de arquivo CSV
Grafo* carregarGrafoDeArquivo(const char* nomeArquivo) {
    FILE* arq = fopen(nomeArquivo, "r");
    if (!arq) {
        printf("Erro ao abrir o arquivo %s!\n", nomeArquivo);
        return NULL;
    }

    Grafo* g = criarGrafo();
    char linha[MAX_LINHA];

    while (fgets(linha, sizeof(linha), arq)) {
        linha[strcspn(linha, "\r\n")] = 0;
        if (strlen(linha) == 0) continue;

        char* token = strtok(linha, ",");
        if (!token) continue;
        int id = atoi(token);

        token = strtok(NULL, ",");
        if (!token) continue;
        while (*token == ' ') token++;
        
        g->cidades[id].id = id;
        strcpy(g->cidades[id].nome, token);
        if (id >= g->totalCidades) {
            g->totalCidades = id + 1;
        }

        // Lê os vizinhos e adiciona como caronas padrão (Sistema/Linha)
        while ((token = strtok(NULL, ",")) != NULL) {
            int vizinhoId = atoi(token);
            oferecerCarona(g, id, vizinhoId, "Sistema/Linha");
        }
    }

    fclose(arq);
    return g;
}

int buscarIdPorNome(Grafo* g, const char* nome) {
    for (int i = 0; i < g->totalCidades; i++) {
        if (g->cidades[i].id != -1 && strcasecmp(g->cidades[i].nome, nome) == 0) {
            return i;
        }
    }
    return -1;
}

// 3. FUNÇÕES DE BUSCA DE ROTAS (DFS + BACKTRACKING)

void buscarTodasRotasDFS(Grafo* g, int atual, int destino, int visitado[], Trecho caminho[], int tamCaminho, int* contadorRotas) {
    if (atual == destino) {
        (*contadorRotas)++;
        printf("\n--- OPÇÃO DE ROTA %d ---\n", *contadorRotas);
        for (int i = 0; i < tamCaminho; i++) {
            printf("  Trecho %d: %s -> %s (Motorista: %s)\n", 
                   i + 1, 
                   g->cidades[caminho[i].idOrigem].nome, 
                   g->cidades[caminho[i].idDestino].nome, 
                   caminho[i].nomeMotorista);
        }
        return;
    }

    visitado[atual] = 1;

    Carona* c = g->cidades[atual].listaCaronas;
    while (c != NULL) {
        int vizinho = c->idDestino;

        if (!visitado[vizinho]) {
            caminho[tamCaminho].idOrigem = atual;
            caminho[tamCaminho].idDestino = vizinho;
            strcpy(caminho[tamCaminho].nomeMotorista, c->nomeMotorista);

            buscarTodasRotasDFS(g, vizinho, destino, visitado, caminho, tamCaminho + 1, contadorRotas);
        }
        c = c->prox;
    }

    visitado[atual] = 0; // Backtracking
}

void listarTodasAsRotasCaronas(Grafo* g, const char* nomeOrigem, const char* nomeDestino) {
    int origem = buscarIdPorNome(g, nomeOrigem);
    int destino = buscarIdPorNome(g, nomeDestino);

    if (origem == -1 || destino == -1) {
        printf("Origem ou destino nao encontrados!\n");
        return;
    }

    int visitado[MAX_CIDADES] = {0};
    Trecho caminho[MAX_CIDADES];
    int contadorRotas = 0;

    printf("\n=======================================================");
    printf("\n  BUSCANDO TODAS AS ROTAS: %s -> %s", g->cidades[origem].nome, g->cidades[destino].nome);
    printf("\n=======================================================\n");

    buscarTodasRotasDFS(g, origem, destino, visitado, caminho, 0, &contadorRotas);

    if (contadorRotas == 0) {
        printf("Nenhuma rota encontrada.\n");
    } else {
        printf("\nTotal de combinações de rotas encontradas: %d\n", contadorRotas);
    }
}

void imprimirGrafo(Grafo* g) {
    printf("\n=== GRAFO DE CARONAS ===\n");
    for (int i = 0; i < g->totalCidades; i++) {
        if (g->cidades[i].id != -1) {
            printf("\n[%d] Cidade: %s\n", g->cidades[i].id, g->cidades[i].nome);
            printf("    Caronas saindo daqui:\n");
            
            Carona* c = g->cidades[i].listaCaronas;
            if (c == NULL) {
                printf("    Nenhuma carona cadastrada.\n");
            }
            while (c != NULL) {
                printf("    -> Para: %s (Motorista: %s)\n", g->cidades[c->idDestino].nome, c->nomeMotorista);
                c = c->prox;
            }
        }
    }
}