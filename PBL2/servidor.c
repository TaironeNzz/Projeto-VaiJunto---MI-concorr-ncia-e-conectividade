#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "cJSON.h"
#include "grafomapa.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdarg.h>

#define PORT 65432
#define BUFFER_SIZE 4096
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

//Se 1, os avisos são apagados do arquivo depois de entregues ao cliente (cada aviso é mostrado uma única vez)
//Se 0, os avisos permanecem no arquivo e o cliente recebe todos eles a cada consulta
#define REMOVER_AVISOS_APOS_ENVIO 1
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

// Função auxiliar para construir e enviar a resposta HTTP completa
void enviar_resposta_http(int client_fd, int status_code, const char *status_text, const char *json_body) {
    char resposta[BUFFER_SIZE];
    int body_len = json_body ? strlen(json_body) : 0;

    // Monta a estrutura do protocolo HTTP/1.1
    int response_len = snprintf(resposta, sizeof(resposta),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, status_text, body_len, json_body ? json_body : ""
    );

    send(client_fd, resposta, response_len, 0);
}

//função para cadastrar o cliente
void cadastrarCliente(cJSON *jsonLogin, int socketCliente, FILE *arquivoLogin){
    char dadosLogin[1024] = {0};
    int emailEncontrado = 0;
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));

    pthread_mutex_lock(&loginClienteMutex);
    rewind(arquivoLogin);

    while(fgets(dadosLogin, sizeof(dadosLogin), arquivoLogin) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                enviar_resposta_http(socketCliente, 409, "Conflict", "{\"erro\": \"Email ja cadastrado\"}");
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
        enviar_resposta_http(socketCliente, 201, "Created", "{\"mensagem\": \"Cadastro realizado com sucesso\"}");
        free(saida);
    }
    pthread_mutex_unlock(&loginClienteMutex);
    
    fflush(arquivoLogin);
}

//função para cadastrar o motorista
void cadastrarMotorista(cJSON *jsonLogin, int socketMotorista, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));

    pthread_mutex_lock(&loginMotoristaMutex);
    rewind(arquivo);

    while(fgets(dadosLogin, sizeof(dadosLogin), arquivo) != NULL) {
        cJSON *dadosJson = cJSON_Parse(dadosLogin);
        if(dadosJson != NULL) {
            char *email = cJSON_GetStringValue(cJSON_GetObjectItem(dadosJson, "email"));
            if (strcmp(email, emailBuscado) == 0) {
                enviar_resposta_http(socketMotorista, 409, "Conflict", "{\"erro\": \"Email ja cadastrado\"}");
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
            enviar_resposta_http(socketMotorista, 201, "Created", "{\"mensagem\": \"Cadastro realizado com sucesso\"}");
            free(saida);
        }
    pthread_mutex_unlock(&loginMotoristaMutex);
}

int cadastrarTrecho(cJSON *jsonLogin, int socketMotorista){
    if (mapa == NULL) {
        printf("Mapa nao carregado. Nao e possivel cadastrar trecho.\n");
        enviar_resposta_http(socketMotorista, 500, "Internal Server Error", "{\"erro\": \"Mapa nao carregado\"}");
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
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Dados incompletos para cadastro de trecho\"}");
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
            enviar_resposta_http(socketMotorista, 500, "Internal Server Error", "{\"erro\": \"Falha ao cadastrar trecho\"}");
            return 0;
        }

        ajustarContadorTrechos(1);

        log_mensagem(LOG_INFO, "Trecho ID %d cadastrado por %s (%s -> %s)", meuIdTrecho, nomeMotorista, origem, destino);
        enviar_resposta_http(socketMotorista, 201, "Created", "{\"mensagem\": \"Trecho cadastrado com sucesso\"}");
        return 1;
    } else {
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Caminho nao encontrado\"}");
        return 0;
    }
}

//função para pegar os trechos de um motorista dos arquivos e retornar para o motorista
void listar_trechos(cJSON *jsonLogin, int socketMotorista){
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
    enviar_resposta_http(socketMotorista, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para acessar o arquivo de login e verificar se o email e senha estão certos
void loginMotorista(cJSON *jsonLogin, int socketMotorista, FILE *arquivo){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));

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
                enviar_resposta_http(socketMotorista, 200, "OK", resposta);
                emailEncontrado = 1;
                cJSON_Delete(respostaJson);
                break;
                }
            }
        cJSON_Delete(dadosJson);
        }
        if (!emailEncontrado) {
            log_mensagem(LOG_WARN, "Falha de autenticação para o email: %s", emailBuscado);
            enviar_resposta_http(socketMotorista, 401, "Unauthorized", "{\"erro\": \"Credenciais invalidas\"}");
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

//Função auxiliar: envia o buffer inteiro (send pode enviar menos bytes do que o pedido). Retorna 1 em sucesso.
static int enviarTudo(int socket, const char *dados, size_t tamanho) {
    size_t enviados = 0;
    while (enviados < tamanho) {
        char resposta[BUFFER_SIZE];

        // Monta a estrutura do protocolo HTTP/1.1
        int response_len = snprintf(resposta, sizeof(resposta),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{%s}",
            tamanho - enviados, dados + enviados
        );

        ssize_t n = send(socket, dados + enviados, tamanho - enviados, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        enviados += (size_t)n;
    }
    return 1;
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
//Requisição: {"classe":"Cliente","acao":"listar_avisos","email":"cliente@email.com"}
//Resposta: array JSON com os avisos do cliente (ou "[]" se não houver nenhum)
//Erros: "EMAIL_INVALIDO" (requisição sem email) e "FALHA_LER_AVISOS" (arquivo ilegível)
void listarAvisosCliente(cJSON *jsonLogin, int socketCliente) {
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    if (emailCliente == NULL) {
        emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    }
    if (emailCliente == NULL) {
        log_mensagem(LOG_WARN, "Consulta de avisos sem email do cliente");
        enviar_resposta_http(socketCliente, 400, "Bad Request", "{\"erro\": \"Email invalido\"}");
        return;
    }

    pthread_mutex_lock(&avisosMutex);

    cJSON *raiz = NULL;
    if (!carregarAvisos(&raiz)) {
        pthread_mutex_unlock(&avisosMutex);
        enviar_resposta_http(socketCliente, 500, "Internal Server Error", "{\"erro\": \"Falha ao ler avisos\"}");
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
        enviar_resposta_http(socketCliente, 500, "Internal Server Error", "{\"erro\": \"Falha ao ler avisos\"}");
        return;
    }

    int enviou = enviarTudo(socketCliente, resposta, strlen(resposta));
    log_mensagem(LOG_INFO, "Consulta de avisos - Cliente: %s | Avisos enviados: %d", emailCliente,
                 temAvisos ? cJSON_GetArraySize(listaAvisos) : 0);

    //só apaga os avisos do arquivo se eles realmente foram enviados, para o cliente não perdê-los
    if (REMOVER_AVISOS_APOS_ENVIO && enviou && temAvisos) {
        cJSON_DeleteItemFromObjectCaseSensitive(raiz, emailCliente);
        salvarAvisos(raiz);
    }

    free(resposta);
    cJSON_Delete(raiz);
    pthread_mutex_unlock(&avisosMutex);
}

//função para cancelar trecho de um motorista
void cancelarTrechoMotorista(cJSON *jsonLogin, int socketMotorista){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    char *emailMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    if (emailMotorista == NULL){
        log_mensagem(LOG_ERROR, "Falha ao cancelar trecho do motorista %s -> email nulo", nomeMotorista);
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Trecho não encontrado\"}");
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
        enviar_resposta_http(socketMotorista, 200, "OK", "{\"mensagem\": \"Trecho cancelado com sucesso\"}");
    } else {
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Trecho não encontrado\"}");
    }
}

//função para um motorista cadastrar sua rota informando os trechos
void cadastrarRota(cJSON *jsonLogin, int socketMotorista){
    char *nomeMotorista = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nome"));
    cJSON *trechosArray = cJSON_GetObjectItem(jsonLogin, "trechos");

    if (mapa == NULL) {
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Mapa não carregado\"}");
        return;
    }
    if (trechosArray == NULL || !cJSON_IsArray(trechosArray) || nomeMotorista == NULL) {
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"Rota invalida\"}");
        return;
    }

    int totalTrechos = cJSON_GetArraySize(trechosArray);
    if (totalTrechos == 0) {
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"ROTA_INVALIDA\"}");
        return;
    }

    char *destinoAnterior = NULL;
    for (int i = 0; i < totalTrechos; i++) {
        cJSON *trecho = cJSON_GetArrayItem(trechosArray, i);
        if (trecho == NULL){
            enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
            return;
        }
        char *origem = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "origem"));
        char *destino = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "destino"));
        char *data = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "data"));
        char *hora = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "hora"));
        char *email = cJSON_GetStringValue(cJSON_GetObjectItem(trecho, "emailMotorista"));

        if (origem == NULL || destino == NULL || data == NULL || hora == NULL || email == NULL) {
            enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"ROTA_INVALIDA\"}");
            return;
        }
        if (!existeCaminhoBFSPorNome(mapa, origem, destino)) {
            enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
            return;
        }

        if (destinoAnterior != NULL && strcmp(origem, destinoAnterior) != 0) {
            enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"ROTA_DESCONECTADA\"}");
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
        enviar_resposta_http(socketMotorista, 400, "Bad Request", "{\"erro\": \"FALHA_CADASTRO_ROTA\"}");
        return;
    }

    ajustarContadorTrechos(totalTrechos);
    log_mensagem(LOG_INFO, "Rota (multi-trechos) cadastrada por %s | Total de trechos: %d", nomeMotorista, totalTrechos);
    enviar_resposta_http(socketMotorista, 200, "Created", "{\"mensagem\": \"ROTA_CADASTRADA\"}");
}

//função para acessar o arquivo de login e verificar se o email e senha do cliente estão certos 
void loginCliente(cJSON *jsonLogin, int socketCliente, FILE *arquivoLogin){
    int emailEncontrado = 0;
    char dadosLogin[1024] = {0};
    char *emailBuscado = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "email"));
    char *senhaBuscada = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "senha"));

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
                enviar_resposta_http(socketCliente, 200, "OK", resposta);
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
        enviar_resposta_http(socketCliente, 401, "Unauthorized", "{\"erro\": \"NÃO_AUTENTICADO\"}");
    }
    pthread_mutex_unlock(&loginClienteMutex);
}

//função para buscar as caronas que o usuário pediu
void buscar_carona(cJSON *jsonLogin, int socketCliente){
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
    enviar_resposta_http(socketCliente, 200, "OK", resposta);
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
void selecionar_carona(cJSON *jsonLogin, int socketCliente){
    int resultado = reservarAssentoTrecho(jsonLogin, NULL);

    if (resultado == RESERVA_OK) {
        char *emailStr = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
        int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
        log_mensagem(LOG_INFO, "Reserva realizada - Cliente: %s | Trecho ID: %d", emailStr, idSelecionado);
        enviar_resposta_http(socketCliente, 200, "OK", "{\"mensagem\": \"CARONA_RESERVADA\"}");
    } else if (resultado == RESERVA_SEM_ASSENTO || resultado == RESERVA_ERRO) {
        enviar_resposta_http(socketCliente, 400, "Bad Request", "{\"erro\": \"ASSENTO_INDISPONIVEL\"}");
    } else {
        enviar_resposta_http(socketCliente, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
    }
}

//função para adicionar o trecho na rota do cliente
void AddTrechoNaRota(cJSON *jsonLogin, int socketCliente){
    char *retornoTrecho = NULL;
    int resultado = reservarAssentoTrecho(jsonLogin, &retornoTrecho);

    if (resultado == RESERVA_OK && retornoTrecho != NULL) {
        enviar_resposta_http(socketCliente, 200, "OK", retornoTrecho);
    } else if (resultado == RESERVA_OK || resultado == RESERVA_SEM_ASSENTO || resultado == RESERVA_ERRO) {
        enviar_resposta_http(socketCliente, 400, "Bad Request", "{\"erro\": \"ASSENTO_INDISPONIVEL\"}");
    } else {
        enviar_resposta_http(socketCliente, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
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
void finalizar_Rota(cJSON *jsonLogin, int socketCliente){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    int trechoEncontrado = 0;
    char *destinoRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "destinoRota"));
    char *origemRota = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "origemRota"));

    cJSON *rota = cJSON_GetObjectItem(jsonLogin, "rota");
    if (rota == NULL){
        printf("Objeto rotas nao criado!\n");
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
        enviar_resposta_http(socketCliente, 200, "OK", "{\"mensagem\": \"CARONA_CADASTRADA\"}");
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
        enviar_resposta_http(socketCliente, 400, "Bad Request", "{\"erro\": \"CARONA_NAO_CADASTRADA\"}");
    }
}

//função para obter as reservas do cliente
void listar_reservas(cJSON *jsonLogin, int socketCliente){
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
    enviar_resposta_http(socketCliente, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para cancelar a reserva do cliente
void cancelar_carona(cJSON *jsonLogin, int socketCliente){
    int idSelecionado = cJSON_GetNumberValue(cJSON_GetObjectItem(jsonLogin, "idSelecionado"));
    char *emailCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "emailCliente"));
    char *nomeCliente = cJSON_GetStringValue(cJSON_GetObjectItem(jsonLogin, "nomeCliente"));

    int cancelou = cancelarReservaInterna(idSelecionado, emailCliente, nomeCliente);

    if (cancelou) {
        log_mensagem(LOG_INFO, "Reserva cancelada pelo cliente - Cliente: %s | ID Trecho: %d", emailCliente, idSelecionado);
        enviar_resposta_http(socketCliente, 200, "OK", "{\"mensagem\": \"CARONA_CANCELADA\"}");
    } else {
        log_mensagem(LOG_WARN, "Falha ao cancelar reserva - Cliente: %s | ID Trecho: %d", emailCliente, idSelecionado);
        enviar_resposta_http(socketCliente, 404, "Not Found", "{\"erro\": \"TRECHO_NAO_ENCONTRADO\"}");
    }
}

//função para buscar os possiveis trechos partindo de uma origem
void buscar_trechos_partida(cJSON *jsonLogin, int socketCliente){
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
    enviar_resposta_http(socketCliente, 200, "OK", resposta);
    free(resposta);
    cJSON_Delete(arrayResposta);
}

//função para tratar todas as ações do motorista
void tratarMotorista(int socketMotorista, cJSON *jsonBody, char *metodo, char *path) {

    //POST /api/motoristas/login -> Autenticação do motorista
    if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/login") == 0) {
        FILE *arquivo = fopen("dados/loginMotorista.json", "r");
        if (arquivo == NULL) {
            enviar_resposta_http(socketMotorista, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        loginMotorista(jsonBody, socketMotorista, arquivo);
        fclose(arquivo);
    } 
    //POST /api/motoristas/cadastro -> Cadastro de novo motorista (Criação do recurso)
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/motoristas/cadastro") == 0) {
        FILE *arquivo = fopen("dados/loginMotorista.json", "a+");
        if (arquivo == NULL) {
            enviar_resposta_http(socketMotorista, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        cadastrarMotorista(jsonBody, socketMotorista, arquivo);
        fclose(arquivo);
    } 
    //POST /api/trechos -> Cadastro de um novo trecho
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/trechos") == 0) {
        cadastrarTrecho(jsonBody, socketMotorista);
    } 
    //GET /api/trechos -> Listagem de trechos do motorista (suporta query params)
    else if (strcmp(metodo, "GET") == 0 && strncmp(path, "/api/trechos", 12) == 0) {
        listar_trechos(jsonBody, socketMotorista);
    } 
    //DELETE /api/trechos -> Cancelamento/remoção de um trecho
    else if (strcmp(metodo, "DELETE") == 0 && strncmp(path, "/api/trechos", 12) == 0) {
        cancelarTrechoMotorista(jsonBody, socketMotorista);
    } 
    //POST /api/rotas -> Cadastro de uma nova rota
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/rotas") == 0) {
        cadastrarRota(jsonBody, socketMotorista);
    } 
    else {
        enviar_resposta_http(socketMotorista, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }
}

//função para tratar as ações do cliente
void tratarCliente(int socketCliente, cJSON *jsonBody, char *metodo, char *path) {
    
    //POST /api/clientes/login -> Autenticação do cliente
    if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/login") == 0) {
        FILE *arquivo = fopen("dados/loginCliente.json", "r");
        if (arquivo == NULL) {
            enviar_resposta_http(socketCliente, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        loginCliente(jsonBody, socketCliente, arquivo);
        fclose(arquivo);
    } 
    //POST /api/clientes/cadastro -> Cadastro de novo cliente (Criação de recurso)
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/clientes/cadastro") == 0) {
        FILE *arquivo = fopen("dados/loginCliente.json", "a+");
        if (arquivo == NULL) {
            enviar_resposta_http(socketCliente, 500, "Internal Server Error", "{\"erro\": \"Erro ao acessar base de dados\"}");
            return;
        }
        cadastrarCliente(jsonBody, socketCliente, arquivo);
        fclose(arquivo);
    } 
    //GET /api/caronas -> Consulta de caronas disponíveis (via query params)
    else if (strcmp(metodo, "GET") == 0 && strncmp(path, "/api/caronas", 12) == 0) {
        buscar_carona(jsonBody, socketCliente);
    } 
    //POST /api/reservas -> Criação de uma reserva de carona
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/reservas") == 0) {
        selecionar_carona(jsonBody, socketCliente);
    } 
    //GET /api/reservas -> Listagem das reservas do cliente
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/reservas") == 0) {
        listar_reservas(jsonBody, socketCliente);
    } 
    //DELETE /api/reservas -> Cancelamento de reserva
    else if (strcmp(metodo, "DELETE") == 0 && strncmp(path, "/api/reservas", 13) == 0) {
        cancelar_carona(jsonBody, socketCliente);
    } 
    //POST /api/rotas/trechos -> Adição de trecho a uma rota em montagem
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/rotas/trechos") == 0) {
        AddTrechoNaRota(jsonBody, socketCliente);
    } 
    //POST /api/rotas -> Finalização/criação da rota
    else if (strcmp(metodo, "POST") == 0 && strcmp(path, "/api/rotas") == 0) {
        finalizar_Rota(jsonBody, socketCliente);
    } 
    //GET /api/trechos -> Consulta de trechos de partida
    else if (strcmp(metodo, "GET") == 0 && strncmp(path, "/api/trechos", 12) == 0) {
        buscar_trechos_partida(jsonBody, socketCliente);
    } 
    //GET /api/avisos -> Obtenção de notificações e avisos
    else if (strcmp(metodo, "GET") == 0 && strcmp(path, "/api/avisos") == 0) {
        listarAvisosCliente(jsonBody, socketCliente);
    } 
    else {
        enviar_resposta_http(socketCliente, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }
}

// Rotina de tratamento para cada thread (Modelo Stateless REST)
void *rotinaTratamento(void *arg) {
    int socket = *(int*)arg;
    free(arg);

    char buffer_mensagem[8192];
    memset(buffer_mensagem, 0, sizeof(buffer_mensagem));

    //Lê a requisição HTTP do socket (apenas uma leitura por conexão)
    ssize_t bytes_lidos = read(socket, buffer_mensagem, sizeof(buffer_mensagem) - 1);
    if (bytes_lidos <= 0) {
        log_mensagem(LOG_ERROR, "Erro de leitura ou conexao fechada pelo cliente no socket %d", socket);
        close(socket);
        return NULL;
    }
    buffer_mensagem[bytes_lidos] = '\0';

    //Extrai o Método HTTP, o Path (URL) e o Protocolo da primeira linha
    char metodo[16] = {0};
    char path[256] = {0};
    char protocolo[16] = {0};

    if (sscanf(buffer_mensagem, "%15s %255s %15s", metodo, path, protocolo) < 2) {
        log_mensagem(LOG_ERROR, "Requisicao HTTP malformada no socket %d", socket);
        enviar_resposta_http(socket, 400, "Bad Request", "{\"erro\": \"Requisicao HTTP invalida\"}");
        close(socket);
        return NULL;
    }

    //Isola o Corpo (JSON) se presente no cabeçalho
    cJSON *json = NULL;
    char *corpo_str = strstr(buffer_mensagem, "\r\n\r\n");

    if (corpo_str != NULL) {
        // Pula a sequência \r\n\r\n
        corpo_str += 4;
        
        // Apenas faz parse do JSON para métodos de modificação/criação
        if ((strcmp(metodo, "POST") == 0 || strcmp(metodo, "PUT") == 0 || strcmp(metodo, "PATCH") == 0) && strlen(corpo_str) > 0) {
            json = cJSON_Parse(corpo_str);
            if (json == NULL) {
                log_mensagem(LOG_ERROR, "Payload JSON invalido no socket %d", socket);
                enviar_resposta_http(socket, 400, "Bad Request", "{\"erro\": \"Sintaxe JSON invalida\"}");
                close(socket);
                return NULL;
            }
        }
    }

    if (strncmp(path, "/api/motoristas", 15) == 0) {
        tratarMotorista(socket, json, metodo, path);
    } 
    else if (strncmp(path, "/api/clientes", 13) == 0) {
        tratarCliente(socket, json, metodo, path);
    } else {
        enviar_resposta_http(socket, 404, "Not Found", "{\"erro\": \"Recurso nao encontrado\"}");
    }

    //Limpeza de memória e encerramento do socket (Stateless)
    if (json != NULL) {
        cJSON_Delete(json);
    }

    log_mensagem(LOG_INFO, "Requisicao %s %s processada. Fechando socket %d", metodo, path, socket);
    fflush(stdout);
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
    //atualiza o id do trecho
    encontrarID();
    //acerta o contador conforme os arquivos que existem
    recalcularContadorTrechos();
    mapa = carregarGrafoDeArquivo("mapa.txt");

    if (mapa == NULL){
        log_mensagem(LOG_ERROR, "Falha ao criar o grafo de mapa: %m");
        exit(EXIT_FAILURE);
    }
    
    //cria o socket do servidor
    if ((socketServidor = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        log_mensagem(LOG_ERROR, "Falha ao criar o socket do servidor: %m");
        exit(EXIT_FAILURE);
    }

    int status = setsockopt(socketServidor, SOL_SOCKET,SO_REUSEADDR , &valor_opcao,sizeof(valor_opcao));

    if(status < 0){
        log_mensagem(LOG_ERROR, "Falha ao criar o socket do servidor: %m");
        exit(EXIT_FAILURE);
    }

    //relaciona o endereço
    memset(&endereco_servidor, 0, sizeof(endereco_servidor));
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_port = htons(PORT);
    endereco_servidor.sin_addr.s_addr = INADDR_ANY;

    //ativa o socket 
    status = bind(socketServidor, (struct sockaddr*)&endereco_servidor, sizeof(struct sockaddr));

    if(status < 0){
        log_mensagem(LOG_ERROR, "Falha ao criar o socket do servidor: %m");
        exit(EXIT_FAILURE);
    }

    //espera outros sockets conectarem
    status = listen(socketServidor, limite_clientes);

    if(status < 0){
        log_mensagem(LOG_ERROR, "Falha ao criar esperar dispositivos: %m");
        exit(EXIT_FAILURE);
    }

    pthread_t threadLimpeza;
    //cria a thread de limpeza
    if (pthread_create(&threadLimpeza, NULL, rotinaLimpezaTrechos, NULL) != 0) {
        log_mensagem(LOG_ERROR, "Falha ao criar a thread de limpeza: %m");
    } else {
        pthread_detach(threadLimpeza);
    }

    tamanho_endereco = sizeof(endereco_conexao);
    
    printf("==================================================\n");
    printf("        DISPOSITIVOS CONECTADOS NA PORTA %d\n", PORT);
    printf("==================================================\n");
    log_mensagem(LOG_INFO, "Servidor iniciado na porta %d", PORT);

    int i = 0;
    while (1){
        //função para aceitar a conexão de outros sockets
        socketCliente = accept(socketServidor, (struct sockaddr*)&endereco_conexao, &tamanho_endereco);
        if(socketCliente < 0){
            log_mensagem(LOG_ERROR, "Falha ao criar o socket do servidor: %m");
            continue;
        }
            
        log_mensagem(LOG_INFO, "Novo dispositivo conectado. Socket ID: %d", socketCliente);

        int *novo_sock = malloc(sizeof(int));
        *novo_sock = socketCliente;
        pthread_t threadID;
        //cria e coloca a thread para cuidar do socket que chegou (cliente ou motorista)
        if ((pthread_create(&threadID, NULL, rotinaTratamento, novo_sock)) != 0){
            log_mensagem(LOG_ERROR, "Falha ao criar a thread para o cliente: %m");
            free(novo_sock);
            close(socketCliente);
        } else {
            pthread_detach(threadID);
        }
        i++;
    }
    //fecha o socket do servidor
    close(socketServidor);
    pthread_mutex_destroy(&logMutex);
    destruirGerenciadorDeTravas();
    return 0;
}