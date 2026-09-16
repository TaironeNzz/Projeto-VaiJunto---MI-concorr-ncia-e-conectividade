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

void criarRota(int socketCliente, Cliente *cliente, char *origem, char *destino, char *data, char *hora) {
    char buffer_mensagem[1024] = {0};
    cJSON *arrayRotas = cJSON_CreateArray();
    if (arrayRotas == NULL) { printf("Erro ao criar objeto JSON\n"); return; }

    char origemAtual[50];
    strncpy(origemAtual, origem, sizeof(origemAtual) - 1);
    origemAtual[sizeof(origemAtual) - 1] = '\0';

    int sair = 0;
    while (!sair) {
        int total = 0;
        memset(buffer_mensagem, 0, sizeof(buffer_mensagem));

        cJSON *pedido = cJSON_CreateObject();
        cJSON_AddStringToObject(pedido, "classe", "Cliente");
        cJSON_AddStringToObject(pedido, "acao", "buscar_trechos_partida");
        cJSON_AddStringToObject(pedido, "origem", origemAtual);
        char *mensagem = cJSON_PrintUnformatted(pedido);
        if (mensagem != NULL) { write(socketCliente, mensagem, strlen(mensagem)); free(mensagem); }
        cJSON_Delete(pedido);

        printf("=============================================\n");
        printf("CARONAS DISPONIVEIS PARTINDO DE %s\n", origemAtual);
        printf("=============================================\n");

        cJSON *arrayResposta = NULL;
        while (arrayResposta == NULL && total < (int)sizeof(buffer_mensagem) - 1) {
            ssize_t bytes = read(socketCliente, buffer_mensagem + total, sizeof(buffer_mensagem) - 1 - total);
            if (bytes <= 0) break;
            total += bytes;
            buffer_mensagem[total] = '\0';
            arrayResposta = cJSON_Parse(buffer_mensagem);
        }
        if (arrayResposta == NULL) { printf("Erro ao obter lista de trechos.\n"); cJSON_Delete(arrayRotas); return; }

        int n = cJSON_GetArraySize(arrayResposta);
        if (n == 0) {
            printf("NENHUMA CARONA ENCONTRADA PARTINDO DE %s\n", origemAtual);
            cJSON_Delete(arrayResposta); cJSON_Delete(arrayRotas);
            return;
        }
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
            int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
            char *cOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
            char *cDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
            int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
            char *dItem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
            char *hItem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
            char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
            float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "preco"));
            printf("=============================================\n");
            printf("ID: %d | Motorista: %s\n", id, nomeMotorista);
            printf("Origem: %s -> Destino: %s\n", cOrigem, cDestino);
            printf("Data: %s Hora: %s Capacidade: %d Preco: %.2f\n", dItem, hItem, capacidade, preco);
        }
        printf("=============================================\n");
        cJSON_Delete(arrayResposta);

        printf("Digite o ID do trecho para adicionar a rota (ou 00 para cancelar): ");
        int idSelecionado;
        scanf("%d", &idSelecionado);
        if (idSelecionado == 0) { cJSON_Delete(arrayRotas); return; }

        char origemEscolhida[50], destinoEscolhido[50];
        printf("Confirme a origem exata do trecho escolhido: ");
        scanf(" %49[^\n]", origemEscolhida);
        printf("Confirme o destino exato do trecho escolhido: ");
        scanf(" %49[^\n]", destinoEscolhido);

        cJSON *respostaSelecionada = cJSON_CreateObject();
        cJSON_AddStringToObject(respostaSelecionada, "classe", "Cliente");
        cJSON_AddStringToObject(respostaSelecionada, "acao", "selecionar_rota");
        cJSON_AddNumberToObject(respostaSelecionada, "idSelecionado", idSelecionado);
        cJSON_AddStringToObject(respostaSelecionada, "emailCliente", cliente->email);
        cJSON_AddStringToObject(respostaSelecionada, "origem", origemEscolhida);
        cJSON_AddStringToObject(respostaSelecionada, "destino", destinoEscolhido);
        char *mensagemSel = cJSON_PrintUnformatted(respostaSelecionada);
        if (mensagemSel != NULL) { write(socketCliente, mensagemSel, strlen(mensagemSel)); free(mensagemSel); }
        cJSON_Delete(respostaSelecionada);

        memset(buffer_mensagem, 0, sizeof(buffer_mensagem));
        int bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
        if (bytes > 0) {
            buffer_mensagem[bytes] = '\0';
            cJSON *respostaServidor = cJSON_Parse(buffer_mensagem);
            if (respostaServidor != NULL) {
                cJSON_AddItemToArray(arrayRotas, respostaServidor);
                strncpy(origemAtual, destinoEscolhido, sizeof(origemAtual) - 1);
                origemAtual[sizeof(origemAtual) - 1] = '\0';
                if (strcmp(origemAtual, destino) == 0) {
                    printf("Voce chegou ao destino final! Finalizando rota...\n");
                    sair = 1;
                } else {
                    printf("Trecho adicionado! Continuando de %s...\n", origemAtual);
                }
            } else if (strcmp(buffer_mensagem, "ASSENTO_INDISPONIVEL") == 0) {
                printf("Assento indisponivel. Escolha outra carona.\n");
            } else {
                printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
            }
        }
    }

    cJSON *respostaFinalizar = cJSON_CreateObject();
    cJSON_AddStringToObject(respostaFinalizar, "classe", "Cliente");
    cJSON_AddStringToObject(respostaFinalizar, "acao", "finalizar_rota");
    cJSON_AddItemToObject(respostaFinalizar, "rota", arrayRotas);
    cJSON_AddStringToObject(respostaFinalizar, "origemRota", origem);
    cJSON_AddStringToObject(respostaFinalizar, "destinoRota", destino);
    char *mensagemFinal = cJSON_PrintUnformatted(respostaFinalizar);
    if (mensagemFinal != NULL) { write(socketCliente, mensagemFinal, strlen(mensagemFinal)); free(mensagemFinal); }
    cJSON_Delete(respostaFinalizar);

    int bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        printf(strcmp(buffer_mensagem, "CARONA_CADASTRADA") == 0 ? "Rota cadastrada com sucesso!\n" : "Falha ao finalizar a rota.\n");
    }
}

void buscarCarona(int socketCliente, Cliente *cliente) {
    char buffer_mensagem[1024] = {0};
    char origem[50], destino[50];
    char data[11], hora[6];
    int total = 0;

    printf("Digite a origem da carona: ");
    scanf(" %49[^\n]", origem);
    printf("Digite o destino da carona: ");
    scanf(" %49[^\n]", destino);
    printf("Digite a data da carona: ");
    scanf(" %10[^\n]", data);
    printf("Digite a hora da carona: ");
    scanf(" %5[^\n]", hora);

    cJSON *carona = cJSON_CreateObject();
    cJSON_AddStringToObject(carona, "classe", "Cliente");
    cJSON_AddStringToObject(carona, "acao", "buscar_carona");
    cJSON_AddStringToObject(carona, "origem", origem);
    cJSON_AddStringToObject(carona, "destino", destino);
    cJSON_AddStringToObject(carona, "data", data);
    cJSON_AddStringToObject(carona, "hora", hora);

    char *mensagem = cJSON_PrintUnformatted(carona);
    if (mensagem != NULL) {
        write(socketCliente, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(carona);

    printf("====================================\n");
    printf("         Caronas encontradas        \n");
    printf("====================================\n");

    cJSON *arrayResposta = NULL;
    while (arrayResposta == NULL && total < (int)sizeof(buffer_mensagem) - 1) {
        ssize_t bytes = read(socketCliente, buffer_mensagem + total, sizeof(buffer_mensagem) - 1 - total);
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
        printf("CARONAS DIRETAS NAO ENCONTRADAS\n");
        int sair = 0;
        while(!sair){
            printf("================================================\n");
            printf("DESEJA CRIAR UMA ROTA COM MOTORISTAS DIFERENTES?\n");
            printf("================================================\n");
            printf(" 1- SIM\n");
            printf(" 2- NAO\n");
            printf("====================================\n");
            printf("Escolha uma opcao: ");
            int escolha;
            scanf("%d", &escolha);
            if (escolha == 1) {
                criarRota(socketCliente, cliente, origem, destino, data, hora);
                sair = 1;
            } else if (escolha == 2) {
                sair = 1;
            } else {
                printf("Opcao invalida. Tente novamente.\n");
            }
        }

        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "preco"));
        printf("====================================\n");
        printf("ID: %d\n", id);
        printf("Motorista: %s\n", nomeMotorista);
        printf("Origem: %s\n", cidadeOrigem);
        printf("Destino: %s\n", cidadeDestino);
        printf("Data: %s\n", data);
        printf("Hora: %s\n", hora);
        printf("Capacidade: %d\n", capacidade);
        printf("Preco: %.2f\n", preco);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);

    printf("Selecione a carona desejada pelo ID (ou 00 para voltar): \n");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    cJSON *respostaSelecionada = cJSON_CreateObject();
    cJSON_AddStringToObject(respostaSelecionada, "classe", "Cliente");
    cJSON_AddStringToObject(respostaSelecionada, "acao", "selecionar_carona");
    cJSON_AddStringToObject(respostaSelecionada, "emailCliente", cliente->email);
    cJSON_AddNumberToObject(respostaSelecionada, "idSelecionado", idSelecionado);
    cJSON_AddStringToObject(respostaSelecionada, "origem", origem);
    cJSON_AddStringToObject(respostaSelecionada, "destino", destino);
    char *mensagemSelecionada = cJSON_PrintUnformatted(respostaSelecionada);
    if (mensagemSelecionada != NULL) {
        write(socketCliente, mensagemSelecionada, strlen(mensagemSelecionada));
        free(mensagemSelecionada);
    }
    cJSON_Delete(respostaSelecionada);

    int bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "CARONA_RESERVADA") == 0) {
            printf("Carona reservada com sucesso!\n");
        } else if (strcmp(buffer_mensagem, "ASSENTO_INDISPONIVEL") == 0) {
            printf("Assento indisponível. Tente outra carona.\n");
        } else {
            printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
        }
    }

}

void ver_caronas(int socketCliente, Cliente *cliente){
    char buffer_mensagem[4096] = {0};
    int total = 0;

    cJSON *enviar_dados = cJSON_CreateObject();
    cJSON_AddStringToObject(enviar_dados, "classe", "Cliente");
    cJSON_AddStringToObject(enviar_dados, "nome", cliente->nome);
    cJSON_AddStringToObject(enviar_dados, "email", cliente->email);
    cJSON_AddStringToObject(enviar_dados, "senha", cliente->senha);
    cJSON_AddStringToObject(enviar_dados, "acao", "listar_reservas");

    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    if (mensagem != NULL) {
        write(socketCliente, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(enviar_dados);

    printf("====================================\n");
    printf("           MINHAS CARONAS          \n");
    printf("====================================\n");

    cJSON *arrayResposta = NULL;
    while (arrayResposta == NULL && total < (int)sizeof(buffer_mensagem) - 1) {
        ssize_t bytes = read(socketCliente, buffer_mensagem + total, sizeof(buffer_mensagem) - 1 - total);
        if (bytes <= 0) break;
        total += bytes;
        buffer_mensagem[total] = '\0';
        arrayResposta = cJSON_Parse(buffer_mensagem);
    }

    if (arrayResposta == NULL) {
        printf("Erro ao obter lista de caronas.\n");
        return;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        printf("NENHUMA CARONA CADASTRADA!\n");
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "preco"));
        printf("====================================\n");
        printf("ID: %d\n", id);
        printf("Nome do Motorista: %s\n", nomeMotorista);
        printf("Origem: %s\n", cidadeOrigem);
        printf("Destino: %s\n", cidadeDestino);
        printf("Data: %s\n", data);
        printf("Hora: %s\n", hora);
        printf("Capacidade: %d\n", capacidade);
        printf("Preco: %.2f\n", preco);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);
}

void cancelar_carona(int socketCliente, Cliente *cliente){
    char buffer_mensagem[4096] = {0};
    int total = 0;

    cJSON *enviar_dados = cJSON_CreateObject();
    cJSON_AddStringToObject(enviar_dados, "classe", "Cliente");
    cJSON_AddStringToObject(enviar_dados, "nome", cliente->nome);
    cJSON_AddStringToObject(enviar_dados, "email", cliente->email);
    cJSON_AddStringToObject(enviar_dados, "senha", cliente->senha);
    cJSON_AddStringToObject(enviar_dados, "acao", "listar_reservas");

    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    if (mensagem != NULL) {
        write(socketCliente, mensagem, strlen(mensagem));
        free(mensagem);
    }
    cJSON_Delete(enviar_dados);

    printf("====================================\n");
    printf("           MINHAS CARONAS          \n");
    printf("====================================\n");

    cJSON *arrayResposta = NULL;
    while (arrayResposta == NULL && total < (int)sizeof(buffer_mensagem) - 1) {
        ssize_t bytes = read(socketCliente, buffer_mensagem + total, sizeof(buffer_mensagem) - 1 - total);
        if (bytes <= 0) break;
        total += bytes;
        buffer_mensagem[total] = '\0';
        arrayResposta = cJSON_Parse(buffer_mensagem);
    }

    if (arrayResposta == NULL) {
        printf("Erro ao obter lista de caronas.\n");
        return;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        printf("NENHUMA CARONA CADASTRADA!\n");
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "preco"));
        printf("====================================\n");
        printf("ID: %d\n", id);
        printf("Nome do Motorista: %s\n", nomeMotorista);
        printf("Origem: %s\n", cidadeOrigem);
        printf("Destino: %s\n", cidadeDestino);
        printf("Data: %s\n", data);
        printf("Hora: %s\n", hora);
        printf("Capacidade: %d\n", capacidade);
        printf("Preco: %.2f\n", preco);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);

    printf("Selecione a carona que voce deseja cancelar pelo ID (ou 00 para voltar): \n");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    cJSON *respostaSelecionada = cJSON_CreateObject();
    cJSON_AddStringToObject(respostaSelecionada, "classe", "Cliente");
    cJSON_AddStringToObject(respostaSelecionada, "acao", "cancelar_carona");
    cJSON_AddStringToObject(respostaSelecionada, "emailCliente", cliente->email);
    cJSON_AddNumberToObject(respostaSelecionada, "idSelecionado", idSelecionado);
    char *mensagemSelecionada = cJSON_PrintUnformatted(respostaSelecionada);
    if (mensagemSelecionada != NULL) {
        write(socketCliente, mensagemSelecionada, strlen(mensagemSelecionada));
        free(mensagemSelecionada);
    }
    cJSON_Delete(respostaSelecionada);

    int bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes > 0) {
        buffer_mensagem[bytes] = '\0';
        if (strcmp(buffer_mensagem, "CARONA_CANCELADA") == 0) {
            printf("Carona cancelada com sucesso!\n");
        } else if (strcmp(buffer_mensagem, "MOTORISTA_CANCELOU") == 0) {
            printf("O motorista cancelou esta carona!\n");
        } else {
            printf("Carona nao cancelada! motivo: %s\n", buffer_mensagem);
        }
    }
}

void telaMenu(int socketCliente, Cliente *cliente){
    char buffer_mensagem[18] = {0};
    int escolha = 0;
    int sair = 0;
    int enviou = 0;

    while(sair != 1){
        printf("====================================\n");
        printf("                MENU                \n");
        printf("====================================\n");
        printf(" 1- Buscar Carona\n");
        printf(" 2- Cancelar Reserva\n");
        printf(" 3- Ver minhas Caronas\n");
        printf(" 4- Sair da Conta\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            buscarCarona(socketCliente, cliente);
        } else if (escolha == 2) {
            cancelar_carona(socketCliente, cliente);
        } else if (escolha == 3) {
            ver_caronas(socketCliente, cliente);
        } else if (escolha == 4) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

}

void telaLogin(int socketCliente, Cliente *cliente){
    char buffer_mensagem[18] = {0};
    int escolha = 0;
    int sair = 0;
    int autenticado = 0;
    fflush(stdin);

    while (!autenticado && !sair) {
        int enviou = 0;
        while (sair != 1 && !enviou) {
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
                enviou = 1;
            } else if (escolha == 4) {
                sair = 1;
            } else {
                printf("Opcao invalida. Tente novamente.\n");
            }
        }

        if (sair) {
            return;
        }

        int bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
        if (bytes > 0) {
            buffer_mensagem[bytes] = '\0';
            if (strcmp(buffer_mensagem, "NAO_AUTENTICADO") == 0) {
                printf("Email ou senha incorretos. Tente novamente.\n");
            } else if (strcmp(buffer_mensagem, "AUTENTICADO") == 0) {
                printf("Login realizado com sucesso!\n");
                cliente->status = AUTENTICADO;
                autenticado = 1;
                telaMenu(socketCliente, cliente);
            } else {
                printf("Resposta desconhecida do servidor: %s\n", buffer_mensagem);
            }
        }
    }
}

void telaCadastro(int socketCliente, Cliente *cliente){
    int escolha = 0;
    char buffer_mensagem[20] = {0};
    int sair = 0;
    int enviou = 0;
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

    ssize_t bytes = read(socketCliente, buffer_mensagem, sizeof(buffer_mensagem) - 1);
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
        printf("      Sistema de Login Cliente      \n");
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