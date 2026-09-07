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
    
    cJSON_AddStringToObject(enviar_dados, "classe", "Motorista");
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

void cadastrarTrecho(int socketMotorista, Motorista *motorista) {
    char buffer_mensagem[256] = {0};
    char origem[50], destino[50];
    char data[11], hora[6];
    int capacidade;

    printf("Digite a origem do trecho: ");
    scanf(" %49[^\n]", origem);
    printf("Digite o destino do trecho: ");
    scanf(" %49[^\n]", destino);
    printf("Digite a data do trecho: ");
    scanf(" %10[^\n]", data);
    printf("Digite a hora do trecho: ");
    scanf(" %5[^\n]", hora);
    printf("Digite a capacidade do trecho: ");
    scanf("%d", &capacidade);

    cJSON *trecho = cJSON_CreateObject();
    cJSON_AddStringToObject(trecho, "classe", "Motorista");
    cJSON_AddStringToObject(trecho, "acao", "cadastrar_trecho");
    cJSON_AddStringToObject(trecho, "origem", origem);
    cJSON_AddStringToObject(trecho, "destino", destino);
    cJSON_AddStringToObject(trecho, "data", data);
    cJSON_AddStringToObject(trecho, "hora", hora);
    cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
    cJSON_AddStringToObject(trecho, "nome", motorista->nome);

    char *mensagem = cJSON_PrintUnformatted(trecho);
    
    if (mensagem != NULL) {
        write(socketMotorista, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(trecho);

    ssize_t bytes = read(socketMotorista, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "TRECHO_CADASTRADO") == 0) {
            printf("Trecho cadastrado com sucesso!\n");
        } else {
            printf("Falha ao cadastrar trecho, caminho possivel nao encontrado\n");
        }
    }
}

void listarTrechos(int socketMotorista, Motorista *motorista){
    char buffer_mensagem[4096] = {0};
    int total = 0;

    cJSON *enviar_dados = cJSON_CreateObject();
    cJSON_AddStringToObject(enviar_dados, "classe", "Motorista");
    cJSON_AddStringToObject(enviar_dados, "nome", motorista->nome);
    cJSON_AddStringToObject(enviar_dados, "email", motorista->email);
    cJSON_AddStringToObject(enviar_dados, "senha", motorista->senha);
    cJSON_AddStringToObject(enviar_dados, "acao", "listar_trechos");

    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    if (mensagem != NULL) {
        write(socketMotorista, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(enviar_dados);

    printf("====================================\n");
    printf("          Meus Trechos              \n");

    cJSON *arrayResposta = NULL;
    while (arrayResposta == NULL && total < (int)sizeof(buffer_mensagem) - 1) {
        ssize_t bytes = read(socketMotorista, buffer_mensagem + total, sizeof(buffer_mensagem) - 1 - total);
        if (bytes <= 0) break;
        total += bytes;
        buffer_mensagem[total] = '\0';
        arrayResposta = cJSON_Parse(buffer_mensagem);
    }

    if (arrayResposta == NULL) {
        printf("Erro ao obter lista de trechos.\n");
        return;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        printf("Nenhum trecho cadastrado\n");
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
        printf("====================================\n");
        printf("ID: %d\n", id);
        printf("Origem: %s\n", cidadeOrigem);
        printf("Destino: %s\n", cidadeDestino);
        printf("Data: %s\n", data);
        printf("Hora: %s\n", hora);
        printf("Capacidade: %d\n", capacidade);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);
}

void telaMenu(int socketMotorista, Motorista *motorista){
    char buffer_mensagem[18] = {0};
    int escolha = 0;
    int sair = 0;
    int enviou = 0;

    while(sair != 1){
        printf("====================================\n");
        printf("                MENU                \n");
        printf("====================================\n");
        printf(" 1- Cadastrar um Trecho\n");
        printf(" 2- Cadastrar Trechos\n");
        printf(" 3- Ver meus Trechos\n");
        printf(" 4- Sair da Conta\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            cadastrarTrecho(socketMotorista, motorista);
        } else if (escolha == 2) {
            printf("Digite sua senha: ");
            scanf(" %49[^\n]", motorista->senha);
        } else if (escolha == 3) {
            listarTrechos(socketMotorista, motorista);
        } else if (escolha == 4) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

}

void telaLogin(int socketMotorista, Motorista *motorista){
    char buffer_mensagem[50] = {0};
    int escolha = 0;
    int sair = 0;
    int enviou = 0;

    while(sair != 1){
        printf("====================================\n");
        printf("                Login               \n");
        printf("====================================\n");
        printf(" 1- Email: %s\n", motorista->email);
        printf(" 2- Senha: %s\n", motorista->senha);
        printf(" 3- Enviar Login                    \n");
        printf(" 4- Voltar                          \n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            printf("Digite seu email: ");
            scanf(" %49[^\n]", motorista->email);
        } else if (escolha == 2) {
            printf("Digite sua senha: ");
            scanf(" %49[^\n]", motorista->senha);
        } else if (escolha == 3) {
            enviarCadastro(socketMotorista, motorista->nome, motorista->email, motorista->senha, 1);
            enviou = 1;
            break;
        } else if (escolha == 4) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

    if (!enviou) {
        return;
    }

    int bytes = read(socketMotorista, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "NAO_AUTENTICADO") == 0) {
            printf("Email ou senha incorretos. Tente novamente.\n");
            telaLogin(socketMotorista, motorista);
            return;
        }
        cJSON *respostaJSON = cJSON_Parse(buffer_mensagem);
        if (respostaJSON != NULL){
            char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(respostaJSON, "nome"));
            if (nome != NULL){
                printf("Login realizado com sucesso!\n");
                motorista->status = AUTENTICADO;
                strncpy(motorista->nome, nome, sizeof(nome) - 1);
                motorista->nome[sizeof(nome) - 1] = '\0';
                telaMenu(socketMotorista, motorista);
            }
        }
    }
}

void telaCadastro(int socketMotorista, Motorista *motorista){
    int escolha = 0;
    char buffer_mensagem[20] = {0};
    int sair = 0;
    int enviou = 0;
    while(sair != 1){
        printf("====================================\n");
        printf("              Cadastro              \n");
        printf("====================================\n");
        printf(" 1- Nome: %s\n", motorista->nome);
        printf(" 2- Email: %s\n", motorista->email);
        printf(" 3- Senha: %s\n", motorista->senha);
        printf(" 4- Enviar Cadastro                 \n");
        printf(" 5- Voltar                          \n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            printf("Digite seu nome: ");
            scanf(" %49[^\n]", motorista->nome);
        } else if (escolha == 2) {
            printf("Digite seu email: ");
            scanf(" %49[^\n]", motorista->email);
        } else if (escolha == 3) {
            printf("Digite sua senha: ");
            scanf(" %49[^\n]", motorista->senha);
        } else if (escolha == 4) {
            enviarCadastro(socketMotorista, motorista->nome, motorista->email, motorista->senha, 2);
            enviou = 1;
            break;
        } else if (escolha == 5) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

    if (!enviou) {
        return;
    }

    ssize_t bytes = read(socketMotorista, buffer_mensagem, 20);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "EMAIL_JA_CADASTRADO") == 0) {
            printf("Email ja cadastrado. Tente novamente.\n");
            telaCadastro(socketMotorista, motorista);
        } else if (strcmp(buffer_mensagem, "CADASTRO_REALIZADO") == 0) {
            printf("Cadastro realizado com sucesso!\n");
            motorista->status = AUTENTICADO;
        } else {
            printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
        }
    }
}

void telaInicial(int socketMotorista, Motorista *motorista){
    int sair = 0;
    while (!sair) {
        int opcao = 0;
        printf("====================================\n");
        printf("     Sistema de Login motorista     \n");
        printf("====================================\n");
        printf("| 1- LOGIN                         |\n");
        printf("| 2- CADASTRAR                     |\n");
        printf("| 3- SAIR                          |\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &opcao);

        switch (opcao) {
            case 1:
                telaLogin(socketMotorista, motorista);
                break;
            case 2:
                telaCadastro(socketMotorista, motorista);
                break;
            case 3:
                printf("Saindo...\n");
                send(socketMotorista, "DESCONECTADO", 13, 0);
                motorista->status = DESCONECTADO;
                sair = 1;
                break;
            default:
                printf("Opcao invalida. Tente novamente.\n");
                break;
        }
    }
}

int main(){
    int socketMotorista;
    struct sockaddr_in endereco_servidor;
    char buffer_mensagem[81] = {0};
    
    Motorista *motorista = calloc(1, sizeof(Motorista));

    if ((socketMotorista = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("Socket nao criado");
        free(motorista);
        exit(EXIT_FAILURE);
    }

    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);

    struct hostent *host = gethostbyname("localhost");
    if (host == NULL) {
        perror("Erro ao resolver nome do host 'localhost'");
        free(motorista);
        close(socketMotorista);
        exit(EXIT_FAILURE);
    }

    memcpy(&endereco_servidor.sin_addr, host->h_addr_list[0], host->h_length);

    int status = connect(socketMotorista, (struct sockaddr*)&endereco_servidor, sizeof(endereco_servidor));

    if (status < 0){
        perror("conexao com o servidor nao estabelecida");
        free(motorista);
        close(socketMotorista);
        exit(EXIT_FAILURE);
    }

    telaInicial(socketMotorista, motorista);
    
    ssize_t bytes = read(socketMotorista, buffer_mensagem, 80);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        printf("Mensagem do servidor: %s\n", buffer_mensagem);
    }
    
    free(motorista);
    close(socketMotorista);
    return 0;
}