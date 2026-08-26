#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#define PORT 65432

int main(){
    int socketCliente;
    struct sockaddr_in endereco_servidor;
    char *mensagem = "Ola servidor";
    char mensagem2[50];
    char buffer_mensagem[80] = {0};

    if ((socketCliente = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("Socket nao criado");
        exit(EXIT_FAILURE);
    }

    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);
    

    /*
    if (inet_pton(AF_INET, "192.168.1.15", &endereco_servidor.sin_addr) <= 0) {
    perror("Endereco IP invalido ou nao encontrado");
    exit(EXIT_FAILURE);
    }*/

    struct hostent *host = gethostbyname("servidor");
    if (host == NULL) {
    perror("Erro ao resolver nome do host 'servidor'");
    exit(EXIT_FAILURE);
    }

    memcpy(&endereco_servidor.sin_addr, host->h_addr_list[0], host->h_length);

    int status = connect(socketCliente, (struct sockaddr*)&endereco_servidor, sizeof(endereco_servidor));

    if (status < 0){
        perror("conexao com o servidor nao estabelecida");
        exit(EXIT_FAILURE);
    }

    fgets(mensagem2, sizeof(mensagem2), stdin);
    write(socketCliente, mensagem, strlen(mensagem));
    read(socketCliente, buffer_mensagem, 80);
    printf("Messagem do servidor: %s\n", buffer_mensagem);
    close(socketCliente);
    return 0;
}