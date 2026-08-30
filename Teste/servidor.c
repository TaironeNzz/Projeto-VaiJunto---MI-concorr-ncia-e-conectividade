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

void salvarCadastro(char *cadastro){
    FILE *arquivo = fopen("dados/cadastro.json", "a");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo");
        return;
    }
    fprintf(arquivo, "%s\n", cadastro);
    fflush(arquivo);
    fclose(arquivo);
}

void *tratarCliente(void *arg){
    int socketCliente = *(int*)arg;
    free(arg);
    char buffer_mensagem [81] = {0};
    char *mensagem = "Recebi a mensagem cliente";

    ssize_t bytes_lidos = read(socketCliente, buffer_mensagem, 80);
    buffer_mensagem[bytes_lidos] ='\0';
    if (bytes_lidos > 0) {
        buffer_mensagem[bytes_lidos] = '\0';
        printf("Mensagem do cliente: %s\n", buffer_mensagem);
        fflush(stdout);
        salvarCadastro(buffer_mensagem);
    }
    send(socketCliente, mensagem, strlen(mensagem), 0);
    close(socketCliente);
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
        if ((pthread_create(&threadID, NULL, tratarCliente, novo_sock)) != 0){
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