#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include "formatos.h"

#define PORT 65432

void enviarCadastro(int socketCliente, char *nome, char *email, char *senha, int escolha){
    cJSON *enviar_dados = cJSON_CreateObject();
    
    cJSON_AddStringToObject(enviar_dados, "classe", "Cliente");
    cJSON_AddStringToObject(enviar_dados, "nome", nome ? nome : "");
    cJSON_AddStringToObject(enviar_dados, "email", email ? email : "");
    cJSON_AddStringToObject(enviar_dados, "senha", senha ? senha : "");
    cJSON_AddStringToObject(enviar_dados, "status", "");
    
    if (escolha == 1) {
        cJSON_AddStringToObject(enviar_dados, "acao", "login");
    } else if (escolha == 2) {
        cJSON_AddStringToObject(enviar_dados, "acao", "cadastro");
    }

    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    
    if (mensagem != NULL) {
        write(socketCliente, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(enviar_dados);
}

void telaLogin(int socketCliente, Cliente *cliente){
    char buffer_mensagem[18] = {0};
    int escolha = 0;
    int sair = 0;
    while(sair != 1){
        printf("====================================\n");
        printf("                Login               \n");
        printf("====================================\n");
        printf(" 1- Email: %s\n", cliente->email);
        printf(" 2- Senha: %s\n", cliente->senha);
        printf(" 3- Enviar Login                    \n");
        printf(" 4- Voltar                          \n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            printf("Digite seu email: ");
            scanf(" %49[^\n]", cliente->email);
        } else if (escolha == 2) {
            printf("Digite sua senha: ");
            scanf(" %49[^\n]", cliente->senha);
        } else if (escolha == 3) {
            enviarCadastro(socketCliente, cliente->nome, cliente->email, cliente->senha, 1);
            break;
        } else if (escolha == 4) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }
    int bytes = read(socketCliente, buffer_mensagem, 17);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "NAO_AUTENTICADO") == 0) {
            printf("Email ou senha incorretos. Tente novamente.\n");
            telaLogin(socketCliente, cliente);
        } else if (strcmp(buffer_mensagem, "AUTENTICADO") == 0) {
            printf("Login realizado com sucesso!\n");
            cliente->status = AUTENTICADO;
        } else {
            printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
        }
    }
}

void telaCadastro(int socketCliente, Cliente *cliente){
    int escolha = 0;
    char buffer_mensagem[20] = {0};
    int sair = 0;
    while(sair != 1){
        printf("====================================\n");
        printf("              Cadastro              \n");
        printf("====================================\n");
        printf(" 1- Nome: %s\n", cliente->nome);
        printf(" 2- Email: %s\n", cliente->email);
        printf(" 3- Senha: %s\n", cliente->senha);
        printf(" 4- Enviar Cadastro                 \n");
        printf(" 5- Voltar                          \n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            printf("Digite seu nome: ");
            scanf(" %49[^\n]", cliente->nome);
        } else if (escolha == 2) {
            printf("Digite seu email: ");
            scanf(" %49[^\n]", cliente->email);
        } else if (escolha == 3) {
            printf("Digite sua senha: ");
            scanf(" %49[^\n]", cliente->senha);
        } else if (escolha == 4) {
            enviarCadastro(socketCliente, cliente->nome, cliente->email, cliente->senha, 2);
            break;
        } else if (escolha == 5) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

    ssize_t bytes = read(socketCliente, buffer_mensagem, 19);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "EMAIL_JA_CADASTRADO") == 0) {
            printf("Email ja cadastrado. Tente novamente.\n");
            telaCadastro(socketCliente, cliente);
        } else if (strcmp(buffer_mensagem, "CADASTRO_REALIZADO") == 0) {
            printf("Cadastro realizado com sucesso!\n");
            cliente->status = AUTENTICADO;
        } else {
            printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
        }
    }
}

void telaInicial(int socketCliente, Cliente *cliente){
    int sair = 0;
    while (!sair) {
        int opcao = 0;
        printf("====================================\n");
        printf("          Sistema de Login          \n");
        printf("====================================\n");
        printf("| 1- LOGIN                         |\n");
        printf("| 2- CADASTRAR                     |\n");
        printf("| 3- SAIR                          |\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &opcao);

        switch (opcao) {
            case 1:
                telaLogin(socketCliente, cliente);
                break;
            case 2:
                telaCadastro(socketCliente, cliente);
                break;
            case 3:
                printf("Saindo...\n");
                send(socketCliente, "DESCONECTADO", 13, 0);
                cliente->status = DESCONECTADO;
                sair = 1;
                break;
            default:
                printf("Opcao invalida. Tente novamente.\n");
                break;
        }
    }
}

int main(){
    int socketCliente;
    struct sockaddr_in endereco_servidor;
    char buffer_mensagem[81] = {0};
    
    Cliente *cliente = calloc(1, sizeof(Cliente));

    if ((socketCliente = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("Socket nao criado");
        free(cliente);
        exit(EXIT_FAILURE);
    }

    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);

    struct hostent *host = gethostbyname("localhost");
    if (host == NULL) {
        perror("Erro ao resolver nome do host 'localhost'");
        free(cliente);
        close(socketCliente);
        exit(EXIT_FAILURE);
    }

    memcpy(&endereco_servidor.sin_addr, host->h_addr_list[0], host->h_length);

    int status = connect(socketCliente, (struct sockaddr*)&endereco_servidor, sizeof(endereco_servidor));

    if (status < 0){
        perror("conexao com o servidor nao estabelecida");
        free(cliente);
        close(socketCliente);
        exit(EXIT_FAILURE);
    }

    telaInicial(socketCliente, cliente);
    
    ssize_t bytes = read(socketCliente, buffer_mensagem, 80);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        printf("Mensagem do servidor: %s\n", buffer_mensagem);
    }
    
    free(cliente);
    close(socketCliente);
    return 0;
}