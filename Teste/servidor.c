#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "cJSON.h"
#include "grafomapa.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>
#define PORT 65432

Grafo *mapa;

int idTrecho = 0;
pthread_mutex_t trechosMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t loginMotoristaMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t loginClienteMutex = PTHREAD_MUTEX_INITIALIZER;


void encontrarID(){
    FILE *arquivo = fopen("trechosCadastrados/trechos.json", "r");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo de trechos");
        return;
    }

    char linha[256];
    while (fgets(linha, sizeof(linha), arquivo) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "idTrecho"));
            if (id >= idTrecho) {
                idTrecho = id + 1;
            }
            cJSON_Delete(trechoJson);
        }
    }
    fclose(arquivo);
}

int trechoExpirado(const char *data, const char *hora) {
    struct tm trechoTm = {0};
    int dia, mes, ano, horaInt, minuto;

    if (data == NULL || hora == NULL) return 0;
    if (sscanf(data, "%d/%d/%d", &dia, &mes, &ano) != 3) return 0;
    if (sscanf(hora, "%d:%d", &horaInt, &minuto) != 2) return 0;

    trechoTm.tm_mday = dia;
    trechoTm.tm_mon  = mes - 1;
    trechoTm.tm_year = ano - 1900;
    trechoTm.tm_hour = horaInt;
    trechoTm.tm_min  = minuto;
    trechoTm.tm_sec  = 0;
    trechoTm.tm_isdst = -1;

    time_t tempoTrecho = mktime(&trechoTm);
    if (tempoTrecho == (time_t)-1) return 0;

    return tempoTrecho < time(NULL);
}

void limparTrechosExpirados(void) {
    pthread_mutex_lock(&trechosMutex);

    FILE *origem = fopen("trechosCadastrados/trechos.json", "r");
    if (origem == NULL) {
        pthread_mutex_unlock(&trechosMutex);
        return;
    }

    FILE *temp = fopen("trechosCadastrados/trechos.tmp", "w");
    if (temp == NULL) {
        fclose(origem);
        pthread_mutex_unlock(&trechosMutex);
        return;
    }

    char linha[256];
    while (fgets(linha, sizeof(linha), origem) != NULL) {
        cJSON *trecho = cJSON_Parse(linha);
        if (trecho == NULL) continue;

        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "hora"));

        if (!trechoExpirado(data, hora)) {
            fputs(linha, temp);
        }

        cJSON_Delete(trecho);
    }

    fclose(origem);
    fclose(temp);

    remove("trechosCadastrados/trechos.json");
    rename("trechosCadastrados/trechos.tmp", "trechosCadastrados/trechos.json");

    pthread_mutex_unlock(&trechosMutex);
}

void *rotinaLimpezaTrechos(void *arg) {
    (void)arg;
    while (1) {
        limparTrechosExpirados();
        sleep(60);
    }
    return NULL;
}

void cadastrarCliente(cJSON *jsonLogin, int socketCliente, FILE *arquivoLogin){
    char dadosLogin[256] = {0};
    int emailEncontrado = 0;
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    rewind(arquivoLogin);

    pthread_mutex_lock(&loginClienteMutex);
    while(fgets(dadosLogin, sizeof(dadosLogin), arquivoLogin) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                send(socketCliente, "EMAIL_JA_CADASTRADO", 19, 0);
                emailEncontrado = 1;
                break;
            }
        }
        cJSON_Delete(dadosJson);
    }
    if (!emailEncontrado) {
        char *saida = cJSON_PrintUnformatted(jsonLogin);
        fprintf(arquivoLogin, "%s\n", saida);
        send(socketCliente, "CADASTRO_REALIZADO", 18, 0);
        free(saida);
    }
    pthread_mutex_unlock(&loginClienteMutex);
    
    fflush(arquivoLogin);
}

void cadastrarMotorista(cJSON *jsonLogin, int socketMotorista, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[256] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    rewind(arquivo);

    pthread_mutex_lock(&loginMotoristaMutex);
    while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                send(socketMotorista, "EMAIL_JA_CADASTRADO", 19, 0);
                emailEncontrado = 1;
                break;
                }
            }
        cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            char *saida = cJSON_PrintUnformatted(jsonLogin);
            fprintf(arquivo, "%s\n", saida);
            fflush(arquivo);
            send(socketMotorista, "CADASTRO_REALIZADO", 18, 0);
            free(saida);
        }
    pthread_mutex_unlock(&loginMotoristaMutex);
}

int cadastrarTrecho(cJSON *jsonLogin, int socketMotorista, FILE *arquivoTrechos){
    if (mapa == NULL) {
        printf("Mapa nao carregado. Nao e possivel cadastrar trecho.\n");
        send(socketMotorista, "MAPA_NAO_CARREGADO", 18, 0);
        return 0;
    }
    rewind(arquivoTrechos);

    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
    int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "capacidade"));
    char *data = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "data"));
    char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "hora"));
    float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "preco"));
    cJSON *arrayClientes = cJSON_GetObjectItem(jsonLogin, "clientes");

    if (mapa != NULL) {
        if (existeCaminhoBFSPorNome(mapa, origem, destino)) {
            pthread_mutex_lock(&trechosMutex);
            if (arquivoTrechos == NULL) {
                perror("Erro ao abrir o arquivo de trechos");
                pthread_mutex_unlock(&trechosMutex);
                return 0;
            }
            cJSON *trecho = cJSON_CreateObject();
            cJSON_AddNumberToObject(trecho, "idTrecho", idTrecho);
            cJSON_AddStringToObject(trecho, "nomeMotorista", nomeMotorista);
            cJSON_AddStringToObject(trecho, "origem", origem);
            cJSON_AddStringToObject(trecho, "destino", destino);
            cJSON_AddStringToObject(trecho, "data", data);
            cJSON_AddStringToObject(trecho, "hora", hora);
            cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
            cJSON_AddNumberToObject(trecho, "preco", preco);

            cJSON_AddItemToObject(trecho, "clientes", cJSON_Duplicate(arrayClientes, 1));
            char *saida = cJSON_PrintUnformatted(trecho);
            fprintf(arquivoTrechos, "%s\n", saida);
            free(saida);
            cJSON_Delete(trecho);
            idTrecho++;
            fflush(arquivoTrechos);
            pthread_mutex_unlock(&trechosMutex);
            send(socketMotorista, "TRECHO_CADASTRADO", 17, 0);
            return 1;
        } else {
        send(socketMotorista, "FALHA_CADASTRO_TRECHO", 21, 0);
        return 0;
        }
    }
}

void listar_trechos(cJSON *jsonLogin, int socketMotorista, FILE *arquivoTrechos){
    char dadosTrechos[256] = {0};
    pthread_mutex_lock(&trechosMutex);
    cJSON *arrayResposta = cJSON_CreateArray();
    rewind(arquivoTrechos);

    while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivoTrechos) != NULL) {
        cJSON *trechosJson = cJSON_Parse(dadosTrechos);
        if (trechosJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
            char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
            char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
            float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

            if (nomeMotoristaTrecho != NULL && nomeMotorista != NULL &&
                strcmp(nomeMotorista, nomeMotoristaTrecho) == 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                cJSON_AddStringToObject(item, "destino", cidadeDestino);
                cJSON_AddNumberToObject(item, "capacidade", capacidade);
                cJSON_AddStringToObject(item, "data", data);
                cJSON_AddStringToObject(item, "hora", hora);
                cJSON_AddNumberToObject(item, "preco", preco);
                cJSON_AddItemToArray(arrayResposta, item);
            }
        }
        cJSON_Delete(trechosJson);
    }
    pthread_mutex_unlock(&trechosMutex);
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    send(socketMotorista, resposta, strlen(resposta), 0);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

void loginMotorista(cJSON *jsonLogin, int socketMotorista, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[256] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));
    rewind(arquivo);

    pthread_mutex_lock(&loginMotoristaMutex);
    while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
        char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
        char *senha = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "senha"));
        if (strcmp(email, emailBuscado) == 0 && strcmp(senha, senhaBuscada) == 0) {
            char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson,"nome"));
            cJSON *respostaJson = cJSON_CreateObject();
            if (respostaJson != NULL){
                if (nome != NULL){
                    cJSON_AddStringToObject(respostaJson,"nome", nome);
                    }
                }
                char *resposta = cJSON_PrintUnformatted(respostaJson);
                    
                send(socketMotorista, resposta, strlen(resposta), 0);
                emailEncontrado = 1;
                cJSON_Delete(respostaJson);
                break;
                }
            }
        cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            send(socketMotorista, "NAO_AUTENTICADO", 15, 0);
        }
    pthread_mutex_unlock(&loginMotoristaMutex);
}

void cancelarTrechoMotorista(cJSON *jsonLogin, int socketMotorista, FILE *arquivoTrechos){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    int trechoEncontrado = 0;

    pthread_mutex_lock(&trechosMutex);

    FILE *origem = fopen("trechosCadastrados/trechos.json", "r");
    if (origem == NULL) {
        perror("Erro ao abrir o arquivo");
        pthread_mutex_unlock(&trechosMutex);
        send(socketMotorista, "TRECHO_NAO_ENCONTRADO", 21, 0);
        return;
    }

    FILE *temp = fopen("trechosCadastrados/trechos.tmp", "w");
    if (temp == NULL) {
        perror("Erro ao abrir arquivo temporario");
        fclose(origem);
        pthread_mutex_unlock(&trechosMutex);
        send(socketMotorista, "TRECHO_NAO_ENCONTRADO", 21, 0);
        return;
    }

    char linha[256];
    while (fgets(linha, sizeof(linha), origem) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "idTrecho"));
            char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "nomeMotorista"));

            if (id == idSelecionado && nomeMotorista != NULL && nomeMotoristaTrecho != NULL &&
                strcmp(nomeMotorista, nomeMotoristaTrecho) == 0) {
                trechoEncontrado = 1;
            } else {
                char *saida = cJSON_PrintUnformatted(trechoJson);
                fprintf(temp, "%s\n", saida);
                free(saida);
            }
            cJSON_Delete(trechoJson);
        } else {
            fputs(linha, temp);
        }
    }

    fclose(origem);
    fclose(temp);
    remove("trechosCadastrados/trechos.json");
    rename("trechosCadastrados/trechos.tmp", "trechosCadastrados/trechos.json");

    pthread_mutex_unlock(&trechosMutex);

    if (trechoEncontrado) {
        send(socketMotorista, "TRECHO_CANCELADO", 16, 0);
    } else {
        send(socketMotorista, "TRECHO_NAO_ENCONTRADO", 21, 0);
    }
}

void tratarMotorista(int socketMotorista, cJSON *jsonLogin, char *acao){
    char dadosLogin[256] = {0};
    char dadosTrechos[256] = {0};
    int emailEncontrado = 0;
    FILE *arquivo = fopen("dados/loginMotorista.json", "a+");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo");
        close(socketMotorista);
        return;
    }
    rewind(arquivo);

    FILE *arquivo2 = fopen("trechosCadastrados/trechos.json", "a+");
    if (arquivo2 == NULL) {
        perror("Erro ao abrir o arquivo");
        close(socketMotorista);
        return;
    }
    rewind(arquivo2);

    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));
    
    if (strcmp(acao, "login") == 0) {
        loginMotorista(jsonLogin, socketMotorista, arquivo);
    } else if (strcmp(acao, "cadastro") == 0) {
        cadastrarMotorista(jsonLogin, socketMotorista, arquivo);
    } else if (strcmp(acao, "cadastrar_trecho") == 0) {
        cadastrarTrecho(jsonLogin, socketMotorista, arquivo2);
    } else if (strcmp(acao, "listar_trechos") == 0) {
        listar_trechos(jsonLogin, socketMotorista, arquivo2);
    } else if (strcmp(acao, "cancelar_trecho") == 0) {
    cancelarTrechoMotorista(jsonLogin, socketMotorista, arquivo2);
    } else {
        send(socketMotorista, "ACAO_DESCONHECIDA", 17, 0);
    }
    fclose(arquivo);
    fclose(arquivo2);
    return;
}

void loginCliente(cJSON *jsonLogin, int socketCliente, FILE *arquivoLogin){
    int emailEncontrado = 0;
    char dadosLogin[256] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));
    rewind(arquivoLogin);

    pthread_mutex_lock(&loginClienteMutex);
    while(fgets(dadosLogin, sizeof(dadosLogin), arquivoLogin) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            char *senha = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "senha"));
            if (strcmp(email, emailBuscado) == 0 && strcmp(senha, senhaBuscada) == 0) {
                send(socketCliente, "AUTENTICADO", 11, 0);
                emailEncontrado = 1;
                break;
            }
        }
        cJSON_Delete(dadosJson);
    }
    if (!emailEncontrado) {
        send(socketCliente, "NAO_AUTENTICADO", 15, 0);
    }
    pthread_mutex_unlock(&loginClienteMutex);
}

void buscar_carona(cJSON *jsonLogin, int socketCliente, FILE *arquivoTrechos){
    char dadosTrechos[256] = {0};
    pthread_mutex_lock(&trechosMutex);
    if (arquivoTrechos == NULL) {
        perror("Erro ao abrir o arquivo");
        pthread_mutex_unlock(&trechosMutex);
        return;
    }
    rewind(arquivoTrechos);
    cJSON *arrayResposta = cJSON_CreateArray();
    char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *destinoBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));

    while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivoTrechos) != NULL) {
        cJSON *trechosJson = cJSON_Parse(dadosTrechos);
        if (trechosJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
            char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
            float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

            if (cidadeOrigem != NULL && cidadeDestino != NULL &&
                origemBuscada != NULL && destinoBuscado != NULL &&
                strcmp(cidadeOrigem, origemBuscada) == 0 && strcmp(cidadeDestino, destinoBuscado) == 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                cJSON_AddStringToObject(item, "nomeMotorista", nomeMotorista);
                cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                cJSON_AddStringToObject(item, "destino", cidadeDestino);
                cJSON_AddNumberToObject(item, "capacidade", capacidade);
                cJSON_AddStringToObject(item, "data", data);
                cJSON_AddStringToObject(item, "hora", hora);
                cJSON_AddNumberToObject(item, "preco", preco);
                cJSON_AddItemToArray(arrayResposta, item);
            }
        }
        cJSON_Delete(trechosJson);
    }
    pthread_mutex_unlock(&trechosMutex);
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    send(socketCliente, resposta, strlen(resposta), 0);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

void selecionar_carona(cJSON *jsonLogin, int socketCliente){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    int trechoEncontrado = 0;
    int semAssento = 0;

    pthread_mutex_lock(&trechosMutex);

    FILE *origem = fopen("trechosCadastrados/trechos.json", "r");
    if (origem == NULL) {
        perror("Erro ao abrir o arquivo");
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
        return;
    }

    FILE *temp = fopen("trechosCadastrados/trechos.tmp", "w");
    if (temp == NULL) {
        perror("Erro ao abrir arquivo temporario");
        fclose(origem);
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
        return;
    }

    char linha[256];
    char *emailStr = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    while (fgets(linha, sizeof(linha), origem) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "idTrecho"));
            char *cidadeOrigemCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
            char *cidadeDestinoCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "destino"));
            cJSON *arrayClientes = cJSON_GetObjectItem(trechoJson, "clientes");

            int reservouEsteAqui = 0;
            if (id == idSelecionado &&
                cidadeOrigem != NULL && cidadeDestino != NULL &&
                cidadeOrigemCliente != NULL && cidadeDestinoCliente != NULL &&
                strcmp(cidadeOrigem, cidadeOrigemCliente) == 0 &&
                strcmp(cidadeDestino, cidadeDestinoCliente) == 0) {

                int capacidadeAtual = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));
                if (capacidadeAtual > 0) {
                    trechoEncontrado = 1;
                    reservouEsteAqui = 1;
                    cJSON_ReplaceItemInObject(trechoJson, "capacidade", cJSON_CreateNumber(capacidadeAtual - 1));
                    if (emailStr != NULL) {
                        cJSON_AddItemToArray(arrayClientes, cJSON_CreateString(emailStr));
                    }
                } else {
                    semAssento = 1;
                }
            }
            char *saida = cJSON_PrintUnformatted(trechoJson);
            fprintf(temp, "%s\n", saida);
            cJSON_Delete(trechoJson);
            free(saida);
        } else {
            fputs(linha, temp);
        }
    }

    fclose(origem);
    fclose(temp);
    remove("trechosCadastrados/trechos.json");
    rename("trechosCadastrados/trechos.tmp", "trechosCadastrados/trechos.json");

    pthread_mutex_unlock(&trechosMutex);

    if (trechoEncontrado) {
        send(socketCliente, "CARONA_RESERVADA", 16, 0);
    } else if (semAssento) {
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
    } else {
        send(socketCliente, "TRECHO_NAO_ENCONTRADO", 21, 0); 
    }
}

void selecionar_Rota(cJSON *jsonLogin, int socketCliente){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    int trechoEncontrado = 0;
    int semAssento = 0;
    char *retornoTrecho = NULL;

    pthread_mutex_lock(&trechosMutex);

    FILE *origem = fopen("trechosCadastrados/trechos.json", "r");
    if (origem == NULL) {
        perror("Erro ao abrir o arquivo");
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
        return;
    }

    FILE *temp = fopen("trechosCadastrados/trechos.tmp", "w");
    if (temp == NULL) {
        perror("Erro ao abrir arquivo temporario");
        fclose(origem);
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
        return;
    }

    char linha[256];
    char *emailStr = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    while (fgets(linha, sizeof(linha), origem) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "idTrecho"));
            char *cidadeOrigemCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
            char *cidadeDestinoCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "destino"));

            int reservouEsteAqui = 0;
            if (id == idSelecionado &&
                cidadeOrigem != NULL && cidadeDestino != NULL &&
                cidadeOrigemCliente != NULL && cidadeDestinoCliente != NULL &&
                strcmp(cidadeOrigem, cidadeOrigemCliente) == 0 &&
                strcmp(cidadeDestino, cidadeDestinoCliente) == 0) {

                int capacidadeAtual = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));
                if (capacidadeAtual > 0) {
                    trechoEncontrado = 1;
                    reservouEsteAqui = 1;
                    cJSON_ReplaceItemInObject(trechoJson, "capacidade", cJSON_CreateNumber(capacidadeAtual - 1));
                    cJSON *arrayClientes = cJSON_GetObjectItem(trechoJson, "clientes");
                    if (emailStr != NULL && arrayClientes != NULL) {
                        cJSON_AddItemToArray(arrayClientes, cJSON_CreateString(emailStr));
                    }
                    retornoTrecho = cJSON_PrintUnformatted(trechoJson);
                } else {
                    semAssento = 1;
                }
            }
            char *saida = cJSON_PrintUnformatted(trechoJson);
            fprintf(temp, "%s\n", saida);
            free(saida);
            cJSON_Delete(trechoJson);
        } else {
            fputs(linha, temp);
        }
    }

    fclose(origem);
    fclose(temp);
    remove("trechosCadastrados/trechos.json");
    rename("trechosCadastrados/trechos.tmp", "trechosCadastrados/trechos.json");

    pthread_mutex_unlock(&trechosMutex);

    if (trechoEncontrado) {
        send(socketCliente, retornoTrecho, strlen(retornoTrecho), 0);
        free(retornoTrecho);
    } else if (semAssento) {
        send(socketCliente, "ASSENTO_INDISPONIVEL", 20, 0);
    } else {
        send(socketCliente, "TRECHO_NAO_ENCONTRADO", 21, 0); 
    }
    
}

//naaaaaaoooooo termineeeeeii
void combinarTrechos(cJSON *jsonLogin, int socketCliente, FILE *arquivoTrechos){
    cJSON *rotasArray = buscar_rotas_no_grafo(jsonLogin, arquivoTrechos);
    char dadosTrechos[256] = {0};

    if (rotasArray == NULL){
        send(socketCliente, "NENHUMA_ROTA_ENCONTRADA", 23, 0);
        return;
    }
    int totalRotas = cJSON_GetArraySize(rotasArray);
    if (totalRotas == 0) {
        send(socketCliente, "NENHUMA_ROTA_ENCONTRADA", 23, 0);
        cJSON_Delete(rotasArray);
        return;
    }
    cJSON *arrayResposta = cJSON_CreateArray();

    for (int i = 0; i < totalRotas; i++) {
        cJSON *rotaObj = cJSON_GetArrayItem(rotasArray, i);
        cJSON *trechosArray = cJSON_GetObjectItem(rotaObj, "trechos");

        if (!trechosArray || !cJSON_IsArray(trechosArray)) continue;
            
        int totalTrechos = cJSON_GetArraySize(trechosArray);
        for (int j = 0; j < totalTrechos; j++) {
            cJSON *trechoObj = cJSON_GetArrayItem(trechosArray, j);

            cJSON *idObj = cJSON_GetObjectItem(trechoObj, "id");
            cJSON *origemObj = cJSON_GetObjectItem(trechoObj, "origem");
            cJSON *destinoObj = cJSON_GetObjectItem(trechoObj, "destino");
            cJSON *motoristaObj = cJSON_GetObjectItem(trechoObj, "nomeMotorista");

            int id = idObj ? idObj->valueint : -1;
            char *origem = cJSON_GetStringValue(origemObj);
            char *destino = cJSON_GetStringValue(destinoObj);
            char *motorista = cJSON_GetStringValue(motoristaObj);
            
            pthread_mutex_lock(&trechosMutex);
            if (arquivoTrechos == NULL) {
                perror("Erro ao abrir o arquivo");
                pthread_mutex_unlock(&trechosMutex);
                return;
            }
            rewind(arquivoTrechos);

            while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivoTrechos) != NULL) {
                cJSON *trechosJson = cJSON_Parse(dadosTrechos);
                if (trechosJson != NULL) {
                    int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
                    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
                    char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
                    char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
                    int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
                    char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
                    char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
                    float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

                    if (cidadeOrigem != NULL && cidadeDestino != NULL &&
                        origem != NULL && destino != NULL &&
                        strcmp(cidadeOrigem, origem) == 0 && strcmp(cidadeDestino, destino) == 0
                        && capacidade > 0) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddNumberToObject(item, "id", id);
                        cJSON_AddStringToObject(item, "nomeMotorista", nomeMotorista);
                        cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                        cJSON_AddStringToObject(item, "destino", cidadeDestino);
                        cJSON_AddNumberToObject(item, "capacidade", capacidade);
                        cJSON_AddStringToObject(item, "data", data);
                        cJSON_AddStringToObject(item, "hora", hora);
                        cJSON_AddNumberToObject(item, "preco", preco);
                        cJSON_AddItemToArray(arrayResposta, item);
                    }
                    cJSON_Delete(trechosJson);
                }  
            }
            pthread_mutex_unlock(&trechosMutex);
        }
    }
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    send(socketCliente, resposta, strlen(resposta), 0);
    free(resposta);
    cJSON_Delete(arrayResposta);
    cJSON_Delete(rotasArray);
}

//naaao termineei
void finalizar_Rota(cJSON *jsonLogin, int socketCliente){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    int trechoEncontrado = 0;
    char *destinoRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destinoRota"));
    char *origemRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origemRota"));

    cJSON *rota = cJSON_GetObjectItem(jsonLogin, "rota");
    if (rota == NULL){
        printf("Objeto rotas nao criado!\n");
        return;
    }
    int total = cJSON_GetArraySize(rota);

    char origemTemporaria[50] = {0};
    char destinoTemporario[50] = {0};
    for (int i=0; i<total; i++){
        cJSON *trecho = cJSON_GetArrayItem(rota, i);
        if (trecho == NULL){
            printf("Objeto trecho nao encontrado!\n");
            continue;
        }
        int idTrecho = cJSON_GetNumberValue(cJSON_GetObjectItem(trecho, "id"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "destino"));
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "origem"));
        if (i == 0){
            strcpy(origemTemporaria, origem);
        }
        if (existeCaminhoBFSPorNome(mapa, origem, destino)){
            strcpy(destinoTemporario, destino);
        }
    }

    if (strcmp(origemTemporaria, origemRota) == 0 && strcmp(destinoTemporario, destinoRota) == 0) {
        send(socketCliente, "CARONA_CADASTRADA", 17, 0);
    } else {
        send(socketCliente, "CARONA_NAO_CADASTRADA", 21, 0); 
    }
}

void listar_reservas(cJSON *jsonLogin, int socketCliente, FILE *arquivoTrechos){
    char dadosTrechos[256] = {0};
    pthread_mutex_lock(&trechosMutex);
    cJSON *arrayResposta = cJSON_CreateArray();
    rewind(arquivoTrechos);
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));

    while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivoTrechos) != NULL) {
        cJSON *trechosJson = cJSON_Parse(dadosTrechos);
        if (trechosJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
            char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
            float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));
            cJSON *arrayClientes = cJSON_GetObjectItem(trechosJson, "clientes");
            int total = cJSON_GetArraySize(arrayClientes);

            if (total == 0){
                printf("Array de clientes vazio\n");
            }

            for (int i=0; i < total; i++){
                char *emailClienteCarona = cJSON_GetStringValue(cJSON_GetArrayItem(arrayClientes, i));
                if (emailCliente != NULL && emailClienteCarona != NULL &&
                strcmp(emailCliente, emailClienteCarona) == 0) {
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddNumberToObject(item, "id", id);
                    cJSON_AddStringToObject(item, "nomeMotorista", nomeMotoristaTrecho);
                    cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                    cJSON_AddStringToObject(item, "destino", cidadeDestino);
                    cJSON_AddNumberToObject(item, "capacidade", capacidade);
                    cJSON_AddStringToObject(item, "data", data);
                    cJSON_AddStringToObject(item, "hora", hora);
                    cJSON_AddNumberToObject(item, "preco", preco);
                    cJSON_AddItemToArray(arrayResposta, item);
                }
            }
            
        }
        cJSON_Delete(trechosJson);
    }
    pthread_mutex_unlock(&trechosMutex);
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    send(socketCliente, resposta, strlen(resposta), 0);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

void cancelar_carona(cJSON *jsonLogin, int socketCliente, FILE *arquivoTrechos){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    int trechoEncontrado = 0;
    int clienteEncontrado = 0;

    pthread_mutex_lock(&trechosMutex);

    FILE *origem = fopen("trechosCadastrados/trechos.json", "r");
    if (origem == NULL) {
        perror("Erro ao abrir o arquivo");
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "MOTORISTA_CANCELOU", 18, 0);
        return;
    }

    FILE *temp = fopen("trechosCadastrados/trechos.tmp", "w");
    if (temp == NULL) {
        perror("Erro ao abrir arquivo temporario");
        fclose(origem);
        pthread_mutex_unlock(&trechosMutex);
        send(socketCliente, "MOTORISTA_CANCELOU", 18, 0);
        return;
    }

    char linha[256];
    while (fgets(linha, sizeof(linha), origem) != NULL) {
        cJSON *trechoJson = cJSON_Parse(linha);
        if (trechoJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "idTrecho"));

            if (id == idSelecionado) {
                trechoEncontrado = 1;
                cJSON *arrayClientes = cJSON_GetObjectItem(trechoJson, "clientes");
                int total = arrayClientes ? cJSON_GetArraySize(arrayClientes) : 0;

                for (int i = 0; i < total; i++) {
                    char *email = cJSON_GetStringValue(cJSON_GetArrayItem(arrayClientes, i));
                    if (email != NULL && emailCliente != NULL && strcmp(email, emailCliente) == 0) {
                        cJSON_DeleteItemFromArray(arrayClientes, i);
                        clienteEncontrado = 1;
                        int capacidadeAtual = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));
                        cJSON_ReplaceItemInObject(trechoJson, "capacidade", cJSON_CreateNumber(capacidadeAtual + 1));
                        break;
                    }
                }
            }

            char *saida = cJSON_PrintUnformatted(trechoJson);
            fprintf(temp, "%s\n", saida);
            free(saida);
            cJSON_Delete(trechoJson);
        } else {
            fputs(linha, temp);
        }
    }

    fclose(origem);
    fclose(temp);
    remove("trechosCadastrados/trechos.json");
    rename("trechosCadastrados/trechos.tmp", "trechosCadastrados/trechos.json");

    pthread_mutex_unlock(&trechosMutex);

    if (clienteEncontrado) {
        send(socketCliente, "CARONA_CANCELADA", 16, 0);
    } else if (trechoEncontrado) {
        send(socketCliente, "TRECHO_NAO_ENCONTRADO", 21, 0);
    } else {
        send(socketCliente, "MOTORISTA_CANCELOU", 18, 0);
    }
}

void buscar_trechos_partida(cJSON *jsonLogin, int socketCliente, FILE *arquivoTrechos){
    char dadosTrechos[256] = {0};
    pthread_mutex_lock(&trechosMutex);
    rewind(arquivoTrechos);
    cJSON *arrayResposta = cJSON_CreateArray();
    char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));

    while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivoTrechos) != NULL) {
        cJSON *trechosJson = cJSON_Parse(dadosTrechos);
        if (trechosJson != NULL) {
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
            char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
            float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

            if (cidadeOrigem != NULL && origemBuscada != NULL &&
                strcmp(cidadeOrigem, origemBuscada) == 0 && capacidade > 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                cJSON_AddStringToObject(item, "nomeMotorista", nomeMotorista);
                cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                cJSON_AddStringToObject(item, "destino", cidadeDestino);
                cJSON_AddNumberToObject(item, "capacidade", capacidade);
                cJSON_AddStringToObject(item, "data", data);
                cJSON_AddStringToObject(item, "hora", hora);
                cJSON_AddNumberToObject(item, "preco", preco);
                cJSON_AddItemToArray(arrayResposta, item);
            }
        }
        cJSON_Delete(trechosJson);
    }
    pthread_mutex_unlock(&trechosMutex);
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    send(socketCliente, resposta, strlen(resposta), 0);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

void tratarCliente(int socketCliente, cJSON *jsonLogin, char *acao){
    char dadosLogin[256] = {0};
    char dadosTrechos[256] = {0};
    int emailEncontrado = 0;
    FILE *arquivo = fopen("dados/loginCliente.json", "a+");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo");
        close(socketCliente);
        return;
    }
    rewind(arquivo);

    FILE *arquivoTrechos = fopen("trechosCadastrados/trechos.json", "r+");
    if (arquivoTrechos == NULL) {
        perror("Erro ao abrir o arquivo");
        close(socketCliente);
        fclose(arquivo);
        return;
    }
    
    if (strcmp(acao, "login") == 0) {
        loginCliente(jsonLogin, socketCliente, arquivo);
    } else if (strcmp(acao, "cadastro") == 0) {
        cadastrarCliente(jsonLogin, socketCliente, arquivo);
    } else if (strcmp(acao, "buscar_carona") == 0) {
        buscar_carona(jsonLogin, socketCliente, arquivoTrechos);
    } else if (strcmp(acao, "selecionar_carona") == 0) {
        selecionar_carona(jsonLogin, socketCliente);
    } else if (strcmp(acao, "combinar_trechos") == 0) {
        combinarTrechos(jsonLogin, socketCliente, arquivoTrechos);
    } else if (strcmp(acao, "selecionar_rota") == 0) {
        selecionar_Rota(jsonLogin, socketCliente);
    } else if (strcmp(acao, "finalizar_rota") == 0) {
        finalizar_Rota(jsonLogin, socketCliente);
    } else if (strcmp(acao, "listar_reservas") == 0) {
        listar_reservas(jsonLogin, socketCliente, arquivoTrechos);
    } else if (strcmp(acao, "cancelar_carona") == 0) {
        cancelar_carona(jsonLogin, socketCliente, arquivoTrechos);
    } else if (strcmp(acao, "buscar_trechos_partida") == 0) {
        buscar_trechos_partida(jsonLogin, socketCliente, arquivoTrechos);
    } else {
        send(socketCliente, "ACAO_DESCONHECIDA", 17, 0);
    }
    fclose(arquivo);
    fclose(arquivoTrechos);
    return;
}

void *rotinaTratamento(void *arg){
    int socket = *(int*)arg;
    free(arg);
    char buffer_mensagem [256] = {0};
    cJSON *json = NULL;
    while (1){
        ssize_t bytes_lidos = read(socket, buffer_mensagem, 255);

        if (bytes_lidos <= 0) {
            break;
        }
        buffer_mensagem[bytes_lidos] = '\0';

        if (strcmp(buffer_mensagem, "DESCONECTADO") == 0) {
            break;
        }
            
        json = cJSON_Parse(buffer_mensagem);
        if (json == NULL) {
            printf("Erro ao analisar JSON: %s\n", cJSON_GetErrorPtr());
            close(socket);
            cJSON_Delete(json);
            return NULL;
        }
        if (cJSON_GetStringValue(cJSON_GetObjectItem(json, "classe")) != NULL) {
            const char *classe = cJSON_GetStringValue(cJSON_GetObjectItem(json, "classe"));
            if (strcmp(classe, "Cliente") == 0) {
                tratarCliente(socket, json, cJSON_GetStringValue(cJSON_GetObjectItem(json, "acao")));
            } else if (strcmp(classe, "Motorista") == 0) {
                tratarMotorista(socket, json, cJSON_GetStringValue(cJSON_GetObjectItem(json, "acao")));
            } else {
                printf("Classe desconhecida: %s\n", classe);
                close(socket);
                cJSON_Delete(json);
                return NULL;
            }
        } else {
            printf("Campo 'classe' não encontrado no JSON.\n");
            close(socket);
            cJSON_Delete(json);
            return NULL;
        }
        cJSON_Delete(json);
        fflush(stdout);
        }
    
    printf("Dispositivo desconectado! Socket ID: %d\n", socket);
    fflush(stdout);
    close(socket);
    return NULL;
}

int main(){
    int socketServidor;
    int socketCliente;
    struct sockaddr_in endereco_servidor;
    struct sockaddr_in endereco_conexao;
    int limite_clientes = 10;
    socklen_t tamanho_endereco;
    int valor_opcao = 1;

    encontrarID();
    mapa = carregarGrafoDeArquivo("mapa.txt");
    
    if ((socketServidor = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("Arquivo socket nao criado");
        exit(EXIT_FAILURE);
    }

    int status = setsockopt(socketServidor, SOL_SOCKET,SO_REUSEADDR , &valor_opcao,sizeof(valor_opcao));

    if(status < 0){
        perror("Nao eh possivel colocar opcoes");
        exit(EXIT_FAILURE);
    }

    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);
    endereco_servidor.sin_addr.s_addr = INADDR_ANY;

    status = bind(socketServidor, (struct sockaddr*)&endereco_servidor, sizeof(struct sockaddr));

    if(status < 0){
        perror("Socket nao ligado");
        exit(EXIT_FAILURE);
    }

    status = listen(socketServidor, limite_clientes);

    if(status < 0){
        perror("Espera nao feita");
        exit(EXIT_FAILURE);
    }

    pthread_t threadLimpeza;
    if (pthread_create(&threadLimpeza, NULL, rotinaLimpezaTrechos, NULL) != 0) {
        perror("Erro ao criar thread de limpeza");
    } else {
        pthread_detach(threadLimpeza);
    }

    tamanho_endereco = sizeof(endereco_conexao);
    
    printf("==================================================\n");
    printf("        DISPOSITIVOS CONECTADOS NA PORTA %d\n", PORT);
    printf("==================================================\n");

    int i = 0;
    while (i<limite_clientes){
        socketCliente = accept(socketServidor, (struct sockaddr*)&endereco_conexao, &tamanho_endereco);
        if(socketCliente < 0){
            perror("Nao foi possível estabelecer a conexao com o cliente");
            continue;
        }
            
        printf("Novo Dispositivo conectado! Socket ID: %d\n", socketCliente);

        int *novo_sock = malloc(sizeof(int));
        *novo_sock = socketCliente;
        pthread_t threadID;
        if ((pthread_create(&threadID, NULL, rotinaTratamento, novo_sock)) != 0){
            perror("Erro ao criar thread para o cliente");
            free(novo_sock);
            close(socketCliente);
        } else {
            pthread_detach(threadID);
        }
        i++;
    }
    
    close(socketServidor);
    return 0;
}   