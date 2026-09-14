#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include "grafomapa.h"

extern pthread_mutex_t trechosMutex;
extern Grafo *mapa;

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