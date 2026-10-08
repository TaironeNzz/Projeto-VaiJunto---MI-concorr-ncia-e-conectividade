#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <microhttpd.h>
#include "cJSON.h"
#include "grafomapa.h"

#define PORT 65432
//tamanho máximo aceito para o corpo de uma requisição
#define MAX_CORPO (64 * 1024)

//grafo do mapa para verificação de caminhos
Grafo *mapa;
//id do próximo trecho e contador de trechos cadastrados (persistidos em ArquivoID/id.txt e ArquivoID/contadorTrechos.txt)
//são protegidos pelo idMutex; cada trecho fica em trechosCadastrados/trecho<ID>.json, protegido pelo mutex do próprio trecho
int idTrecho = 0;
int contadorTrechos = 0;
//mutexes para os arquivos
pthread_mutex_t loginMotoristaMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t loginClienteMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t avisosMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t idMutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_TRECHOS 1000

typedef struct {
    pthread_mutex_t mutexTrecho;
    int ativo;
} TrechoLock;

TrechoLock travasTrechos[MAX_TRECHOS];
pthread_mutex_t gerencialTrechosMutex = PTHREAD_MUTEX_INITIALIZER;

// Função para inicializar/pegar o mutex de um trecho específico
// Os ids dos trechos só crescem, então a posição no vetor é id % MAX_TRECHOS (depois de MAX_TRECHOS ids a posição é reaproveitada).
// Dois ids só dividem o mesmo mutex se diferirem por múltiplo de MAX_TRECHOS, o que é inofensivo porque
// nenhuma função segura o mutex de mais de um trecho ao mesmo tempo.
pthread_mutex_t* obterMutexTrecho(int idTrecho) {
    if (idTrecho < 0) {
        return NULL;
    }
    int posicao = idTrecho % MAX_TRECHOS;
    pthread_mutex_lock(&gerencialTrechosMutex);
    if (!travasTrechos[posicao].ativo) {
        pthread_mutex_init(&travasTrechos[posicao].mutexTrecho, NULL);
        travasTrechos[posicao].ativo = 1;
    }
    pthread_mutex_t *m = &travasTrechos[posicao].mutexTrecho;
    pthread_mutex_unlock(&gerencialTrechosMutex);
    return m;
}

//Destroi os mutexes ao encerrar o servidor
void destruirGerenciadorDeTravas() {
    pthread_mutex_lock(&gerencialTrechosMutex);
    for (int i = 0; i < MAX_TRECHOS; i++) {
        if (travasTrechos[i].ativo) {
            pthread_mutex_destroy(&travasTrechos[i].mutexTrecho);
            travasTrechos[i].ativo = 0;
        }
    }
    pthread_mutex_unlock(&gerencialTrechosMutex);
    pthread_mutex_destroy(&gerencialTrechosMutex);
}

// Níveis de log disponíveis
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

// Mutex exclusivo para sincronizar a escrita de logs
pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

void log_mensagem(LogLevel level, const char *format, ...) {
    time_t agora = time(NULL);
    struct tm *t = localtime(&agora);
    char buffer_data[20];
    strftime(buffer_data, sizeof(buffer_data), "%Y-%m-%d %H:%M:%S", t);

    const char *níveis[] = {"INFO", "WARN", "ERROR"};

    pthread_mutex_lock(&logMutex);

    // 1. Escreve no arquivo de log no disco
    FILE *arquivo = fopen("logs/servidor.log", "a");
    if (arquivo != NULL) {
        va_list args1;
        va_start(args1, format);
        fprintf(arquivo, "[%s] [%s] ", buffer_data, níveis[level]);
        vfprintf(arquivo, format, args1);
        fprintf(arquivo, "\n");
        va_end(args1);
        fclose(arquivo);
    }

    // 2. Imprime no terminal (stdout)
    va_list args2;
    va_start(args2, format);
    printf("[%s] [%s] ", buffer_data, níveis[level]);
    vprintf(format, args2);
    printf("\n");
    va_end(args2);

    pthread_mutex_unlock(&logMutex);
}

//Caminhos usados pelos trechos: cada trecho fica no seu próprio arquivo trechosCadastrados/trecho<ID>.json
#define DIR_TRECHOS  "trechosCadastrados"
#define ARQ_ID       "ArquivoID/id.txt"
#define ARQ_CONTADOR "ArquivoID/contadorTrechos.txt"

//função que carrega o próximo idTrecho e o contador de trechos dos arquivos (chamada na inicialização)
void encontrarID(){
    FILE *arquivo = fopen(ARQ_ID, "r");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo de ID");
    } else {
        if (fscanf(arquivo, "%d", &idTrecho) != 1) {
            fprintf(stderr, "Erro ao ler o ID do arquivo\n");
            idTrecho = 0;
        }
        fclose(arquivo);
    }

    FILE *arquivo2 = fopen(ARQ_CONTADOR, "r");
    if (arquivo2 == NULL) {
        perror("Erro ao abrir o arquivo de contador de trechos");
    } else {
        if (fscanf(arquivo2, "%d", &contadorTrechos) != 1) {
            fprintf(stderr, "Erro ao ler o contador de trechos do arquivo\n");
            contadorTrechos = 0;
        }
        fclose(arquivo2);
    }
}

//grava o id e o contador de trechos nos arquivos. Chamar com idMutex travado (exceto na inicialização)
void atualizarID(int novoID, int novoContador) {
    FILE *arquivo = fopen(ARQ_ID, "w");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo de ID para escrita");
        return;
    }
    fprintf(arquivo, "%d", novoID);
    fclose(arquivo);

    FILE *arquivo2 = fopen(ARQ_CONTADOR, "w");
    if (arquivo2 == NULL) {
        perror("Erro ao abrir o arquivo de contador de trechos");
        return;
    }
    fprintf(arquivo2, "%d", novoContador);
    fclose(arquivo2);
}

//reserva 'quantidade' ids seguidos e devolve o primeiro. O id já é gravado no arquivo, então nunca se repete
int reservarIdsTrecho(int quantidade) {
    pthread_mutex_lock(&idMutex);
    int primeiro = idTrecho;
    idTrecho += quantidade;
    atualizarID(idTrecho, contadorTrechos);
    pthread_mutex_unlock(&idMutex);
    return primeiro;
}

//soma 'delta' ao contador de trechos cadastrados (+1 ao criar, -1 ao cancelar/expirar) e grava no arquivo
void ajustarContadorTrechos(int delta) {
    pthread_mutex_lock(&idMutex);
    contadorTrechos += delta;
    if (contadorTrechos < 0) contadorTrechos = 0;
    atualizarID(idTrecho, contadorTrechos);
    pthread_mutex_unlock(&idMutex);
}

//devolve o limite (exclusivo) dos ids já distribuídos; usado para percorrer todos os arquivos de trechos
int lerLimiteIdTrechos(void) {
    pthread_mutex_lock(&idMutex);
    int limite = idTrecho;
    pthread_mutex_unlock(&idMutex);
    return limite;
}

//monta o caminho do arquivo de um trecho
static void caminhoTrecho(int id, char *destino, size_t tamanho) {
    snprintf(destino, tamanho, "%s/trecho%d.json", DIR_TRECHOS, id);
}

//lê um arquivo inteiro para memória (quem chamar libera com free). Devolve NULL se não existir ou estiver vazio
static char *lerArquivoInteiro(const char *caminho) {
    FILE *arquivo = fopen(caminho, "r");
    if (arquivo == NULL) return NULL;

    fseek(arquivo, 0, SEEK_END);
    long tamanho = ftell(arquivo);
    rewind(arquivo);
    if (tamanho <= 0) {
        fclose(arquivo);
        return NULL;
    }

    char *conteudo = malloc(tamanho + 1);
    if (conteudo == NULL) {
        fclose(arquivo);
        return NULL;
    }
    size_t lidos = fread(conteudo, 1, tamanho, arquivo);
    conteudo[lidos] = '\0';
    fclose(arquivo);
    return conteudo;
}

//lê o trecho do arquivo
//Devolve NULL se o trecho não existe (nunca foi criado, foi cancelado ou expirou)
cJSON *lerTrechoNoLock(int id) {
    char caminho[128];
    caminhoTrecho(id, caminho, sizeof(caminho));
    char *conteudo = lerArquivoInteiro(caminho);
    if (conteudo == NULL) return NULL;
    cJSON *trecho = cJSON_Parse(conteudo);
    free(conteudo);
    return trecho;
}

//versão que trava e destrava o mutex do trecho sozinha (para leituras simples)
cJSON *lerTrecho(int id) {
    pthread_mutex_t *mutexTrecho = obterMutexTrecho(id);
    if (mutexTrecho == NULL) return NULL;
    pthread_mutex_lock(mutexTrecho);
    cJSON *trecho = lerTrechoNoLock(id);
    pthread_mutex_unlock(mutexTrecho);
    return trecho;
}

//grava o trecho no arquivo (deve chamar com a trava)
//Escreve num .tmp e usa rename, assim o arquivo nunca fica pela metade. Retorna 1 em sucesso e 0 em erro
static int gravarTrechoNoLock(int id, cJSON *trecho) {
    char destino[128];
    char temporario[128];
    caminhoTrecho(id, destino, sizeof(destino));
    snprintf(temporario, sizeof(temporario), "%s/trecho%d.tmp", DIR_TRECHOS, id);

    char *saida = cJSON_PrintUnformatted(trecho);
    if (saida == NULL) return 0;

    FILE *arquivo = fopen(temporario, "w");
    if (arquivo == NULL) {
        perror("Erro ao abrir o arquivo do trecho");
        free(saida);
        return 0;
    }
    int ok = (fprintf(arquivo, "%s\n", saida) >= 0);
    ok = (fclose(arquivo) == 0) && ok;
    free(saida);

    if (ok && rename(temporario, destino) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(temporario);
    }
    return ok;
}

//garante que idTrecho seja maior que 'id' (usado na migração, para nunca reaproveitar um id existente)
static void garantirIdMinimo(int id) {
    pthread_mutex_lock(&idMutex);
    if (id >= idTrecho) {
        idTrecho = id + 1;
        atualizarID(idTrecho, contadorTrechos);
    }
    pthread_mutex_unlock(&idMutex);
}

//conta os arquivos de trechos que existem de verdade e acerta o contador (chamada na inicialização)
void recalcularContadorTrechos(void) {
    int limite = lerLimiteIdTrechos();
    int total = 0;
    for (int i = 0; i < limite; i++) {
        char caminho[128];
        caminhoTrecho(i, caminho, sizeof(caminho));
        if (access(caminho, F_OK) == 0) total++;
    }
    pthread_mutex_lock(&idMutex);
    contadorTrechos = total;
    atualizarID(idTrecho, contadorTrechos);
    pthread_mutex_unlock(&idMutex);
}

//função auxiliar para remover trechos que chegaram na data de partida
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

//função para remover/deletar arquivos de trechos que expiraram
void limparTrechosExpirados(void) {
    int limiteId = lerLimiteIdTrechos();

    for (int i = 0; i < limiteId; i++) {
        pthread_mutex_t *mutexTrecho = obterMutexTrecho(i);
        if (mutexTrecho == NULL) continue;

        int removido = 0;
        pthread_mutex_lock(mutexTrecho);
        cJSON *trecho = lerTrechoNoLock(i);
        if (trecho != NULL) {
            char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "data"));
            char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "hora"));

            if (trechoExpirado(data, hora)) {
                char caminhoArquivo[128];
                caminhoTrecho(i, caminhoArquivo, sizeof(caminhoArquivo));
                if (remove(caminhoArquivo) == 0) {
                    removido = 1;
                }
            }
            cJSON_Delete(trecho);
        }
        pthread_mutex_unlock(mutexTrecho);

        //o contador é ajustado fora do mutex do trecho para nunca travar dois mutexes ao mesmo tempo
        if (removido) {
            ajustarContadorTrechos(-1);
            log_mensagem(LOG_INFO, "Trecho ID %d expirado e removido", i);
        }
    }
}

//rotina para a thread de limpeza
void *rotinaLimpezaTrechos(void *arg) {
    (void)arg;
    while (1) {
        limparTrechosExpirados();
        sleep(60);
    }
    return NULL;
}

//Resposta HTTP montada pelos handlers. Cada requisição tem a sua (fica na pilha da thread que atende o pedido).
typedef struct {
    int status;     //código HTTP (0 = nenhum handler respondeu)
    char *corpo;    //corpo JSON (alocado com malloc; liberado depois do envio)
} Resposta;

// Guarda o status e o corpo da resposta. Mantém a mesma assinatura de antes, então os handlers continuam iguais.
void enviar_resposta_http(Resposta *resp, int status_code, const char *status_text, const char *json_body) {
    (void)status_text;
    free(resp->corpo);
    resp->status = status_code;
    resp->corpo = strdup(json_body != NULL ? json_body : "");
}

//função para cadastrar o cliente
void cadastrarCliente(cJSON *jsonLogin, Resposta *resp, FILE *arquivoLogin){
    char dadosLogin[1024] = {0};
    int emailEncontrado = 0;
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));

    if (emailBuscado == NULL || nome == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Dados incompletos para cadastro de cliente\"}");
        return;
    }

    pthread_mutex_lock(&loginClienteMutex);
    rewind(arquivoLogin);

    while(fgets(dadosLogin, sizeof(dadosLogin), arquivoLogin) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                enviar_resposta_http(resp, 409, "Conflict", "{\"erro\": \"Email ja cadastrado\"}");
                emailEncontrado = 1;
                break;
            }
        }
        cJSON_Delete(dadosJson);
    }
    if (!emailEncontrado) {
        char *saida = cJSON_PrintUnformatted(jsonLogin);
        fprintf(arquivoLogin, "%s\n", saida);
        log_mensagem(LOG_INFO, "Novo cliente cadastrado - Nome: %s | Email: %s", nome, emailBuscado);
        enviar_resposta_http(resp, 201, "Created", "{\"mensagem\": \"Cadastro realizado com sucesso\"}");
        free(saida);
    }
    pthread_mutex_unlock(&loginClienteMutex);
    
    fflush(arquivoLogin);
}

//função para cadastrar o motorista
void cadastrarMotorista(cJSON *jsonLogin, Resposta *resp, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));

    if (emailBuscado == NULL || nome == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Dados incompletos para cadastro de motorista\"}");
        return;
    }

    pthread_mutex_lock(&loginMotoristaMutex);
    rewind(arquivo);

    while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                enviar_resposta_http(resp, 409, "Conflict", "{\"erro\": \"Email ja cadastrado\"}");
                emailEncontrado = 1;
                break;
                }
            }
        cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            char *saida = cJSON_PrintUnformatted(jsonLogin);
            log_mensagem(LOG_INFO, "Novo motorista cadastrado - Nome: %s | Email: %s", nome, emailBuscado);
            fprintf(arquivo, "%s\n", saida);
            fflush(arquivo);
            enviar_resposta_http(resp, 201, "Created", "{\"mensagem\": \"Cadastro realizado com sucesso\"}");
            free(saida);
        }
    pthread_mutex_unlock(&loginMotoristaMutex);
}

int cadastrarTrecho(cJSON *jsonLogin, Resposta *resp){
    if (mapa == NULL) {
        printf("Mapa nao carregado. Nao e possivel cadastrar trecho.\n");
        enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Mapa nao carregado\"}");
        return 0;
    }

    char *emailMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailMotorista"));
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
    int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "capacidade"));
    char *data = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "data"));
    char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "hora"));
    float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "preco"));
    cJSON *arrayClientes = cJSON_GetObjectItem(jsonLogin, "clientes");
    cJSON *arrayNomesClientes = cJSON_GetObjectItem(jsonLogin, "nomesClientes");

    if (nomeMotorista == NULL || emailMotorista == NULL || origem == NULL || destino == NULL || data == NULL || hora == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Dados incompletos para cadastro de trecho\"}");
        return 0;
    }

    //verifica se tem caminho pelo grafo do mapa
    if (existeCaminhoBFSPorNome(mapa, origem, destino)) {

        //pega um id novo (já gravado no arquivo de id)
        int meuIdTrecho = reservarIdsTrecho(1);

        cJSON *trecho = cJSON_CreateObject();
        cJSON_AddNumberToObject(trecho, "idTrecho", meuIdTrecho);
        cJSON_AddStringToObject(trecho, "nomeMotorista", nomeMotorista);
        cJSON_AddStringToObject(trecho, "emailMotorista", emailMotorista);
        cJSON_AddStringToObject(trecho, "origem", origem);
        cJSON_AddStringToObject(trecho, "destino", destino);
        cJSON_AddStringToObject(trecho, "data", data);
        cJSON_AddStringToObject(trecho, "hora", hora);
        cJSON_AddNumberToObject(trecho, "capacidade", capacidade);
        cJSON_AddNumberToObject(trecho, "preco", preco);
        cJSON_AddItemToObject(trecho, "clientes", arrayClientes != NULL ? cJSON_Duplicate(arrayClientes, 1) : cJSON_CreateArray());
        cJSON_AddItemToObject(trecho, "nomesClientes", arrayNomesClientes != NULL ? cJSON_Duplicate(arrayNomesClientes, 1) : cJSON_CreateArray());

        //grava o trecho no arquivo próprio, protegido pelo mutex do trecho
        int gravou = 0;
        pthread_mutex_t *mutexTrecho = obterMutexTrecho(meuIdTrecho);
        if (mutexTrecho != NULL) {
            pthread_mutex_lock(mutexTrecho);
            gravou = gravarTrechoNoLock(meuIdTrecho, trecho);
            pthread_mutex_unlock(mutexTrecho);
        }
        cJSON_Delete(trecho);

        if (!gravou) {
            log_mensagem(LOG_ERROR, "Falha ao gravar o arquivo do trecho ID %d", meuIdTrecho);
            enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Falha ao cadastrar trecho\"}");
            return 0;
        }

        ajustarContadorTrechos(1);

        log_mensagem(LOG_INFO, "Trecho ID %d cadastrado por %s (%s -> %s)", meuIdTrecho, nomeMotorista, origem, destino);
        enviar_resposta_http(resp, 201, "Created", "{\"mensagem\": \"Trecho cadastrado com sucesso\"}");
        return 1;
    } else {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Caminho nao encontrado\"}");
        return 0;
    }
}

//função para pegar os trechos de um motorista dos arquivos e retornar para o motorista
void listar_trechos(cJSON *jsonLogin, Resposta *resp){
    cJSON *arrayResposta = cJSON_CreateArray();
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    char *emailMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));

    log_mensagem(LOG_INFO, "Busca de trechos feito pelo motorista: %s", emailMotorista);

    int limite = lerLimiteIdTrechos();
    for (int i = 0; i < limite; i++) {
        cJSON *trechosJson = lerTrecho(i);
        if (trechosJson == NULL) continue;

        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
        char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));
        cJSON *arrayNomesClientes = cJSON_GetObjectItem(trechosJson, "nomesClientes");
        char *emailMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "emailMotorista"));

        if (nomeMotoristaTrecho != NULL && nomeMotorista != NULL && emailMotorista != NULL && emailMotoristaTrecho != NULL
            && strcmp(nomeMotorista, nomeMotoristaTrecho) == 0 && strcmp(emailMotorista, emailMotoristaTrecho) == 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddStringToObject(item, "origem", cidadeOrigem);
            cJSON_AddStringToObject(item, "destino", cidadeDestino);
            cJSON_AddNumberToObject(item, "capacidade", capacidade);
            cJSON_AddStringToObject(item, "data", data);
            cJSON_AddStringToObject(item, "hora", hora);
            cJSON_AddNumberToObject(item, "preco", preco);
            cJSON_AddItemToObject(item, "nomesClientes", cJSON_Duplicate(arrayNomesClientes, 1));
            cJSON_AddItemToArray(arrayResposta, item);
        }
        cJSON_Delete(trechosJson);
    }
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    enviar_resposta_http(resp, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para acessar o arquivo de login e verificar se o email e senha estão certos
void loginMotorista(cJSON *jsonLogin, Resposta *resp, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));

    if (emailBuscado == NULL || senhaBuscada == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Dados incompletos para login de motorista\"}");
        return;
    }

    pthread_mutex_lock(&loginMotoristaMutex);
    rewind(arquivo);

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
                log_mensagem(LOG_INFO, "Motorista logado - Nome: %s | Email: %s", nome, emailBuscado);
                enviar_resposta_http(resp, 200, "OK", resposta);
                emailEncontrado = 1;
                cJSON_Delete(respostaJson);
                break;
                }
            }
        cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            log_mensagem(LOG_WARN, "Falha de autenticação para o email: %s", emailBuscado);
            enviar_resposta_http(resp, 401, "Unauthorized", "{\"erro\": \"Credenciais invalidas\"}");
        }
    pthread_mutex_unlock(&loginMotoristaMutex);
}

//Função auxiliar: lê avisos/avisos.json e devolve o objeto cJSON (chave = email, valor = array de avisos)
//Se o arquivo não existe ou está vazio, devolve um objeto vazio. Retorna 1 em sucesso e 0 em erro.
//Deve ser chamada com avisosMutex travado. Quem chamar deve liberar *raiz com cJSON_Delete.
static int carregarAvisos(cJSON **raiz) {
    *raiz = NULL;
    FILE *arquivo = fopen("Avisos/avisos.json", "r");
    if (arquivo != NULL) {
        fseek(arquivo, 0, SEEK_END);
        long tamanho = ftell(arquivo);
        rewind(arquivo);

        if (tamanho > 0) {
            char *conteudo = malloc(tamanho + 1);
            if (conteudo == NULL) {
                log_mensagem(LOG_ERROR, "Falha de memória ao ler arquivo de avisos");
                fclose(arquivo);
                return 0;
            }
            size_t lidos = fread(conteudo, 1, tamanho, arquivo);
            conteudo[lidos] = '\0';
            *raiz = cJSON_Parse(conteudo);
            free(conteudo);

            //não devolve nada se o arquivo estiver corrompido, para ninguém sobrescrevê-lo e perder avisos
            if (*raiz == NULL || !cJSON_IsObject(*raiz)) {
                log_mensagem(LOG_ERROR, "Arquivo de avisos com formato invalido");
                cJSON_Delete(*raiz);
                *raiz = NULL;
                fclose(arquivo);
                return 0;
            }
        }
        fclose(arquivo);
    }

    if (*raiz == NULL) {
        *raiz = cJSON_CreateObject();
        if (*raiz == NULL) {
            log_mensagem(LOG_ERROR, "Falha ao criar objeto JSON de avisos");
            return 0;
        }
    }
    return 1;
}

//Função auxiliar: grava o objeto em avisos/avisos.json usando um arquivo temporário (avisos.tmp) e rename,
//para nunca deixar o JSON pela metade. Retorna 1 em sucesso e 0 em erro. Chamar com avisosMutex travado.
static int salvarAvisos(cJSON *raiz) {
    char *saida = cJSON_Print(raiz);
    if (saida == NULL) {
        log_mensagem(LOG_ERROR, "Falha ao gerar string JSON dos avisos");
        return 0;
    }

    int ok = 0;
    FILE *temp = fopen("Avisos/avisos.tmp", "w");
    if (temp == NULL) {
        log_mensagem(LOG_ERROR, "Falha ao abrir arquivo temporario de avisos para escrita");
    } else {
        ok = (fprintf(temp, "%s\n", saida) >= 0);
        ok = (fclose(temp) == 0) && ok;
        if (ok && rename("Avisos/avisos.tmp", "Avisos/avisos.json") != 0) {
            ok = 0;
        }
        if (!ok) {
            log_mensagem(LOG_ERROR, "Falha ao gravar arquivo de avisos");
            remove("Avisos/avisos.tmp");
        }
    }
    free(saida);
    return ok;
}

//Função para adicionar no arquivo de avisos os trechos removidos
void adicionarAvisoTrechoRemovido(cJSON *trechoJson) {
    char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "origem"));
    char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "destino"));
    char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "data"));
    char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "hora"));
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "nomeMotorista"));
    cJSON *clientesArray = cJSON_GetObjectItem(trechoJson, "clientes");

    if (origem == NULL || destino == NULL || data == NULL || hora == NULL || nomeMotorista == NULL ||
        clientesArray == NULL || !cJSON_IsArray(clientesArray)) {
        log_mensagem(LOG_ERROR, "Falha ao adicionar aviso de trecho removido: dados incompletos");
        return;
    }

    int n = cJSON_GetArraySize(clientesArray);
    if (n == 0) {
        return; //nenhum cliente reservou esse trecho, não há ninguém para avisar
    }

    pthread_mutex_lock(&avisosMutex);

    //1. Lê o arquivo de avisos (ou cria um objeto vazio se ainda não existir)
    cJSON *raiz = NULL;
    if (!carregarAvisos(&raiz)) {
        log_mensagem(LOG_ERROR, "Aviso de trecho removido nao adicionado");
        pthread_mutex_unlock(&avisosMutex);
        return;
    }

    //2. Para cada cliente do trecho, adiciona o aviso no array da chave com o email dele
    int adicionados = 0;
    for (int i = 0; i < n; i++) {
        char *emailCliente = cJSON_GetStringValue(cJSON_GetArrayItem(clientesArray, i));
        if (emailCliente == NULL) {
            log_mensagem(LOG_WARN, "Falha ao adicionar aviso de trecho removido para cliente: email nulo");
            continue;
        }

        cJSON *listaAvisos = cJSON_GetObjectItemCaseSensitive(raiz, emailCliente);
        if (listaAvisos == NULL || !cJSON_IsArray(listaAvisos)) {
            //email ainda não existe no arquivo: cria a chave com um array novo
            if (listaAvisos != NULL) {
                cJSON_DeleteItemFromObjectCaseSensitive(raiz, emailCliente);
            }
            listaAvisos = cJSON_CreateArray();
            if (listaAvisos == NULL) continue;
            cJSON_AddItemToObject(raiz, emailCliente, listaAvisos);
        }
        //se o email já existe, apenas acrescenta o novo aviso no array existente

        cJSON *aviso = cJSON_CreateObject();
        if (aviso == NULL) continue;
        cJSON_AddStringToObject(aviso, "nomeMotorista", nomeMotorista);
        cJSON_AddStringToObject(aviso, "origem", origem);
        cJSON_AddStringToObject(aviso, "destino", destino);
        cJSON_AddStringToObject(aviso, "data", data);
        cJSON_AddStringToObject(aviso, "hora", hora);
        cJSON_AddItemToArray(listaAvisos, aviso);
        adicionados++;
    }

    //3. Grava de volta no arquivo de avisos (avisos.tmp + rename, independente dos arquivos dos trechos)
    if (salvarAvisos(raiz)) {
        log_mensagem(LOG_INFO, "Aviso de trecho removido adicionado para %d cliente(s): %s -> %s em %s %s pelo motorista %s",
                     adicionados, origem, destino, data, hora, nomeMotorista);
    }

    cJSON_Delete(raiz);
    pthread_mutex_unlock(&avisosMutex);
}

//Função para retornar ao cliente os avisos de trechos removidos
//Requisição: GET /api/clientes/avisos?email=cliente@email.com
//Resposta: array JSON com os avisos do cliente (ou "[]" se não houver nenhum)
//Erros: "EMAIL_INVALIDO" (requisição sem email) e "FALHA_LER_AVISOS" (arquivo ilegível)
void listarAvisosCliente(cJSON *jsonLogin, Resposta *resp) {
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    if (emailCliente == NULL) {
        emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    }
    if (emailCliente == NULL) {
        log_mensagem(LOG_WARN, "Consulta de avisos sem email do cliente");
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Email invalido\"}");
        return;
    }

    pthread_mutex_lock(&avisosMutex);

    cJSON *raiz = NULL;
    if (!carregarAvisos(&raiz)) {
        pthread_mutex_unlock(&avisosMutex);
        enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Falha ao ler avisos\"}");
        return;
    }

    //a chave do objeto é o email do cliente; o valor é o array de avisos dele
    cJSON *listaAvisos = cJSON_GetObjectItemCaseSensitive(raiz, emailCliente);
    int temAvisos = (listaAvisos != NULL && cJSON_IsArray(listaAvisos));

    char *resposta = temAvisos ? cJSON_PrintUnformatted(listaAvisos) : strdup("[]");
    if (resposta == NULL) {
        log_mensagem(LOG_ERROR, "Falha ao gerar resposta de avisos para o cliente: %s", emailCliente);
        cJSON_Delete(raiz);
        pthread_mutex_unlock(&avisosMutex);
        enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Falha ao ler avisos\"}");
        return;
    }

    enviar_resposta_http(resp, 200, "OK", resposta);
    log_mensagem(LOG_INFO, "Consulta de avisos - Cliente: %s | Avisos enviados: %d", emailCliente,
                 temAvisos ? cJSON_GetArraySize(listaAvisos) : 0);

    free(resposta);
    cJSON_Delete(raiz);
    pthread_mutex_unlock(&avisosMutex);
}

//DELETE /api/clientes/avisos -> apaga os avisos do cliente (o GET só lê, porque GET não pode alterar dados)
void apagarAvisosCliente(cJSON *jsonLogin, Resposta *resp) {
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    if (emailCliente == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Email invalido\"}");
        return;
    }

    pthread_mutex_lock(&avisosMutex);
    cJSON *raiz = NULL;
    if (!carregarAvisos(&raiz)) {
        pthread_mutex_unlock(&avisosMutex);
        enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Falha ao ler avisos\"}");
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(raiz, emailCliente);
    int ok = salvarAvisos(raiz);
    cJSON_Delete(raiz);
    pthread_mutex_unlock(&avisosMutex);

    if (ok) {
        enviar_resposta_http(resp, 200, "OK", "{\"mensagem\": \"Avisos removidos\"}");
    } else {
        enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Falha ao apagar avisos\"}");
    }
}

//função para cancelar trecho de um motorista
void cancelarTrechoMotorista(cJSON *jsonLogin, Resposta *resp){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    char *emailMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    if (emailMotorista == NULL){
        log_mensagem(LOG_ERROR, "Falha ao cancelar trecho do motorista %s -> email nulo", nomeMotorista);
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Trecho não encontrado\"}");
        return;
    }
    int trechoEncontrado = 0;

    //só o mutex do trecho selecionado fica travado; os outros trechos continuam livres
    pthread_mutex_t *mutexTrecho = obterMutexTrecho(idSelecionado);
    if (mutexTrecho != NULL) {
        pthread_mutex_lock(mutexTrecho);
        cJSON *trechoJson = lerTrechoNoLock(idSelecionado);
        if (trechoJson != NULL) {
            char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "nomeMotorista"));
            char *emailMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "emailMotorista"));

            if (nomeMotorista != NULL && nomeMotoristaTrecho != NULL && emailMotoristaTrecho != NULL &&
                strcmp(nomeMotorista, nomeMotoristaTrecho) == 0 && strcmp(emailMotorista, emailMotoristaTrecho) == 0) {
                char caminho[128];
                caminhoTrecho(idSelecionado, caminho, sizeof(caminho));
                if (remove(caminho) == 0) {
                    adicionarAvisoTrechoRemovido(trechoJson);
                    log_mensagem(LOG_INFO, "Trecho ID %d cancelado pelo motorista %s", idSelecionado, emailMotorista);
                    trechoEncontrado = 1;
                } else {
                    perror("Erro ao remover o arquivo do trecho");
                }
            }
            cJSON_Delete(trechoJson);
        }
        pthread_mutex_unlock(mutexTrecho);
    }

    if (trechoEncontrado) {
        ajustarContadorTrechos(-1);
        enviar_resposta_http(resp, 200, "OK", "{\"mensagem\": \"Trecho cancelado com sucesso\"}");
    } else {
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"Trecho não encontrado\"}");
    }
}

//função para um motorista cadastrar sua rota informando os trechos
void cadastrarRota(cJSON *jsonLogin, Resposta *resp){
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    cJSON *trechosArray = cJSON_GetObjectItem(jsonLogin, "trechos");

    if (mapa == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Mapa não carregado\"}");
        return;
    }
    if (trechosArray == NULL || !cJSON_IsArray(trechosArray) || nomeMotorista == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Rota invalida\"}");
        return;
    }

    int totalTrechos = cJSON_GetArraySize(trechosArray);
    if (totalTrechos == 0) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"ROTA_INVALIDA\"}");
        return;
    }

    char *destinoAnterior = NULL;
    for (int i = 0; i < totalTrechos; i++) {
        cJSON *trecho = cJSON_GetArrayItem(trechosArray, i);
        if (trecho == NULL){
            enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
            return;
        }
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "origem"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "destino"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "hora"));
        char *email = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "emailMotorista"));

        if (origem == NULL || destino == NULL || data == NULL || hora == NULL || email == NULL) {
            enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"ROTA_INVALIDA\"}");
            return;
        }
        if (!existeCaminhoBFSPorNome(mapa, origem, destino)) {
            enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
            return;
        }

        if (destinoAnterior != NULL && strcmp(origem, destinoAnterior) != 0) {
            enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"ROTA_DESCONECTADA\"}");
            return;
        }
        destinoAnterior = destino;
    }

    //reserva de uma vez todos os ids da rota (gravados no arquivo de id)
    int primeiroId = reservarIdsTrecho(totalTrechos);
    int gravados = 0;
    int falhou = 0;

    for (int i = 0; i < totalTrechos; i++) {
        cJSON *trechoOrigem = cJSON_GetArrayItem(trechosArray, i);
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoOrigem, "origem"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoOrigem, "destino"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechoOrigem, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechoOrigem, "hora"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoOrigem, "capacidade"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoOrigem, "preco"));
        char *email = cJSON_GetStringValue(cJSON_GetObjectItem(trechoOrigem, "emailMotorista"));
        int idAtual = primeiroId + i;

        cJSON *trechoSalvar = cJSON_CreateObject();
        cJSON_AddNumberToObject(trechoSalvar, "idTrecho", idAtual);
        cJSON_AddStringToObject(trechoSalvar, "nomeMotorista", nomeMotorista);
        cJSON_AddStringToObject(trechoSalvar, "emailMotorista", email);
        cJSON_AddStringToObject(trechoSalvar, "origem", origem);
        cJSON_AddStringToObject(trechoSalvar, "destino", destino);
        cJSON_AddStringToObject(trechoSalvar, "data", data);
        cJSON_AddStringToObject(trechoSalvar, "hora", hora);
        cJSON_AddNumberToObject(trechoSalvar, "capacidade", capacidade);
        cJSON_AddNumberToObject(trechoSalvar, "preco", preco);

        cJSON *arrayClientesVazio = cJSON_CreateArray();
        cJSON *arrayNomesClientesVazio = cJSON_CreateArray();
        cJSON_AddItemToObject(trechoSalvar, "nomesClientes", arrayNomesClientesVazio);
        cJSON_AddItemToObject(trechoSalvar, "clientes", arrayClientesVazio);

        int ok = 0;
        pthread_mutex_t *mutexTrecho = obterMutexTrecho(idAtual);
        if (mutexTrecho != NULL) {
            pthread_mutex_lock(mutexTrecho);
            ok = gravarTrechoNoLock(idAtual, trechoSalvar);
            pthread_mutex_unlock(mutexTrecho);
        }
        cJSON_Delete(trechoSalvar);

        if (!ok) {
            falhou = 1;
            break;
        }
        gravados++;
    }

    if (falhou) {
        //desfaz os trechos já gravados, para a rota não ficar cadastrada pela metade
        for (int i = 0; i < gravados; i++) {
            int idAtual = primeiroId + i;
            pthread_mutex_t *mutexTrecho = obterMutexTrecho(idAtual);
            if (mutexTrecho == NULL) continue;
            char caminho[128];
            caminhoTrecho(idAtual, caminho, sizeof(caminho));
            pthread_mutex_lock(mutexTrecho);
            remove(caminho);
            pthread_mutex_unlock(mutexTrecho);
        }
        log_mensagem(LOG_ERROR, "Falha ao gravar a rota do motorista %s; trechos ja gravados foram desfeitos", nomeMotorista);
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
        return;
    }

    ajustarContadorTrechos(totalTrechos);
    log_mensagem(LOG_INFO, "Rota (multi-trechos) cadastrada por %s | Total de trechos: %d", nomeMotorista, totalTrechos);
    enviar_resposta_http(resp, 201, "Created", "{\"mensagem\": \"ROTA_CADASTRADA\"}");
}

//função para acessar o arquivo de login e verificar se o email e senha do cliente estão certos 
void loginCliente(cJSON *jsonLogin, Resposta *resp, FILE *arquivoLogin){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));

    if (emailBuscado == NULL || senhaBuscada == NULL) {
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"Dados incompletos para login de cliente\"}");
        return;
    }

    pthread_mutex_lock(&loginClienteMutex);
    rewind(arquivoLogin);

    while(fgets(dadosLogin, sizeof(dadosLogin), arquivoLogin) != NULL) {
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
                enviar_resposta_http(resp, 200, "OK", resposta);
                log_mensagem(LOG_INFO, "Cliente logado - Nome: %s | Email: %s", nome, emailBuscado);
                emailEncontrado = 1;
                cJSON_Delete(respostaJson);
                break;
            }
        }
        cJSON_Delete(dadosJson);
    }
    if (!emailEncontrado) {
        log_mensagem(LOG_WARN, "Falha de autenticação para o email: %s", emailBuscado);
        enviar_resposta_http(resp, 401, "Unauthorized", "{\"erro\": \"NÃO_AUTENTICADO\"}");
    }
    pthread_mutex_unlock(&loginClienteMutex);
}

//função para buscar as caronas que o usuário pediu
void buscar_carona(cJSON *jsonLogin, Resposta *resp){
    cJSON *arrayResposta = cJSON_CreateArray();
    char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *destinoBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));
    char *dataBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "data"));

    int limite = lerLimiteIdTrechos();
    for (int i = 0; i < limite; i++) {
        cJSON *trechosJson = lerTrecho(i);
        if (trechosJson == NULL) continue;

        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

        if (cidadeOrigem != NULL && cidadeDestino != NULL && origemBuscada != NULL && destinoBuscado != NULL
            && dataBuscada != NULL && data != NULL && (strcmp(dataBuscada, data) == 0 || strcmp(dataBuscada, "dd/mm/aaaa") == 0) &&
            strcmp(cidadeOrigem, origemBuscada) == 0 && strcmp(cidadeDestino, destinoBuscado) == 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddStringToObject(item, "nomeMotorista", nomeMotorista);
            cJSON_AddStringToObject(item, "origem", cidadeOrigem);
            cJSON_AddStringToObject(item, "destino", cidadeDestino);
            cJSON_AddNumberToObject(item, "capacidade", capacidade);
            cJSON_AddStringToObject(item, "data", data);
            cJSON_AddStringToObject(item, "hora", hora);
            cJSON_AddNumberToObject(item, "preco", preco);
            cJSON_AddItemToArray(arrayResposta, item);
        }
        cJSON_Delete(trechosJson);
    }
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    enviar_resposta_http(resp, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//resultados possíveis da tentativa de reservar um assento
#define RESERVA_OK              1
#define RESERVA_NAO_ENCONTRADO  0
#define RESERVA_SEM_ASSENTO    -1
#define RESERVA_ERRO           -2

//Reserva um assento no trecho idSelecionado (lido de jsonLogin), travando só o mutex desse trecho.
//Se jsonTrecho != NULL e a reserva der certo, devolve nele o JSON do trecho atualizado (quem chamar libera com free).
static int reservarAssentoTrecho(cJSON *jsonLogin, char **jsonTrecho) {
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *emailStr = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    char *nomeCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nomeCliente"));
    char *cidadeOrigemCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));
    char *cidadeDestinoCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destino"));

    if (jsonTrecho != NULL) *jsonTrecho = NULL;

    pthread_mutex_t *mutexTrecho = obterMutexTrecho(idSelecionado);
    if (mutexTrecho == NULL) return RESERVA_NAO_ENCONTRADO;

    int resultado = RESERVA_NAO_ENCONTRADO;

    pthread_mutex_lock(mutexTrecho);
    cJSON *trechoJson = lerTrechoNoLock(idSelecionado);
    if (trechoJson != NULL) {
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechoJson, "destino"));

        if (cidadeOrigem != NULL && cidadeDestino != NULL &&
            cidadeOrigemCliente != NULL && cidadeDestinoCliente != NULL &&
            strcmp(cidadeOrigem, cidadeOrigemCliente) == 0 &&
            strcmp(cidadeDestino, cidadeDestinoCliente) == 0) {

            int capacidadeAtual = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));
            if (capacidadeAtual > 0) {
                cJSON_ReplaceItemInObject(trechoJson, "capacidade", cJSON_CreateNumber(capacidadeAtual - 1));
                cJSON *arrayClientes = cJSON_GetObjectItem(trechoJson, "clientes");
                cJSON *arrayNomesClientes = cJSON_GetObjectItem(trechoJson, "nomesClientes");
                if (emailStr != NULL && arrayClientes != NULL) {
                    cJSON_AddItemToArray(arrayClientes, cJSON_CreateString(emailStr));
                }
                if (nomeCliente != NULL && arrayNomesClientes != NULL) {
                    cJSON_AddItemToArray(arrayNomesClientes, cJSON_CreateString(nomeCliente));
                }

                if (gravarTrechoNoLock(idSelecionado, trechoJson)) {
                    resultado = RESERVA_OK;
                    if (jsonTrecho != NULL) {
                        *jsonTrecho = cJSON_PrintUnformatted(trechoJson);
                    }
                } else {
                    log_mensagem(LOG_ERROR, "Falha ao gravar a reserva no trecho ID %d", idSelecionado);
                    resultado = RESERVA_ERRO;
                }
            } else {
                resultado = RESERVA_SEM_ASSENTO;
            }
        }
        cJSON_Delete(trechoJson);
    }
    pthread_mutex_unlock(mutexTrecho);

    return resultado;
}

//função para reservar a carona que o cliente escolheu
void selecionar_carona(cJSON *jsonLogin, Resposta *resp){
    int resultado = reservarAssentoTrecho(jsonLogin, NULL);

    if (resultado == RESERVA_OK) {
        char *emailStr = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
        int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
        log_mensagem(LOG_INFO, "Reserva realizada - Cliente: %s | Trecho ID: %d", emailStr, idSelecionado);
        enviar_resposta_http(resp, 201, "Created", "{\"mensagem\": \"CARONA_RESERVADA\"}");
    } else if (resultado == RESERVA_SEM_ASSENTO || resultado == RESERVA_ERRO) {
        enviar_resposta_http(resp, 409, "Conflict", "{\"erro\": \"ASSENTO_INDISPONIVEL\"}");
    } else {
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
    }
}

//função para adicionar o trecho na rota do cliente
void AddTrechoNaRota(cJSON *jsonLogin, Resposta *resp){
    char *retornoTrecho = NULL;
    int resultado = reservarAssentoTrecho(jsonLogin, &retornoTrecho);

    if (resultado == RESERVA_OK && retornoTrecho != NULL) {
        enviar_resposta_http(resp, 201, "Created", retornoTrecho);
    } else if (resultado == RESERVA_OK || resultado == RESERVA_SEM_ASSENTO || resultado == RESERVA_ERRO) {
        enviar_resposta_http(resp, 409, "Conflict", "{\"erro\": \"ASSENTO_INDISPONIVEL\"}");
    } else {
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
    }
    free(retornoTrecho);
}

//função para o cliente cancelar uma reserva da carona
int cancelarReservaInterna(int idSelecionado, char *emailCliente, char *nomeCliente) {
    int clienteEncontrado = 0;

    pthread_mutex_t *mutexTrecho = obterMutexTrecho(idSelecionado);
    if (mutexTrecho == NULL) return 0;

    pthread_mutex_lock(mutexTrecho);
    cJSON *trechoJson = lerTrechoNoLock(idSelecionado);
    if (trechoJson != NULL) {
        cJSON *arrayClientes = cJSON_GetObjectItem(trechoJson, "clientes");
        cJSON *arrayNomesClientes = cJSON_GetObjectItem(trechoJson, "nomesClientes");
        int total = arrayClientes ? cJSON_GetArraySize(arrayClientes) : 0;
        int totalNomes = arrayNomesClientes ? cJSON_GetArraySize(arrayNomesClientes) : 0;
        for (int i = 0; i < total; i++) {
            char *email = cJSON_GetStringValue(cJSON_GetArrayItem(arrayClientes, i));
            if (email != NULL && emailCliente != NULL && strcmp(email, emailCliente) == 0) {
                cJSON_DeleteItemFromArray(arrayClientes, i);
                clienteEncontrado = 1;
                int capacidadeAtual = cJSON_GetNumberValue(cJSON_GetObjectItem(trechoJson, "capacidade"));
                cJSON_ReplaceItemInObject(trechoJson, "capacidade", cJSON_CreateNumber(capacidadeAtual + 1));
                break;
            }
        }
        for (int i = 0; i < totalNomes; i++) {
            char *nome = cJSON_GetStringValue(cJSON_GetArrayItem(arrayNomesClientes, i));
            if (nome != NULL && nomeCliente != NULL && strcmp(nome, nomeCliente) == 0) {
                cJSON_DeleteItemFromArray(arrayNomesClientes, i);
                break;
            }
        }

        //só regrava o arquivo se a reserva realmente foi removida
        if (clienteEncontrado && !gravarTrechoNoLock(idSelecionado, trechoJson)) {
            log_mensagem(LOG_ERROR, "Falha ao gravar o cancelamento da reserva no trecho ID %d", idSelecionado);
            clienteEncontrado = 0;
        }
        cJSON_Delete(trechoJson);
    }
    pthread_mutex_unlock(mutexTrecho);

    return clienteEncontrado;
}

//função para verificar se a rota cumpre o caminho da origem até o destino, caso não, as reservas são canceladas
void finalizar_Rota(cJSON *jsonLogin, Resposta *resp){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    int trechoEncontrado = 0;
    char *destinoRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destinoRota"));
    char *origemRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origemRota"));

    cJSON *rota = cJSON_GetObjectItem(jsonLogin, "rota");
    if (rota == NULL){
        printf("Objeto rotas nao criado!\n");
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"ROTA_INVALIDA\"}");
        return;
    }
    int total = cJSON_GetArraySize(rota);

    char origemTemporaria[50] = {0};
    char destinoTemporario[50] = {0};
    for (int i=0; i<total; i++){
        cJSON *trecho = cJSON_GetArrayItem(rota, i);
        if (trecho == NULL){
            printf("Objeto trecho nao encontrado!\n");
            continue;
        }
        int idTrecho = cJSON_GetNumberValue(cJSON_GetObjectItem(trecho, "id"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "destino"));
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "origem"));
        if (i == 0){
            strcpy(origemTemporaria, origem);
        }
        if (existeCaminhoBFSPorNome(mapa, origem, destino)){
            strcpy(destinoTemporario, destino);
        }
    }

    if (strcmp(origemTemporaria, origemRota) == 0 && strcmp(destinoTemporario, destinoRota) == 0) {
        log_mensagem(LOG_INFO, "Rota finalizada com sucesso (%s -> %s)", origemRota, destinoRota);
        enviar_resposta_http(resp, 200, "OK", "{\"mensagem\": \"CARONA_CADASTRADA\"}");
    } else {
        char *email = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
        char *nomeCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nomeCliente"));
        log_mensagem(LOG_WARN, "Falha ao finalizar rota para o email: %s", email);

        if (total > 0){
            printf("EMAIL CLIENTE: %s\nROTA REMOVIDA [IDs TRECHOS]: |", email);
        }

        for (int i=0; i<total; i++){
            cJSON *trecho = cJSON_GetArrayItem(rota, i);
            if (trecho == NULL){
                printf("Objeto trecho nao encontrado!\n");
                continue;
            }
            int idTrecho = cJSON_GetNumberValue(cJSON_GetObjectItem(trecho, "idTrecho"));
            int removeu = cancelarReservaInterna(idTrecho, email, nomeCliente);
            if (removeu){
                printf(" %d |", idTrecho);
            }
        }
        
        printf("\n");
        enviar_resposta_http(resp, 400, "Bad Request", "{\"erro\": \"CARONA_NAO_CADASTRADA\"}");
    }
}

//função para obter as reservas do cliente
void listar_reservas(cJSON *jsonLogin, Resposta *resp){
    cJSON *arrayResposta = cJSON_CreateArray();
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    log_mensagem(LOG_INFO, "Consulta de reservas solicitada - Cliente: %s", emailCliente);

    int limite = lerLimiteIdTrechos();
    for (int t = 0; t < limite; t++) {
        cJSON *trechosJson = lerTrecho(t);
        if (trechosJson == NULL) continue;

        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
        char *nomeMotoristaTrecho = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));
        cJSON *arrayClientes = cJSON_GetObjectItem(trechosJson, "clientes");
        int total = cJSON_GetArraySize(arrayClientes);

        for (int i=0; i < total; i++){
            char *emailClienteCarona = cJSON_GetStringValue(cJSON_GetArrayItem(arrayClientes, i));
            if (emailCliente != NULL && emailClienteCarona != NULL &&
            strcmp(emailCliente, emailClienteCarona) == 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                cJSON_AddStringToObject(item, "nomeMotorista", nomeMotoristaTrecho);
                cJSON_AddStringToObject(item, "origem", cidadeOrigem);
                cJSON_AddStringToObject(item, "destino", cidadeDestino);
                cJSON_AddNumberToObject(item, "capacidade", capacidade);
                cJSON_AddStringToObject(item, "data", data);
                cJSON_AddStringToObject(item, "hora", hora);
                cJSON_AddNumberToObject(item, "preco", preco);
                cJSON_AddItemToArray(arrayResposta, item);
            }
        }
        cJSON_Delete(trechosJson);
    }

    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    enviar_resposta_http(resp, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para cancelar a reserva do cliente
void cancelar_carona(cJSON *jsonLogin, Resposta *resp){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    char *nomeCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nomeCliente"));

    int cancelou = cancelarReservaInterna(idSelecionado, emailCliente, nomeCliente);

    if (cancelou) {
        log_mensagem(LOG_INFO, "Reserva cancelada pelo cliente - Cliente: %s | ID Trecho: %d", emailCliente, idSelecionado);
        enviar_resposta_http(resp, 200, "OK", "{\"mensagem\": \"CARONA_CANCELADA\"}");
    } else {
        log_mensagem(LOG_WARN, "Falha ao cancelar reserva - Cliente: %s | ID Trecho: %d", emailCliente, idSelecionado);
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
    }
}

//função para buscar os possiveis trechos partindo de uma origem
void buscar_trechos_partida(cJSON *jsonLogin, Resposta *resp){
    cJSON *arrayResposta = cJSON_CreateArray();
    char *origemBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origem"));

    int limite = lerLimiteIdTrechos();
    for (int i = 0; i < limite; i++) {
        cJSON *trechosJson = lerTrecho(i);
        if (trechosJson == NULL) continue;

        int id = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "idTrecho"));
        char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "nomeMotorista"));
        char *cidadeOrigem = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "origem"));
        char *cidadeDestino = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "destino"));
        int capacidade = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "capacidade"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trechosJson, "hora"));
        float preco = cJSON_GetNumberValue(cJSON_GetObjectItem(trechosJson, "preco"));

        if (cidadeOrigem != NULL && origemBuscada != NULL &&
            strcmp(cidadeOrigem, origemBuscada) == 0 && capacidade > 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddStringToObject(item, "nomeMotorista", nomeMotorista);
            cJSON_AddStringToObject(item, "origem", cidadeOrigem);
            cJSON_AddStringToObject(item, "destino", cidadeDestino);
            cJSON_AddNumberToObject(item, "capacidade", capacidade);
            cJSON_AddStringToObject(item, "data", data);
            cJSON_AddStringToObject(item, "hora", hora);
            cJSON_AddNumberToObject(item, "preco", preco);
            cJSON_AddItemToArray(arrayResposta, item);
        }
        cJSON_Delete(trechosJson);
    }
    char *resposta = cJSON_PrintUnformatted(arrayResposta);
    enviar_resposta_http(resp, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para tratar todas as ações do motorista
void tratarMotorista(Resposta *resp, cJSON *jsonBody, char *metodo, char *path) {

    //POST /api/motoristas/login -> Autenticação do motorista
    if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/login") == 0) {
        FILE *arquivo = fopen("dados/loginMotorista.json", "r");
        if (arquivo == NULL) {
            enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        loginMotorista(jsonBody, resp, arquivo);
        fclose(arquivo);
    } 
    //POST /api/motoristas/cadastro -> Cadastro de novo motorista (Criação do recurso)
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/cadastro") == 0) {
        FILE *arquivo = fopen("dados/loginMotorista.json", "a+");
        if (arquivo == NULL) {
            enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        cadastrarMotorista(jsonBody, resp, arquivo);
        fclose(arquivo);
    } 
    //POST /api/motoristas/trechos -> Cadastro de um novo trecho
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/trechos") == 0) {
        cadastrarTrecho(jsonBody, resp);
    } 
    //GET /api/motoristas/trechos?email=...&nome=... -> Listagem de trechos do motorista
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/motoristas/trechos") == 0) {
        listar_trechos(jsonBody, resp);
    } 
    //DELETE /api/motoristas/trechos/{id}?email=...&nome=... -> Cancelamento/remoção de um trecho
    else if (strcmp(metodo, "DELETE") == 0 && strncmp(path, "/api/motoristas/trechos/", 24) == 0) {
        int id;
        if (sscanf(path + 24, "%d", &id) != 1) {
            enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
            return;
        }
        cJSON_AddNumberToObject(jsonBody, "idSelecionado", id);
        cancelarTrechoMotorista(jsonBody, resp);
    } 
    //POST /api/motoristas/rotas -> Cadastro de uma nova rota
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/rotas") == 0) {
        cadastrarRota(jsonBody, resp);
    } 
    else {
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }
}

//função para tratar as ações do cliente
void tratarCliente(Resposta *resp, cJSON *jsonBody, char *metodo, char *path) {
    
    //POST /api/clientes/login -> Autenticação do cliente
    if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/login") == 0) {
        FILE *arquivo = fopen("dados/loginCliente.json", "r");
        if (arquivo == NULL) {
            enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        loginCliente(jsonBody, resp, arquivo);
        fclose(arquivo);
    } 
    //POST /api/clientes/cadastro -> Cadastro de novo cliente (Criação de recurso)
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/cadastro") == 0) {
        FILE *arquivo = fopen("dados/loginCliente.json", "a+");
        if (arquivo == NULL) {
            enviar_resposta_http(resp, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        cadastrarCliente(jsonBody, resp, arquivo);
        fclose(arquivo);
    } 
    //GET /api/clientes/caronas?origem=...&destino=...&data=... -> Consulta de caronas disponíveis
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/clientes/caronas") == 0) {
        buscar_carona(jsonBody, resp);
    } 
    //POST /api/clientes/reservas -> Criação de uma reserva de carona
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/reservas") == 0) {
        selecionar_carona(jsonBody, resp);
    } 
    //GET /api/clientes/reservas?email=... -> Listagem das reservas do cliente
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/clientes/reservas") == 0) {
        listar_reservas(jsonBody, resp);
    } 
    //DELETE /api/clientes/reservas/{id}?emailCliente=...&nomeCliente=... -> Cancelamento de reserva
    else if (strcmp(metodo, "DELETE") == 0 && strncmp(path, "/api/clientes/reservas/", 23) == 0) {
        int id;
        if (sscanf(path + 23, "%d", &id) != 1) {
            enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
            return;
        }
        cJSON_AddNumberToObject(jsonBody, "idSelecionado", id);
        cancelar_carona(jsonBody, resp);
    } 
    //POST /api/clientes/rotas/trechos -> Adição de trecho a uma rota em montagem
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/rotas/trechos") == 0) {
        AddTrechoNaRota(jsonBody, resp);
    } 
    //POST /api/clientes/rotas -> Finalização/criação da rota
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/rotas") == 0) {
        finalizar_Rota(jsonBody, resp);
    } 
    //GET /api/clientes/trechos?origem=... -> Consulta de trechos de partida
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/clientes/trechos") == 0) {
        buscar_trechos_partida(jsonBody, resp);
    } 
    //GET /api/clientes/avisos?email=... -> Obtenção de notificações e avisos (somente leitura)
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/clientes/avisos") == 0) {
        listarAvisosCliente(jsonBody, resp);
    } 
    //DELETE /api/clientes/avisos?email=... -> Remoção dos avisos já lidos
    else if (strcmp(metodo, "DELETE") == 0 && strcmp(path, "/api/clientes/avisos") == 0) {
        apagarAvisosCliente(jsonBody, resp);
    } 
    else {
        enviar_resposta_http(resp, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }
}

//decodifica %XX e '+' de um valor da query string (ex.: joao%40email.com -> joao@email.com)
static void decodificarURL(char *texto) {
    char *destino = texto;
    for (char *origem = texto; *origem != '\0'; origem++) {
        if (*origem == '%' && isxdigit((unsigned char)origem[1]) && isxdigit((unsigned char)origem[2])) {
            char hex[3] = {origem[1], origem[2], '\0'};
            *destino++ = (char)strtol(hex, NULL, 16);
            origem += 2;
        } else if (*origem == '+') {
            *destino++ = ' ';
        } else {
            *destino++ = *origem;
        }
    }
    *destino = '\0';
}

//callback registrado na libmicrohttpd (MHD_OPTION_UNESCAPE_CALLBACK): ela chama esta função para decodificar
//cada parâmetro da query string, assim o '+' vira espaço
static size_t unescapeURL(void *cls, struct MHD_Connection *conn, char *texto) {
    (void)cls; (void)conn;
    decodificarURL(texto);
    return strlen(texto);
}

//a libmicrohttpd chama esta função uma vez para cada parâmetro da query string (?email=a@b.com&nome=Joao)
//copiamos para o JSON, assim os handlers leem o GET/DELETE do mesmo jeito que leem o corpo dos POST
static enum MHD_Result coletarParametro(void *cls, enum MHD_ValueKind tipo, const char *chave, const char *valor) {
    (void)tipo;
    cJSON *json = (cJSON *)cls;
    if (chave != NULL && valor != NULL) {
        cJSON_DeleteItemFromObject(json, chave);
        cJSON_AddStringToObject(json, chave, valor);
    }
    return MHD_YES;
}

//"comanda" de uma requisição: a libmicrohttpd entrega o corpo em pedaços, então vamos juntando aqui (via *con_cls)
typedef struct {
    char *dados;     //corpo acumulado (sempre terminado em '\0')
    size_t tamanho;
    int excedeu;     //1 se o corpo passou de MAX_CORPO
} CorpoReq;

//a libmicrohttpd chama esta função quando a requisição termina (com sucesso ou não): libera a comanda
static void requisicaoConcluida(void *cls, struct MHD_Connection *conn, void **con_cls,
                                enum MHD_RequestTerminationCode motivo) {
    (void)cls; (void)conn; (void)motivo;
    CorpoReq *corpo = (CorpoReq *)*con_cls;
    if (corpo != NULL) {
        free(corpo->dados);
        free(corpo);
        *con_cls = NULL;
    }
}

//Envia a resposta pela libmicrohttpd: embrulha, etiqueta, despacha e limpa
static enum MHD_Result enviarPelaMHD(struct MHD_Connection *conn, Resposta *resp) {
    int status = resp->status != 0 ? resp->status : MHD_HTTP_INTERNAL_SERVER_ERROR;
    const char *texto = resp->corpo != NULL ? resp->corpo : "{\"erro\": \"Sem resposta\"}";

    struct MHD_Response *resposta = MHD_create_response_from_buffer(strlen(texto), (void *)texto, MHD_RESPMEM_MUST_COPY);
    if (resposta == NULL) return MHD_NO;
    MHD_add_response_header(resposta, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(conn, status, resposta);
    MHD_destroy_response(resposta);
    return ret;
}

// Função chamada pela libmicrohttpd a cada requisição (Modelo Stateless REST).
// Ela é chamada VÁRIAS vezes para a mesma requisição:
//   1ª chamada: só chegaram método e URL (criamos a comanda);
//   chamadas seguintes: chegam pedaços do corpo (acumulamos);
//   última chamada (upload_data_size == 0): corpo completo, aí processamos e respondemos.
// Cada conexão roda em uma thread própria (MHD_USE_THREAD_PER_CONNECTION)
static enum MHD_Result tratarRequisicaoHTTP(void *cls, struct MHD_Connection *conn,
                                            const char *url, const char *method, const char *version,
                                            const char *upload_data, size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;
    CorpoReq *corpo = (CorpoReq *)*con_cls;
    Resposta resp = {0, NULL};
    enum MHD_Result ret;

    //1ª chamada: cria a comanda
    if (corpo == NULL) {
        corpo = calloc(1, sizeof(CorpoReq));
        if (corpo == NULL) return MHD_NO;
        *con_cls = corpo;
        return MHD_YES;
    }

    //chegou um pedaço do corpo: acumula
    if (*upload_data_size > 0) {
        if (!corpo->excedeu) {
            if (corpo->tamanho + *upload_data_size > MAX_CORPO) {
                corpo->excedeu = 1;   //continua consumindo os dados (descartando) para a conexão não travar
            } else {
                char *novo = realloc(corpo->dados, corpo->tamanho + *upload_data_size + 1);
                if (novo == NULL) return MHD_NO;
                corpo->dados = novo;
                memcpy(corpo->dados + corpo->tamanho, upload_data, *upload_data_size);
                corpo->tamanho += *upload_data_size;
                corpo->dados[corpo->tamanho] = '\0';
            }
        }
        *upload_data_size = 0;   //obrigatório: avisa que consumimos os dados
        return MHD_YES;
    }

    //corpo completo: daqui para baixo é o processamento da requisição
    char metodo[16];
    char path[256];
    snprintf(metodo, sizeof(metodo), "%s", method);
    snprintf(path, sizeof(path), "%s", url);   //a libmicrohttpd já separa a query string do path

    if (corpo->excedeu) {
        log_mensagem(LOG_WARN, "Requisicao %s %s rejeitada: corpo maior que %d bytes", metodo, path, MAX_CORPO);
        enviar_resposta_http(&resp, 413, "Payload Too Large", "{\"erro\": \"Corpo da requisicao muito grande\"}");
        ret = enviarPelaMHD(conn, &resp);
        free(resp.corpo);
        return ret;
    }

    //Apenas faz parse do JSON para métodos de modificação/criação
    cJSON *json = NULL;
    if ((strcmp(metodo, "POST") == 0 || strcmp(metodo, "PUT") == 0 || strcmp(metodo, "PATCH") == 0)
        && corpo->tamanho > 0) {
        json = cJSON_Parse(corpo->dados);
        if (json == NULL) {
            log_mensagem(LOG_ERROR, "Payload JSON invalido em %s %s", metodo, path);
            enviar_resposta_http(&resp, 400, "Bad Request", "{\"erro\": \"Sintaxe JSON invalida\"}");
            ret = enviarPelaMHD(conn, &resp);
            free(resp.corpo);
            return ret;
        }
    }

    //GET e DELETE não têm corpo: os parâmetros chegam na query string (?email=...&nome=...).
    //Eles são copiados para o JSON, que é o que os handlers leem.
    if (json == NULL) json = cJSON_CreateObject();
    MHD_get_connection_values(conn, MHD_GET_ARGUMENT_KIND, coletarParametro, json);

    if (strncmp(path, "/api/motoristas", 15) == 0) {
        tratarMotorista(&resp, json, metodo, path);
    }
    else if (strncmp(path, "/api/clientes", 13) == 0) {
        tratarCliente(&resp, json, metodo, path);
    } else {
        enviar_resposta_http(&resp, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }

    //Limpeza de memória (Stateless)
    cJSON_Delete(json);

    log_mensagem(LOG_INFO, "Requisicao %s %s processada. Status %d", metodo, path, resp.status);
    fflush(stdout);

    ret = enviarPelaMHD(conn, &resp);
    free(resp.corpo);
    return ret;
}

int main(){
    //atualiza o id do trecho
    encontrarID();
    //acerta o contador conforme os arquivos que existem
    recalcularContadorTrechos();
    mapa = carregarGrafoDeArquivo("mapa.txt");

    if (mapa == NULL){
        log_mensagem(LOG_ERROR, "Falha ao criar o grafo de mapa: %m");
        exit(EXIT_FAILURE);
    }

    //porta: padrão PORT, ou a variável de ambiente PORT (útil para rodar vários servidores / Docker)
    unsigned short porta = PORT;
    const char *portaEnv = getenv("PORT");
    if (portaEnv != NULL && atoi(portaEnv) > 0 && atoi(portaEnv) < 65536) {
        porta = (unsigned short)atoi(portaEnv);
    }

    //bloqueia SIGINT/SIGTERM nesta thread ANTES de criar as outras (elas herdam o bloqueio),
    //assim só o main recebe o sinal, via sigwait, lá no final
    sigset_t sinais;
    sigemptyset(&sinais);
    sigaddset(&sinais, SIGINT);
    sigaddset(&sinais, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sinais, NULL);

    pthread_t threadLimpeza;
    //cria a thread de limpeza
    if (pthread_create(&threadLimpeza, NULL, rotinaLimpezaTrechos, NULL) != 0) {
        log_mensagem(LOG_ERROR, "Falha ao criar a thread de limpeza: %m");
    } else {
        pthread_detach(threadLimpeza);
    }

    //liga o servidor HTTP. A libmicrohttpd cuida de socket, bind, listen, accept e do parsing do HTTP.
    //  INTERNAL_POLLING_THREAD: ela cria a thread que aceita conexões (o main fica livre)
    //  THREAD_PER_CONNECTION:   cada conexão é atendida em uma thread própria (como no modelo anterior)
    //  USE_POLL:                usa poll() em vez de select(), sem o limite de 1024 descritores
    struct MHD_Daemon *servidor = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION | MHD_USE_POLL,
        porta,
        NULL, NULL,
        &tratarRequisicaoHTTP, NULL, 
        MHD_OPTION_NOTIFY_COMPLETED, &requisicaoConcluida, NULL,
        MHD_OPTION_UNESCAPE_CALLBACK, &unescapeURL, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,   //fecha conexões ociosas após 30 s
        MHD_OPTION_END);

    if (servidor == NULL) {
        log_mensagem(LOG_ERROR, "Falha ao iniciar o servidor HTTP na porta %u", (unsigned)porta);
        exit(EXIT_FAILURE);
    }

    printf("==================================================\n");
    printf("        SERVIDOR REST NA PORTA %u\n", (unsigned)porta);
    printf("==================================================\n");
    log_mensagem(LOG_INFO, "Servidor iniciado na porta %u", (unsigned)porta);

    //espera Ctrl+C (SIGINT) ou SIGTERM (docker stop)
    int sinalRecebido = 0;
    sigwait(&sinais, &sinalRecebido);
    log_mensagem(LOG_INFO, "Sinal %d recebido, encerrando o servidor", sinalRecebido);

    MHD_stop_daemon(servidor);
    pthread_mutex_destroy(&logMutex);
    destruirGerenciadorDeTravas();
    return 0;
}