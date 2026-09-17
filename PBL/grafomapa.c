#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include "grafomapa.h"

extern pthread_mutex_t trechosMutex;
extern Grafo *mapa;

bool existeCaminhoBFSPorNome(Grafo* g, const char* origem, const char* destino) {
    if (!g || !origem || !destino) return false;

    // 1. Obtem os IDs correspondentes aos nomes
    int origId = buscarIdPorNome(g, origem);
    int destId = buscarIdPorNome(g, destino);

    // Se alguma das cidades nao existir no mapa
    if (origId == -1 || destId == -1) return false;

    // Se origem e destino forem a mesma cidade
    if (origId == destId) return true;

    // 2. Estruturas para o algoritmo BFS
    bool visitado[MAX_CIDADES] = { false };
    int fila[MAX_CIDADES];
    int inicio = 0, fim = 0;

    // Enfileira a cidade de origem
    fila[fim++] = origId;
    visitado[origId] = true;

    // 3. Execucao da BFS
    while (inicio < fim) {
        int atual = fila[inicio++];

        // Se chegou ao destino, existe caminho
        if (atual == destId) {
            return true;
        }

        // Percorre todas as cidades vizinhas na lista de adjacência
        Vizinho* v = g->cidades[atual].listaAdj;
        while (v != NULL) {
            int vizinhoId = v->idDestino;

            if (!visitado[vizinhoId]) {
                visitado[vizinhoId] = true;
                fila[fim++] = vizinhoId;
            }
            v = v->prox;
        }
    }

    // Se esvaziou a fila e nao encontrou o destino
    return false;
}

Grafo* criarGrafo(void) {
    Grafo* g = (Grafo*)malloc(sizeof(Grafo));
    g->totalCidades = 0;
    for (int i = 0; i < MAX_CIDADES; i++) {
        g->cidades[i].id = -1;
        g->cidades[i].nome[0] = '\0';
        g->cidades[i].listaAdj = NULL;
        g->cidades[i].listaCaronas = NULL;
    }
    return g;
}

void liberarGrafo(Grafo* g) {
    if (!g) return;
    for (int i = 0; i < MAX_CIDADES; i++) {
        Vizinho* v = g->cidades[i].listaAdj;
        while (v) {
            Vizinho* tmp = v;
            v = v->prox;
            free(tmp);
        }
        Carona* c = g->cidades[i].listaCaronas;
        while (c) {
            Carona* tmp = c;
            c = c->prox;
            free(tmp);
        }
    }
    free(g);
}

int buscarIdPorNome(Grafo* g, const char* nome) {
    if (!g || !nome) return -1;
    for (int i = 0; i < g->totalCidades; i++) {
        if (strcasecmp(g->cidades[i].nome, nome) == 0) {
            return i;
        }
    }
    return -1;
}

void oferecerCarona(Grafo* g, int idTrecho, int origId, int destId, const char* nomeMotorista) {
    Carona* nova = (Carona*)malloc(sizeof(Carona));
    nova->idTrecho = idTrecho;
    nova->idDestino = destId;
    strncpy(nova->nomeMotorista, nomeMotorista, 49);
    nova->nomeMotorista[49] = '\0';
    nova->prox = g->cidades[origId].listaCaronas;
    g->cidades[origId].listaCaronas = nova;
}

static void dfsParaJSON(Grafo* mapaBase, Grafo* grafoCaronas, int atual, int destino, 
                        int visitado[], Trecho caminho[], int tamCaminho, cJSON *arrayResposta) {
    if (atual == destino) {
        cJSON *objetoRota = cJSON_CreateObject();
        cJSON *arrayTrechos = cJSON_CreateArray();

        for (int i = 0; i < tamCaminho; i++) {
            cJSON *itemTrecho = cJSON_CreateObject();
            cJSON_AddNumberToObject(itemTrecho, "id", caminho[i].idTrecho);
            cJSON_AddStringToObject(itemTrecho, "origem", mapaBase->cidades[caminho[i].idOrigem].nome);
            cJSON_AddStringToObject(itemTrecho, "destino", mapaBase->cidades[caminho[i].idDestino].nome);
            cJSON_AddStringToObject(itemTrecho, "nomeMotorista", caminho[i].nomeMotorista);
            cJSON_AddItemToArray(arrayTrechos, itemTrecho);
        }

        cJSON_AddItemToObject(objetoRota, "trechos", arrayTrechos);
        cJSON_AddItemToArray(arrayResposta, objetoRota);
        return;
    }

    visitado[atual] = 1;

    Carona* c = grafoCaronas->cidades[atual].listaCaronas;
    while (c != NULL) {
        int vizinho = c->idDestino;

        if (!visitado[vizinho]) {
            caminho[tamCaminho].idTrecho = c->idTrecho;
            caminho[tamCaminho].idOrigem = atual;
            caminho[tamCaminho].idDestino = vizinho;
            strcpy(caminho[tamCaminho].nomeMotorista, c->nomeMotorista);

            dfsParaJSON(mapaBase, grafoCaronas, vizinho, destino, visitado, caminho, tamCaminho + 1, arrayResposta);
        }
        c = c->prox;
    }

    visitado[atual] = 0;
}

// RETORNA O OBJETO cJSON PARA O SERVIDOR
cJSON* buscar_rotas_no_grafo(cJSON *jsonLogin, FILE *arquivoTrechos) {
    cJSON *arrayResposta = cJSON_CreateArray();

    char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *destinoBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));

    if (origemBuscada == NULL || destinoBuscado == NULL) {
        return arrayResposta;
    }

    pthread_mutex_lock(&trechosMutex);
    if (arquivoTrechos == NULL) {
        pthread_mutex_unlock(&trechosMutex);
        return arrayResposta;
    }
    rewind(arquivoTrechos);

    Grafo *grafoCaronas = criarGrafo();
    char linha[512];

    while (fgets(linha, sizeof(linha), arquivoTrechos) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            cJSON *idObj = cJSON_GetObjectItem(trechoJson, "id");
            char *motorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "nomeMotorista"));
            char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "origem"));
            char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));

            if (capacidade > 0 && origem != NULL && destino != NULL && motorista != NULL && idObj != NULL) {
                int idOrigem = buscarIdPorNome(mapa, origem);
                int idDestino = buscarIdPorNome(mapa, destino);

                if (idOrigem != -1 && idDestino != -1) {
                    oferecerCarona(grafoCaronas, idObj->valueint, idOrigem, idDestino, motorista);
                }
            }
            cJSON_Delete(trechoJson);
        }
    }
    pthread_mutex_unlock(&trechosMutex);

    int origId = buscarIdPorNome(mapa, origemBuscada);
    int destId = buscarIdPorNome(mapa, destinoBuscado);

    if (origId != -1 && destId != -1) {
        int visitado[MAX_CIDADES] = {0};
        Trecho caminho[MAX_CIDADES];
        dfsParaJSON(mapa, grafoCaronas, origId, destId, visitado, caminho, 0, arrayResposta);
    }
    liberarGrafo(grafoCaronas);
    return arrayResposta; // Retorna o JSON direto para a lógica interna do Servidor
}

void adicionarVizinho(Grafo* g, int origId, int destId) {
    if (!g || origId < 0 || origId >= MAX_CIDADES) return;

    Vizinho* novo = (Vizinho*)malloc(sizeof(Vizinho));
    if (!novo) return;

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
    char linha[256];

    while (fgets(linha, sizeof(linha), arq) != NULL) {
        // Remove quebra de linha (\r ou \n) no final
        linha[strcspn(linha, "\r\n")] = 0;
        if (strlen(linha) == 0) continue; // Pula linhas vazias

        // 1. Extrai o ID da cidade
        char* token = strtok(linha, ",");
        if (!token) continue;
        int id = atoi(token);

        if (id < 0 || id >= MAX_CIDADES) continue; // Valida limites do array

        // 2. Extrai o Nome da cidade
        token = strtok(NULL, ",");
        if (!token) continue;

        // Remove espacos em branco no inicio do nome
        while (*token == ' ') token++;

        g->cidades[id].id = id;
        strncpy(g->cidades[id].nome, token, 49);
        g->cidades[id].nome[49] = '\0';

        // Atualiza o total de cidades com base no maior ID encontrado
        if (id + 1 > g->totalCidades) {
            g->totalCidades = id + 1;
        }

        // 3. Extrai os IDs dos vizinhos (demais elementos separados por vírgula)
        while ((token = strtok(NULL, ",")) != NULL) {
            int destId = atoi(token);
            if (destId >= 0 && destId < MAX_CIDADES) {
                adicionarVizinho(g, id, destId);
            }
        }
    }

    fclose(arq);
    return g;
}