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
//metodo: "GET", "POST", "DELETE"...  caminho: ex. "/api/clientes/login"  json: corpo (ou NULL)
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

//codifica um texto para a query string (espaço vira %20, '@' vira %40, '/' vira %2F...)
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

//texto seguro para printf (evita imprimir NULL quando o servidor não mandar um campo)
static const char *txt(const char *s) {
    return s != NULL ? s : "?";
}

//---------------------------------------------------------------------------------------------
// Funções do cliente
//---------------------------------------------------------------------------------------------

//Função para enviar a requisição de login (escolha == 1) ou cadastro (escolha == 2)
//POST /api/clientes/login      {email, senha}
//POST /api/clientes/cadastro   {nome, email, senha}
//retorna 1 se o servidor respondeu (resposta em *resp, quem chamar libera resp->corpo) e 0 se estiver offline
int enviarCadastro(char *nome, char *email, char *senha, int escolha, RespostaHTTP *resp){
    cJSON *enviar_dados = cJSON_CreateObject();
    //Cria o pacote cJSON para a requisição
    if (escolha == 2) {
        cJSON_AddStringToObject(enviar_dados, "nome", nome ? nome : "");
    }
    cJSON_AddStringToObject(enviar_dados, "email", email ? email : "");
    cJSON_AddStringToObject(enviar_dados, "senha", senha ? senha : "");

    //transforma o cJSON em string
    char *mensagem = cJSON_PrintUnformatted(enviar_dados);
    cJSON_Delete(enviar_dados);
    if (mensagem == NULL) return 0;

    //envia a string para o servidor
    const char *caminho = (escolha == 1) ? "/api/clientes/login" : "/api/clientes/cadastro";
    int respondeu = chamarAPI("POST", caminho, mensagem, resp);
    free(mensagem);
    return respondeu;
}

//Desfaz as reservas já feitas para uma rota que NÃO vai ser concluída (o cliente desistiu ou não há mais caronas).
//No servidor cada trecho da rota é reservado assim que é escolhido, então sem isto os assentos ficariam presos.
//DELETE /api/clientes/reservas/{idTrecho}?emailCliente=...&nomeCliente=...
static void liberarReservasRota(Cliente *cliente, cJSON *arrayRotas) {
    int total = cJSON_GetArraySize(arrayRotas);
    if (total == 0) return;

    char email[200], nome[200];
    escaparURL(cliente->email, email, sizeof(email));
    escaparURL(cliente->nome, nome, sizeof(nome));

    int liberados = 0;
    for (int i = 0; i < total; i++) {
        cJSON *trecho = cJSON_GetArrayItem(arrayRotas, i);
        //o servidor devolve o trecho reservado com o id em "idTrecho"
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(trecho, "idTrecho"));

        char caminho[600];
        snprintf(caminho, sizeof(caminho), "/api/clientes/reservas/%d?emailCliente=%s&nomeCliente=%s", id, email, nome);

        RespostaHTTP resp;
        if (chamarAPI("DELETE", caminho, NULL, &resp)) {
            if (resp.status == 200) liberados++;
            free(resp.corpo);
        } else {
            printf("Nao foi possivel liberar a reserva do trecho %d (servidor offline).\n", id);
        }
    }
    printf("Rota nao concluida: %d reserva(s) parcial(is) liberada(s).\n", liberados);
}

//Aviso para quando o servidor cai no meio da montagem da rota e não dá para desfazer as reservas agora
static void avisarReservasParciais(int total) {
    if (total > 0) {
        printf("Atencao: %d trecho(s) da rota ja estava(m) reservado(s). Use 'Cancelar Reserva' quando o servidor voltar.\n", total);
    }
}

//Função para o cliente selecionar os trechos para a rota até a cidade destino
void criarRota(Cliente *cliente, char *origem, char *destino, char *data, char *hora) {
    (void)data; (void)hora;   //o servidor não filtra a rota por data/hora
    cJSON *arrayRotas = cJSON_CreateArray();
    if (arrayRotas == NULL) { printf("Erro ao criar objeto JSON\n"); return; }

    char origemAtual[50];
    strncpy(origemAtual, origem, sizeof(origemAtual) - 1);
    origemAtual[sizeof(origemAtual) - 1] = '\0';

    int sair = 0;
    while (!sair) {
        //GET /api/clientes/trechos?origem=...  -> trechos com assento partindo da cidade atual
        char origemEsc[200];
        char caminho[300];
        escaparURL(origemAtual, origemEsc, sizeof(origemEsc));
        snprintf(caminho, sizeof(caminho), "/api/clientes/trechos?origem=%s", origemEsc);

        RespostaHTTP resp;
        if (!chamarAPI("GET", caminho, NULL, &resp)) {
            printf("O SERVIDOR ESTA OFFLINE\n");
            printf("Itinerario nao concluido!\n");
            avisarReservasParciais(cJSON_GetArraySize(arrayRotas));
            cJSON_Delete(arrayRotas);
            return;
        }
        cJSON *arrayResposta = (resp.status == 200) ? cJSON_Parse(resp.corpo) : NULL;
        free(resp.corpo);

        if (arrayResposta == NULL || !cJSON_IsArray(arrayResposta)) {
            printf("Erro ao obter lista de trechos.\n");
            cJSON_Delete(arrayResposta);
            liberarReservasRota(cliente, arrayRotas);
            cJSON_Delete(arrayRotas);
            return;
        }

        int n = cJSON_GetArraySize(arrayResposta);
        if (n == 0) {
            printf("NENHUMA CARONA ENCONTRADA PARTINDO DE %s\n", origemAtual);
            cJSON_Delete(arrayResposta);
            liberarReservasRota(cliente, arrayRotas);
            cJSON_Delete(arrayRotas);
            return;
        }
        printf("=============================================\n");
        printf("CARONAS DISPONIVEIS PARTINDO DE %s\n", origemAtual);
        printf("=============================================\n");
        //lista as possiveis caronas
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
            printf("ID: %d | Motorista: %s\n", id, txt(nomeMotorista));
            printf("Origem: %s -> Destino: %s\n", txt(cOrigem), txt(cDestino));
            printf("Data: %s Hora: %s Capacidade: %d Preco: %.2f\n", txt(dItem), txt(hItem), capacidade, preco);
        }
        printf("=============================================\n");
        cJSON_Delete(arrayResposta);

        printf("Digite o ID do trecho para adicionar a rota (ou -1 para cancelar): ");
        int idSelecionado;
        scanf("%d", &idSelecionado);
        if (idSelecionado == -1) {
            liberarReservasRota(cliente, arrayRotas);   //desistiu: devolve os assentos já reservados
            cJSON_Delete(arrayRotas);
            return;
        }

        char origemEscolhida[50], destinoEscolhido[50];
        printf("Confirme a origem exata do trecho escolhido: ");
        scanf(" %49[^\n]", origemEscolhida);
        printf("Confirme o destino exato do trecho escolhido: ");
        scanf(" %49[^\n]", destinoEscolhido);
        //Cria o cJSON para enviar a requisição
        cJSON *respostaSelecionada = cJSON_CreateObject();
        cJSON_AddNumberToObject(respostaSelecionada, "idSelecionado", idSelecionado);
        cJSON_AddStringToObject(respostaSelecionada, "emailCliente", cliente->email);
        cJSON_AddStringToObject(respostaSelecionada, "nomeCliente", cliente->nome);
        cJSON_AddStringToObject(respostaSelecionada, "origem", origemEscolhida);
        cJSON_AddStringToObject(respostaSelecionada, "destino", destinoEscolhido);
        char *mensagemSel = cJSON_PrintUnformatted(respostaSelecionada);
        cJSON_Delete(respostaSelecionada);
        if (mensagemSel == NULL) continue;

        //POST /api/clientes/rotas/trechos -> reserva o assento do trecho e devolve o trecho atualizado
        RespostaHTTP respSel;
        int respondeu = chamarAPI("POST", "/api/clientes/rotas/trechos", mensagemSel, &respSel);
        free(mensagemSel);

        if (!respondeu) {
            printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
            avisarReservasParciais(cJSON_GetArraySize(arrayRotas));
            cJSON_Delete(arrayRotas);
            return;
        }

        if (respSel.status == 201) {
            cJSON *respostaServidor = cJSON_Parse(respSel.corpo);
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
            } else {
                printf("Resposta invalida do servidor: %s\n", respSel.corpo);
            }
        } else if (respSel.status == 409) {
            printf("Assento indisponivel. Escolha outra carona.\n");
        } else if (respSel.status == 404) {
            printf("Trecho nao encontrado. Confira a origem e o destino informados.\n");
        } else {
            printf("Resposta desconhecida do servidor (HTTP %d): %s\n", respSel.status, respSel.corpo);
        }
        free(respSel.corpo);
    }

    //envia a requisição final com os trechos selecionados
    //POST /api/clientes/rotas -> o servidor confere se a rota liga origem a destino; se não ligar, desfaz as reservas
    int totalTrechos = cJSON_GetArraySize(arrayRotas);
    cJSON *respostaFinalizar = cJSON_CreateObject();
    cJSON_AddStringToObject(respostaFinalizar, "email", cliente->email);
    cJSON_AddStringToObject(respostaFinalizar, "nomeCliente", cliente->nome);   //o servidor usa o nome para limpar a reserva se a rota falhar
    cJSON_AddItemToObject(respostaFinalizar, "rota", arrayRotas);
    cJSON_AddStringToObject(respostaFinalizar, "origemRota", origem);
    cJSON_AddStringToObject(respostaFinalizar, "destinoRota", destino);
    char *mensagemFinal = cJSON_PrintUnformatted(respostaFinalizar);
    cJSON_Delete(respostaFinalizar);   //libera também o arrayRotas, que agora pertence a este objeto
    if (mensagemFinal == NULL) return;

    RespostaHTTP respFinal;
    int respondeu = chamarAPI("POST", "/api/clientes/rotas", mensagemFinal, &respFinal);
    free(mensagemFinal);

    if (!respondeu) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        avisarReservasParciais(totalTrechos);
        return;
    }

    if (respFinal.status == 200) {
        printf("Rota cadastrada com sucesso!\n");
    } else {
        printf("Falha ao finalizar a rota.\n");
    }
    free(respFinal.corpo);
}

//função para buscar as caronas disponiveis
//GET /api/clientes/caronas?origem=...&destino=...&data=...&hora=...
void buscarCarona(Cliente *cliente) {
    char origem[50], destino[50];
    char data[11], hora[6];

    printf("Digite a origem da carona: ");
    scanf(" %49[^\n]", origem);
    printf("Digite o destino da carona: ");
    scanf(" %49[^\n]", destino);
    printf("Digite a data da carona ou digite (dd/mm/aaaa) para qualquer data: ");
    scanf(" %10[^\n]", data);
    printf("Digite a hora da carona: ");
    scanf(" %5[^\n]", hora);

    //GET não tem corpo: os dados da busca vão na query string
    char origemEsc[200], destinoEsc[200], dataEsc[100], horaEsc[50];
    escaparURL(origem, origemEsc, sizeof(origemEsc));
    escaparURL(destino, destinoEsc, sizeof(destinoEsc));
    escaparURL(data, dataEsc, sizeof(dataEsc));
    escaparURL(hora, horaEsc, sizeof(horaEsc));
    char caminho[700];
    snprintf(caminho, sizeof(caminho), "/api/clientes/caronas?origem=%s&destino=%s&data=%s&hora=%s",
             origemEsc, destinoEsc, dataEsc, horaEsc);

    RespostaHTTP resp;
    if (!chamarAPI("GET", caminho, NULL, &resp)) {
        printf("O SERVIDOR ESTA OFFLINE\n");
        return;
    }
    cJSON *arrayResposta = (resp.status == 200) ? cJSON_Parse(resp.corpo) : NULL;
    free(resp.corpo);

    if (arrayResposta == NULL || !cJSON_IsArray(arrayResposta)) {
        printf("Erro ao obter lista de trechos.\n");
        cJSON_Delete(arrayResposta);
        return;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        cJSON_Delete(arrayResposta);
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
                criarRota(cliente, origem, destino, data, hora);
                sair = 1;
            } else if (escolha == 2) {
                sair = 1;
            } else {
                printf("Opcao invalida. Tente novamente.\n");
            }
        }
        return;
    }
    printf("====================================\n");
    printf("         Caronas encontradas        \n");
    printf("====================================\n");

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arrayResposta, i);
        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "capacidade"));
        char *dItem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
        char *hItem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "preco"));
        printf("====================================\n");
        printf("ID: %d\n", id);
        printf("Motorista: %s\n", txt(nomeMotorista));
        printf("Origem: %s\n", txt(cidadeOrigem));
        printf("Destino: %s\n", txt(cidadeDestino));
        printf("Data: %s\n", txt(dItem));
        printf("Hora: %s\n", txt(hItem));
        printf("Capacidade: %d\n", capacidade);
        printf("Preco: %.2f\n", preco);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);

    printf("Selecione a carona desejada pelo ID (ou -1 para voltar): \n");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    if (idSelecionado == -1) return;

    cJSON *respostaSelecionada = cJSON_CreateObject();
    cJSON_AddStringToObject(respostaSelecionada, "emailCliente", cliente->email);
    cJSON_AddStringToObject(respostaSelecionada, "nomeCliente", cliente->nome);
    cJSON_AddNumberToObject(respostaSelecionada, "idSelecionado", idSelecionado);
    cJSON_AddStringToObject(respostaSelecionada, "origem", origem);
    cJSON_AddStringToObject(respostaSelecionada, "destino", destino);
    char *mensagemSelecionada = cJSON_PrintUnformatted(respostaSelecionada);
    cJSON_Delete(respostaSelecionada);
    if (mensagemSelecionada == NULL) return;

    //POST /api/clientes/reservas -> reserva um assento no trecho
    RespostaHTTP respReserva;
    int respondeu = chamarAPI("POST", "/api/clientes/reservas", mensagemSelecionada, &respReserva);
    free(mensagemSelecionada);

    if (!respondeu) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    if (respReserva.status == 201) {
        printf("Carona reservada com sucesso!\n");
    } else if (respReserva.status == 409) {
        printf("Assento indisponível. Tente outra carona.\n");
    } else if (respReserva.status == 404) {
        printf("Carona nao encontrada. Confira o ID, a origem e o destino.\n");
    } else {
        printf("Resposta desconhecida do servidor (HTTP %d): %s\n", respReserva.status, respReserva.corpo);
    }
    free(respReserva.corpo);
}

//Pede ao servidor as reservas do cliente e mostra na tela.
//GET /api/clientes/reservas?email=...
//retorna a quantidade de reservas listadas, ou -1 se o servidor estiver offline/houver erro
static int listarReservas(Cliente *cliente) {
    char emailEsc[200];
    char caminho[300];
    escaparURL(cliente->email, emailEsc, sizeof(emailEsc));
    snprintf(caminho, sizeof(caminho), "/api/clientes/reservas?email=%s", emailEsc);

    RespostaHTTP resp;
    if (!chamarAPI("GET", caminho, NULL, &resp)) {
        printf("O SERVIDOR ESTA OFFLINE\n");
        return -1;
    }
    cJSON *arrayResposta = (resp.status == 200) ? cJSON_Parse(resp.corpo) : NULL;
    free(resp.corpo);

    if (arrayResposta == NULL || !cJSON_IsArray(arrayResposta)) {
        printf("Erro ao obter lista de caronas.\n");
        cJSON_Delete(arrayResposta);
        return -1;
    }

    int n = cJSON_GetArraySize(arrayResposta);
    if (n == 0) {
        printf("NENHUMA CARONA CADASTRADA!\n");
        cJSON_Delete(arrayResposta);
        return 0;
    }

    printf("====================================\n");
    printf("           MINHAS CARONAS          \n");
    printf("====================================\n");

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
        printf("Nome do Motorista: %s\n", txt(nomeMotorista));
        printf("Origem: %s\n", txt(cidadeOrigem));
        printf("Destino: %s\n", txt(cidadeDestino));
        printf("Data: %s\n", txt(data));
        printf("Hora: %s\n", txt(hora));
        printf("Capacidade: %d\n", capacidade);
        printf("Preco: %.2f\n", preco);
    }
    printf("====================================\n");
    cJSON_Delete(arrayResposta);
    return n;
}

//função para mostrar as caronas que o cliente reservou
void ver_caronas(Cliente *cliente){
    listarReservas(cliente);
}

//função para cancelar uma carona do cliente
//DELETE /api/clientes/reservas/{id}?emailCliente=...&nomeCliente=...
void cancelar_carona(Cliente *cliente){
    if (listarReservas(cliente) <= 0) {
        return; //servidor offline, erro ou nenhuma reserva para cancelar
    }

    printf("Selecione a carona que voce deseja cancelar pelo ID (ou -1 para voltar): \n");
    int idSelecionado;
    scanf("%d", &idSelecionado);
    if (idSelecionado == -1){
        return;
    }

    //o id vai no caminho e o cliente é identificado pela query string
    char email[200], nome[200];
    char caminho[600];
    escaparURL(cliente->email, email, sizeof(email));
    escaparURL(cliente->nome, nome, sizeof(nome));
    snprintf(caminho, sizeof(caminho), "/api/clientes/reservas/%d?emailCliente=%s&nomeCliente=%s", idSelecionado, email, nome);

    RespostaHTTP resp;
    if (!chamarAPI("DELETE", caminho, NULL, &resp)) {
        printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    if (resp.status == 200) {
        printf("Carona cancelada com sucesso!\n");
    } else {
        char erro[200];
        extrairTexto(resp.corpo, "erro", erro, sizeof(erro));
        printf("Carona nao cancelada! motivo: %s\n", erro[0] ? erro : "erro desconhecido");
    }
    free(resp.corpo);
}

//função para pedir ao servidor os avisos de trechos removidos (cancelados pelo motorista) e mostrá-los ao cliente
//GET    /api/clientes/avisos?email=...  -> lê os avisos (somente leitura)
//DELETE /api/clientes/avisos?email=...  -> apaga os avisos já mostrados, para não aparecerem de novo
void ver_notificacoes(Cliente *cliente){
    char emailEsc[200];
    char caminho[300];
    escaparURL(cliente->email, emailEsc, sizeof(emailEsc));
    snprintf(caminho, sizeof(caminho), "/api/clientes/avisos?email=%s", emailEsc);

    RespostaHTTP resp;
    if (!chamarAPI("GET", caminho, NULL, &resp)) {
        printf("O SERVIDOR ESTA OFFLINE\n");
        return;
    }

    //a resposta válida é um array JSON com status 200; qualquer outra coisa é um erro do servidor
    if (resp.status != 200) {
        char erro[200];
        extrairTexto(resp.corpo, "erro", erro, sizeof(erro));
        printf("Nao foi possivel obter os avisos. Motivo: %s\n", erro[0] ? erro : "erro desconhecido");
        free(resp.corpo);
        return;
    }

    cJSON *arrayAvisos = cJSON_Parse(resp.corpo);
    free(resp.corpo);

    if (!cJSON_IsArray(arrayAvisos)) {
        printf("Erro ao obter avisos.\n");
        cJSON_Delete(arrayAvisos);
        return;
    }

    int n = cJSON_GetArraySize(arrayAvisos);
    if (n == 0) {
        printf("NENHUM AVISO NOVO!\n");
    } else {
        printf("====================================\n");
        printf("   AVISOS DE TRECHOS CANCELADOS (%d)\n", n);
        printf("====================================\n");
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arrayAvisos, i);
            char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nomeMotorista"));
            char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origem"));
            char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(item, "destino"));
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(item, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(item, "hora"));
            printf("Aviso %d\n", i + 1);
            printf("O motorista %s cancelou o trecho que voce reservou:\n", nomeMotorista ? nomeMotorista : "(desconhecido)");
            printf("Origem: %s\n", cidadeOrigem ? cidadeOrigem : "-");
            printf("Destino: %s\n", cidadeDestino ? cidadeDestino : "-");
            printf("Data: %s\n", data ? data : "-");
            printf("Hora: %s\n", hora ? hora : "-");
            printf("====================================\n");
        }

        //marca os avisos como lidos
        RespostaHTTP respApagar;
        if (chamarAPI("DELETE", caminho, NULL, &respApagar)) {
            free(respApagar.corpo);
        }
    }

    cJSON_Delete(arrayAvisos);
}

void telaMenu(Cliente *cliente){
    int escolha = 0;
    int sair = 0;

    while(sair != 1){
        printf("====================================\n");
        printf("                MENU                \n");
        printf("====================================\n");
        printf(" 1- Buscar Carona\n");
        printf(" 2- Cancelar Reserva\n");
        printf(" 3- Ver minhas Caronas\n");
        printf(" 4- Ver Notificacoes\n");
        printf(" 5- Sair da Conta\n");
        printf("====================================\n");
        printf("Escolha uma opcao: ");
        scanf("%d", &escolha);

        if (escolha == 1) {
            buscarCarona(cliente);
        } else if (escolha == 2) {
            cancelar_carona(cliente);
        } else if (escolha == 3) {
            ver_caronas(cliente);
        } else if (escolha == 4) {
            ver_notificacoes(cliente);
        } else if (escolha == 5) {
            sair = 1;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

}

void telaLogin(Cliente *cliente){
    int escolha = 0;
    int sair = 0;
    int autenticado = 0;
    fflush(stdin);

    while (!autenticado && !sair) {
        int enviou = 0;
        RespostaHTTP resp = {0, NULL};
        int respondeu = 0;
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
                respondeu = enviarCadastro(cliente->nome, cliente->email, cliente->senha, 1, &resp);
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

        if (!respondeu) {
            printf("ERRO: O SERVIDOR ESTA OFFLINE\n");
            continue;
        }

        if (resp.status == 401) {
            printf("Email ou senha incorretos. Tente novamente.\n");
        } else if (resp.status == 200) {
            char nome[50];
            if (extrairTexto(resp.corpo, "nome", nome, sizeof(nome))) {
                printf("Login realizado com sucesso!\n");
                cliente->status = AUTENTICADO;
                strncpy(cliente->nome, nome, sizeof(cliente->nome) - 1);
                cliente->nome[sizeof(cliente->nome) - 1] = '\0';
                autenticado = 1;
                telaMenu(cliente);
            }
        } else {
            printf("Resposta inesperada do servidor (HTTP %d)\n", resp.status);
        }
        free(resp.corpo);
    }
}

void telaCadastro(Cliente *cliente){
    int escolha = 0;
    int sair = 0;
    int enviou = 0;
    RespostaHTTP resp = {0, NULL};
    int respondeu = 0;
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
            respondeu = enviarCadastro(cliente->nome, cliente->email, cliente->senha, 2, &resp);
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
        telaCadastro(cliente);
        return;
    } else if (resp.status == 201) {
        printf("Cadastro realizado com sucesso!\n");
        cliente->status = AUTENTICADO;
    } else {
        printf("Resposta desconhecida do servidor (HTTP %d): %s\n", resp.status, resp.corpo);
    }
    free(resp.corpo);
}

void telaInicial(Cliente *cliente){
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
                telaLogin(cliente);
                break;
            case 2:
                telaCadastro(cliente);
                break;
            case 3:
                //REST não mantém conexão aberta, então não há o que avisar ao servidor ao sair
                printf("Saindo...\n");
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
    char ip_servidor[100];

    Cliente *cliente = calloc(1, sizeof(Cliente));
    if (cliente == NULL) {
        perror("Memoria insuficiente");
        exit(EXIT_FAILURE);
    }

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
    if (!chamarAPI("GET", "/api/clientes", NULL, &teste)) {
        printf("conexao com o servidor nao estabelecida (%s)\n", baseURL);
        free(cliente);
        curl_global_cleanup();
        exit(EXIT_FAILURE);
    }
    free(teste.corpo);

    telaInicial(cliente);

    free(cliente);
    curl_global_cleanup();
    return 0;
}