/*
 * teste_concorrencia.c
 * ---------------------------------------------------------------------
 * Bateria de testes de CONCORRENCIA para o servidor de caronas.
 *
 * Este programa NAO usa cJSON: ele monta as mensagens JSON manualmente
 * (com snprintf) e extrai os campos das respostas com uma busca simples
 * de texto (extrair_int). Isso mantem o teste independente de qualquer
 * biblioteca externa - basta compilar e apontar para o servidor.
 *
 * Cada teste abre varias conexoes (uma por thread, exatamente como
 * varios clientes/motoristas reais fariam) e usa uma pthread_barrier_t
 * para garantir que todas as threads disparem a requisicao critica
 * (cadastro, reserva, cancelamento, etc.) o mais proximo possivel do
 * mesmo instante, maximizando a chance de expor uma condicao de corrida.
 *
 * ARMAZENAMENTO DOS TRECHOS (como o servidor funciona agora):
 *   - cada trecho fica no seu proprio arquivo
 *       trechosCadastrados/trecho<ID>.json
 *     protegido por um mutex proprio (um mutex por trecho);
 *   - o proximo ID fica em ArquivoID/id.txt e o contador de trechos
 *     cadastrados em ArquivoID/contadorTrechos.txt (+1 ao cadastrar,
 *     -1 ao cancelar/expirar).
 *
 * VERIFICACAO DOS ARQUIVOS (opcional):
 *   Os testes funcionam so pelo protocolo (socket), entao rodam contra
 *   qualquer servidor. Se o teste for executado NA MESMA MAQUINA do
 *   servidor, ele tambem confere os arquivos acima (informe a pasta onde
 *   o servidor roda, a que contem ArquivoID/ e trechosCadastrados/).
 *   Se a pasta nao for acessivel, essas conferencias sao puladas.
 *
 * TESTES INCLUIDOS:
 *   1) Cadastro concorrente do MESMO email de cliente
 *        -> so pode haver 1 CADASTRO_REALIZADO
 *   2) Reserva concorrente da MESMA carona (capacidade limitada)
 *        -> numero de CARONA_RESERVADA nao pode passar da capacidade
 *        -> (arquivos) capacidade e clientes gravados batem com as respostas
 *   3) Cancelamento concorrente da MESMA reserva
 *        -> so uma das duas tentativas pode ter sucesso
 *        -> (arquivos) a capacidade do trecho so sobe 1 vez
 *   4) Cadastro concorrente de trechos por varios motoristas
 *        -> os IDs de trecho gerados nao podem se repetir
 *        -> (arquivos) id.txt e contadorTrechos.txt sobem N, e cada
 *           trecho tem o seu arquivo
 *   5) Cadastro concorrente do MESMO email de motorista
 *        -> mesma ideia do teste 1, agora para a classe Motorista
 *   6) Reservas simultaneas em VARIOS trechos DIFERENTES
 *        -> nao deve haver contencao/deadlock quando nao ha disputa
 *           pelo mesmo recurso (mutex por trecho); mede o tempo total
 *   7) Leituras (listar_trechos) concorrentes com escritas
 *      (cadastrar_trecho) em varios arquivos de trecho
 *        -> nenhuma leitura pode vir corrompida/parcial
 *   8) Motorista cancelando um trecho x Cliente reservando o MESMO
 *      trecho, ao mesmo tempo (varias rodadas)
 *        -> como os dois usam o mutex do trecho, o resultado e sempre
 *           consistente: se a reserva venceu, o cliente DEVE receber o
 *           aviso de trecho removido (sem reserva "orfa")
 *   9) Cancelamento concorrente do MESMO trecho pelo motorista
 *        -> so um TRECHO_CANCELADO; (arquivos) arquivo removido e
 *           contador decrementado uma unica vez
 *  10) Cadastro concorrente de ROTAS (varios trechos por rota)
 *        -> os ids de cada rota sao reservados em bloco (consecutivos) e
 *           nunca se repetem entre rotas
 *
 * Compilar:
 *   gcc -Wall -O2 -o teste_concorrencia teste_concorrencia.c -lpthread
 *
 * Executar (servidor precisa estar rodando):
 *   ./teste_concorrencia <ip> [pasta_do_servidor]
 *   ex.: ./teste_concorrencia 127.0.0.1 ../servidor
 *   (se o IP nao for passado como argumento, o programa pergunta;
 *    se a pasta nao for passada, usa a pasta atual ".")
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define PORTA 65432
#define TAM_BUFFER 4096

char ip_servidor[100] = {0};
char dir_servidor[256] = ".";  /* pasta onde o servidor roda (para conferir os arquivos) */

/* ======================================================================
 *  Funcoes utilitarias de rede / parsing
 * ====================================================================== */

int conectar(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_port = htons(PORTA);

    struct hostent *host = gethostbyname(ip_servidor);
    if (host == NULL) {
        fprintf(stderr, "Nao foi possivel resolver o host: %s\n", ip_servidor);
        close(sock);
        return -1;
    }
    memcpy(&endereco.sin_addr, host->h_addr_list[0], host->h_length);

    if (connect(sock, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    return sock;
}

/* Envia uma string crua pelo socket (mesmo formato usado por cliente.c/motorista.c) */
void enviar(int sock, const char *msg) {
    ssize_t n = write(sock, msg, strlen(msg));
    (void)n;
}

/* Le uma resposta do servidor. Suficiente para as respostas curtas
 * ("CARONA_RESERVADA", etc.) e para os arrays JSON pequenos usados aqui. */
int receber(int sock, char *buffer, int tamanho) {
    memset(buffer, 0, tamanho);
    int total = 0;
    int bytes = read(sock, buffer, tamanho - 1);
    if (bytes > 0) total = bytes;
    buffer[total] = '\0';
    return total;
}

/* Extrai o valor inteiro de um campo "chave":numero dentro de um texto JSON. */
int extrair_int(const char *texto, const char *chave) {
    char alvo[64];
    snprintf(alvo, sizeof(alvo), "\"%s\":", chave);
    const char *p = strstr(texto, alvo);
    if (p == NULL) return -1;
    return atoi(p + strlen(alvo));
}

/* ======================================================================
 *  Verificacao dos arquivos do servidor (so funciona se o teste roda na
 *  mesma maquina do servidor e dir_servidor aponta para a pasta dele)
 * ====================================================================== */

/* 1 se a pasta do servidor esta acessivel (ArquivoID/id.txt legivel) */
int arquivos_acessiveis(void) {
    char caminho[512];
    snprintf(caminho, sizeof(caminho), "%s/ArquivoID/id.txt", dir_servidor);
    return access(caminho, R_OK) == 0;
}

/* Le o inteiro de um arquivo (caminho relativo a pasta do servidor). -1 se falhar. */
int ler_int_arquivo(const char *relativo) {
    char caminho[512];
    snprintf(caminho, sizeof(caminho), "%s/%s", dir_servidor, relativo);
    FILE *f = fopen(caminho, "r");
    if (f == NULL) return -1;
    int valor = -1;
    if (fscanf(f, "%d", &valor) != 1) valor = -1;
    fclose(f);
    return valor;
}

int arquivo_trecho_existe(int id) {
    char caminho[512];
    snprintf(caminho, sizeof(caminho), "%s/trechosCadastrados/trecho%d.json", dir_servidor, id);
    return access(caminho, F_OK) == 0;
}

/* Copia o conteudo de trechosCadastrados/trecho<ID>.json para buf. Retorna 1 se leu. */
int ler_arquivo_trecho(int id, char *buf, size_t tam) {
    char caminho[512];
    snprintf(caminho, sizeof(caminho), "%s/trechosCadastrados/trecho%d.json", dir_servidor, id);
    FILE *f = fopen(caminho, "r");
    if (f == NULL) return 0;
    size_t n = fread(buf, 1, tam - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

/* Capacidade gravada no arquivo do trecho, ou -1 se nao for possivel ler. */
int capacidade_arquivo(int id) {
    if (!arquivos_acessiveis()) return -1;
    char conteudo[TAM_BUFFER];
    if (!ler_arquivo_trecho(id, conteudo, sizeof(conteudo))) return -1;
    return extrair_int(conteudo, "capacidade");
}

int contar_ocorrencias(const char *texto, const char *trecho) {
    int total = 0;
    size_t len = strlen(trecho);
    for (const char *p = strstr(texto, trecho); p != NULL; p = strstr(p + len, trecho))
        total++;
    return total;
}

/* Extrai todos os valores de "id":N de um array JSON (resposta do listar_trechos). */
int extrair_todos_ids(const char *texto, int *ids, int max) {
    int n = 0;
    const char *chave = "\"id\":";
    for (const char *p = strstr(texto, chave); p != NULL && n < max; p = strstr(p + strlen(chave), chave))
        ids[n++] = atoi(p + strlen(chave));
    return n;
}

void aviso_arquivos_pulados(void) {
    printf("(conferencia dos arquivos pulada: pasta do servidor nao acessivel em '%s')\n", dir_servidor);
}

void email_unico(char *destino, size_t tamanho, const char *prefixo) {
    static pthread_mutex_t seqMutex = PTHREAD_MUTEX_INITIALIZER;
    static int seq = 0;
    int minhaSeq;
    pthread_mutex_lock(&seqMutex);
    minhaSeq = seq++;
    pthread_mutex_unlock(&seqMutex);
    snprintf(destino, tamanho, "%s_%ld_%d_%d@teste.com",
             prefixo, (long)time(NULL), (int)getpid(), minhaSeq);
}

void imprimir_titulo(const char *titulo) {
    printf("\n============================================================\n");
    printf(" %s\n", titulo);
    printf("============================================================\n");
}

/* ======================================================================
 *  TESTE 1 - Cadastro concorrente do mesmo email
 * ====================================================================== */

#define N_TESTE1 8

typedef struct {
    pthread_barrier_t *barreira;
    char email[80];
    char senha[20];
    char nome[50];
    int  sucesso;      /* 1 se recebeu CADASTRO_REALIZADO           */
    int  jaCadastrado; /* 1 se recebeu EMAIL_JA_CADASTRADO          */
    int  outro;        /* 1 se recebeu qualquer outra coisa/erro    */
} ArgTeste1;

void *thread_teste1(void *arg) {
    ArgTeste1 *a = (ArgTeste1 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"%s\","
        "\"status\":\"\",\"acao\":\"cadastro\"}",
        a->nome, a->email, a->senha);

    /* Espera todas as threads chegarem aqui para disparar juntas */
    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "CADASTRO_REALIZADO") == 0) a->sucesso = 1;
    else if (strcmp(buffer, "EMAIL_JA_CADASTRADO") == 0) a->jaCadastrado = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

void teste1_cadastro_concorrente(void) {
    imprimir_titulo("TESTE 1: Cadastro concorrente do MESMO email (cliente)");

    pthread_t threads[N_TESTE1];
    ArgTeste1 args[N_TESTE1];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE1);

    char emailCompartilhado[80];
    email_unico(emailCompartilhado, sizeof(emailCompartilhado), "corrida_cadastro");

    for (int i = 0; i < N_TESTE1; i++) {
        memset(&args[i], 0, sizeof(ArgTeste1));
        args[i].barreira = &barreira;
        snprintf(args[i].email, sizeof(args[i].email), "%s", emailCompartilhado);
        snprintf(args[i].senha, sizeof(args[i].senha), "senha123");
        snprintf(args[i].nome, sizeof(args[i].nome), "ClienteConcorrente%d", i);
    }

    printf("Disparando %d cadastros simultaneos para o email: %s\n", N_TESTE1, emailCompartilhado);

    for (int i = 0; i < N_TESTE1; i++)
        pthread_create(&threads[i], NULL, thread_teste1, &args[i]);
    for (int i = 0; i < N_TESTE1; i++)
        pthread_join(threads[i], NULL);

    int totalSucesso = 0, totalJaCadastrado = 0, totalOutro = 0;
    for (int i = 0; i < N_TESTE1; i++) {
        totalSucesso     += args[i].sucesso;
        totalJaCadastrado+= args[i].jaCadastrado;
        totalOutro       += args[i].outro;
    }

    printf("Resultado: %d sucesso(s) | %d 'ja cadastrado' | %d outro/erro\n",
           totalSucesso, totalJaCadastrado, totalOutro);

    if (totalSucesso == 1 && totalJaCadastrado == N_TESTE1 - 1 && totalOutro == 0)
        printf(">>> PASSOU: exatamente 1 cadastro foi aceito, os demais foram barrados.\n");
    else
        printf(">>> FALHOU: era esperado exatamente 1 sucesso e %d 'ja cadastrado' (condicao de corrida no cadastro!).\n",
               N_TESTE1 - 1);

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  Preparacao comum aos testes 2 e 3: cadastra um motorista e um trecho
 *  com capacidade conhecida, usando UMA conexao (sem concorrencia), e
 *  descobre o ID do trecho recem-criado via "listar_trechos".
 * ====================================================================== */

int cadastrar_trecho_para_teste(const char *emailMotorista, const char *nomeMotorista,
                                 const char *origem, const char *destino,
                                 int capacidade, float preco, char *avisoErro, size_t tamAviso) {
    char buffer[TAM_BUFFER];
    char msg[1024];
    int sock = conectar();
    if (sock < 0) { snprintf(avisoErro, tamAviso, "falha ao conectar"); return -1; }

    /* 1) cadastra o motorista */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"status\":\"\",\"acao\":\"cadastro\"}", nomeMotorista, emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "CADASTRO_REALIZADO") != 0) {
        snprintf(avisoErro, tamAviso, "cadastro do motorista falhou: %s", buffer);
        close(sock);
        return -1;
    }

    /* 2) cadastra o trecho (mesma conexao - o servidor aceita varias
     *    mensagens seguidas no mesmo socket) */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"acao\":\"cadastrar_trecho\",\"emailMotorista\":\"%s\","
        "\"origem\":\"%s\",\"destino\":\"%s\",\"data\":\"31/12/2099\",\"hora\":\"23:59\","
        "\"capacidade\":%d,\"preco\":%.2f,\"nome\":\"%s\",\"clientes\":[]}",
        emailMotorista, origem, destino, capacidade, preco, nomeMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "TRECHO_CADASTRADO") != 0) {
        snprintf(avisoErro, tamAviso, "cadastro do trecho falhou: %s", buffer);
        close(sock);
        return -1;
    }

    /* 3) descobre o ID que o servidor atribuiu ao trecho */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"acao\":\"listar_trechos\"}", nomeMotorista, emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    close(sock);

    int id = extrair_int(buffer, "id");
    if (id < 0) snprintf(avisoErro, tamAviso, "nao foi possivel obter o id do trecho (resposta: %s)", buffer);
    return id;
}

/* ======================================================================
 *  TESTE 2 - Reserva concorrente da mesma carona (estoura a capacidade?)
 * ====================================================================== */

#define N_TESTE2 10
#define CAPACIDADE_TESTE2 3

typedef struct {
    pthread_barrier_t *barreira;
    int  idCarona;
    char origem[50];
    char destino[50];
    char emailCliente[80];
    int  reservada;
    int  semAssento;
    int  outro;
} ArgTeste2;

void *thread_teste2(void *arg) {
    ArgTeste2 *a = (ArgTeste2 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"acao\":\"selecionar_carona\",\"emailCliente\":\"%s\","
        "\"idSelecionado\":%d,\"origem\":\"%s\",\"destino\":\"%s\"}",
        a->emailCliente, a->idCarona, a->origem, a->destino);

    pthread_barrier_wait(a->barreira);   /* todas disparam juntas */

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "CARONA_RESERVADA") == 0) a->reservada = 1;
    else if (strcmp(buffer, "ASSENTO_INDISPONIVEL") == 0) a->semAssento = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

/* Guarda, para o TESTE 3, um cliente que conseguiu reservar no TESTE 2 */
void teste2_reserva_concorrente(int *idCaronaOut, char *emailClienteReservadoOut, size_t tam) {
    imprimir_titulo("TESTE 2: Reserva concorrente da MESMA carona (capacidade limitada)");

    char emailMotorista[80], erro[256] = {0};
    email_unico(emailMotorista, sizeof(emailMotorista), "motorista_reserva");

    printf("Cadastrando 1 trecho Feira_de_Santana -> Salvador com capacidade = %d ...\n", CAPACIDADE_TESTE2);
    int id = cadastrar_trecho_para_teste(emailMotorista, "MotoristaTesteReserva",
                                          "Feira_de_Santana", "Salvador",
                                          CAPACIDADE_TESTE2, 25.0f, erro, sizeof(erro));
    if (id < 0) {
        printf(">>> ERRO DE PREPARACAO: %s\n>>> Teste 2 abortado.\n", erro);
        *idCaronaOut = -1;
        return;
    }
    printf("Trecho cadastrado com ID = %d\n", id);

    pthread_t threads[N_TESTE2];
    ArgTeste2 args[N_TESTE2];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE2);

    for (int i = 0; i < N_TESTE2; i++) {
        memset(&args[i], 0, sizeof(ArgTeste2));
        args[i].barreira = &barreira;
        args[i].idCarona = id;
        strncpy(args[i].origem, "Feira_de_Santana", sizeof(args[i].origem) - 1);
        strncpy(args[i].destino, "Salvador", sizeof(args[i].destino) - 1);
        email_unico(args[i].emailCliente, sizeof(args[i].emailCliente), "cliente_reserva");
    }

    printf("Disparando %d clientes tentando reservar os %d assento(s) ao mesmo tempo...\n",
           N_TESTE2, CAPACIDADE_TESTE2);

    for (int i = 0; i < N_TESTE2; i++)
        pthread_create(&threads[i], NULL, thread_teste2, &args[i]);
    for (int i = 0; i < N_TESTE2; i++)
        pthread_join(threads[i], NULL);

    int totalReservada = 0, totalSemAssento = 0, totalOutro = 0;
    int primeiroReservadoIdx = -1;
    for (int i = 0; i < N_TESTE2; i++) {
        totalReservada  += args[i].reservada;
        totalSemAssento += args[i].semAssento;
        totalOutro      += args[i].outro;
        if (args[i].reservada && primeiroReservadoIdx == -1) primeiroReservadoIdx = i;
    }

    printf("Resultado: %d reservada(s) | %d sem assento | %d outro/erro\n",
           totalReservada, totalSemAssento, totalOutro);

    if (totalReservada == CAPACIDADE_TESTE2 && totalOutro == 0)
        printf(">>> PASSOU: exatamente %d reservas vingaram (nenhum overbooking).\n", CAPACIDADE_TESTE2);
    else
        printf(">>> FALHOU: esperava exatamente %d reservas com sucesso e obteve %d "
               "(possivel condicao de corrida ao decrementar 'capacidade').\n",
               CAPACIDADE_TESTE2, totalReservada);

    if (arquivos_acessiveis()) {
        char conteudo[TAM_BUFFER];
        if (ler_arquivo_trecho(id, conteudo, sizeof(conteudo))) {
            int capArquivo = extrair_int(conteudo, "capacidade");
            int clientesArquivo = contar_ocorrencias(conteudo, "cliente_reserva_");
            printf("Arquivo trecho%d.json: capacidade = %d | clientes gravados = %d\n",
                   id, capArquivo, clientesArquivo);
            if (capArquivo == CAPACIDADE_TESTE2 - totalReservada && clientesArquivo == totalReservada)
                printf(">>> PASSOU (arquivos): o arquivo do trecho bate com as respostas do servidor.\n");
            else
                printf(">>> FALHOU (arquivos): esperava capacidade %d e %d cliente(s) gravado(s).\n",
                       CAPACIDADE_TESTE2 - totalReservada, totalReservada);
        } else {
            printf(">>> FALHOU (arquivos): arquivo trechosCadastrados/trecho%d.json nao encontrado.\n", id);
        }
    } else {
        aviso_arquivos_pulados();
    }
    *idCaronaOut = id;
    if (primeiroReservadoIdx >= 0)
        strncpy(emailClienteReservadoOut, args[primeiroReservadoIdx].emailCliente, tam - 1);
    else
        emailClienteReservadoOut[0] = '\0';

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 3 - Cancelamento concorrente da MESMA reserva
 * ====================================================================== */

typedef struct {
    pthread_barrier_t *barreira;
    int  idCarona;
    char emailCliente[80];
    int  cancelada;
    int  naoEncontrada;
    int  outro;
} ArgTeste3;

void *thread_teste3(void *arg) {
    ArgTeste3 *a = (ArgTeste3 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"acao\":\"cancelar_carona\",\"emailCliente\":\"%s\","
        "\"idSelecionado\":%d}", a->emailCliente, a->idCarona);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "CARONA_CANCELADA") == 0) a->cancelada = 1;
    else if (strcmp(buffer, "TRECHO_NAO_ENCONTRADO") == 0) a->naoEncontrada = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

void teste3_cancelamento_concorrente(int idCarona, const char *emailCliente) {
    imprimir_titulo("TESTE 3: Cancelamento concorrente da MESMA reserva");

    if (idCarona < 0 || emailCliente[0] == '\0') {
        printf(">>> Teste 3 pulado (nao ha uma reserva valida vinda do Teste 2).\n");
        return;
    }

    const int N = 2;
    pthread_t threads[N];
    ArgTeste3 args[N];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N);

    int capAntes = capacidade_arquivo(idCarona);

    for (int i = 0; i < N; i++) {
        memset(&args[i], 0, sizeof(ArgTeste3));
        args[i].barreira = &barreira;
        args[i].idCarona = idCarona;
        strncpy(args[i].emailCliente, emailCliente, sizeof(args[i].emailCliente) - 1);
    }

    printf("Duas threads tentando cancelar a MESMA reserva (id=%d, cliente=%s) ao mesmo tempo...\n",
           idCarona, emailCliente);

    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], NULL, thread_teste3, &args[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], NULL);

    int totalCancelada = 0, totalNaoEncontrada = 0, totalOutro = 0;
    for (int i = 0; i < N; i++) {
        totalCancelada    += args[i].cancelada;
        totalNaoEncontrada+= args[i].naoEncontrada;
        totalOutro        += args[i].outro;
    }

    printf("Resultado: %d cancelada(s) | %d 'nao encontrada' | %d outro/erro\n",
           totalCancelada, totalNaoEncontrada, totalOutro);

    if (totalCancelada == 1 && totalNaoEncontrada == 1 && totalOutro == 0)
        printf(">>> PASSOU: apenas uma das duas tentativas cancelou a reserva.\n");
    else
        printf(">>> FALHOU: esperava 1 cancelamento e 1 'nao encontrado' "
               "(possivel condicao de corrida / cancelamento duplicado).\n");

    if (capAntes >= 0) {
        int capDepois = capacidade_arquivo(idCarona);
        printf("Capacidade no arquivo do trecho %d: %d -> %d\n", idCarona, capAntes, capDepois);
        if (capDepois == capAntes + 1)
            printf(">>> PASSOU (arquivos): a capacidade subiu apenas 1 vez.\n");
        else
            printf(">>> FALHOU (arquivos): a capacidade deveria subir exatamente 1 (cancelamento duplicado?).\n");
    } else {
        aviso_arquivos_pulados();
    }
    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 4 - Cadastro concorrente de trechos por varios motoristas
 *            (o id reservado em ArquivoID/id.txt nao pode gerar IDs repetidos)
 * ====================================================================== */

#define N_TESTE4 12

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  idObtido;   /* -1 se deu erro */
    char erro[4200];
} ArgTeste4;

void *thread_teste4(void *arg) {
    ArgTeste4 *a = (ArgTeste4 *)arg;
    char buffer[TAM_BUFFER];
    char msg[1024];
    a->idObtido = -1;
    a->erro[0] = '\0';

    int sock = conectar();
    if (sock < 0) { snprintf(a->erro, sizeof(a->erro), "falha ao conectar"); return NULL; }

    /* cadastro do motorista (fora da secao cronometrada) */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"status\":\"\",\"acao\":\"cadastro\"}", a->nomeMotorista, a->emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "CADASTRO_REALIZADO") != 0) {
        snprintf(a->erro, sizeof(a->erro), "cadastro falhou: %s", buffer);
        close(sock);
        return NULL;
    }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"acao\":\"cadastrar_trecho\",\"emailMotorista\":\"%s\","
        "\"origem\":\"Feira_de_Santana\",\"destino\":\"Salvador\",\"data\":\"31/12/2099\","
        "\"hora\":\"22:00\",\"capacidade\":4,\"preco\":30.0,\"nome\":\"%s\",\"clientes\":[]}",
        a->emailMotorista, a->nomeMotorista);

    /* Todos os motoristas cadastram o trecho no MESMO instante */
    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "TRECHO_CADASTRADO") != 0) {
        snprintf(a->erro, sizeof(a->erro), "cadastro do trecho falhou: %s", buffer);
        close(sock);
        return NULL;
    }

    /* pergunta ao servidor qual ID ele proprio atribuiu a este trecho */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"acao\":\"listar_trechos\"}", a->nomeMotorista, a->emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    close(sock);

    a->idObtido = extrair_int(buffer, "id");
    return NULL;
}

void teste4_cadastro_trechos_concorrente(void) {
    imprimir_titulo("TESTE 4: Cadastro concorrente de trechos (varios motoristas)");

    pthread_t threads[N_TESTE4];
    ArgTeste4 args[N_TESTE4];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE4);

    int idAntes = ler_int_arquivo("ArquivoID/id.txt");
    int contAntes = ler_int_arquivo("ArquivoID/contadorTrechos.txt");

    for (int i = 0; i < N_TESTE4; i++) {
        memset(&args[i], 0, sizeof(ArgTeste4));
        args[i].barreira = &barreira;
        email_unico(args[i].emailMotorista, sizeof(args[i].emailMotorista), "motorista_idconc");
        snprintf(args[i].nomeMotorista, sizeof(args[i].nomeMotorista), "MotoristaIdConc%d", i);
    }

    printf("Disparando %d motoristas cadastrando um trecho cada, ao mesmo tempo...\n", N_TESTE4);

    for (int i = 0; i < N_TESTE4; i++)
        pthread_create(&threads[i], NULL, thread_teste4, &args[i]);
    for (int i = 0; i < N_TESTE4; i++)
        pthread_join(threads[i], NULL);

    int idsValidos[N_TESTE4];
    int totalValidos = 0, totalErros = 0;
    for (int i = 0; i < N_TESTE4; i++) {
        if (args[i].idObtido >= 0) {
            idsValidos[totalValidos++] = args[i].idObtido;
        } else {
            totalErros++;
            printf("  - motorista %d falhou: %s\n", i, args[i].erro);
        }
    }

    int duplicados = 0;
    for (int i = 0; i < totalValidos; i++)
        for (int j = i + 1; j < totalValidos; j++)
            if (idsValidos[i] == idsValidos[j]) duplicados++;

    printf("Resultado: %d trechos cadastrados com sucesso | %d erro(s) | %d ID(s) duplicado(s)\n",
           totalValidos, totalErros, duplicados);

    if (totalValidos == N_TESTE4 && duplicados == 0)
        printf(">>> PASSOU: todos os trechos foram cadastrados com IDs unicos.\n");
    else
        printf(">>> FALHOU: era esperado %d cadastros com IDs unicos e sem erros "
               "(possivel condicao de corrida na reserva de ids do servidor).\n", N_TESTE4);

    if (arquivos_acessiveis()) {
        int idDepois = ler_int_arquivo("ArquivoID/id.txt");
        int contDepois = ler_int_arquivo("ArquivoID/contadorTrechos.txt");
        int semArquivo = 0;
        for (int i = 0; i < totalValidos; i++)
            if (!arquivo_trecho_existe(idsValidos[i])) semArquivo++;
        printf("Arquivos: id.txt %d -> %d | contadorTrechos.txt %d -> %d | trechos sem arquivo proprio: %d\n",
               idAntes, idDepois, contAntes, contDepois, semArquivo);
        if (idDepois - idAntes == N_TESTE4 && contDepois - contAntes == N_TESTE4 && semArquivo == 0)
            printf(">>> PASSOU (arquivos): id e contador subiram %d e cada trecho tem o seu arquivo.\n", N_TESTE4);
        else
            printf(">>> FALHOU (arquivos): esperava id e contador +%d e um arquivo por trecho.\n", N_TESTE4);
    } else {
        aviso_arquivos_pulados();
    }
    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 5 - Cadastro concorrente do mesmo email de MOTORISTA
 *            (mesma ideia do Teste 1, mas para a classe Motorista)
 * ====================================================================== */

#define N_TESTE5 8

typedef struct {
    pthread_barrier_t *barreira;
    char email[80];
    char senha[20];
    char nome[50];
    int  sucesso;
    int  jaCadastrado;
    int  outro;
} ArgTeste5;

void *thread_teste5(void *arg) {
    ArgTeste5 *a = (ArgTeste5 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"%s\","
        "\"status\":\"\",\"acao\":\"cadastro\"}",
        a->nome, a->email, a->senha);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "CADASTRO_REALIZADO") == 0) a->sucesso = 1;
    else if (strcmp(buffer, "EMAIL_JA_CADASTRADO") == 0) a->jaCadastrado = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

void teste5_cadastro_motorista_concorrente(void) {
    imprimir_titulo("TESTE 5: Cadastro concorrente do MESMO email (motorista)");

    pthread_t threads[N_TESTE5];
    ArgTeste5 args[N_TESTE5];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE5);

    char emailCompartilhado[80];
    email_unico(emailCompartilhado, sizeof(emailCompartilhado), "corrida_cadastro_motorista");

    for (int i = 0; i < N_TESTE5; i++) {
        memset(&args[i], 0, sizeof(ArgTeste5));
        args[i].barreira = &barreira;
        snprintf(args[i].email, sizeof(args[i].email), "%s", emailCompartilhado);
        snprintf(args[i].senha, sizeof(args[i].senha), "senha123");
        snprintf(args[i].nome, sizeof(args[i].nome), "MotoristaConcorrente%d", i);
    }

    printf("Disparando %d cadastros simultaneos para o email: %s\n", N_TESTE5, emailCompartilhado);

    for (int i = 0; i < N_TESTE5; i++)
        pthread_create(&threads[i], NULL, thread_teste5, &args[i]);
    for (int i = 0; i < N_TESTE5; i++)
        pthread_join(threads[i], NULL);

    int totalSucesso = 0, totalJaCadastrado = 0, totalOutro = 0;
    for (int i = 0; i < N_TESTE5; i++) {
        totalSucesso     += args[i].sucesso;
        totalJaCadastrado+= args[i].jaCadastrado;
        totalOutro       += args[i].outro;
    }

    printf("Resultado: %d sucesso(s) | %d 'ja cadastrado' | %d outro/erro\n",
           totalSucesso, totalJaCadastrado, totalOutro);

    if (totalSucesso == 1 && totalJaCadastrado == N_TESTE5 - 1 && totalOutro == 0)
        printf(">>> PASSOU: exatamente 1 cadastro de motorista foi aceito, os demais foram barrados.\n");
    else
        printf(">>> FALHOU: era esperado exatamente 1 sucesso e %d 'ja cadastrado' (condicao de corrida no cadastro!).\n",
               N_TESTE5 - 1);

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 6 - Reservas simultaneas em VARIOS trechos DIFERENTES
 *            (sem contencao real entre elas -> nao deveria travar nem
 *            demorar, e todas devem ter sucesso, ja que cada uma mexe
 *            num trecho distinto)
 * ====================================================================== */

#define N_TESTE6 6

typedef struct {
    pthread_barrier_t *barreira;
    int  idCarona;
    char emailCliente[80];
    int  reservada;
    int  outro;
} ArgTeste6;

void *thread_teste6(void *arg) {
    ArgTeste6 *a = (ArgTeste6 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"acao\":\"selecionar_carona\",\"emailCliente\":\"%s\","
        "\"idSelecionado\":%d,\"origem\":\"Feira_de_Santana\",\"destino\":\"Salvador\"}",
        a->emailCliente, a->idCarona);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "CARONA_RESERVADA") == 0) a->reservada = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

void teste6_trechos_diferentes_simultaneos(void) {
    imprimir_titulo("TESTE 6: Reservas simultaneas em trechos DIFERENTES (sem contencao / sem deadlock)");

    int ids[N_TESTE6];
    printf("Cadastrando %d trechos independentes (Feira_de_Santana -> Salvador, capacidade=1)...\n", N_TESTE6);
    for (int i = 0; i < N_TESTE6; i++) {
        char emailMotorista[80], nomeMotorista[50], erro[256] = {0};
        email_unico(emailMotorista, sizeof(emailMotorista), "motorista_multi");
        snprintf(nomeMotorista, sizeof(nomeMotorista), "MotoristaMulti%d", i);
        ids[i] = cadastrar_trecho_para_teste(emailMotorista, nomeMotorista,
                                              "Feira_de_Santana", "Salvador", 1, 20.0f,
                                              erro, sizeof(erro));
        if (ids[i] < 0) {
            printf(">>> ERRO DE PREPARACAO no trecho %d: %s\n>>> Teste 6 abortado.\n", i, erro);
            return;
        }
    }

    pthread_t threads[N_TESTE6];
    ArgTeste6 args[N_TESTE6];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE6);

    for (int i = 0; i < N_TESTE6; i++) {
        memset(&args[i], 0, sizeof(ArgTeste6));
        args[i].barreira = &barreira;
        args[i].idCarona = ids[i];
        email_unico(args[i].emailCliente, sizeof(args[i].emailCliente), "cliente_multi");
    }

    printf("Disparando %d clientes reservando %d trechos DIFERENTES ao mesmo tempo...\n",
           N_TESTE6, N_TESTE6);

    struct timespec ini, fim;
    clock_gettime(CLOCK_MONOTONIC, &ini);

    for (int i = 0; i < N_TESTE6; i++)
        pthread_create(&threads[i], NULL, thread_teste6, &args[i]);
    for (int i = 0; i < N_TESTE6; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &fim);
    double segundos = (fim.tv_sec - ini.tv_sec) + (fim.tv_nsec - ini.tv_nsec) / 1e9;

    int totalReservada = 0, totalOutro = 0;
    for (int i = 0; i < N_TESTE6; i++) {
        totalReservada += args[i].reservada;
        totalOutro     += args[i].outro;
    }

    printf("Resultado: %d reservada(s) de %d | %d outro/erro | tempo total: %.3fs\n",
           totalReservada, N_TESTE6, totalOutro, segundos);

    if (totalReservada == N_TESTE6 && totalOutro == 0)
        printf(">>> PASSOU: todas as reservas em trechos distintos foram bem-sucedidas, sem travar.\n");
    else
        printf(">>> FALHOU: esperava %d reservas bem-sucedidas (trechos sem relacao entre si).\n", N_TESTE6);

    if (segundos > 5.0)
        printf(">>> ATENCAO: demorou %.3fs para %d operacoes simples e independentes - "
               "investigar contencao excessiva no mutex de trechos.\n", segundos, N_TESTE6);

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 7 - Leitura concorrente (listar_trechos) enquanto outros
 *            motoristas ESCREVEM (cadastram trechos novos) ao mesmo
 *            tempo (cada trecho e gravado no seu proprio arquivo)
 * ====================================================================== */

#define N_LEITORES_TESTE7 5
#define N_ESCRITORES_TESTE7 5

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  respostaValida; /* comeca com '[' e termina com ']' */
} ArgLeitorTeste7;

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  sucesso;
} ArgEscritorTeste7;

void *thread_leitor_teste7(void *arg) {
    ArgLeitorTeste7 *a = (ArgLeitorTeste7 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->respostaValida = 0; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"acao\":\"listar_trechos\"}", a->nomeMotorista, a->emailMotorista);

    pthread_barrier_wait(a->barreira);

    /* varias leituras seguidas na mesma conexao, no meio das escritas */
    a->respostaValida = 1;
    for (int i = 0; i < 5; i++) {
        enviar(sock, msg);
        receber(sock, buffer, sizeof(buffer));
        size_t len = strlen(buffer);
        if (len == 0 || buffer[0] != '[' || buffer[len - 1] != ']') {
            a->respostaValida = 0;
        }
    }

    close(sock);
    return NULL;
}

void *thread_escritor_teste7(void *arg) {
    ArgEscritorTeste7 *a = (ArgEscritorTeste7 *)arg;
    char buffer[TAM_BUFFER];
    char msg[1024];

    int sock = conectar();
    if (sock < 0) { a->sucesso = 0; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"status\":\"\",\"acao\":\"cadastro\"}", a->nomeMotorista, a->emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "CADASTRO_REALIZADO") != 0) { a->sucesso = 0; close(sock); return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"acao\":\"cadastrar_trecho\",\"emailMotorista\":\"%s\","
        "\"origem\":\"Feira_de_Santana\",\"destino\":\"Salvador\",\"data\":\"31/12/2099\","
        "\"hora\":\"21:00\",\"capacidade\":2,\"preco\":15.0,\"nome\":\"%s\",\"clientes\":[]}",
        a->emailMotorista, a->nomeMotorista);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    a->sucesso = (strcmp(buffer, "TRECHO_CADASTRADO") == 0);

    close(sock);
    return NULL;
}

void teste7_leitura_escrita_concorrente(void) {
    imprimir_titulo("TESTE 7: Leituras concorrentes durante escritas em arquivos de trechos");

    char emailMotoristaBase[80], erro[256] = {0};
    email_unico(emailMotoristaBase, sizeof(emailMotoristaBase), "motorista_leitor_base");
    printf("Preparando 1 motorista com um trecho ja cadastrado (alvo das leituras)...\n");
    int idBase = cadastrar_trecho_para_teste(emailMotoristaBase, "MotoristaLeitorBase",
                                              "Feira_de_Santana", "Salvador", 2, 10.0f,
                                              erro, sizeof(erro));
    if (idBase < 0) {
        printf(">>> ERRO DE PREPARACAO: %s\n>>> Teste 7 abortado.\n", erro);
        return;
    }

    const int total = N_LEITORES_TESTE7 + N_ESCRITORES_TESTE7;
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, total);

    pthread_t threadsLeitores[N_LEITORES_TESTE7];
    ArgLeitorTeste7 argsLeitores[N_LEITORES_TESTE7];
    for (int i = 0; i < N_LEITORES_TESTE7; i++) {
        memset(&argsLeitores[i], 0, sizeof(ArgLeitorTeste7));
        argsLeitores[i].barreira = &barreira;
        snprintf(argsLeitores[i].emailMotorista, sizeof(argsLeitores[i].emailMotorista), "%s", emailMotoristaBase);
        snprintf(argsLeitores[i].nomeMotorista, sizeof(argsLeitores[i].nomeMotorista), "MotoristaLeitorBase");
    }

    pthread_t threadsEscritores[N_ESCRITORES_TESTE7];
    ArgEscritorTeste7 argsEscritores[N_ESCRITORES_TESTE7];
    for (int i = 0; i < N_ESCRITORES_TESTE7; i++) {
        memset(&argsEscritores[i], 0, sizeof(ArgEscritorTeste7));
        argsEscritores[i].barreira = &barreira;
        email_unico(argsEscritores[i].emailMotorista, sizeof(argsEscritores[i].emailMotorista), "motorista_escritor");
        snprintf(argsEscritores[i].nomeMotorista, sizeof(argsEscritores[i].nomeMotorista), "MotoristaEscritor%d", i);
    }

    printf("Disparando %d leitores (listar_trechos x5 cada) e %d escritores (cadastrar_trecho) ao mesmo tempo...\n",
           N_LEITORES_TESTE7, N_ESCRITORES_TESTE7);

    for (int i = 0; i < N_LEITORES_TESTE7; i++)
        pthread_create(&threadsLeitores[i], NULL, thread_leitor_teste7, &argsLeitores[i]);
    for (int i = 0; i < N_ESCRITORES_TESTE7; i++)
        pthread_create(&threadsEscritores[i], NULL, thread_escritor_teste7, &argsEscritores[i]);

    for (int i = 0; i < N_LEITORES_TESTE7; i++)
        pthread_join(threadsLeitores[i], NULL);
    for (int i = 0; i < N_ESCRITORES_TESTE7; i++)
        pthread_join(threadsEscritores[i], NULL);

    int leiturasValidas = 0, escritasValidas = 0;
    for (int i = 0; i < N_LEITORES_TESTE7; i++) leiturasValidas += argsLeitores[i].respostaValida;
    for (int i = 0; i < N_ESCRITORES_TESTE7; i++) escritasValidas += argsEscritores[i].sucesso;

    printf("Resultado: %d/%d leitores com respostas sempre validas | %d/%d escritas com sucesso\n",
           leiturasValidas, N_LEITORES_TESTE7, escritasValidas, N_ESCRITORES_TESTE7);

    if (leiturasValidas == N_LEITORES_TESTE7 && escritasValidas == N_ESCRITORES_TESTE7)
        printf(">>> PASSOU: nenhuma leitura recebeu resposta corrompida durante as escritas simultaneas.\n");
    else
        printf(">>> FALHOU: houve leitura(s) corrompida(s) ou escrita(s) que falharam sob concorrencia "
               "(possivel corrupcao dos arquivos de trechos).\n");

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 8 - Corrida entre o MOTORISTA cancelando um trecho inteiro
 *            (cancelar_trecho) e um CLIENTE reservando esse mesmo
 *            trecho (selecionar_carona), ao mesmo tempo
 * ====================================================================== */

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  idTrecho;
    char resposta[TAM_BUFFER];
} ArgCancelaTeste8;

typedef struct {
    pthread_barrier_t *barreira;
    char emailCliente[80];
    int  idTrecho;
    char resposta[TAM_BUFFER];
} ArgReservaTeste8;

void *thread_cancela_teste8(void *arg) {
    ArgCancelaTeste8 *a = (ArgCancelaTeste8 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { snprintf(a->resposta, sizeof(a->resposta), "ERRO_CONEXAO"); return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"idSelecionado\":%d,"
        "\"acao\":\"cancelar_trecho\"}", a->nomeMotorista, a->emailMotorista, a->idTrecho);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    snprintf(a->resposta, sizeof(a->resposta), "%s", buffer);

    close(sock);
    return NULL;
}

void *thread_reserva_teste8(void *arg) {
    ArgReservaTeste8 *a = (ArgReservaTeste8 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { snprintf(a->resposta, sizeof(a->resposta), "ERRO_CONEXAO"); return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"acao\":\"selecionar_carona\",\"emailCliente\":\"%s\","
        "\"idSelecionado\":%d,\"origem\":\"Feira_de_Santana\",\"destino\":\"Salvador\"}",
        a->emailCliente, a->idTrecho);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    snprintf(a->resposta, sizeof(a->resposta), "%s", buffer);

    close(sock);
    return NULL;
}

/* Consulta os avisos de trecho removido de um cliente (listar_avisos). */
void consultar_avisos(const char *emailCliente, char *buffer, size_t tam) {
    char msg[512];
    buffer[0] = '\0';
    int sock = conectar();
    if (sock < 0) return;
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Cliente\",\"acao\":\"listar_avisos\",\"email\":\"%s\"}", emailCliente);
    enviar(sock, msg);
    receber(sock, buffer, (int)tam);
    close(sock);
}

#define RODADAS_TESTE8 10

void teste8_cancelar_trecho_vs_reservar(void) {
    imprimir_titulo("TESTE 8: Motorista cancelando o trecho x Cliente reservando o MESMO trecho");

    int reservaVenceu = 0, cancelVenceu = 0, inconsistencias = 0, errosPrep = 0;

    for (int rodada = 1; rodada <= RODADAS_TESTE8; rodada++) {
        char emailMotorista[80], erro[256] = {0};
        email_unico(emailMotorista, sizeof(emailMotorista), "motorista_corrida_cancel");
        int id = cadastrar_trecho_para_teste(emailMotorista, "MotoristaCorridaCancel",
                                              "Feira_de_Santana", "Salvador", 1, 40.0f,
                                              erro, sizeof(erro));
        if (id < 0) {
            printf("  rodada %d: ERRO DE PREPARACAO: %s\n", rodada, erro);
            errosPrep++;
            continue;
        }

        pthread_barrier_t barreira;
        pthread_barrier_init(&barreira, NULL, 2);

        pthread_t tCancela, tReserva;
        ArgCancelaTeste8 argCancela;
        ArgReservaTeste8 argReserva;
        memset(&argCancela, 0, sizeof(argCancela));
        memset(&argReserva, 0, sizeof(argReserva));

        argCancela.barreira = &barreira;
        snprintf(argCancela.emailMotorista, sizeof(argCancela.emailMotorista), "%s", emailMotorista);
        snprintf(argCancela.nomeMotorista, sizeof(argCancela.nomeMotorista), "MotoristaCorridaCancel");
        argCancela.idTrecho = id;

        argReserva.barreira = &barreira;
        email_unico(argReserva.emailCliente, sizeof(argReserva.emailCliente), "cliente_corrida_cancel");
        argReserva.idTrecho = id;

        pthread_create(&tCancela, NULL, thread_cancela_teste8, &argCancela);
        pthread_create(&tReserva, NULL, thread_reserva_teste8, &argReserva);
        pthread_join(tCancela, NULL);
        pthread_join(tReserva, NULL);
        pthread_barrier_destroy(&barreira);

        /* O mutex do trecho serializa as duas operacoes. So existem 2 desfechos validos:
         *   (a) reserva primeiro: CARONA_RESERVADA + TRECHO_CANCELADO  -> cliente recebe aviso
         *   (b) cancelamento primeiro: TRECHO_NAO_ENCONTRADO + TRECHO_CANCELADO -> sem aviso */
        int cancelOk = strcmp(argCancela.resposta, "TRECHO_CANCELADO") == 0;
        int reservaOk = strcmp(argReserva.resposta, "CARONA_RESERVADA") == 0;
        int reservaNaoEnc = strcmp(argReserva.resposta, "TRECHO_NAO_ENCONTRADO") == 0;

        if (!cancelOk || (!reservaOk && !reservaNaoEnc)) {
            printf("  rodada %d (trecho %d): respostas inesperadas -> cancelamento='%s' | reserva='%s'\n",
                   rodada, id, argCancela.resposta, argReserva.resposta);
            inconsistencias++;
            continue;
        }

        char avisos[TAM_BUFFER];
        consultar_avisos(argReserva.emailCliente, avisos, sizeof(avisos));
        int temAviso = (strstr(avisos, "nomeMotorista") != NULL);

        if (reservaOk) {
            reservaVenceu++;
            if (!temAviso) {
                printf("  rodada %d (trecho %d): reserva 'orfa' - cliente reservou, o trecho foi cancelado e NAO recebeu aviso\n",
                       rodada, id);
                inconsistencias++;
            }
        } else {
            cancelVenceu++;
            if (temAviso) {
                printf("  rodada %d (trecho %d): cliente recebeu aviso sem ter reservado\n", rodada, id);
                inconsistencias++;
            }
        }

        if (arquivos_acessiveis() && arquivo_trecho_existe(id)) {
            printf("  rodada %d (trecho %d): o arquivo do trecho cancelado ainda existe\n", rodada, id);
            inconsistencias++;
        }
    }

    printf("Resultado: %d rodada(s) | reserva antes do cancelamento: %d | cancelamento antes da reserva: %d | "
           "inconsistencias: %d | erros de preparacao: %d\n",
           RODADAS_TESTE8, reservaVenceu, cancelVenceu, inconsistencias, errosPrep);

    if (inconsistencias == 0 && errosPrep == 0)
        printf(">>> PASSOU: em todas as rodadas o resultado foi consistente (sem reserva orfa).\n");
    else
        printf(">>> FALHOU: houve resposta inesperada, reserva orfa ou arquivo que nao foi removido.\n");

    if (reservaVenceu == 0 || cancelVenceu == 0)
        printf("(obs.: so um dos dois desfechos apareceu nestas rodadas; rode de novo para ver o outro)\n");
}

/* ======================================================================
 *  TESTE 9 - Cancelamento concorrente do MESMO trecho pelo motorista
 *            (so um pode ter sucesso; o contador cai 1 unica vez)
 * ====================================================================== */

#define N_TESTE9 6

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  idTrecho;
    int  cancelado;
    int  naoEncontrado;
    int  outro;
} ArgTeste9;

void *thread_teste9(void *arg) {
    ArgTeste9 *a = (ArgTeste9 *)arg;
    char buffer[TAM_BUFFER];
    char msg[512];

    int sock = conectar();
    if (sock < 0) { a->outro = 1; return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"idSelecionado\":%d,"
        "\"acao\":\"cancelar_trecho\"}", a->nomeMotorista, a->emailMotorista, a->idTrecho);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));

    if (strcmp(buffer, "TRECHO_CANCELADO") == 0) a->cancelado = 1;
    else if (strcmp(buffer, "TRECHO_NAO_ENCONTRADO") == 0) a->naoEncontrado = 1;
    else a->outro = 1;

    close(sock);
    return NULL;
}

void teste9_cancelar_mesmo_trecho(void) {
    imprimir_titulo("TESTE 9: Cancelamento concorrente do MESMO trecho (motorista)");

    char emailMotorista[80], erro[256] = {0};
    email_unico(emailMotorista, sizeof(emailMotorista), "motorista_cancel_conc");
    int id = cadastrar_trecho_para_teste(emailMotorista, "MotoristaCancelConc",
                                          "Feira_de_Santana", "Salvador", 2, 18.0f,
                                          erro, sizeof(erro));
    if (id < 0) {
        printf(">>> ERRO DE PREPARACAO: %s\n>>> Teste 9 abortado.\n", erro);
        return;
    }
    printf("Trecho cadastrado com ID = %d\n", id);

    int contAntes = arquivos_acessiveis() ? ler_int_arquivo("ArquivoID/contadorTrechos.txt") : -1;

    pthread_t threads[N_TESTE9];
    ArgTeste9 args[N_TESTE9];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE9);

    for (int i = 0; i < N_TESTE9; i++) {
        memset(&args[i], 0, sizeof(ArgTeste9));
        args[i].barreira = &barreira;
        snprintf(args[i].emailMotorista, sizeof(args[i].emailMotorista), "%s", emailMotorista);
        snprintf(args[i].nomeMotorista, sizeof(args[i].nomeMotorista), "MotoristaCancelConc");
        args[i].idTrecho = id;
    }

    printf("Disparando %d cancelamentos simultaneos do trecho %d...\n", N_TESTE9, id);

    for (int i = 0; i < N_TESTE9; i++)
        pthread_create(&threads[i], NULL, thread_teste9, &args[i]);
    for (int i = 0; i < N_TESTE9; i++)
        pthread_join(threads[i], NULL);

    int totalCancelado = 0, totalNaoEncontrado = 0, totalOutro = 0;
    for (int i = 0; i < N_TESTE9; i++) {
        totalCancelado     += args[i].cancelado;
        totalNaoEncontrado += args[i].naoEncontrado;
        totalOutro         += args[i].outro;
    }

    printf("Resultado: %d cancelado(s) | %d 'nao encontrado' | %d outro/erro\n",
           totalCancelado, totalNaoEncontrado, totalOutro);

    if (totalCancelado == 1 && totalNaoEncontrado == N_TESTE9 - 1 && totalOutro == 0)
        printf(">>> PASSOU: apenas um cancelamento valeu, os demais viram 'nao encontrado'.\n");
    else
        printf(">>> FALHOU: esperava 1 'TRECHO_CANCELADO' e %d 'TRECHO_NAO_ENCONTRADO'.\n", N_TESTE9 - 1);

    if (contAntes >= 0) {
        int contDepois = ler_int_arquivo("ArquivoID/contadorTrechos.txt");
        int existe = arquivo_trecho_existe(id);
        printf("Arquivos: contadorTrechos.txt %d -> %d | arquivo do trecho ainda existe: %s\n",
               contAntes, contDepois, existe ? "sim" : "nao");
        if (contDepois == contAntes - 1 && !existe)
            printf(">>> PASSOU (arquivos): arquivo removido e contador decrementado uma unica vez.\n");
        else
            printf(">>> FALHOU (arquivos): esperava contador -1 e arquivo removido.\n");
    } else {
        aviso_arquivos_pulados();
    }

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  TESTE 10 - Cadastro concorrente de ROTAS (varios trechos por rota)
 *             Cada rota reserva seus ids em bloco: os 2 ids da mesma rota
 *             devem ser consecutivos e nenhum id pode se repetir entre rotas.
 *             Usa a ida e a volta CIDADE_A <-> CIDADE_B; o mapa do servidor
 *             precisa permitir os dois sentidos.
 * ====================================================================== */

#define N_TESTE10 6
#define TRECHOS_POR_ROTA 2

typedef struct {
    pthread_barrier_t *barreira;
    char emailMotorista[80];
    char nomeMotorista[50];
    int  rotaCadastrada;
    char respostaRota[128];
    int  ids[8];
    int  nIds;
} ArgTeste10;

void *thread_teste10(void *arg) {
    ArgTeste10 *a = (ArgTeste10 *)arg;
    char buffer[TAM_BUFFER];
    char msg[2048];

    int sock = conectar();
    if (sock < 0) { snprintf(a->respostaRota, sizeof(a->respostaRota), "ERRO_CONEXAO"); return NULL; }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"status\":\"\",\"acao\":\"cadastro\"}", a->nomeMotorista, a->emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    if (strcmp(buffer, "CADASTRO_REALIZADO") != 0) {
        snprintf(a->respostaRota, sizeof(a->respostaRota), "cadastro falhou: %.80s", buffer);
        close(sock);
        return NULL;
    }

    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"acao\":\"cadastrar_rota\",\"nome\":\"%s\",\"trechos\":["
        "{\"origem\":\"Feira_de_Santana\",\"destino\":\"Salvador\",\"data\":\"31/12/2099\",\"hora\":\"08:00\","
        "\"capacidade\":3,\"preco\":10.0,\"emailMotorista\":\"%s\"},"
        "{\"origem\":\"Salvador\",\"destino\":\"Feira_de_Santana\",\"data\":\"31/12/2099\",\"hora\":\"12:00\","
        "\"capacidade\":3,\"preco\":10.0,\"emailMotorista\":\"%s\"}]}",
        a->nomeMotorista, a->emailMotorista, a->emailMotorista);

    pthread_barrier_wait(a->barreira);

    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    snprintf(a->respostaRota, sizeof(a->respostaRota), "%.120s", buffer);
    if (strcmp(buffer, "ROTA_CADASTRADA") != 0) {
        close(sock);
        return NULL;
    }
    a->rotaCadastrada = 1;

    /* descobre os ids que o servidor deu aos trechos da rota */
    snprintf(msg, sizeof(msg),
        "{\"classe\":\"Motorista\",\"nome\":\"%s\",\"email\":\"%s\",\"senha\":\"senha123\","
        "\"acao\":\"listar_trechos\"}", a->nomeMotorista, a->emailMotorista);
    enviar(sock, msg);
    receber(sock, buffer, sizeof(buffer));
    close(sock);

    a->nIds = extrair_todos_ids(buffer, a->ids, 8);
    return NULL;
}

void teste10_rotas_concorrentes(void) {
    imprimir_titulo("TESTE 10: Cadastro concorrente de ROTAS (ids reservados em bloco)");

    pthread_t threads[N_TESTE10];
    ArgTeste10 args[N_TESTE10];
    pthread_barrier_t barreira;
    pthread_barrier_init(&barreira, NULL, N_TESTE10);

    int idAntes = arquivos_acessiveis() ? ler_int_arquivo("ArquivoID/id.txt") : -1;
    int contAntes = arquivos_acessiveis() ? ler_int_arquivo("ArquivoID/contadorTrechos.txt") : -1;

    for (int i = 0; i < N_TESTE10; i++) {
        memset(&args[i], 0, sizeof(ArgTeste10));
        args[i].barreira = &barreira;
        email_unico(args[i].emailMotorista, sizeof(args[i].emailMotorista), "motorista_rota");
        snprintf(args[i].nomeMotorista, sizeof(args[i].nomeMotorista), "MotoristaRota%d", i);
    }

    printf("Disparando %d motoristas cadastrando uma rota de %d trechos cada, ao mesmo tempo...\n",
           N_TESTE10, TRECHOS_POR_ROTA);

    for (int i = 0; i < N_TESTE10; i++)
        pthread_create(&threads[i], NULL, thread_teste10, &args[i]);
    for (int i = 0; i < N_TESTE10; i++)
        pthread_join(threads[i], NULL);

    int rotasOk = 0, rotasBemFormadas = 0, duplicados = 0;
    int todosIds[N_TESTE10 * TRECHOS_POR_ROTA];
    int totalIds = 0;
    for (int i = 0; i < N_TESTE10; i++) {
        if (!args[i].rotaCadastrada) {
            printf("  - motorista %d: rota nao cadastrada (resposta: %s)\n", i, args[i].respostaRota);
            continue;
        }
        rotasOk++;
        if (args[i].nIds == TRECHOS_POR_ROTA && args[i].ids[1] == args[i].ids[0] + 1) {
            rotasBemFormadas++;
            for (int k = 0; k < TRECHOS_POR_ROTA; k++) todosIds[totalIds++] = args[i].ids[k];
        } else {
            printf("  - motorista %d: ids da rota fora do esperado (%d id(s) lidos; ids consecutivos esperados)\n",
                   i, args[i].nIds);
        }
    }
    for (int i = 0; i < totalIds; i++)
        for (int j = i + 1; j < totalIds; j++)
            if (todosIds[i] == todosIds[j]) duplicados++;

    printf("Resultado: %d/%d rota(s) cadastrada(s) | %d com ids consecutivos | %d ID(s) duplicado(s)\n",
           rotasOk, N_TESTE10, rotasBemFormadas, duplicados);

    if (rotasOk == N_TESTE10 && rotasBemFormadas == N_TESTE10 && duplicados == 0)
        printf(">>> PASSOU: todas as rotas foram gravadas com blocos de ids consecutivos e sem repeticao.\n");
    else
        printf(">>> FALHOU: esperava %d rotas, ids consecutivos dentro de cada rota e nenhum id repetido "
               "(se as rotas foram recusadas, confira se o mapa liga %s <-> %s nos dois sentidos).\n",
               N_TESTE10, "Feira_de_Santana", "Salvador");

    if (idAntes >= 0) {
        int total = N_TESTE10 * TRECHOS_POR_ROTA;
        int idDepois = ler_int_arquivo("ArquivoID/id.txt");
        int contDepois = ler_int_arquivo("ArquivoID/contadorTrechos.txt");
        int semArquivo = 0;
        for (int i = 0; i < totalIds; i++)
            if (!arquivo_trecho_existe(todosIds[i])) semArquivo++;
        printf("Arquivos: id.txt %d -> %d | contadorTrechos.txt %d -> %d | trechos sem arquivo proprio: %d\n",
               idAntes, idDepois, contAntes, contDepois, semArquivo);
        if (idDepois - idAntes == total && contDepois - contAntes == total && semArquivo == 0)
            printf(">>> PASSOU (arquivos): id e contador subiram %d e cada trecho tem o seu arquivo.\n", total);
        else
            printf(">>> FALHOU (arquivos): esperava id e contador +%d e um arquivo por trecho.\n", total);
    } else {
        aviso_arquivos_pulados();
    }

    pthread_barrier_destroy(&barreira);
}

/* ======================================================================
 *  main
 * ====================================================================== */

int main(int argc, char *argv[]) {
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    if (argc >= 2) {
        strncpy(ip_servidor, argv[1], sizeof(ip_servidor) - 1);
    } else {
        printf("Digite o IP do servidor: ");
        if (scanf("%99s", ip_servidor) != 1) {
            fprintf(stderr, "IP invalido.\n");
            return EXIT_FAILURE;
        }
    }
    if (argc >= 3) {
        strncpy(dir_servidor, argv[2], sizeof(dir_servidor) - 1);
    }

    printf("Testes de concorrencia contra o servidor em %s:%d\n", ip_servidor, PORTA);
    if (arquivos_acessiveis())
        printf("Pasta do servidor acessivel em '%s': os arquivos de trechos, id e contador tambem serao conferidos.\n",
               dir_servidor);
    else
        printf("Pasta do servidor NAO acessivel em '%s': so as respostas do servidor serao conferidas "
               "(passe a pasta como 2o argumento para conferir os arquivos).\n", dir_servidor);

    teste1_cadastro_concorrente();

    int idCarona = -1;
    char emailClienteReservado[80] = {0};
    teste2_reserva_concorrente(&idCarona, emailClienteReservado, sizeof(emailClienteReservado));

    teste3_cancelamento_concorrente(idCarona, emailClienteReservado);

    teste4_cadastro_trechos_concorrente();

    teste5_cadastro_motorista_concorrente();

    teste6_trechos_diferentes_simultaneos();

    teste7_leitura_escrita_concorrente();

    teste8_cancelar_trecho_vs_reservar();

    teste9_cancelar_mesmo_trecho();

    teste10_rotas_concorrentes();

    imprimir_titulo("FIM DOS TESTES DE CONCORRENCIA");
    return 0;
}