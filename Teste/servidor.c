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

Grafo *mapa;
int idTrecho = 0;
pthread_mutex_t trechosMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t loginMotoristaMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t loginClienteMutex = PTHREAD_MUTEX_INITIALIZER;
#define PORT 65432

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

void salvarLoginCliente(char *login){
    FILE *arquivo = fopen("dados/loginCliente.json", "a");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo");
        return;
    }
    fprintf(arquivo, "%s\n", login);
    fflush(arquivo);
    fclose(arquivo);
}

void salvarLoginMotorista(char *login){
    FILE *arquivo = fopen("dados/loginMotorista.json", "a");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo");
        return;
    }
    fprintf(arquivo, "%s\n", login);
    fflush(arquivo);
    fclose(arquivo);
}

int cadastrarTrecho(char *nomeMotorista,char *origem, char *destino, char *data, char *hora, int capacidade) {
    if (mapa == NULL) {
        printf("Mapa nao carregado. Nao e possivel cadastrar trecho.\n");
        return 0;
    }
    if (existeCaminhoBFSPorNome(mapa, origem, destino)) {
        pthread_mutex_lock(&trechosMutex);
        FILE *arquivo = fopen("trechosCadastrados/trechos.json", "a");
        if (arquivo == NULL) {
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
        char *saida = cJSON_PrintUnformatted(trecho);
        fprintf(arquivo, "%s\n", saida);
        free(saida);
        cJSON_Delete(trecho);
        fclose(arquivo);
        idTrecho++;
        pthread_mutex_unlock(&trechosMutex);
        return 1;
    } else {
        return 0;
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
            send(socketMotorista, "NAO_AUTENTICADO", 17, 0);
        }
        pthread_mutex_unlock(&loginMotoristaMutex);
    } else if (strcmp(acao, "cadastro") == 0) {
        pthread_mutex_lock(&loginMotoristaMutex);
        while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
            cJSON *dadosJson = cJSON_Parse(dadosLogin);
            if(dadosJson != NULL) {
                char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
                if (strcmp(email, emailBuscado) == 0) {
                    send(socketMotorista, "EMAIL_JA_CADASTRADO", 20, 0);
                    emailEncontrado = 1;
                    break;
                }
            }
            cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            char *saida = cJSON_PrintUnformatted(jsonLogin);
            salvarLoginMotorista(saida);
            send(socketMotorista, "CADASTRO_REALIZADO", 18, 0);
            free(saida);
        }
        pthread_mutex_unlock(&loginMotoristaMutex);
    } else if (strcmp(acao, "cadastrar_trecho") == 0) {
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "hora"));

        if (mapa != NULL) {
            int caminhoEncontrado = cadastrarTrecho(nomeMotorista,origem, destino, data, hora, capacidade);
            if (caminhoEncontrado) {
                send(socketMotorista, "TRECHO_CADASTRADO", 18, 0);
            } else {
                send(socketMotorista, "FALHA_CADASTRO_TRECHO", 22, 0);
            }
        } else {
            send(socketMotorista, "MAPA_NAO_CARREGADO", 20, 0);
        }
    } else if (strcmp(acao, "listar_trechos") == 0) {
        pthread_mutex_lock(&trechosMutex);
        cJSON *arrayResposta = cJSON_CreateArray();

        while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivo2) != NULL) {
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

                if (nomeMotoristaTrecho != NULL && nomeMotorista != NULL &&
                    strcmp(nomeMotorista, nomeMotoristaTrecho) == 0) {
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddNumberToObject(item, "id", id);
                    cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                    cJSON_AddStringToObject(item, "destino", cidadeDestino);
                    cJSON_AddNumberToObject(item, "capacidade", capacidade);
                    cJSON_AddStringToObject(item, "data", data);
                    cJSON_AddStringToObject(item, "hora", hora);
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
    } else {
        send(socketMotorista, "ACAO_DESCONHECIDA", 18, 0);
    }
    fclose(arquivo);
    fclose(arquivo2);
    return;
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

    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));
    
    if (strcmp(acao, "login") == 0) {
        pthread_mutex_lock(&loginClienteMutex);
        while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
            cJSON *dadosJson = cJSON_Parse(dadosLogin);
            if(dadosJson != NULL) {
                char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
                char *senha = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "senha"));
                if (strcmp(email, emailBuscado) == 0 && strcmp(senha, senhaBuscada) == 0) {
                    send(socketCliente, "AUTENTICADO", 12, 0);
                    emailEncontrado = 1;
                    break;
                }
            }
            cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            send(socketCliente, "NAO_AUTENTICADO", 17, 0);
        }
        pthread_mutex_unlock(&loginClienteMutex);
    } else if (strcmp(acao, "cadastro") == 0) {
        pthread_mutex_lock(&loginClienteMutex);
        while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
            cJSON *dadosJson = cJSON_Parse(dadosLogin);
            if(dadosJson != NULL) {
                char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
                if (strcmp(email, emailBuscado) == 0) {
                    send(socketCliente, "EMAIL_JA_CADASTRADO", 20, 0);
                    emailEncontrado = 1;
                    break;
                }
            }
            cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            char *saida = cJSON_PrintUnformatted(jsonLogin);
            salvarLoginCliente(saida);
            send(socketCliente, "CADASTRO_REALIZADO", 18, 0);
            free(saida);
        }
        pthread_mutex_unlock(&loginClienteMutex);
    } else if (strcmp(acao, "buscar_carona") == 0) {
        pthread_mutex_lock(&trechosMutex);
        FILE *arquivo2 = fopen("trechosCadastrados/trechos.json", "a+");
        if (arquivo2 == NULL) {
            perror("Erro ao abrir o arquivo");
            pthread_mutex_unlock(&trechosMutex);
            fclose(arquivo);
            return;
        }
        rewind(arquivo2);
        cJSON *arrayResposta = cJSON_CreateArray();
        char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
        char *destinoBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));

        while (fgets(dadosTrechos, sizeof(dadosTrechos), arquivo2) != NULL) {
            cJSON *trechosJson = cJSON_Parse(dadosTrechos);
            if (trechosJson != NULL) {
                int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
                char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
                char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
                char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
                int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
                char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
                char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));

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
                    cJSON_AddItemToArray(arrayResposta, item);
                }
            }
            cJSON_Delete(trechosJson);
        }
        fclose(arquivo2);
        pthread_mutex_unlock(&trechosMutex);
        char *resposta = cJSON_PrintUnformatted(arrayResposta);
        send(socketCliente, resposta, strlen(resposta), 0);
        free(resposta);
        cJSON_Delete(arrayResposta);
    } else {
        send(socketCliente, "ACAO_DESCONHECIDA", 18, 0);
    }
    fclose(arquivo);
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