#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "cJSON.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

#define PORT 65432

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

void tratarMotorista(int socketMotorista, char *acao){
    char buffer_mensagem [150] = {0};
    char *mensagem = "Recebi a mensagem motorista";

    ssize_t bytes_lidos = read(socketMotorista, buffer_mensagem, 149);
    buffer_mensagem[bytes_lidos] ='\0';
    if (bytes_lidos > 0) {
        buffer_mensagem[bytes_lidos] = '\0';
        printf("Mensagem do motorista: %s\n", buffer_mensagem);
        fflush(stdout);
        salvarLoginMotorista(buffer_mensagem);
    }
    send(socketMotorista, mensagem, strlen(mensagem), 0);
    return;
}

void tratarCliente(int socketCliente, cJSON *jsonLogin, char *acao){
    char dadosLogin[256] = {0};
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
    } else if (strcmp(acao, "cadastro") == 0) {
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
    } else {
        send(socketCliente, "ACAO_DESCONHECIDA", 18, 0);
    }
    fclose(arquivo);
    return;
}

void *rotinaTratamento(void *arg){
    int socket = *(int*)arg;
    free(arg);
    char buffer_mensagem [150] = {0};
    cJSON *json = NULL;
    while (strcmp(buffer_mensagem, "DESCONECTADO") != 0){
        ssize_t bytes_lidos = read(socket, buffer_mensagem, 149);
        buffer_mensagem[bytes_lidos] ='\0';
        if (bytes_lidos > 0) {
            buffer_mensagem[bytes_lidos] = '\0';
            json = cJSON_Parse(buffer_mensagem);
            if (json == NULL) {
                printf("Erro ao analisar JSON: %s\n", cJSON_GetErrorPtr());
                close(socket);
                return NULL;
            }
            if (cJSON_GetStringValue(cJSON_GetObjectItem(json, "classe")) != NULL) {
                const char *classe = cJSON_GetStringValue(cJSON_GetObjectItem(json, "classe"));
                if (strcmp(classe, "Cliente") == 0) {
                    tratarCliente(socket, json, cJSON_GetStringValue(cJSON_GetObjectItem(json, "acao")));
                } else if (strcmp(classe, "Motorista") == 0) {
                    tratarMotorista(socket, cJSON_GetStringValue(cJSON_GetObjectItem(json, "acao")));
                } else {
                    printf("Classe desconhecida: %s\n", classe);
                    close(socket);
                }
            } else {
                printf("Campo 'classe' não encontrado no JSON.\n");
                close(socket);
            }
            cJSON_Delete(json);
            fflush(stdout);
        }
    }
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

    tamanho_endereco = sizeof(endereco_conexao);
    int i = 0;
    while (i<limite_clientes){
        socketCliente = accept(socketServidor, (struct sockaddr*)&endereco_conexao, &tamanho_endereco);
        if(socketCliente < 0){
            perror("Nao foi possível estabelecer a conexao com o cliente");
            continue;
        }
            
        printf("Novo cliente conectado! Socket ID: %d\n", socketCliente);

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