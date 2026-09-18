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
//função para a requisição de cadastrar
void enviarCadastro(int socketCliente, char *nome, char *email, char *senha, int escolha){
    cJSON *enviar_dados = cJSON_CreateObject();
    //monta o cjson para a requisição
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
    //transforma a requisição em string
    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    //envia a requisição para o servidor
    if (mensagem != NULL) {
        write(socketCliente, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(enviar_dados);
}

//função para enviar uma requisição de cadastrar trecho
void cadastrarTrecho(int socketMotorista, Motorista *motorista) {
    char buffer_mensagem[256] = {0};
    char origem[50], destino[50];
    char data[11], hora[6];
    int capacidade;
    float preco;

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
    printf("Digite o preco do trecho: ");
    scanf("%f", &preco);

    cJSON *trecho = cJSON_CreateObject();
    cJSON *arrayClientes = cJSON_CreateArray();
    if (arrayClientes == NULL){
        printf("Array de clientes nao criado!\n");
        return;
    }

    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return;
    }
    //monta o cjson para a requisição
    cJSON_AddStringToObject(trecho, "classe", "Motorista");
    cJSON_AddStringToObject(trecho, "acao", "cadastrar_trecho");
    cJSON_AddStringToObject(trecho, "emailMotorista", email);
    cJSON_AddStringToObject(trecho, "origem", origem);
    cJSON_AddStringToObject(trecho, "destino", destino);
    cJSON_AddStringToObject(trecho, "data", data);
    cJSON_AddStringToObject(trecho, "hora", hora);
    cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
    cJSON_AddNumberToObject(trecho, "preco", preco);
    cJSON_AddStringToObject(trecho, "nome", motorista->nome);
    cJSON_AddItemToObject(trecho, "clientes", arrayClientes);

    char *mensagem = cJSON_PrintUnformatted(trecho);
    //envia a requisição
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

//função para cadastrar n trechos
void cadastrarTrechos(int socketMotorista, Motorista *motorista) {
    int quantidade;
    printf("Digite a quantidade de trechos que deseja cadastrar: ");
    scanf("%d", &quantidade);

    for (int i = 0; i < quantidade; i++) {
        printf("Cadastro do trecho %d:\n", i + 1);
        cadastrarTrecho(socketMotorista, motorista);
    }
}

//função para fazer uma requisição para listar os trechos do motorista
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

//função para a requisição de cancelar um trecho do motorista
void cancelarTrecho(int socketMotorista, Motorista *motorista){
    listarTrechos(socketMotorista, motorista);

    printf("Digite o ID do trecho que deseja cancelar (ou -1 para voltar): ");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    if (idSelecionado == -1) return;

    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return;
    }
    cJSON *cancelar = cJSON_CreateObject();
    cJSON_AddStringToObject(cancelar, "classe", "Motorista");
    cJSON_AddStringToObject(cancelar, "acao", "cancelar_trecho");
    cJSON_AddStringToObject(cancelar, "nome", motorista->nome);
    cJSON_AddStringToObject(cancelar, "email", email);
    cJSON_AddNumberToObject(cancelar, "idSelecionado", idSelecionado);
    char *mensagem = cJSON_PrintUnformatted(cancelar);
    if (mensagem != NULL) {
        write(socketMotorista, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(cancelar);

    char buffer_mensagem[32] = {0};
    int bytes = read(socketMotorista, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "TRECHO_CANCELADO") == 0) {
            printf("Trecho cancelado com sucesso!\n");
        } else {
            printf("Trecho nao encontrado.\n");
        }
    }
}

//requisição para cadastrar uma rota do motorista
void cadastrarRotaMotorista(int socketMotorista, Motorista *motorista){
    cJSON *arrayTrechos = cJSON_CreateArray();
    if (arrayTrechos == NULL) { printf("Erro ao criar objeto JSON\n"); return; }

    char destinoAnterior[50] = {0};
    int totalAdicionados = 0;
    int sair = 0;

    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return;
    }

    while (!sair) {
        char origem[50], destino[50];
        char data[11], hora[6];
        int capacidade;
        float preco;

        printf("====================================\n");
        printf("     Trecho %d da rota               \n", totalAdicionados + 1);
        printf("====================================\n");

        if (totalAdicionados == 0) {
            printf("Digite a origem do trecho: ");
            scanf(" %49[^\n]", origem);
        } else {
            strncpy(origem, destinoAnterior, sizeof(origem) - 1);
            origem[sizeof(origem) - 1] = '\0';
            printf("Origem (continuando do trecho anterior): %s\n", origem);
        }

        printf("Digite o destino do trecho: ");
        scanf(" %49[^\n]", destino);
        printf("Digite a data do trecho: ");
        scanf(" %10[^\n]", data);
        printf("Digite a hora do trecho: ");
        scanf(" %5[^\n]", hora);
        printf("Digite a capacidade do trecho: ");
        scanf("%d", &capacidade);
        printf("Digite o preco do trecho: ");
        scanf("%f", &preco);

        //monta o cjson do trecho para a requisição
        cJSON *trecho = cJSON_CreateObject();
        cJSON_AddStringToObject(trecho, "origem", origem);
        cJSON_AddStringToObject(trecho, "destino", destino);
        cJSON_AddStringToObject(trecho, "nome", motorista->nome);
        cJSON_AddStringToObject(trecho, "emailMotorista", email);
        cJSON_AddStringToObject(trecho, "data", data);
        cJSON_AddStringToObject(trecho, "hora", hora);
        cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
        cJSON_AddNumberToObject(trecho, "preco", preco);
        //adiciona o cjson no array cjson
        cJSON_AddItemToArray(arrayTrechos, trecho);

        strncpy(destinoAnterior, destino, sizeof(destinoAnterior) - 1);
        destinoAnterior[sizeof(destinoAnterior) - 1] = '\0';
        totalAdicionados++;

        printf("1- Adicionar mais um trecho a rota\n");
        printf("2- Finalizar e enviar a rota\n");
        printf("Escolha uma opcao: ");
        int escolha;
        scanf("%d", &escolha);
        if (escolha == 2) {
            sair = 1;
        } else if (escolha != 1) {
            printf("Digite uma opcao valida!\n");
        }
    }
    //criar um cjson final que contem o array de trechos da rota para a requisiçao
    cJSON *pedido = cJSON_CreateObject();
    cJSON_AddStringToObject(pedido, "classe", "Motorista");
    cJSON_AddStringToObject(pedido, "acao", "cadastrar_rota");
    cJSON_AddStringToObject(pedido, "nome", motorista->nome);
    cJSON_AddItemToObject(pedido, "trechos", arrayTrechos);
    //transforma em string
    char *mensagem = cJSON_PrintUnformatted(pedido);
    //envia a requisição para o servidor
    if (mensagem != NULL) {
        write(socketMotorista, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(pedido);

    char buffer_mensagem[50] = {0};
    int bytes = read(socketMotorista, buffer_mensagem, sizeof(buffer_mensagem) - 1);

    if (bytes <= 0) {
        printf("Erro: Conexao com o servidor perdida.\n");
        return;
    }

    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "ROTA_CADASTRADA") == 0) {
            printf("Rota cadastrada com sucesso!\n");
        } else if (strcmp(buffer_mensagem, "ROTA_DESCONECTADA") == 0) {
            printf("Os trechos nao formam uma rota conectada.\n");
        } else if (strcmp(buffer_mensagem, "FALHA_CADASTRO_ROTA") == 0) {
            printf("Um ou mais trechos nao tem caminho possivel no mapa.\n");
        } else {
            printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
        }
    }
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
        printf(" 4- Cancelar Trecho\n");
        printf(" 5- Cadastrar Rota (varios trechos conectados)\n");
        printf(" 6- Voltar\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            cadastrarTrecho(socketMotorista, motorista);
        } else if (escolha == 2) {
            cadastrarTrechos(socketMotorista, motorista);
        } else if (escolha == 3) {
            listarTrechos(socketMotorista, motorista);
        } else if (escolha == 4) {
            cancelarTrecho(socketMotorista, motorista);
        } else if (escolha == 5) {
            cadastrarRotaMotorista(socketMotorista, motorista);
        } else if (escolha == 6) {
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
    int c = 0;
    while (( c = getchar()) != '\n' && c != EOF);

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
                strncpy(motorista->nome, nome, sizeof(motorista->nome) - 1);
                motorista->nome[sizeof(motorista->nome) - 1] = '\0';
                telaMenu(socketMotorista, motorista);
            }
            cJSON_Delete(respostaJSON);
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
    //cria o socket do motorista
    if ((socketMotorista = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("Socket nao criado");
        free(motorista);
        exit(EXIT_FAILURE);
    }
    //relaciona o endereço
    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);

    char ip_servidor[100];
    
    printf("Digite o IP do servidor: ");
    scanf("%99s", ip_servidor);

    struct hostent *host = gethostbyname(ip_servidor);
    if (host == NULL) {
        perror("Erro ao resolver nome do host");
        free(motorista);
        close(socketMotorista);
        exit(EXIT_FAILURE);
    }

    memcpy(&endereco_servidor.sin_addr, host->h_addr_list[0], host->h_length);
    //conecta ao servidor
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