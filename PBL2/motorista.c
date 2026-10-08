#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>   // libcurl: cliente HTTP (substitui socket/connect/read/write)
#include "cJSON.h"
#include "formatos.h"

#define PORT 65432                 // porta padrão do servidor
#define TIMEOUT_CONEXAO_MS 3000    // tempo máximo para conectar
#define TIMEOUT_TOTAL_MS   10000   // tempo máximo para a requisição inteira

//URL base do servidor (ex.: http://192.168.0.10:65432), montada no main.
//Com REST não existe conexão aberta: cada requisição é independente, então só guardamos o endereço.
static char baseURL[256];

//resposta de uma requisição HTTP: código de status + corpo (alocado com malloc; quem usar libera com free)
typedef struct {
    int status;
    char *corpo;
} RespostaHTTP;

//---------------------------------------------------------------------------------------------
// Camada HTTP (libcurl)
//---------------------------------------------------------------------------------------------

//buffer que a libcurl vai preenchendo conforme o corpo da resposta chega
typedef struct {
    char *dados;
    size_t tamanho;
} Buffer;

//callback da libcurl: é chamada a cada pedaço do corpo da resposta recebido
static size_t escreverResposta(char *pedaco, size_t tamanhoItem, size_t qtdItens, void *usuario) {
    Buffer *buf = (Buffer *)usuario;
    size_t n = tamanhoItem * qtdItens;
    char *novo = realloc(buf->dados, buf->tamanho + n + 1);
    if (novo == NULL) return 0;   //devolver menos que n faz a libcurl abortar com erro
    buf->dados = novo;
    memcpy(buf->dados + buf->tamanho, pedaco, n);
    buf->tamanho += n;
    buf->dados[buf->tamanho] = '\0';
    return n;
}

//faz uma requisição HTTP ao servidor.
//metodo: "GET", "POST", "DELETE"...  caminho: ex. "/api/motoristas/login"  json: corpo (ou NULL)
//retorna 1 se o servidor respondeu (resposta em *resp) e 0 se estiver offline / der timeout
static int chamarAPI(const char *metodo, const char *caminho, const char *json, RespostaHTTP *resp) {
    resp->status = 0;
    resp->corpo = NULL;

    CURL *curl = curl_easy_init();
    if (curl == NULL) return 0;

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", baseURL, caminho);

    Buffer buf = {NULL, 0};
    struct curl_slist *cabecalhos = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, metodo);
    if (json != NULL) {
        cabecalhos = curl_slist_append(cabecalhos, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, cabecalhos);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);   //o texto de 'json' precisa continuar válido até o curl_easy_perform (é do chamador, então continua)
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escreverResposta);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)TIMEOUT_CONEXAO_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)TIMEOUT_TOTAL_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);

    int respondeu = 0;
    if (rc == CURLE_OK) {
        long codigo = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &codigo);
        resp->status = (int)codigo;
        resp->corpo = buf.dados != NULL ? buf.dados : strdup("");
        respondeu = 1;
    } else {
        free(buf.dados);   //conexão recusada, timeout etc.: servidor offline
    }

    curl_slist_free_all(cabecalhos);
    curl_easy_cleanup(curl);
    return respondeu;
}

//lê um campo de texto do JSON de resposta (ex.: "erro" ou "nome"). Retorna 1 se achou e 0 se não achou
static int extrairTexto(const char *corpo, const char *campo, char *destino, size_t tamanho) {
    destino[0] = '\0';
    if (corpo == NULL) return 0;
    cJSON *json = cJSON_Parse(corpo);
    if (json == NULL) return 0;
    char *valor = cJSON_GetStringValue(cJSON_GetObjectItem(json, campo));
    if (valor != NULL) snprintf(destino, tamanho, "%s", valor);
    cJSON_Delete(json);
    return valor != NULL;
}

//codifica um texto para a query string (espaço vira %20, '@' vira %40...)
static void escaparURL(const char *texto, char *destino, size_t tamanho) {
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; texto[i] != '\0' && j + 4 < tamanho; i++) {
        unsigned char c = (unsigned char)texto[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            destino[j++] = (char)c;
        } else {
            destino[j++] = '%';
            destino[j++] = hex[c >> 4];
            destino[j++] = hex[c & 0x0F];
        }
    }
    destino[j] = '\0';
}

//monta a query string "?email=...&nome=..." que o servidor usa para identificar o motorista (GET/DELETE)
static void montarQueryMotorista(Motorista *motorista, char *destino, size_t tamanho) {
    char email[200], nome[200];
    escaparURL(motorista->email, email, sizeof(email));
    escaparURL(motorista->nome, nome, sizeof(nome));
    snprintf(destino, tamanho, "?email=%s&nome=%s", email, nome);
}

//---------------------------------------------------------------------------------------------
// Funções do motorista
//---------------------------------------------------------------------------------------------

//função para a requisição de login (escolha == 1) ou cadastro (escolha == 2)
//POST /api/motoristas/login      {email, senha}
//POST /api/motoristas/cadastro   {nome, email, senha}
//retorna 1 se o servidor respondeu (resposta em *resp, quem chamar libera resp->corpo) e 0 se estiver offline
int enviarCadastro(char *nome, char *email, char *senha, int escolha, RespostaHTTP *resp){
    cJSON *enviar_dados = cJSON_CreateObject();
    //monta o cjson para a requisição
    if (escolha == 2) {
        cJSON_AddStringToObject(enviar_dados, "nome", nome ? nome : "");
    }
    cJSON_AddStringToObject(enviar_dados, "email", email ? email : "");
    cJSON_AddStringToObject(enviar_dados, "senha", senha ? senha : "");

    //transforma a requisição em string
    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    cJSON_Delete(enviar_dados);
    if (mensagem == NULL) return 0;

    //envia a requisição para o servidor
    const char *caminho = (escolha == 1) ? "/api/motoristas/login" : "/api/motoristas/cadastro";
    int respondeu = chamarAPI("POST", caminho, mensagem, resp);
    free(mensagem);
    return respondeu;
}

//função para enviar uma requisição de cadastrar trecho
//POST /api/motoristas/trechos
//retorna 0 se o servidor estiver offline e 1 nos outros casos
int cadastrarTrecho(Motorista *motorista) {
    char origem[50], destino[50];
    char data[11], hora[6];
    int capacidade;
    float preco;

    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return 1;
    }

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
        cJSON_Delete(trecho);
        return 1;
    }
    cJSON *arrayNomesClientes = cJSON_CreateArray();
    if (arrayNomesClientes == NULL){
        printf("Array de nomes de clientes nao criado!\n");
        cJSON_Delete(arrayClientes);
        cJSON_Delete(trecho);
        return 1;
    }

    //monta o cjson para a requisição
    cJSON_AddStringToObject(trecho, "emailMotorista", email);
    cJSON_AddStringToObject(trecho, "origem", origem);
    cJSON_AddStringToObject(trecho, "destino", destino);
    cJSON_AddStringToObject(trecho, "data", data);
    cJSON_AddStringToObject(trecho, "hora", hora);
    cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
    cJSON_AddNumberToObject(trecho, "preco", preco);
    cJSON_AddStringToObject(trecho, "nome", motorista->nome);
    cJSON_AddItemToObject(trecho, "clientes", arrayClientes);
    cJSON_AddItemToObject(trecho, "nomesClientes", arrayNomesClientes);
    char *mensagem = cJSON_PrintUnformatted(trecho);
    cJSON_Delete(trecho);
    if (mensagem == NULL) return 1;

    //envia a requisição
    RespostaHTTP resp;
    int respondeu = chamarAPI("POST", "/api/motoristas/trechos", mensagem, &resp);
    free(mensagem);

    if (!respondeu) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return 0;
    }

    if (resp.status == 201) {
        printf("Trecho cadastrado com sucesso!\n");
    } else {
        char erro[200];
        extrairTexto(resp.corpo, "erro", erro, sizeof(erro));
        if (strcmp(erro, "Caminho nao encontrado") == 0) {
            printf("Falha ao cadastrar trecho, caminho possivel nao encontrado\n");
        } else {
            printf("Falha ao cadastrar trecho (HTTP %d): %s\n", resp.status, erro[0] ? erro : "erro desconhecido");
        }
    }
    free(resp.corpo);
    return 1;
}

//função para cadastrar n trechos
void cadastrarTrechos(Motorista *motorista) {
    int quantidade;
    printf("Digite a quantidade de trechos que deseja cadastrar: ");
    scanf("%d", &quantidade);

    for (int i = 0; i < quantidade; i++) {
        printf("Cadastro do trecho %d:\n", i + 1);
        if (!cadastrarTrecho(motorista)) {
            break; //servidor offline, não adianta tentar os próximos trechos
        }
    }
}

//função para fazer uma requisição para listar os trechos do motorista
//GET /api/motoristas/trechos?email=...&nome=...
//retorna a quantidade de trechos listados, ou -1 se o servidor estiver offline/houver erro
int listarTrechos(Motorista *motorista){
    char query[450];
    char caminho[500];
    montarQueryMotorista(motorista, query, sizeof(query));
    snprintf(caminho, sizeof(caminho), "/api/motoristas/trechos%s", query);

    RespostaHTTP resp;
    if (!chamarAPI("GET", caminho, NULL, &resp)) {
        printf("O SERVIDOR ESTA OFFLINE\n");
        return -1;
    }

    cJSON *arrayResposta = (resp.status == 200) ? cJSON_Parse(resp.corpo) : NULL;
    free(resp.corpo);

    if (arrayResposta == NULL || !cJSON_IsArray(arrayResposta)) {
        printf("Erro ao obter lista de trechos.\n");
        cJSON_Delete(arrayResposta);
        return -1;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        printf("Nenhum trecho cadastrado\n");
        cJSON_Delete(arrayResposta);
        return 0;
    }
    printf("====================================\n");
    printf("          Meus Trechos              \n");

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
        printf("Origem: %s\n", cidadeOrigem ? cidadeOrigem : "?");
        printf("Destino: %s\n", cidadeDestino ? cidadeDestino : "?");
        printf("Data: %s\n", data ? data : "?");
        printf("Hora: %s\n", hora ? hora : "?");
        printf("Capacidade: %d\n", capacidade);
        printf("Passageiros: ");
        cJSON *arrayNomesClientes = cJSON_GetObjectItem(item, "nomesClientes");
        int numClientes = cJSON_GetArraySize(arrayNomesClientes);
        if (numClientes == 0){
            printf("Nenhum passageiro cadastrado\n");
        } else {
            for (int j = 0; j < numClientes; j++) {
                printf("%s", cJSON_GetStringValue(cJSON_GetArrayItem(arrayNomesClientes, j)));
                if (j < numClientes - 1) {
                    printf(", ");
                }
            }
            printf("\n");
        }

    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);
    return n;
}

//função para a requisição de cancelar um trecho do motorista
//DELETE /api/motoristas/trechos/{id}?email=...&nome=...
void cancelarTrecho(Motorista *motorista){
    if (listarTrechos(motorista) <= 0) {
        return; //servidor offline, erro ou nenhum trecho para cancelar
    }

    printf("Digite o ID do trecho que deseja cancelar (ou -1 para voltar): ");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    if (idSelecionado == -1) return;

    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return;
    }

    //o id vai no caminho e o motorista é identificado pela query string
    char query[450];
    char caminho[550];
    montarQueryMotorista(motorista, query, sizeof(query));
    snprintf(caminho, sizeof(caminho), "/api/motoristas/trechos/%d%s", idSelecionado, query);

    RespostaHTTP resp;
    if (!chamarAPI("DELETE", caminho, NULL, &resp)) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    if (resp.status == 200) {
        printf("Trecho cancelado com sucesso!\n");
    } else if (resp.status == 404) {
        printf("Trecho nao encontrado.\n");
    } else {
        printf("Falha ao cancelar trecho (HTTP %d).\n", resp.status);
    }
    free(resp.corpo);
}

//requisição para cadastrar uma rota do motorista
//POST /api/motoristas/rotas
void cadastrarRotaMotorista(Motorista *motorista){
    char *email = motorista->email;
    if (email == NULL){
        printf("email inválido!\n");
        return;
    }

    cJSON *arrayTrechos = cJSON_CreateArray();
    if (arrayTrechos == NULL) { printf("Erro ao criar objeto JSON\n"); return; }

    char destinoAnterior[50] = {0};
    int totalAdicionados = 0;
    int sair = 0;

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
    cJSON_AddStringToObject(pedido, "nome", motorista->nome);
    cJSON_AddItemToObject(pedido, "trechos", arrayTrechos);
    //transforma em string
    char *mensagem = cJSON_PrintUnformatted(pedido);
    cJSON_Delete(pedido);
    if (mensagem == NULL) return;

    //envia a requisição para o servidor
    RespostaHTTP resp;
    int respondeu = chamarAPI("POST", "/api/motoristas/rotas", mensagem, &resp);
    free(mensagem);

    if (!respondeu) {
        printf("ERRO: SERVIDOR OFFLINE\n");
        return;
    }

    if (resp.status == 201) {
        printf("Rota cadastrada com sucesso!\n");
    } else {
        //nos erros o servidor manda {"erro": "<CODIGO>"}
        char erro[200];
        extrairTexto(resp.corpo, "erro", erro, sizeof(erro));
        if (strcmp(erro, "ROTA_DESCONECTADA") == 0) {
            printf("Os trechos nao formam uma rota conectada.\n");
        } else if (strcmp(erro, "FALHA_CADASTRO_ROTA") == 0) {
            printf("Um ou mais trechos nao tem caminho possivel no mapa.\n");
        } else if (strcmp(erro, "ROTA_INVALIDA") == 0) {
            printf("Rota invalida: ha trechos com dados incompletos.\n");
        } else {
            printf("Resposta desconhecida do servidor (HTTP %d): %s\n", resp.status, erro[0] ? erro : resp.corpo);
        }
    }
    free(resp.corpo);
}

void telaMenu(Motorista *motorista){
    int escolha = 0;
    int sair = 0;

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
            cadastrarTrecho(motorista);
        } else if (escolha == 2) {
            cadastrarTrechos(motorista);
        } else if (escolha == 3) {
            listarTrechos(motorista);
        } else if (escolha == 4) {
            cancelarTrecho(motorista);
        } else if (escolha == 5) {
            cadastrarRotaMotorista(motorista);
        } else if (escolha == 6) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }
}

void telaLogin(Motorista *motorista){
    int escolha = 0;
    int sair = 0;
    int enviou = 0;
    int c = 0;
    RespostaHTTP resp = {0, NULL};
    int respondeu = 0;
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
            respondeu = enviarCadastro(motorista->nome, motorista->email, motorista->senha, 1, &resp);
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

    if (!respondeu) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    if (resp.status == 401) {
        printf("Email ou senha incorretos. Tente novamente.\n");
        free(resp.corpo);
        telaLogin(motorista);
        return;
    }

    if (resp.status == 200) {
        char nome[50];
        if (extrairTexto(resp.corpo, "nome", nome, sizeof(nome))){
            printf("Login realizado com sucesso!\n");
            motorista->status = AUTENTICADO;
            strncpy(motorista->nome, nome, sizeof(motorista->nome) - 1);
            motorista->nome[sizeof(motorista->nome) - 1] = '\0';
            free(resp.corpo);
            telaMenu(motorista);
            return;
        }
    }
    printf("Resposta inesperada do servidor (HTTP %d)\n", resp.status);
    free(resp.corpo);
}

void telaCadastro(Motorista *motorista){
    int escolha = 0;
    int sair = 0;
    int enviou = 0;
    RespostaHTTP resp = {0, NULL};
    int respondeu = 0;
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
            respondeu = enviarCadastro(motorista->nome, motorista->email, motorista->senha, 2, &resp);
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

    if (!respondeu) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    if (resp.status == 409) {
        printf("Email ja cadastrado. Tente novamente.\n");
        free(resp.corpo);
        telaCadastro(motorista);
        return;
    } else if (resp.status == 201) {
        printf("Cadastro realizado com sucesso!\n");
        motorista->status = AUTENTICADO;
    } else {
        printf("Resposta desconhecida do servidor (HTTP %d): %s\n", resp.status, resp.corpo);
    }
    free(resp.corpo);
}

void telaInicial(Motorista *motorista){
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
                telaLogin(motorista);
                break;
            case 2:
                telaCadastro(motorista);
                break;
            case 3:
                //REST não mantém conexão aberta, então não há o que avisar ao servidor ao sair
                printf("Saindo...\n");
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
    Motorista *motorista = calloc(1, sizeof(Motorista));
    if (motorista == NULL) {
        perror("Memoria insuficiente");
        exit(EXIT_FAILURE);
    }

    char ip_servidor[100];
    printf("Digite o IP do servidor (ou ip:porta): ");
    scanf("%99s", ip_servidor);

    //monta a URL base: se o usuário já informou a porta usa ela, senão usa a porta padrão
    if (strchr(ip_servidor, ':') != NULL) {
        snprintf(baseURL, sizeof(baseURL), "http://%s", ip_servidor);
    } else {
        snprintf(baseURL, sizeof(baseURL), "http://%s:%d", ip_servidor, PORT);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    //teste de alcance: qualquer resposta HTTP (mesmo 404) prova que o servidor está no ar
    RespostaHTTP teste;
    if (!chamarAPI("GET", "/api/motoristas", NULL, &teste)) {
        printf("conexao com o servidor nao estabelecida (%s)\n", baseURL);
        free(motorista);
        curl_global_cleanup();
        exit(EXIT_FAILURE);
    }
    free(teste.corpo);

    telaInicial(motorista);

    free(motorista);
    curl_global_cleanup();
    return 0;
}