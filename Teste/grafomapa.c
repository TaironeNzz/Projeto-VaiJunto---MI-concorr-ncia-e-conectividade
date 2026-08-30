#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CIDADES 100
#define MAX_LINHA 256

// Estrutura para os vizinhos na lista encadeada (sem peso)
typedef struct Vizinho {
    int idDestino;
    struct Vizinho* prox;
} Vizinho;

// Estrutura para a cidade/vértice
typedef struct {
    int id;
    char nome[50];
    Vizinho* listaAdj;
} Cidade;

// Estrutura do Grafo
typedef struct {
    Cidade cidades[MAX_CIDADES];
    int totalCidades;
} Grafo;

Grafo* criarGrafo() {
    Grafo* g = (Grafo*)malloc(sizeof(Grafo));
    g->totalCidades = 0;
    for (int i = 0; i < MAX_CIDADES; i++) {
        g->cidades[i].id = -1;
        g->cidades[i].listaAdj = NULL;
    }
    return g;
}

// Função para adicionar vizinho sem peso
void adicionarVizinho(Grafo* g, int origId, int destId) {
    Vizinho* novo = (Vizinho*)malloc(sizeof(Vizinho));
    novo->idDestino = destId;
    novo->prox = g->cidades[origId].listaAdj;
    g->cidades[origId].listaAdj = novo;
}

Grafo* carregarGrafoDeArquivo(const char* nomeArquivo) {
    FILE* arq = fopen(nomeArquivo, "r");
    if (!arq) {
        printf("Erro ao abrir o arquivo %s!\n", nomeArquivo);
        return NULL;
    }

    Grafo* g = criarGrafo();
    char linha[MAX_LINHA];

    while (fgets(linha, sizeof(linha), arq)) {
        linha[strcspn(linha, "\r\n")] = 0; // Remove quebras de linha
        if (strlen(linha) == 0) continue;

        // 1. Extrai o ID da cidade
        char* token = strtok(linha, ",");
        if (!token) continue;
        int id = atoi(token);

        // 2. Extrai o Nome da cidade
        token = strtok(NULL, ",");
        if (!token) continue;
        while (*token == ' ') token++; // Remove espacos iniciais
        
        g->cidades[id].id = id;
        strcpy(g->cidades[id].nome, token);
        if (id >= g->totalCidades) {
            g->totalCidades = id + 1;
        }

        // 3. Extrai apenas os IDs dos vizinhos
        while ((token = strtok(NULL, ",")) != NULL) {
            int vizinhoId = atoi(token);
            adicionarVizinho(g, id, vizinhoId);
        }
    }

    fclose(arq);
    return g;
}

void imprimirGrafo(Grafo* g) {
    printf("\n=== GRAFO DE CIDADES (SEM PESO) ===\n");
    for (int i = 0; i < g->totalCidades; i++) {
        if (g->cidades[i].id != -1) {
            printf("\n[%d] Cidade: %s\n", g->cidades[i].id, g->cidades[i].nome);
            printf("    Conecta com: ");
            
            Vizinho* v = g->cidades[i].listaAdj;
            if (v == NULL) {
                printf("Nenhuma conexão");
            }
            while (v != NULL) {
                printf("[%d %s] ", v->idDestino, g->cidades[v->idDestino].nome);
                v = v->prox;
            }
            printf("\n");
        }
    }
}

int main() {
    Grafo* mapa = carregarGrafoDeArquivo("mapa.txt");

    if (mapa != NULL) {
        imprimirGrafo(mapa);
    }

    return 0;
}