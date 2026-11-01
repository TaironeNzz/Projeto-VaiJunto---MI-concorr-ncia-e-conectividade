/*
 * consorcio.c - cooperação entre os servidores das empresas do consórcio VaiJunto.
 *
 * Conteúdo:
 *   1. configuração, pares (peers) e detector de falhas por timeout;
 *   2. cliente HTTP mínimo (sockets + poll) com timeout e propagação do relógio de Lamport;
 *   3. PARTICIPANTE do 2PC: PREPARE / COMMIT / ABORT, prioridade wait-die por timestamp de Lamport,
 *      log de transações em disco (write-ahead) e protocolo de terminação cooperativa;
 *   4. COORDENADOR do 2PC: um coordenador POR TRANSAÇÃO (a empresa do passageiro) - não há coordenador fixo;
 *   5. busca de trechos em todo o consórcio;
 *   6. recuperação após queda e thread de manutenção (retransmissão de decisões e resolução de dúvidas);
 *   7. API REST (libmicrohttpd).
 *
 * Invariantes de projeto (ver docs/API.md e o relatório):
 *   - nenhuma decisão é enviada antes de ser gravada (fsync) no log do coordenador;
 *   - nenhum voto SIM é enviado antes de o participante gravar o bloqueio e o registro PREPARED;
 *   - toda operação é idempotente (mensagens podem ser duplicadas, atrasadas ou reordenadas);
 *   - mudança de estado é gravada ANTES de ser aplicada no trecho (write-ahead), então a recuperação
 *     sempre consegue refazer ou desfazer.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <pthread.h>
#include "consorcio.h"
#include "servidor.h"
#include "lamport.h"

#define DIR_TX            "transacoes"
#define MAX_PEERS         16
#define MAX_TX_TRECHOS    16
#define MAX_RESP_BYTES    (8 * 1024 * 1024)

/* ---------------------------------------------------------------- 1. configuração e pares */
static int empresaId       = 1;
static int portaClientes   = 65432;
static int portaRest       = 8080;
static int esperaConflitoMs = 1500;  /* quanto um participante deixa uma transação mais velha esperar */
static int prepareTimeoutMs = 5000;  /* timeout do coordenador para o PREPARE */
static int decisaoTimeoutMs = 2000;  /* timeout para COMMIT/ABORT e consultas */
static int buscaTimeoutMs   = 1500;  /* timeout de busca de trechos nos pares */
static int inDoubtMs        = 3000;  /* tempo em PREPARED antes de começar a perguntar o resultado */
static int preparedTimeoutMs = 30000; /* LEASE do voto SIM: depois disso um participante sem resposta aborta sozinho.
                                         Imposto pelo coordenador (vai no PREPARE). 0 = 2PC estrito (bloqueia até o coordenador voltar) */
static int suspeitaMs       = 3000;  /* durante esse tempo um par que falhou é tratado como fora do ar */
static int modoTeste        = 0;     /* expõe campos de auditoria na API (apenas para o teste automatizado) */

typedef struct {
    int       id;
    char      host[128];
    int       port;
    long long suspeitoAte;
} Peer;

static Peer peers[MAX_PEERS];
static int  nPeers = 0;
static pthread_mutex_t peerMutex = PTHREAD_MUTEX_INITIALIZER;

int consorcioPortaClientes(void) { return portaClientes; }
int consorcioPortaRest(void)     { return portaRest; }
int consorcioEmpresaId(void)     { return empresaId; }

static int envInt(const char *nome, int padrao) {
    const char *v = getenv(nome);
    return (v != NULL && *v != '\0') ? atoi(v) : padrao;
}

static long long nowMs(void) { /* relógio monotônico: só serve para medir intervalos locais */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long realMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* PEERS="2=http://host2:8080,3=http://host3:8080" (a própria empresa, se listada, é ignorada) */
static void carregarPeers(void) {
    const char *env = getenv("PEERS");
    if (env == NULL) return;
    char *copia = strdup(env), *salva = NULL;
    for (char *tok = strtok_r(copia, ",", &salva); tok != NULL; tok = strtok_r(NULL, ",", &salva)) {
        int id = 0;
        char url[256] = {0};
        if (sscanf(tok, "%d=%255s", &id, url) != 2 || id <= 0 || id == empresaId || nPeers >= MAX_PEERS) continue;
        char *p = url;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        size_t n = strlen(p);
        while (n > 0 && p[n - 1] == '/') p[--n] = '\0';
        char *dp = strrchr(p, ':');
        Peer *peer = &peers[nPeers];
        peer->id = id;
        peer->port = 80;
        if (dp != NULL) { *dp = '\0'; peer->port = atoi(dp + 1); }
        snprintf(peer->host, sizeof(peer->host), "%s", p);
        peer->suspeitoAte = 0;
        nPeers++;
    }
    free(copia);
}

static Peer *buscarPeer(int id) {
    for (int i = 0; i < nPeers; i++) if (peers[i].id == id) return &peers[i];
    return NULL;
}

static int peerSuspeito(Peer *p) {
    pthread_mutex_lock(&peerMutex);
    int r = nowMs() < p->suspeitoAte;
    pthread_mutex_unlock(&peerMutex);
    return r;
}
static void peerFalhou(Peer *p) {
    pthread_mutex_lock(&peerMutex);
    if (nowMs() >= p->suspeitoAte) log_mensagem(LOG_WARN, "Empresa %d marcada como INDISPONIVEL (timeout/erro de conexao)", p->id);
    p->suspeitoAte = nowMs() + suspeitaMs;
    pthread_mutex_unlock(&peerMutex);
}
static void peerOk(Peer *p) {
    pthread_mutex_lock(&peerMutex);
    if (p->suspeitoAte != 0) log_mensagem(LOG_INFO, "Empresa %d voltou a responder", p->id);
    p->suspeitoAte = 0;
    pthread_mutex_unlock(&peerMutex);
}

/* ---------------------------------------------------------------- falhas injetadas (teste) */
void pontoDeFalha(const char *nome) {
    static const char *alvo = NULL;
    static int lido = 0;
    if (!lido) { alvo = getenv("CRASH_AT"); lido = 1; }
    if (alvo != NULL && strcmp(alvo, nome) == 0) {
        log_mensagem(LOG_ERROR, "FALHA INJETADA no ponto '%s': encerrando o processo abruptamente", nome);
        _exit(137);
    }
}

/* ---------------------------------------------------------------- utilitários JSON / arquivos */
static const char *jstr(cJSON *o, const char *k) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
}
static double jnum(cJSON *o, const char *k, double def) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

/* grava JSON de forma atômica e durável: .tmp + fsync + rename + fsync do diretório */
static int gravarJsonDuravel(const char *caminho, cJSON *json) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", caminho);
    char *txt = cJSON_PrintUnformatted(json);
    if (!txt) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(txt); return 0; }
    size_t n = strlen(txt);
    int ok = (write(fd, txt, n) == (ssize_t)n) && (write(fd, "\n", 1) == 1) && (fsync(fd) == 0);
    close(fd);
    free(txt);
    if (ok && rename(tmp, caminho) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    int dfd = open(DIR_TX, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 1;
}

static cJSON *lerJsonArquivo(const char *caminho) {
    FILE *f = fopen(caminho, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t lidos = fread(buf, 1, n, f);
    buf[lidos] = '\0';
    fclose(f);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/* o id da transação vira nome de arquivo e vem de outro servidor: validação estrita (evita path traversal) */
static int txIdValido(const char *tx) {
    if (tx == NULL) return 0;
    size_t n = strlen(tx);
    if (n < 3 || n > 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)tx[i]) && tx[i] != '-' && tx[i] != '_') return 0;
    return 1;
}

/* empresa coordenadora codificada no id: "E<empresa>-..." */
static int txEmpresaCoord(const char *tx) {
    int e = 0;
    return (tx != NULL && sscanf(tx, "E%d-", &e) == 1) ? e : -1;
}

/* pseudônimo do passageiro enviado a OUTRAS empresas (privacidade: o e-mail não sai da empresa de origem) */
static void pseudonimo(const char *email, char *out, size_t tam) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *c = (const unsigned char *)email; *c; c++) { h ^= *c; h *= 1099511628211ULL; }
    snprintf(out, tam, "p%d-%016llx", empresaId, (unsigned long long)h);
}

/* ---------------------------------------------------------------- 2. cliente HTTP */
/* Chamada HTTP/1.1 com timeout TOTAL. Retorna 0 se obteve resposta HTTP (status em *status), -1 em falha/timeout.
 * O relógio de Lamport viaja no cabeçalho X-Lamport-Clock (envio: tick; resposta: receive). */
static int httpChamar(Peer *p, const char *metodo, const char *caminho, const char *corpo,
                      int timeoutMs, int *status, char **resposta) {
    *status = 0;
    if (resposta) *resposta = NULL;
    long long fim = nowMs() + timeoutMs;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char porta[16];
    snprintf(porta, sizeof(porta), "%d", p->port);
    if (getaddrinfo(p->host, porta, &hints, &res) != 0) { peerFalhou(p); return -1; }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL && fd < 0; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) { fd = s; break; }
        if (errno == EINPROGRESS) {
            struct pollfd pf = { s, POLLOUT, 0 };
            long long rest = fim - nowMs();
            if (rest < 0) rest = 0;
            if (poll(&pf, 1, (int)rest) > 0) {
                int err = 0; socklen_t l = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
                if (err == 0) { fd = s; break; }
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    if (fd < 0) { peerFalhou(p); return -1; }

    size_t tamCorpo = corpo ? strlen(corpo) : 0;
    char cab[512];
    int nc = snprintf(cab, sizeof(cab),
        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nX-Lamport-Clock: %llu\r\nX-Empresa-Id: %d\r\n\r\n",
        metodo, caminho, p->host, p->port, tamCorpo, (unsigned long long)lamportTick(), empresaId);

    /* envia cabeçalho + corpo */
    size_t total = nc + tamCorpo;
    char *req = malloc(total);
    memcpy(req, cab, nc);
    if (tamCorpo) memcpy(req + nc, corpo, tamCorpo);
    size_t env = 0;
    while (env < total) {
        long long rest = fim - nowMs();
        struct pollfd pf = { fd, POLLOUT, 0 };
        if (rest <= 0 || poll(&pf, 1, (int)rest) <= 0) { free(req); close(fd); peerFalhou(p); return -1; }
        ssize_t n = send(fd, req + env, total - env, MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) { free(req); close(fd); peerFalhou(p); return -1; }
        env += n;
    }
    free(req);

    /* lê a resposta até o corpo completo (Content-Length) ou até o servidor fechar */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';
    int ok = 0;
    long corpoEsperado = -1;
    char *fimCab = NULL;
    for (;;) {
        if (fimCab == NULL && (fimCab = strstr(buf, "\r\n\r\n")) != NULL) {
            char *cl = strcasestr(buf, "\r\nContent-Length:");
            corpoEsperado = (cl != NULL && cl < fimCab) ? atol(cl + 17) : -1;
        }
        if (fimCab != NULL && corpoEsperado >= 0 && (long)(len - (size_t)(fimCab + 4 - buf)) >= corpoEsperado) { ok = 1; break; }
        long long rest = fim - nowMs();
        struct pollfd pf = { fd, POLLIN, 0 };
        if (rest <= 0 || poll(&pf, 1, (int)rest) <= 0) break;
        if (len + 2048 >= cap) {
            if (cap >= MAX_RESP_BYTES) break;
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 0) break;
        if (n == 0) { ok = (fimCab != NULL); break; }
        len += n;
        buf[len] = '\0';
    }
    close(fd);
    buf[len] = '\0';
    if (!ok || len < 12 || strncmp(buf, "HTTP/1.", 7) != 0) { free(buf); peerFalhou(p); return -1; }
    fimCab = strstr(buf, "\r\n\r\n");
    if (!fimCab) { free(buf); peerFalhou(p); return -1; }

    *status = atoi(buf + 9);
    char *lc = strcasestr(buf, "\r\nX-Lamport-Clock:");
    if (lc != NULL && lc < fimCab) lamportReceive(strtoull(lc + 18, NULL, 10));
    if (resposta) *resposta = strdup(fimCab + 4);
    free(buf);
    peerOk(p);
    return 0;
}

/* ---------------------------------------------------------------- 3. PARTICIPANTE do 2PC */
/*
 * Cada trecho guarda, além de "capacidade" (assentos livres), as listas:
 *   "bloqueios": assentos segurados por transações em PREPARED  [{tx,id,nome,ts,empresa}]
 *   "reservas" : assentos confirmados pelo consórcio             [{tx,id}]
 * Invariante: capacidade + |bloqueios| + |clientes| == capacidadeTotal.
 *
 * PRECEDÊNCIA (disputa pelo último assento) - esquema WAIT-DIE com prioridade = (timestamp de Lamport, empresa):
 *   - quem chega e encontra o assento segurado por uma transação MAIS VELHA (menor prioridade) MORRE: vota NÃO na hora;
 *   - quem chega mais VELHO que todos os que seguram, ESPERA (até esperaConflitoMs) pela decisão deles;
 *   - se há outra transação mais velha esperando pelo mesmo trecho, ela passa na frente.
 * Esperas só vão do mais velho para o mais novo, logo nunca há ciclo (sem deadlock), e o critério independe da
 * ordem de chegada das mensagens e de relógios físicos.
 */
enum { EST_PREPARANDO, EST_PREPARED, EST_COMMITTED, EST_ABORTED };
static const char *nomeEstado[] = { "PREPARANDO", "PREPARED", "COMMITTED", "ABORTED" };

typedef struct PTx {
    char       txId[80];
    int        estado;
    Prioridade prio;
    int        coord;
    int        participantes[MAX_PEERS + 1];
    int        nPart;
    char       passageiroId[128];
    char       nome[100];
    int        trechos[MAX_TX_TRECHOS];
    int        nTrechos;
    int        aplicados;      /* quantos bloqueios já foram colocados (durante o PREPARE) */
    int        abortar;        /* ABORT chegou durante o PREPARE */
    int        leaseMs;        /* prazo do voto SIM imposto pelo coordenador (0 = sem prazo) */
    long long  preparadoEm;
    struct PTx *prox;
} PTx;

static PTx *ativas = NULL;                      /* transações em PREPARANDO/PREPARED neste servidor */
static pthread_mutex_t txMutex = PTHREAD_MUTEX_INITIALIZER; /* serializa a lógica 2PC do participante */
static pthread_cond_t  txCond  = PTHREAD_COND_INITIALIZER;  /* acorda quem espera por um assento */

typedef struct { int usado; int trecho; Prioridade p; } Espera;
static Espera esperas[512];

static void caminhoPart(const char *tx, char *out, size_t n) { snprintf(out, n, "%s/part_%s.json", DIR_TX, tx); }

static PTx *partBuscarAtiva(const char *tx) {
    for (PTx *p = ativas; p; p = p->prox) if (strcmp(p->txId, tx) == 0) return p;
    return NULL;
}
static void partRemoverAtiva(PTx *t) {
    for (PTx **pp = &ativas; *pp; pp = &(*pp)->prox) if (*pp == t) { *pp = t->prox; break; }
    free(t);
}

static int partGravar(PTx *t) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "txId", t->txId);
    cJSON_AddStringToObject(j, "estado", nomeEstado[t->estado]);
    cJSON_AddNumberToObject(j, "coordenador", t->coord);
    cJSON *pr = cJSON_AddObjectToObject(j, "prioridade");
    cJSON_AddNumberToObject(pr, "lamport", (double)t->prio.ts);
    cJSON_AddNumberToObject(pr, "empresa", t->prio.empresa);
    cJSON *ps = cJSON_AddArrayToObject(j, "participantes");
    for (int i = 0; i < t->nPart; i++) cJSON_AddItemToArray(ps, cJSON_CreateNumber(t->participantes[i]));
    cJSON_AddStringToObject(j, "passageiroId", t->passageiroId);
    cJSON_AddStringToObject(j, "nome", t->nome);
    cJSON_AddNumberToObject(j, "leaseMs", t->leaseMs);
    cJSON *ts = cJSON_AddArrayToObject(j, "trechos");
    for (int i = 0; i < t->nTrechos; i++) cJSON_AddItemToArray(ts, cJSON_CreateNumber(t->trechos[i]));
    char caminho[160];
    caminhoPart(t->txId, caminho, sizeof(caminho));
    int ok = gravarJsonDuravel(caminho, j);
    cJSON_Delete(j);
    return ok;
}

/* carrega um registro do disco (devolve PTx alocado, fora da lista) */
static PTx *partCarregar(const char *tx) {
    char caminho[160];
    caminhoPart(tx, caminho, sizeof(caminho));
    cJSON *j = lerJsonArquivo(caminho);
    if (!j) return NULL;
    PTx *t = calloc(1, sizeof(PTx));
    snprintf(t->txId, sizeof(t->txId), "%s", tx);
    const char *est = jstr(j, "estado");
    t->estado = EST_ABORTED;
    for (int i = 0; i < 4; i++) if (est && strcmp(est, nomeEstado[i]) == 0) t->estado = i;
    t->coord = (int)jnum(j, "coordenador", -1);
    t->leaseMs = (int)jnum(j, "leaseMs", 0);
    cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "prioridade");
    t->prio.ts = (uint64_t)jnum(pr, "lamport", 0);
    t->prio.empresa = (int)jnum(pr, "empresa", 0);
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "participantes"))
        if (t->nPart < MAX_PEERS + 1 && cJSON_IsNumber(it)) t->participantes[t->nPart++] = (int)it->valuedouble;
    const char *s = jstr(j, "passageiroId"); if (s) snprintf(t->passageiroId, sizeof(t->passageiroId), "%s", s);
    s = jstr(j, "nome"); if (s) snprintf(t->nome, sizeof(t->nome), "%s", s);
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "trechos"))
        if (t->nTrechos < MAX_TX_TRECHOS && cJSON_IsNumber(it)) t->trechos[t->nTrechos++] = (int)it->valuedouble;
    cJSON_Delete(j);
    return t;
}

/* estado de uma transação já encerrada (ou não) no disco; -1 se não existe registro */
static int estadoDisco(const char *tx) {
    PTx *t = partCarregar(tx);
    if (!t) return -1;
    int e = t->estado;
    free(t);
    return e;
}

static void partTombstone(const char *tx) {
    PTx t;
    memset(&t, 0, sizeof(t));
    snprintf(t.txId, sizeof(t.txId), "%s", tx);
    t.estado = EST_ABORTED;
    t.coord = txEmpresaCoord(tx);
    partGravar(&t);
}

/* ---- operações sobre o arquivo do trecho (chamar SEM o mutex do trecho; elas travam sozinhas) ---- */
static cJSON *arrayDe(cJSON *t, const char *k) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(t, k);
    if (!cJSON_IsArray(a)) {
        if (a) cJSON_DeleteItemFromObjectCaseSensitive(t, k);
        a = cJSON_AddArrayToObject(t, k);
    }
    return a;
}
static int arrayTemString(cJSON *arr, const char *s) {
    cJSON *it;
    cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) && strcmp(it->valuestring, s) == 0) return 1;
    return 0;
}
static int bloqueioIndice(cJSON *t, const char *tx) {
    cJSON *bl = cJSON_GetObjectItemCaseSensitive(t, "bloqueios"), *it;
    int i = 0;
    cJSON_ArrayForEach(it, bl) { const char *x = jstr(it, "tx"); if (x && strcmp(x, tx) == 0) return i; i++; }
    return -1;
}

/* visão pública do trecho (sem e-mails). Com auditoria (só em MODO_TESTE) inclui as transações. */
static cJSON *trechoPublico(cJSON *t, int auditoria) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "empresaId", empresaId);
    cJSON_AddNumberToObject(o, "idTrecho", jnum(t, "idTrecho", -1));
    const char *campos[] = { "nomeMotorista", "origem", "destino", "data", "hora" };
    for (size_t i = 0; i < sizeof(campos) / sizeof(campos[0]); i++) {
        const char *v = jstr(t, campos[i]);
        if (v) cJSON_AddStringToObject(o, campos[i], v);
    }
    int cap = (int)jnum(t, "capacidade", 0);
    cJSON *cl = cJSON_GetObjectItemCaseSensitive(t, "clientes");
    cJSON *bl = cJSON_GetObjectItemCaseSensitive(t, "bloqueios");
    int nc = cJSON_IsArray(cl) ? cJSON_GetArraySize(cl) : 0, nb = cJSON_IsArray(bl) ? cJSON_GetArraySize(bl) : 0;
    cJSON_AddNumberToObject(o, "capacidade", cap);
    cJSON_AddNumberToObject(o, "capacidadeTotal", jnum(t, "capacidadeTotal", cap + nc + nb));
    cJSON_AddNumberToObject(o, "preco", jnum(t, "preco", 0));
    if (auditoria && modoTeste) {
        cJSON_AddNumberToObject(o, "clientes", nc);
        cJSON *rs = cJSON_AddArrayToObject(o, "reservas"), *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(t, "reservas")) {
            cJSON *r = cJSON_CreateObject();
            const char *x = jstr(it, "tx"), *y = jstr(it, "id");
            cJSON_AddStringToObject(r, "tx", x ? x : "");
            cJSON_AddStringToObject(r, "id", y ? y : "");
            cJSON_AddItemToArray(rs, r);
        }
        cJSON *bs = cJSON_AddArrayToObject(o, "bloqueios");
        cJSON_ArrayForEach(it, bl) { const char *x = jstr(it, "tx"); cJSON_AddItemToArray(bs, cJSON_CreateString(x ? x : "")); }
        cJSON *ps = cJSON_AddArrayToObject(o, "passageiros");
        cJSON_ArrayForEach(it, cl) if (cJSON_IsString(it)) cJSON_AddItemToArray(ps, cJSON_CreateString(it->valuestring));
    }
    return o;
}

/* converte (confirmar=1) ou libera (confirmar=0) o bloqueio da transação no trecho. Idempotente. */
static void efetivarHold(int idTrecho, const char *tx, int confirmar) {
    pthread_mutex_t *m = obterMutexTrecho(idTrecho);
    if (!m) return;
    pthread_mutex_lock(m);
    cJSON *t = lerTrechoNoLock(idTrecho);
    if (t) {
        int idx = bloqueioIndice(t, tx);
        if (idx >= 0) {
            cJSON *bl = cJSON_GetObjectItemCaseSensitive(t, "bloqueios");
            cJSON *h = cJSON_GetArrayItem(bl, idx);
            char pid[128] = "", nome[100] = "";
            const char *a = jstr(h, "id"), *b = jstr(h, "nome");
            if (a) snprintf(pid, sizeof(pid), "%s", a);
            if (b) snprintf(nome, sizeof(nome), "%s", b);
            cJSON_DeleteItemFromArray(bl, idx);
            if (confirmar) {
                cJSON_AddItemToArray(arrayDe(t, "clientes"), cJSON_CreateString(pid));
                cJSON_AddItemToArray(arrayDe(t, "nomesClientes"), cJSON_CreateString(nome));
                cJSON *r = cJSON_CreateObject();
                cJSON_AddStringToObject(r, "tx", tx);
                cJSON_AddStringToObject(r, "id", pid);
                cJSON_AddItemToArray(arrayDe(t, "reservas"), r);
            } else {
                cJSON_ReplaceItemInObject(t, "capacidade", cJSON_CreateNumber(jnum(t, "capacidade", 0) + 1));
            }
            if (!gravarTrechoNoLock(idTrecho, t))
                log_mensagem(LOG_ERROR, "Tx %s: falha ao gravar trecho %d ao %s o bloqueio", tx, idTrecho, confirmar ? "confirmar" : "liberar");
        }
        cJSON_Delete(t);
    }
    pthread_mutex_unlock(m);
}

/* desfaz uma reserva JÁ CONFIRMADA (compensação, ver partCancelar). Idempotente. */
static void desfazerReserva(int idTrecho, const char *tx) {
    pthread_mutex_t *m = obterMutexTrecho(idTrecho);
    if (!m) return;
    pthread_mutex_lock(m);
    cJSON *t = lerTrechoNoLock(idTrecho);
    if (t) {
        cJSON *rs = cJSON_GetObjectItemCaseSensitive(t, "reservas"), *it;
        int idx = -1, i = 0;
        cJSON_ArrayForEach(it, rs) { const char *x = jstr(it, "tx"); if (x && strcmp(x, tx) == 0) { idx = i; break; } i++; }
        if (idx >= 0) {
            char pid[128] = "";
            const char *a = jstr(cJSON_GetArrayItem(rs, idx), "id");
            if (a) snprintf(pid, sizeof(pid), "%s", a);
            cJSON *cl = cJSON_GetObjectItemCaseSensitive(t, "clientes"), *nm = cJSON_GetObjectItemCaseSensitive(t, "nomesClientes");
            int k = 0;
            cJSON_ArrayForEach(it, cl) {
                if (cJSON_IsString(it) && strcmp(it->valuestring, pid) == 0) {
                    cJSON_DeleteItemFromArray(cl, k);
                    if (cJSON_IsArray(nm)) cJSON_DeleteItemFromArray(nm, k);
                    break;
                }
                k++;
            }
            cJSON_DeleteItemFromArray(rs, idx);
            cJSON_ReplaceItemInObject(t, "capacidade", cJSON_CreateNumber(jnum(t, "capacidade", 0) + 1));
            if (!gravarTrechoNoLock(idTrecho, t)) log_mensagem(LOG_ERROR, "Tx %s: falha ao gravar trecho %d ao desfazer a reserva", tx, idTrecho);
        }
        cJSON_Delete(t);
    }
    pthread_mutex_unlock(m);
}

enum { H_OK, H_NAO_ENC, H_INCOMP, H_DUP, H_SEM, H_ESPERAR, H_MORRER, H_ERRO };

static int existeEsperaMaisVelha(int trecho, Prioridade p) {
    for (int i = 0; i < 512; i++)
        if (esperas[i].usado && esperas[i].trecho == trecho && prioridadeComparar(esperas[i].p, p) < 0) return 1;
    return 0;
}

/* uma tentativa de segurar o assento (chamar com txMutex travado) */
static int tentarHold(PTx *tx, int idTrecho, const char *origem, const char *destino, cJSON **detalhe) {
    pthread_mutex_t *m = obterMutexTrecho(idTrecho);
    pthread_mutex_lock(m);
    cJSON *t = lerTrechoNoLock(idTrecho);
    int r;
    if (!t) { r = H_NAO_ENC; goto fim; }
    const char *o = jstr(t, "origem"), *d = jstr(t, "destino");
    if (!o || !d || strcmp(o, origem) != 0 || strcmp(d, destino) != 0) { r = H_INCOMP; goto fim; }
    if (arrayTemString(cJSON_GetObjectItemCaseSensitive(t, "clientes"), tx->passageiroId)) { r = H_DUP; goto fim; }
    cJSON *bl = cJSON_GetObjectItemCaseSensitive(t, "bloqueios"), *h;
    cJSON_ArrayForEach(h, bl) { const char *x = jstr(h, "id"); if (x && strcmp(x, tx->passageiroId) == 0) { r = H_DUP; goto fim; } }

    int cap = (int)jnum(t, "capacidade", 0);
    if (cap > 0 && !existeEsperaMaisVelha(idTrecho, tx->prio)) {
        cJSON_ReplaceItemInObject(t, "capacidade", cJSON_CreateNumber(cap - 1));
        cJSON *novo = cJSON_CreateObject();
        cJSON_AddStringToObject(novo, "tx", tx->txId);
        cJSON_AddStringToObject(novo, "id", tx->passageiroId);
        cJSON_AddStringToObject(novo, "nome", tx->nome);
        cJSON_AddNumberToObject(novo, "ts", (double)tx->prio.ts);
        cJSON_AddNumberToObject(novo, "empresa", tx->prio.empresa);
        cJSON_AddItemToArray(arrayDe(t, "bloqueios"), novo);
        if (gravarTrechoNoLock(idTrecho, t)) { r = H_OK; *detalhe = trechoPublico(t, 0); }
        else r = H_ERRO;
        goto fim;
    }
    if (cap > 0) { r = H_ESPERAR; goto fim; }          /* há uma transação mais velha na fila */
    int nHold = 0, maisVelho = 0;
    cJSON_ArrayForEach(h, bl) {
        Prioridade hp = { (uint64_t)jnum(h, "ts", 0), (int)jnum(h, "empresa", 0) };
        nHold++;
        if (prioridadeComparar(hp, tx->prio) < 0) maisVelho = 1;
    }
    if (nHold == 0) r = H_SEM;                           /* esgotado por reservas confirmadas */
    else if (maisVelho) r = H_MORRER;                    /* wait-die: o mais novo morre */
    else r = H_ESPERAR;                                  /* wait-die: o mais velho espera */
fim:
    if (t) cJSON_Delete(t);
    pthread_mutex_unlock(m);
    return r;
}

/* segura o assento, esperando se tiver precedência (chamar com txMutex travado; a espera libera o mutex) */
static int adquirirAssento(PTx *tx, int idTrecho, const char *o, const char *d, cJSON **detalhe, const char **motivo) {
    long long limite = nowMs() + esperaConflitoMs;
    int slot = -1, r;
    for (;;) {
        if (tx->abortar) { r = H_MORRER; *motivo = "TX_ABORTADA"; break; }
        r = tentarHold(tx, idTrecho, o, d, detalhe);
        if (r != H_ESPERAR) break;
        if (nowMs() >= limite) { r = H_MORRER; *motivo = "TIMEOUT_CONFLITO"; break; }
        if (slot < 0) {
            for (int i = 0; i < 512; i++) if (!esperas[i].usado) { esperas[i].usado = 1; esperas[i].trecho = idTrecho; esperas[i].p = tx->prio; slot = i; break; }
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&txCond, &txMutex, &ts);
    }
    if (slot >= 0) { esperas[slot].usado = 0; pthread_cond_broadcast(&txCond); }
    switch (r) {
        case H_OK: return 1;
        case H_NAO_ENC: *motivo = "TRECHO_NAO_ENCONTRADO"; break;
        case H_INCOMP:  *motivo = "TRECHO_INCOMPATIVEL"; break;
        case H_DUP:     *motivo = "RESERVA_DUPLICADA"; break;
        case H_SEM:     *motivo = "SEM_ASSENTOS"; break;
        case H_MORRER:  if (!*motivo || strcmp(*motivo, "TIMEOUT_CONFLITO") != 0) { if (!*motivo || strcmp(*motivo, "TX_ABORTADA") != 0) *motivo = "CONFLITO_PRECEDENCIA"; } break;
        default:        *motivo = "ERRO_PERSISTENCIA"; break;
    }
    return 0;
}

static cJSON *respostaNao(const char *motivo) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "voto", "NAO");
    cJSON_AddStringToObject(j, "motivo", motivo);
    return j;
}
static cJSON *respostaSim(PTx *t) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "voto", "SIM");
    cJSON *arr = cJSON_AddArrayToObject(j, "trechos");
    for (int i = 0; i < t->nTrechos; i++) {
        cJSON *tr = lerTrecho(t->trechos[i]);
        if (tr) { cJSON_AddItemToArray(arr, trechoPublico(tr, 0)); cJSON_Delete(tr); }
    }
    return j;
}

typedef struct { int id; const char *o, *d; } ItemTx;
static int cmpItemTx(const void *a, const void *b) { return ((const ItemTx *)a)->id - ((const ItemTx *)b)->id; }

/* fase 1 (voto). Retorna o status HTTP; *resp é o corpo JSON */
static int partPrepare(const char *txId, cJSON *req, cJSON **resp) {
    cJSON *pj = cJSON_GetObjectItemCaseSensitive(req, "passageiro");
    cJSON *tsj = cJSON_GetObjectItemCaseSensitive(req, "ts");
    cJSON *trs = cJSON_GetObjectItemCaseSensitive(req, "trechos");
    cJSON *ps = cJSON_GetObjectItemCaseSensitive(req, "participantes");
    const char *pid = jstr(pj, "id"), *pnome = jstr(pj, "nome");
    int coord = (int)jnum(req, "coordenador", -1);
    int n = cJSON_IsArray(trs) ? cJSON_GetArraySize(trs) : 0;
    Prioridade prio = { (uint64_t)jnum(tsj, "lamport", 0), (int)jnum(tsj, "empresa", 0) };
    ItemTx itens[MAX_TX_TRECHOS];
    int valido = pid && pnome && coord > 0 && prio.empresa > 0 && n >= 1 && n <= MAX_TX_TRECHOS && strlen(pid) < 127 && strlen(pnome) < 99;
    for (int i = 0; valido && i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(trs, i);
        double idd = jnum(it, "idTrecho", -1);
        itens[i].id = (int)idd; itens[i].o = jstr(it, "origem"); itens[i].d = jstr(it, "destino");
        if (idd < 0 || idd >= ID_STRIDE || !itens[i].o || !itens[i].d) valido = 0;
        for (int j = 0; valido && j < i; j++) if (itens[j].id == itens[i].id) valido = 0;
    }
    if (!valido) { *resp = cJSON_CreateObject(); cJSON_AddStringToObject(*resp, "erro", "REQUISICAO_INVALIDA"); return 400; }
    qsort(itens, n, sizeof(ItemTx), cmpItemTx);   /* ordem global de ids: sem deadlock entre trechos */

    pthread_mutex_lock(&txMutex);
    PTx *ex = partBuscarAtiva(txId);
    if (ex) {                                      /* retransmissão do PREPARE */
        int status = 409;
        if (ex->estado == EST_PREPARED) { *resp = respostaSim(ex); status = 200; }
        else *resp = respostaNao("EM_ANDAMENTO");
        pthread_mutex_unlock(&txMutex);
        return status;
    }
    int ed = estadoDisco(txId);
    if (ed == EST_COMMITTED) {
        PTx *c = partCarregar(txId);
        *resp = respostaSim(c); free(c);
        pthread_mutex_unlock(&txMutex);
        return 200;
    }
    if (ed == EST_ABORTED || ed == EST_PREPARED) {  /* tombstone: o ABORT chegou antes do PREPARE */
        *resp = respostaNao("TX_ABORTADA");
        pthread_mutex_unlock(&txMutex);
        return 409;
    }

    PTx *t = calloc(1, sizeof(PTx));
    snprintf(t->txId, sizeof(t->txId), "%s", txId);
    t->estado = EST_PREPARANDO; t->prio = prio; t->coord = coord;
    t->leaseMs = (int)jnum(req, "leaseMs", 0);       /* 0 = coordenador em modo 2PC estrito */
    if (t->leaseMs < 0) t->leaseMs = 0;
    snprintf(t->passageiroId, sizeof(t->passageiroId), "%s", pid);
    snprintf(t->nome, sizeof(t->nome), "%s", pnome);
    cJSON *it;
    cJSON_ArrayForEach(it, ps) if (cJSON_IsNumber(it) && t->nPart < MAX_PEERS + 1) t->participantes[t->nPart++] = (int)it->valuedouble;
    t->prox = ativas; ativas = t;

    const char *motivo = NULL;
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        cJSON *det = NULL;
        motivo = NULL;
        if (adquirirAssento(t, itens[i].id, itens[i].o, itens[i].d, &det, &motivo)) {
            t->trechos[t->nTrechos++] = itens[i].id;
            cJSON_Delete(det);
        } else ok = 0;
    }
    if (ok && t->abortar) { ok = 0; motivo = "TX_ABORTADA"; }
    if (ok) {
        t->estado = EST_PREPARED;
        t->preparadoEm = nowMs();
        if (!partGravar(t)) { ok = 0; motivo = "ERRO_PERSISTENCIA"; }
    }
    if (!ok) {
        for (int i = 0; i < t->nTrechos; i++) efetivarHold(t->trechos[i], txId, 0);
        t->estado = EST_ABORTED;
        partGravar(t);                              /* tombstone: um PREPARE repetido receberá NÃO */
        *resp = respostaNao(motivo ? motivo : "ERRO");
        partRemoverAtiva(t);
        pthread_cond_broadcast(&txCond);
        pthread_mutex_unlock(&txMutex);
        return 409;
    }
    log_mensagem(LOG_INFO, "Tx %s PREPARED (prio=%llu.%d, %d trecho(s))", txId, (unsigned long long)prio.ts, prio.empresa, t->nTrechos);
    *resp = respostaSim(t);
    pthread_mutex_unlock(&txMutex);
    pontoDeFalha("part_apos_prepare");              /* queda DEPOIS de persistir e ANTES de responder */
    return 200;
}

/* fase 2: COMMIT. 200 ok | 404 desconhecida | 409 já abortada */
static int partCommit(const char *txId) {
    pthread_mutex_lock(&txMutex);
    PTx *t = partBuscarAtiva(txId);
    if (!t) {
        int e = estadoDisco(txId);
        pthread_mutex_unlock(&txMutex);
        return e == EST_COMMITTED ? 200 : (e == EST_ABORTED ? 409 : 404);
    }
    if (t->estado != EST_PREPARED) { pthread_mutex_unlock(&txMutex); return 409; }
    pontoDeFalha("part_antes_commit");
    t->estado = EST_COMMITTED;
    partGravar(t);                                  /* decisão registrada ANTES de aplicada */
    for (int i = 0; i < t->nTrechos; i++) efetivarHold(t->trechos[i], txId, 1);
    log_mensagem(LOG_INFO, "Tx %s COMMITTED", txId);
    partRemoverAtiva(t);
    pthread_cond_broadcast(&txCond);
    pthread_mutex_unlock(&txMutex);
    return 200;
}

/* fase 2: ABORT. Sempre 200, exceto se já foi COMMITTED (409). Desconhecida => tombstone */
static int partAbort(const char *txId) {
    pthread_mutex_lock(&txMutex);
    PTx *t = partBuscarAtiva(txId);
    if (t) {
        if (t->estado == EST_PREPARANDO) {          /* a thread do PREPARE desfaz ao terminar */
            t->abortar = 1;
            pthread_cond_broadcast(&txCond);
            pthread_mutex_unlock(&txMutex);
            return 200;
        }
        t->estado = EST_ABORTED;
        partGravar(t);
        for (int i = 0; i < t->nTrechos; i++) efetivarHold(t->trechos[i], txId, 0);
        log_mensagem(LOG_INFO, "Tx %s ABORTED", txId);
        partRemoverAtiva(t);
        pthread_cond_broadcast(&txCond);
        pthread_mutex_unlock(&txMutex);
        return 200;
    }
    int e = estadoDisco(txId);
    int status = 200;
    if (e == EST_COMMITTED) status = 409;
    else if (e < 0) partTombstone(txId);
    pthread_mutex_unlock(&txMutex);
    return status;
}

/* COMPENSAÇÃO: desfaz a transação em qualquer estado, inclusive já COMMITTED. Só o coordenador a usa, e apenas quando
 * outro participante abortou por timeout (lease) e o COMMIT decidido não pode mais valer em todas as empresas. */
static int partCancelar(const char *txId) {
    pthread_mutex_lock(&txMutex);
    PTx *t = partBuscarAtiva(txId);
    pthread_mutex_unlock(&txMutex);
    if (t) return partAbort(txId);                  /* ainda PREPARED: basta abortar */
    pthread_mutex_lock(&txMutex);
    PTx *c = partCarregar(txId);
    if (!c) partTombstone(txId);
    else {
        if (c->estado == EST_COMMITTED) {
            c->estado = EST_ABORTED;
            partGravar(c);                          /* decisão registrada ANTES de desfazer (refeita na recuperação) */
            for (int i = 0; i < c->nTrechos; i++) desfazerReserva(c->trechos[i], txId);
            log_mensagem(LOG_WARN, "Tx %s COMPENSADA: reserva confirmada desfeita (outro participante abortou por timeout)", txId);
        }
        free(c);
    }
    pthread_cond_broadcast(&txCond);
    pthread_mutex_unlock(&txMutex);
    return 200;
}

/* consulta do protocolo de terminação cooperativa. Se desconhecida, grava ABORTED (esta empresa NUNCA mais votará SIM) */
static const char *partTerminacao(const char *txId) {
    pthread_mutex_lock(&txMutex);
    PTx *t = partBuscarAtiva(txId);
    const char *r;
    if (t) r = nomeEstado[t->estado];
    else {
        int e = estadoDisco(txId);
        if (e < 0) { partTombstone(txId); e = EST_ABORTED; }
        r = nomeEstado[e];
    }
    pthread_mutex_unlock(&txMutex);
    return r;
}

static const char *partEstadoLeitura(const char *txId) {
    pthread_mutex_lock(&txMutex);
    PTx *t = partBuscarAtiva(txId);
    int e = t ? t->estado : estadoDisco(txId);
    pthread_mutex_unlock(&txMutex);
    return e < 0 ? "DESCONHECIDA" : nomeEstado[e];
}

/* ---------------------------------------------------------------- 4. COORDENADOR do 2PC */
/*
 * O coordenador é a empresa à qual o passageiro está conectado, e só para AQUELA transação.
 * Registro durável (transacoes/coord_<tx>.json):
 *   estado: PREPARANDO (votação em curso) -> COMMIT | ABORT (decisão, gravada com fsync ANTES de ser enviada)
 *   participantes[].ack: quem já confirmou ter aplicado a decisão. Quando todos confirmam, finalizada=true.
 * Presumed abort: transação sem registro de decisão é tratada como ABORT.
 */
static pthread_mutex_t coordMutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned seqTx = 0;

static void caminhoCoord(const char *tx, char *out, size_t n) { snprintf(out, n, "%s/coord_%s.json", DIR_TX, tx); }
static cJSON *coordLer(const char *tx) {
    char c[160]; caminhoCoord(tx, c, sizeof(c));
    pthread_mutex_lock(&coordMutex);
    cJSON *j = lerJsonArquivo(c);
    pthread_mutex_unlock(&coordMutex);
    return j;
}
static int coordGravar(const char *tx, cJSON *j) {
    char c[160]; caminhoCoord(tx, c, sizeof(c));
    pthread_mutex_lock(&coordMutex);
    int ok = gravarJsonDuravel(c, j);
    pthread_mutex_unlock(&coordMutex);
    return ok;
}

/* transações com decisão tomada mas ainda sem todos os acks (retransmitidas pela thread de manutenção) */
static char pend[2048][80];
static int nPend = 0;
static pthread_mutex_t pendMutex = PTHREAD_MUTEX_INITIALIZER;
static void pendAdicionar(const char *tx) {
    pthread_mutex_lock(&pendMutex);
    int existe = 0;
    for (int i = 0; i < nPend; i++) if (strcmp(pend[i], tx) == 0) existe = 1;
    if (!existe && nPend < 2048) snprintf(pend[nPend++], 80, "%s", tx);
    pthread_mutex_unlock(&pendMutex);
}

typedef struct {
    int         empresa;
    Peer       *peer;
    int         nT;
    int         ids[MAX_TX_TRECHOS];
    const char *o[MAX_TX_TRECHOS], *d[MAX_TX_TRECHOS];
    int         voto;            /* 1 SIM | 0 NAO | -1 sem resposta (timeout) | -2 não contatada (suspeita de queda) */
    char        motivo[48];
    int         ack;
    int         conflito;        /* recusou o COMMIT: já tinha abortado por timeout */
    cJSON      *detalhes;
} Part;

typedef struct {
    char       txId[80];
    Prioridade prio;
    const char *email, *nome, *origemRota, *destinoRota;
    int        idItinerario;
    int        nParts;
    Part       parts[MAX_PEERS + 1];
    int        commit;
    cJSON     *rota;             /* [{idGlobal, empresa, idTrecho, origem, destino}] na ordem do itinerário */
} Ctx;
typedef struct { Ctx *c; int i; } TArg;

static void *thPrepare(void *a) {
    TArg *ta = a;
    Ctx *c = ta->c;
    Part *p = &c->parts[ta->i];
    p->voto = -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "coordenador", empresaId);
    cJSON_AddNumberToObject(req, "leaseMs", preparedTimeoutMs);   /* prazo do voto SIM; o coordenador so decide COMMIT dentro de metade dele */
    cJSON *ts = cJSON_AddObjectToObject(req, "ts");
    cJSON_AddNumberToObject(ts, "lamport", (double)c->prio.ts);
    cJSON_AddNumberToObject(ts, "empresa", c->prio.empresa);
    cJSON *ps = cJSON_AddArrayToObject(req, "participantes");
    for (int i = 0; i < c->nParts; i++) cJSON_AddItemToArray(ps, cJSON_CreateNumber(c->parts[i].empresa));
    cJSON *pj = cJSON_AddObjectToObject(req, "passageiro");
    char pid[128];
    if (p->empresa == empresaId) snprintf(pid, sizeof(pid), "%s", c->email);
    else pseudonimo(c->email, pid, sizeof(pid));       /* o e-mail não sai da empresa do passageiro */
    cJSON_AddStringToObject(pj, "id", pid);
    cJSON_AddStringToObject(pj, "nome", c->nome);
    cJSON *trs = cJSON_AddArrayToObject(req, "trechos");
    for (int i = 0; i < p->nT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "idTrecho", p->ids[i]);
        cJSON_AddStringToObject(t, "origem", p->o[i]);
        cJSON_AddStringToObject(t, "destino", p->d[i]);
        cJSON_AddItemToArray(trs, t);
    }

    cJSON *resp = NULL;
    int status = 0, chamou = 0;
    if (p->empresa == empresaId) {
        status = partPrepare(c->txId, req, &resp);
        chamou = 1;
    } else if (peerSuspeito(p->peer)) {
        p->voto = -2;
    } else {
        char caminho[160], *rb = NULL;
        snprintf(caminho, sizeof(caminho), "/api/v1/transacoes/%s/prepare", c->txId);
        char *corpo = cJSON_PrintUnformatted(req);
        if (httpChamar(p->peer, "POST", caminho, corpo, prepareTimeoutMs, &status, &rb) == 0) {
            chamou = 1;
            if (rb) { resp = cJSON_Parse(rb); free(rb); }
        }
        free(corpo);
    }
    if (chamou) {
        const char *voto = jstr(resp, "voto");
        if (status == 200 && voto && strcmp(voto, "SIM") == 0) {
            p->voto = 1;
            cJSON *d = cJSON_DetachItemFromObjectCaseSensitive(resp, "trechos");
            p->detalhes = d ? d : cJSON_CreateArray();
        } else if (status >= 400 && status < 500) {
            p->voto = 0;
            const char *m = jstr(resp, "motivo");
            if (!m) m = jstr(resp, "erro");
            snprintf(p->motivo, sizeof(p->motivo), "%s", m ? m : "ERRO");
        } else {
            p->voto = -1;                     /* 5xx: tratado como sem resposta */
        }
    }
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return NULL;
}

enum { MODO_ABORT = 0, MODO_COMMIT = 1, MODO_CANCELAR = 2 };
/* envia COMMIT/ABORT/CANCELAR. Retorna 1 se a empresa confirmou (ou se não adianta insistir), 0 se deve tentar de novo,
 * 2 se o COMMIT foi recusado porque a empresa JÁ ABORTOU por timeout (conflito heurístico => compensação) */
static int enviarDecisao(int empresa, const char *tx, int modo) {
    int status = 0;
    if (empresa == empresaId) {
        status = modo == MODO_COMMIT ? partCommit(tx) : modo == MODO_CANCELAR ? partCancelar(tx) : partAbort(tx);
    } else {
        Peer *p = buscarPeer(empresa);
        if (!p) return 1;
        if (peerSuspeito(p)) return 0;
        char caminho[160];
        snprintf(caminho, sizeof(caminho), "/api/v1/transacoes/%s/%s", tx, modo == MODO_COMMIT ? "commit" : modo == MODO_CANCELAR ? "cancelar" : "abort");
        if (httpChamar(p, "POST", caminho, "{}", decisaoTimeoutMs, &status, NULL) != 0) return 0;
    }
    if (status == 200) return 1;
    if (status >= 500) return 0;
    if (status == 409 && modo == MODO_COMMIT) return 2;
    log_mensagem(LOG_WARN, "Tx %s: empresa %d respondeu %d (anomalia; nao sera retransmitido)", tx, empresa, status);
    return 1;
}

static void *thDecisao(void *a) {
    TArg *ta = a;
    Part *p = &ta->c->parts[ta->i];
    if (!p->ack) {
        int r = enviarDecisao(p->empresa, ta->c->txId, ta->c->commit ? MODO_COMMIT : MODO_ABORT);
        if (r == 2) { p->ack = 1; p->conflito = 1; } else p->ack = r;
    }
    return NULL;
}

static void rodarThreads(Ctx *c, void *(*fn)(void *)) {
    pthread_t th[MAX_PEERS + 1];
    TArg args[MAX_PEERS + 1];
    for (int i = 0; i < c->nParts; i++) {
        args[i].c = c; args[i].i = i;
        if (pthread_create(&th[i], NULL, fn, &args[i]) != 0) { th[i] = 0; fn(&args[i]); }
    }
    for (int i = 0; i < c->nParts; i++) if (th[i]) pthread_join(th[i], NULL);
}

static cJSON *coordMontarRegistro(Ctx *c, const char *estado, int finalizada) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "txId", c->txId);
    cJSON_AddStringToObject(j, "estado", estado);
    cJSON_AddBoolToObject(j, "finalizada", finalizada);
    cJSON_AddBoolToObject(j, "itinerarioGravado", 0);
    cJSON *pr = cJSON_AddObjectToObject(j, "prioridade");
    cJSON_AddNumberToObject(pr, "lamport", (double)c->prio.ts);
    cJSON_AddNumberToObject(pr, "empresa", c->prio.empresa);
    cJSON_AddStringToObject(j, "emailCliente", c->email);
    cJSON_AddStringToObject(j, "nomeCliente", c->nome);
    cJSON_AddStringToObject(j, "origem", c->origemRota);
    cJSON_AddStringToObject(j, "destino", c->destinoRota);
    cJSON_AddNumberToObject(j, "idItinerario", c->idItinerario);
    cJSON_AddItemToObject(j, "rota", cJSON_Duplicate(c->rota, 1));
    cJSON *ps = cJSON_AddArrayToObject(j, "participantes");
    for (int i = 0; i < c->nParts; i++) {
        Part *p = &c->parts[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "empresa", p->empresa);
        cJSON_AddBoolToObject(o, "ack", p->ack);
        cJSON_AddItemToObject(o, "detalhes", p->detalhes ? cJSON_Duplicate(p->detalhes, 1) : cJSON_CreateArray());
        cJSON_AddItemToArray(ps, o);
    }
    return j;
}

/* Conflito heurístico: o coordenador decidiu COMMIT mas algum participante já tinha abortado por timeout (lease).
 * Para manter a atomicidade, a decisão passa a ABORT "compensando": as empresas que confirmaram desfazem a reserva
 * (POST .../cancelar, retransmitido até todos confirmarem) e o itinerário é marcado CANCELADO. */
static void compensar(const char *tx, cJSON *rec, const int *abortaram, int n) {
    log_mensagem(LOG_WARN, "Tx %s: %d participante(s) abortaram por timeout antes do COMMIT chegar => compensando as demais empresas", tx, n);
    cJSON_ReplaceItemInObject(rec, "estado", cJSON_CreateString("ABORT"));
    cJSON_DeleteItemFromObject(rec, "compensando");
    cJSON_AddBoolToObject(rec, "compensando", 1);
    cJSON_ReplaceItemInObject(rec, "finalizada", cJSON_CreateFalse());
    cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(rec, "participantes")) {
        int emp = (int)jnum(p, "empresa", 0), ja = 0;
        for (int i = 0; i < n; i++) if (abortaram[i] == emp) ja = 1;
        cJSON_ReplaceItemInObject(p, "ack", cJSON_CreateBool(ja));
    }
    coordGravar(tx, rec);
    cancelarItinerarioCompensado((int)jnum(rec, "idItinerario", -1), "participante abortou por timeout; reservas desfeitas");
    pendAdicionar(tx);
}

/* escreve Itinerarios/itinerario<id>.json (CONFIRMADO) a partir do registro de decisão. */
static void finalizarItinerario(const char *tx, cJSON *rec) {
    int id = (int)jnum(rec, "idItinerario", -1);
    if (id < 0) return;
    cJSON *it = cJSON_CreateObject();
    cJSON_AddNumberToObject(it, "idItinerario", id);
    cJSON_AddStringToObject(it, "idTransacao", tx);
    const char *e = jstr(rec, "emailCliente"), *n = jstr(rec, "nomeCliente"), *o = jstr(rec, "origem"), *d = jstr(rec, "destino");
    cJSON_AddStringToObject(it, "emailCliente", e ? e : "");
    cJSON_AddStringToObject(it, "nomeCliente", n ? n : "");
    cJSON_AddStringToObject(it, "origem", o ? o : "");
    cJSON_AddStringToObject(it, "destino", d ? d : "");
    cJSON_AddStringToObject(it, "status", "CONFIRMADO");
    char hora[24];
    time_t agora = time(NULL);
    struct tm tmv;
    localtime_r(&agora, &tmv);
    strftime(hora, sizeof(hora), "%Y-%m-%d %H:%M:%S", &tmv);
    cJSON_AddStringToObject(it, "criadoEm", hora);
    cJSON *lista = cJSON_AddArrayToObject(it, "trechos"), *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(rec, "rota")) {
        cJSON *t = cJSON_CreateObject();
        int emp = (int)jnum(r, "empresa", 0), idLocal = (int)jnum(r, "idTrecho", -1);
        cJSON_AddNumberToObject(t, "idTrecho", jnum(r, "idGlobal", -1));
        cJSON_AddNumberToObject(t, "empresaId", emp);
        cJSON_AddStringToObject(t, "origem", jstr(r, "origem"));
        cJSON_AddStringToObject(t, "destino", jstr(r, "destino"));
        cJSON *pp, *dd;
        cJSON_ArrayForEach(pp, cJSON_GetObjectItemCaseSensitive(rec, "participantes")) {
            if ((int)jnum(pp, "empresa", 0) != emp) continue;
            cJSON_ArrayForEach(dd, cJSON_GetObjectItemCaseSensitive(pp, "detalhes")) {
                if ((int)jnum(dd, "idTrecho", -1) != idLocal) continue;
                const char *c[] = { "nomeMotorista", "data", "hora" };
                for (int k = 0; k < 3; k++) { const char *v = jstr(dd, c[k]); if (v) cJSON_AddStringToObject(t, c[k], v); }
                cJSON_AddNumberToObject(t, "preco", jnum(dd, "preco", 0));
            }
        }
        cJSON_AddItemToArray(lista, t);
    }
    if (gravarItinerario(id, it)) {
        cJSON_ReplaceItemInObject(rec, "itinerarioGravado", cJSON_CreateTrue());
        coordGravar(tx, rec);
    } else log_mensagem(LOG_ERROR, "Tx %s: falha ao gravar o arquivo do itinerario %d", tx, id);
    cJSON_Delete(it);
}

int consorcioConfirmarItinerario(cJSON *req, int *idItinOut, char *txOut, size_t tamTx) {
    if (idItinOut) *idItinOut = -1;
    if (txOut && tamTx) txOut[0] = '\0';
    const char *email = jstr(req, "emailCliente");
    if (!email) email = jstr(req, "email");
    const char *nome = jstr(req, "nomeCliente"), *oR = jstr(req, "origemRota"), *dR = jstr(req, "destinoRota");
    cJSON *rota = cJSON_GetObjectItemCaseSensitive(req, "rota");
    if (!email || !nome || !oR || !dR || !cJSON_IsArray(rota)) return ITIN_INVALIDO;
    int n = cJSON_GetArraySize(rota);
    if (n < 1 || n > MAX_TX_TRECHOS) return ITIN_INVALIDO;

    Ctx *c = calloc(1, sizeof(Ctx));
    c->email = email; c->nome = nome; c->origemRota = oR; c->destinoRota = dR;
    c->rota = cJSON_CreateArray();
    int ids[MAX_TX_TRECHOS];
    const char *prevDestino = NULL;
    int rc = ITIN_OK;

    /* 1. valida a cadeia origemRota -> ... -> destinoRota e agrupa os trechos por empresa */
    for (int i = 0; i < n && rc == ITIN_OK; i++) {
        cJSON *item = cJSON_GetArrayItem(rota, i);
        cJSON *idj = cJSON_GetObjectItemCaseSensitive(item, "idTrecho");
        if (!idj) idj = cJSON_GetObjectItemCaseSensitive(item, "id");
        const char *o = jstr(item, "origem"), *d = jstr(item, "destino");
        if (!cJSON_IsNumber(idj) || idj->valuedouble < 0 || !o || !d) { rc = ITIN_INVALIDO; break; }
        ids[i] = (int)idj->valuedouble;
        for (int j = 0; j < i; j++) if (ids[j] == ids[i]) rc = ITIN_INVALIDO;
        if (i == 0 && strcmp(o, oR) != 0) rc = ITIN_INVALIDO;
        if (i > 0 && strcmp(o, prevDestino) != 0) rc = ITIN_INVALIDO;
        if (i == n - 1 && strcmp(d, dR) != 0) rc = ITIN_INVALIDO;
        prevDestino = d;
        if (rc != ITIN_OK) break;

        int emp = ids[i] / ID_STRIDE, local = ids[i] % ID_STRIDE;
        if (emp == 0) emp = empresaId;
        Peer *peer = NULL;
        if (emp != empresaId && (peer = buscarPeer(emp)) == NULL) { rc = ITIN_NAO_ENCONTRADO; break; }
        Part *p = NULL;
        for (int k = 0; k < c->nParts; k++) if (c->parts[k].empresa == emp) p = &c->parts[k];
        if (!p) {
            if (c->nParts >= MAX_PEERS + 1) { rc = ITIN_INVALIDO; break; }
            p = &c->parts[c->nParts++];
            p->empresa = emp; p->peer = peer;
        }
        if (p->nT >= MAX_TX_TRECHOS) { rc = ITIN_INVALIDO; break; }
        p->ids[p->nT] = local; p->o[p->nT] = o; p->d[p->nT] = d; p->nT++;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "idGlobal", ids[i]);
        cJSON_AddNumberToObject(r, "empresa", emp);
        cJSON_AddNumberToObject(r, "idTrecho", local);
        cJSON_AddStringToObject(r, "origem", o);
        cJSON_AddStringToObject(r, "destino", d);
        cJSON_AddItemToArray(c->rota, r);
    }
    if (rc != ITIN_OK) { cJSON_Delete(c->rota); free(c); return rc; }

    /* 2. identificador, prioridade (timestamp de Lamport) e registro durável PREPARANDO */
    unsigned s = __sync_fetch_and_add(&seqTx, 1);
    snprintf(c->txId, sizeof(c->txId), "E%d-%llx-%u", empresaId, (unsigned long long)realMs(), s);
    c->prio.ts = lamportTick();
    c->prio.empresa = empresaId;
    c->idItinerario = proximoIdItinerario();
    if (idItinOut) *idItinOut = c->idItinerario;
    if (txOut) snprintf(txOut, tamTx, "%s", c->txId);

    cJSON *rec = coordMontarRegistro(c, "PREPARANDO", 0);
    int gravou = coordGravar(c->txId, rec);
    cJSON_Delete(rec);
    if (!gravou) { log_mensagem(LOG_ERROR, "Tx %s: falha ao gravar o registro do coordenador", c->txId); cJSON_Delete(c->rota); free(c); return ITIN_ERRO; }

    /* 3. FASE 1: PREPARE em paralelo a todas as empresas envolvidas */
    long long tIniPrepare = nowMs();
    rodarThreads(c, thPrepare);
    int todosSim = 1, leaseExpirou = 0;
    for (int i = 0; i < c->nParts; i++) if (c->parts[i].voto != 1) todosSim = 0;
    /* guarda do lease: os participantes podem abortar sozinhos a partir de 'preparedTimeoutMs' depois do PREPARE;
     * só se decide COMMIT se a votação terminou dentro de METADE desse prazo (sobra o resto para a entrega do COMMIT) */
    if (todosSim && preparedTimeoutMs > 0 && nowMs() - tIniPrepare > preparedTimeoutMs / 2) {
        todosSim = 0; leaseExpirou = 1;
        log_mensagem(LOG_WARN, "Tx %s: votacao demorou %lldms (> metade do lease de %dms) => ABORT por seguranca", c->txId, nowMs() - tIniPrepare, preparedTimeoutMs);
    }
    pontoDeFalha("coord_antes_decisao");

    /* 4. decisão: gravada com fsync ANTES de ser enviada. Se não der para gravar COMMIT, decide ABORT. */
    c->commit = todosSim;
    for (int i = 0; i < c->nParts; i++) c->parts[i].ack = (c->parts[i].voto == -2);  /* nunca contatadas: nada a desfazer */
    rec = coordMontarRegistro(c, c->commit ? "COMMIT" : "ABORT", 0);
    if (!coordGravar(c->txId, rec)) {
        if (c->commit) {
            c->commit = 0;
            cJSON_Delete(rec);
            rec = coordMontarRegistro(c, "ABORT", 0);
            coordGravar(c->txId, rec);
        }
    }
    log_mensagem(LOG_INFO, "Tx %s decisao=%s (prio=%llu.%d, %d empresa(s))", c->txId, c->commit ? "COMMIT" : "ABORT",
                 (unsigned long long)c->prio.ts, c->prio.empresa, c->nParts);
    pontoDeFalha("coord_apos_decisao");

    if (c->commit) finalizarItinerario(c->txId, rec);

    /* 5. FASE 2: envia a decisão; quem não responder é retransmitido pela thread de manutenção */
    rodarThreads(c, thDecisao);
    int todosAck = 1, conflitos[MAX_PEERS + 1], nConf = 0, compensado = 0;
    for (int i = 0; i < c->nParts; i++) {
        if (!c->parts[i].ack) todosAck = 0;
        if (c->parts[i].conflito) conflitos[nConf++] = c->parts[i].empresa;
    }
    cJSON *rec2 = coordLer(c->txId);
    if (rec2 && c->commit && nConf > 0) {             /* COMMIT recusado por quem abortou por timeout */
        compensar(c->txId, rec2, conflitos, nConf);
        cJSON_Delete(rec2);
        rec2 = NULL; todosAck = 1; compensado = 1;    /* a retransmissao do CANCELAR e feita pela thread de manutencao */
    }
    if (rec2) {
        cJSON *pp = cJSON_GetObjectItemCaseSensitive(rec2, "participantes");
        for (int i = 0; i < c->nParts; i++)
            cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pp, i), "ack", cJSON_CreateBool(c->parts[i].ack));
        if (todosAck) cJSON_ReplaceItemInObject(rec2, "finalizada", cJSON_CreateTrue());
        coordGravar(c->txId, rec2);
        cJSON_Delete(rec2);
    }
    if (!todosAck) pendAdicionar(c->txId);

    /* 6. resultado para o passageiro */
    if (c->commit && !compensado) rc = ITIN_OK;
    else {
        rc = ITIN_ERRO;
        const char *mot = NULL;
        int indisp = 0;
        for (int i = 0; i < c->nParts; i++) {
            if (c->parts[i].voto < 0) indisp = 1;
            else if (c->parts[i].voto == 0 && !mot) mot = c->parts[i].motivo;
        }
        if (mot) {
            if (!strcmp(mot, "SEM_ASSENTOS") || !strcmp(mot, "CONFLITO_PRECEDENCIA") || !strcmp(mot, "TIMEOUT_CONFLITO")) rc = ITIN_SEM_ASSENTO;
            else if (!strcmp(mot, "TRECHO_NAO_ENCONTRADO") || !strcmp(mot, "TRECHO_INCOMPATIVEL")) rc = ITIN_NAO_ENCONTRADO;
            else if (!strcmp(mot, "RESERVA_DUPLICADA")) rc = ITIN_DUPLICADO;
            log_mensagem(LOG_WARN, "Itinerario recusado (tx %s): %s", c->txId, mot);
        } else if (indisp || leaseExpirou) {
            rc = ITIN_EMPRESA_INDISPONIVEL;
            log_mensagem(LOG_WARN, "Itinerario recusado (tx %s): empresa participante indisponivel", c->txId);
        }
    }
    cJSON_Delete(rec);
    for (int i = 0; i < c->nParts; i++) cJSON_Delete(c->parts[i].detalhes);
    cJSON_Delete(c->rota);
    free(c);
    return rc;
}

/* ---------------------------------------------------------------- 5. busca no consórcio */
static int dataConfere(const char *filtro, const char *dataTrecho) {
    if (!filtro || !*filtro || strcmp(filtro, "dd/mm/aaaa") == 0) return 1;
    return dataTrecho && strcmp(filtro, dataTrecho) == 0;
}

/* item no formato entregue ao cliente (id = id que o cliente deve devolver em confirmar_itinerario) */
static cJSON *itemGateway(cJSON *pub, int empresa) {
    cJSON *o = cJSON_CreateObject();
    int local = (int)jnum(pub, "idTrecho", -1);
    cJSON_AddNumberToObject(o, "id", empresa == empresaId ? local : empresa * ID_STRIDE + local);
    cJSON_AddNumberToObject(o, "empresaId", empresa);
    const char *campos[] = { "nomeMotorista", "origem", "destino", "data", "hora" };
    for (size_t i = 0; i < 5; i++) { const char *v = jstr(pub, campos[i]); if (v) cJSON_AddStringToObject(o, campos[i], v); }
    cJSON_AddNumberToObject(o, "capacidade", jnum(pub, "capacidade", 0));
    cJSON_AddNumberToObject(o, "preco", jnum(pub, "preco", 0));
    return o;
}

typedef struct { Peer *p; char data[32]; cJSON *arr; } BuscaArg;
static void *thBusca(void *a) {
    BuscaArg *b = a;
    b->arr = NULL;
    if (peerSuspeito(b->p)) return NULL;
    char caminho[128], *rb = NULL;
    int status;
    snprintf(caminho, sizeof(caminho), "/api/v1/trechos?disponiveis=true%s%s", b->data[0] ? "&data=" : "", b->data);
    if (httpChamar(b->p, "GET", caminho, NULL, buscaTimeoutMs, &status, &rb) == 0 && status == 200 && rb) b->arr = cJSON_Parse(rb);
    free(rb);
    return NULL;
}

cJSON *listarTrechosConsorcio(const char *data) {
    cJSON *saida = cJSON_CreateArray();
    BuscaArg args[MAX_PEERS];
    pthread_t th[MAX_PEERS];
    int usa = (data && *data && strcmp(data, "dd/mm/aaaa") != 0);
    for (int i = 0; i < nPeers; i++) {
        args[i].p = &peers[i];
        snprintf(args[i].data, sizeof(args[i].data), "%s", usa ? data : "");
        if (pthread_create(&th[i], NULL, thBusca, &args[i]) != 0) { th[i] = 0; thBusca(&args[i]); }
    }
    int limite = lerLimiteIdTrechos();
    for (int i = 0; i < limite; i++) {
        cJSON *t = lerTrecho(i);
        if (!t) continue;
        if ((int)jnum(t, "capacidade", 0) > 0 && dataConfere(data, jstr(t, "data"))) {
            cJSON *pub = trechoPublico(t, 0);
            cJSON_AddItemToArray(saida, itemGateway(pub, empresaId));
            cJSON_Delete(pub);
        }
        cJSON_Delete(t);
    }
    for (int i = 0; i < nPeers; i++) {
        if (th[i]) pthread_join(th[i], NULL);
        cJSON *it;
        if (cJSON_IsArray(args[i].arr))
            cJSON_ArrayForEach(it, args[i].arr) cJSON_AddItemToArray(saida, itemGateway(it, peers[i].id));
        cJSON_Delete(args[i].arr);
    }
    return saida;
}

/* ---------------------------------------------------------------- 6. recuperação e manutenção */
static void recuperarParticipante(void) {
    DIR *dir = opendir(DIR_TX);
    if (!dir) return;
    struct dirent *e;
    int refeitos = 0, duvida = 0;
    while ((e = readdir(dir)) != NULL) {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "part_", 5) != 0 || n < 11 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        char tx[80];
        snprintf(tx, sizeof(tx), "%.*s", (int)(n - 10), e->d_name + 5);
        if (!txIdValido(tx)) continue;
        PTx *t = partCarregar(tx);
        if (!t) continue;
        if (t->estado == EST_PREPARED) {           /* votou SIM e não sabe o resultado: continua segurando o assento */
            t->preparadoEm = nowMs();
            t->prox = ativas; ativas = t;
            duvida++;
            continue;
        }
        for (int i = 0; i < t->nTrechos; i++) {                       /* refaz a decisão gravada */
            efetivarHold(t->trechos[i], tx, t->estado == EST_COMMITTED);
            if (t->estado == EST_ABORTED) desfazerReserva(t->trechos[i], tx);   /* compensação interrompida por queda */
        }
        refeitos++;
        free(t);
    }
    closedir(dir);

    /* bloqueios órfãos: o servidor caiu entre segurar o assento e gravar PREPARED => o voto SIM nunca foi enviado */
    int orfaos = 0, limite = lerLimiteIdTrechos();
    for (int id = 0; id < limite; id++) {
        cJSON *t = lerTrecho(id);
        if (!t) continue;
        char txs[32][80];
        int nt = 0;
        cJSON *h;
        cJSON_ArrayForEach(h, cJSON_GetObjectItemCaseSensitive(t, "bloqueios")) {
            const char *x = jstr(h, "tx");
            if (x && nt < 32 && !partBuscarAtiva(x)) snprintf(txs[nt++], 80, "%s", x);
        }
        cJSON_Delete(t);
        for (int i = 0; i < nt; i++) {
            int est = estadoDisco(txs[i]);
            if (est < 0 && txIdValido(txs[i])) partTombstone(txs[i]);
            efetivarHold(id, txs[i], est == EST_COMMITTED);
            orfaos++;
        }
    }
    log_mensagem(LOG_INFO, "Recuperacao (participante): %d registro(s) refeito(s), %d em duvida (PREPARED), %d bloqueio(s) orfao(s) liberado(s)", refeitos, duvida, orfaos);
}

static void recuperarCoordenador(void) {
    DIR *dir = opendir(DIR_TX);
    if (!dir) return;
    struct dirent *e;
    int abortadas = 0, retransmitir = 0;
    while ((e = readdir(dir)) != NULL) {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "coord_", 6) != 0 || n < 12 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        char tx[80];
        snprintf(tx, sizeof(tx), "%.*s", (int)(n - 11), e->d_name + 6);
        if (!txIdValido(tx)) continue;
        cJSON *rec = coordLer(tx);
        if (!rec) continue;
        int idItin = (int)jnum(rec, "idItinerario", -1);
        if (idItin >= 0) garantirIdItinerarioMinimo(idItin + 1);
        const char *est = jstr(rec, "estado");
        if (est && strcmp(est, "PREPARANDO") == 0) {   /* caiu antes de decidir => ABORT (nenhum COMMIT foi enviado) */
            cJSON_ReplaceItemInObject(rec, "estado", cJSON_CreateString("ABORT"));
            cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(rec, "participantes"))
                cJSON_ReplaceItemInObject(p, "ack", cJSON_CreateFalse());
            coordGravar(tx, rec);
            est = "ABORT";
            abortadas++;
        }
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "finalizada"))) { pendAdicionar(tx); retransmitir++; }
        if (est && strcmp(est, "COMMIT") == 0 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "itinerarioGravado")))
            finalizarItinerario(tx, rec);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "compensando")))
            cancelarItinerarioCompensado(idItin, "participante abortou por timeout; reservas desfeitas");
        cJSON_Delete(rec);
    }
    closedir(dir);
    log_mensagem(LOG_INFO, "Recuperacao (coordenador): %d transacao(oes) sem decisao abortada(s), %d decisao(oes) a retransmitir", abortadas, retransmitir);
}

/* retransmite a decisão aos participantes sem ack. Retorna 1 quando a transação foi finalizada */
static int coordRetentar(const char *tx) {
    cJSON *rec = coordLer(tx);
    if (!rec) return 1;
    const char *est = jstr(rec, "estado");
    int commit = est && strcmp(est, "COMMIT") == 0;
    int comp = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "compensando"));
    int modo = commit ? MODO_COMMIT : (comp ? MODO_CANCELAR : MODO_ABORT);
    int todos = 1, mudou = 0, conflitos[MAX_PEERS + 1], nConf = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(rec, "participantes")) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "ack"))) continue;
        int emp = (int)jnum(p, "empresa", 0);
        int r = enviarDecisao(emp, tx, modo);
        if (r == 2) conflitos[nConf++] = emp;
        if (r) {
            cJSON_ReplaceItemInObject(p, "ack", cJSON_CreateTrue());
            mudou = 1;
        } else todos = 0;
    }
    if (commit && nConf > 0) {                      /* alguem ja tinha abortado por timeout */
        compensar(tx, rec, conflitos, nConf);
        cJSON_Delete(rec);
        return 0;
    }
    if (todos && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "finalizada"))) {
        cJSON_ReplaceItemInObject(rec, "finalizada", cJSON_CreateTrue());
        mudou = 1;
        log_mensagem(LOG_INFO, "Tx %s finalizada (todos os participantes confirmaram o %s)", tx, commit ? "COMMIT" : (comp ? "CANCELAMENTO" : "ABORT"));
    }
    if (mudou) coordGravar(tx, rec);
    cJSON_Delete(rec);
    return todos;
}

/* decisão do coordenador consultada por participante em dúvida: 1 COMMIT, 0 ABORT, -1 pendente, -2 sem resposta */
static int consultarCoordenador(int coord, const char *tx) {
    if (coord == empresaId) {
        cJSON *rec = coordLer(tx);
        if (!rec) return 0;                                    /* presumed abort */
        const char *e = jstr(rec, "estado");
        int r = (e && !strcmp(e, "COMMIT")) ? 1 : (e && !strcmp(e, "ABORT")) ? 0 : -1;
        cJSON_Delete(rec);
        return r;
    }
    Peer *p = buscarPeer(coord);
    if (!p) return -2;
    char caminho[160], *rb = NULL;
    int status;
    snprintf(caminho, sizeof(caminho), "/api/v1/transacoes/%s?papel=coordenador", tx);
    if (httpChamar(p, "GET", caminho, NULL, decisaoTimeoutMs, &status, &rb) != 0) return -2;
    int r = -2;
    if (status == 200 && rb) {
        cJSON *j = cJSON_Parse(rb);
        const char *d = jstr(j, "decisao");
        r = (d && !strcmp(d, "COMMIT")) ? 1 : (d && !strcmp(d, "ABORT")) ? 0 : -1;
        cJSON_Delete(j);
    }
    free(rb);
    return r;
}

/* participante em PREPARED há muito tempo: 1) pergunta ao coordenador; 2) se ele não responde,
 * protocolo de terminação cooperativa perguntando aos demais participantes. */
static int resolverEmDuvida(const char *tx, int coord, int *parts, int nParts) {
    int d = consultarCoordenador(coord, tx);
    if (d == 1) { partCommit(tx); return 1; }
    if (d == 0) { partAbort(tx); return 1; }
    if (d == -1) return 0;                                     /* coordenador ainda está votando */
    for (int i = 0; i < nParts; i++) {
        if (parts[i] == empresaId || parts[i] == coord) continue;
        Peer *p = buscarPeer(parts[i]);
        if (!p || peerSuspeito(p)) continue;
        char caminho[160], *rb = NULL;
        int status;
        snprintf(caminho, sizeof(caminho), "/api/v1/transacoes/%s/terminacao", tx);
        if (httpChamar(p, "POST", caminho, "{}", decisaoTimeoutMs, &status, &rb) == 0 && status == 200 && rb) {
            cJSON *j = cJSON_Parse(rb);
            const char *est = jstr(j, "estado");
            int fim = 0;
            if (est && !strcmp(est, "COMMITTED")) { partCommit(tx); fim = 1; }
            else if (est && !strcmp(est, "ABORTED")) { partAbort(tx); fim = 1; }
            cJSON_Delete(j);
            free(rb);
            if (fim) { log_mensagem(LOG_INFO, "Tx %s resolvida pelo protocolo de terminacao (empresa %d)", tx, parts[i]); return 1; }
        } else free(rb);
    }
    log_mensagem(LOG_WARN, "Tx %s em PREPARED aguardando o coordenador (empresa %d indisponivel); assento mantido ate o fim do lease", tx, coord);
    return 0;
}

static void *rotinaManutencao(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        /* a) participantes em dúvida */
        typedef struct { char tx[80]; int coord; int parts[MAX_PEERS + 1]; int n; long long idade; int lease; } Duvida;
        Duvida *lista = malloc(sizeof(Duvida) * 64);
        int nl = 0;
        pthread_mutex_lock(&txMutex);
        for (PTx *t = ativas; t && nl < 64; t = t->prox) {
            if (t->estado != EST_PREPARED || nowMs() - t->preparadoEm < inDoubtMs) continue;
            snprintf(lista[nl].tx, 80, "%s", t->txId);
            lista[nl].coord = t->coord;
            lista[nl].n = t->nPart;
            lista[nl].idade = nowMs() - t->preparadoEm;
            lista[nl].lease = t->leaseMs;
            memcpy(lista[nl].parts, t->participantes, sizeof(t->participantes));
            nl++;
        }
        pthread_mutex_unlock(&txMutex);
        for (int i = 0; i < nl; i++) {
            if (resolverEmDuvida(lista[i].tx, lista[i].coord, lista[i].parts, lista[i].n)) continue;
            /* TIMEOUT: lease expirado e nem o coordenador nem os demais participantes deram uma resposta decisiva.
             * O coordenador só decide COMMIT dentro de metade do lease, então abortar aqui é seguro salvo atraso
             * extremo de mensagem; nesse caso o coordenador compensa (ver compensar). Libera o assento. */
            if (lista[i].lease > 0 && lista[i].idade >= lista[i].lease) {
                log_mensagem(LOG_WARN, "Tx %s: ABORT unilateral por TIMEOUT (lease de %dms expirou; coordenador %d e demais participantes sem resposta decisiva)",
                             lista[i].tx, lista[i].lease, lista[i].coord);
                partAbort(lista[i].tx);
            }
        }
        free(lista);

        /* b) coordenador: retransmite decisões sem ack */
        char copia[256][80];
        int nc = 0;
        pthread_mutex_lock(&pendMutex);
        for (int i = 0; i < nPend && nc < 256; i++) snprintf(copia[nc++], 80, "%s", pend[i]);
        pthread_mutex_unlock(&pendMutex);
        for (int i = 0; i < nc; i++) {
            if (!coordRetentar(copia[i])) continue;
            pthread_mutex_lock(&pendMutex);
            for (int k = 0; k < nPend; k++)
                if (strcmp(pend[k], copia[i]) == 0) { snprintf(pend[k], 80, "%s", pend[nPend - 1]); nPend--; break; }
            pthread_mutex_unlock(&pendMutex);
        }
    }
    return NULL;
}

int consorcioIniciar(void) {
    empresaId        = envInt("EMPRESA_ID", 1);
    portaClientes    = envInt("PORTA_CLIENTES", 65432);
    portaRest        = envInt("PORTA_REST", 8080);
    esperaConflitoMs = envInt("ESPERA_CONFLITO_MS", esperaConflitoMs);
    prepareTimeoutMs = envInt("PREPARE_TIMEOUT_MS", prepareTimeoutMs);
    decisaoTimeoutMs = envInt("DECISAO_TIMEOUT_MS", decisaoTimeoutMs);
    buscaTimeoutMs   = envInt("BUSCA_TIMEOUT_MS", buscaTimeoutMs);
    inDoubtMs        = envInt("INDOUBT_MS", inDoubtMs);
    preparedTimeoutMs = envInt("PREPARED_TIMEOUT_MS", preparedTimeoutMs);
    suspeitaMs       = envInt("SUSPEITA_MS", suspeitaMs);
    modoTeste        = envInt("MODO_TESTE", 0);
    mkdir(DIR_TX, 0755);
    lamportIniciar("ArquivoID/lamport.txt");
    carregarPeers();
    log_mensagem(LOG_INFO, "Empresa %d | %d par(es) no consorcio | wait-die=%dms prepare-timeout=%dms in-doubt=%dms lease=%dms%s",
                 empresaId, nPeers, esperaConflitoMs, prepareTimeoutMs, inDoubtMs, preparedTimeoutMs, modoTeste ? " | MODO_TESTE" : "");
    for (int i = 0; i < nPeers; i++) log_mensagem(LOG_INFO, "  par: empresa %d em %s:%d", peers[i].id, peers[i].host, peers[i].port);
    recuperarParticipante();
    recuperarCoordenador();
    pthread_t th;
    if (pthread_create(&th, NULL, rotinaManutencao, NULL) != 0) return 0;
    pthread_detach(th);
    return 1;
}

/* ---------------------------------------------------------------- 7. API REST entre servidores */
/*
 *  GET  /api/v1/health
 *  GET  /api/v1/trechos[?origem=&destino=&data=&disponiveis=true]
 *  GET  /api/v1/trechos/{id}
 *  POST /api/v1/transacoes/{tx}/prepare       (2PC fase 1, coordenador -> participante)
 *  POST /api/v1/transacoes/{tx}/commit        (2PC fase 2)
 *  POST /api/v1/transacoes/{tx}/abort         (2PC fase 2)
 *  GET  /api/v1/transacoes/{tx}?papel=coordenador|participante   (consulta de estado/decisão)
 *  POST /api/v1/transacoes/{tx}/terminacao    (terminação cooperativa entre participantes)
 *  POST /api/v1/transacoes/{tx}/cancelar      (compensação: desfaz até uma reserva já COMMITTED; só o coordenador usa)
 * Todas as mensagens carregam X-Lamport-Clock; especificação completa em docs/API.md.
 */
#define REST_BODY_MAX 65536
struct RestReq { char *body; size_t size; };

static enum MHD_Result responder(struct MHD_Connection *conn, unsigned status, cJSON *json) {
    char *txt = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    struct MHD_Response *r = MHD_create_response_from_buffer(strlen(txt), txt, MHD_RESPMEM_MUST_COPY);
    free(txt);
    if (!r) return MHD_NO;
    char clk[32], emp[16];
    snprintf(clk, sizeof(clk), "%llu", (unsigned long long)lamportTick());
    snprintf(emp, sizeof(emp), "%d", empresaId);
    MHD_add_response_header(r, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(r, "X-Lamport-Clock", clk);
    MHD_add_response_header(r, "X-Empresa-Id", emp);
    MHD_add_response_header(r, "Connection", "close");
    enum MHD_Result ret = MHD_queue_response(conn, status, r);
    MHD_destroy_response(r);
    return ret;
}

static cJSON *jsonErro(const char *codigo, const char *msg) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "erro", codigo);
    cJSON_AddStringToObject(j, "mensagem", msg);
    return j;
}

static cJSON *coordStatusJson(const char *tx) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "txId", tx);
    cJSON_AddStringToObject(o, "papel", "coordenador");
    cJSON *rec = coordLer(tx);
    if (!rec) {                                           /* presumed abort */
        cJSON_AddStringToObject(o, "decisao", "ABORT");
        cJSON_AddBoolToObject(o, "presumida", 1);
        return o;
    }
    const char *e = jstr(rec, "estado");
    cJSON_AddStringToObject(o, "decisao", (e && !strcmp(e, "PREPARANDO")) ? "PENDENTE" : (e ? e : "ABORT"));
    cJSON_AddBoolToObject(o, "finalizada", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "finalizada")));
    cJSON *tr = cJSON_AddArrayToObject(o, "trechos"), *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(rec, "rota")) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "empresaId", jnum(r, "empresa", 0));
        cJSON_AddNumberToObject(t, "idTrecho", jnum(r, "idTrecho", -1));
        cJSON_AddItemToArray(tr, t);
    }
    cJSON_Delete(rec);
    return o;
}

static enum MHD_Result rotearTransacao(struct MHD_Connection *conn, const char *method, const char *resto, const char *corpo) {
    char tx[80] = {0};
    size_t n = strcspn(resto, "/");
    if (n >= sizeof(tx)) return responder(conn, 400, jsonErro("TX_INVALIDA", "Identificador de transacao invalido."));
    memcpy(tx, resto, n);
    const char *acao = resto[n] == '/' ? resto + n + 1 : "";
    if (!txIdValido(tx)) return responder(conn, 400, jsonErro("TX_INVALIDA", "Identificador de transacao invalido."));

    int post = strcmp(method, "POST") == 0, get = strcmp(method, "GET") == 0;
    if (post && strcmp(acao, "prepare") == 0) {
        cJSON *body = corpo ? cJSON_Parse(corpo) : NULL;
        if (!body) return responder(conn, 400, jsonErro("JSON_INVALIDO", "Envie um objeto JSON valido."));
        cJSON *resp = NULL;
        int st = partPrepare(tx, body, &resp);
        cJSON_Delete(body);
        return responder(conn, st, resp);
    }
    if (post && strcmp(acao, "commit") == 0) {
        int st = partCommit(tx);
        cJSON *o = cJSON_CreateObject();
        if (st == 200) { cJSON_AddStringToObject(o, "txId", tx); cJSON_AddStringToObject(o, "estado", "COMMITTED"); }
        else if (st == 404) { cJSON_AddStringToObject(o, "erro", "TRANSACAO_DESCONHECIDA"); }
        else { cJSON_AddStringToObject(o, "erro", "ESTADO_INVALIDO"); }
        return responder(conn, st, o);
    }
    if (post && strcmp(acao, "abort") == 0) {
        int st = partAbort(tx);
        cJSON *o = cJSON_CreateObject();
        if (st == 200) { cJSON_AddStringToObject(o, "txId", tx); cJSON_AddStringToObject(o, "estado", "ABORTED"); }
        else cJSON_AddStringToObject(o, "erro", "TRANSACAO_JA_CONFIRMADA");
        return responder(conn, st, o);
    }
    if (post && strcmp(acao, "cancelar") == 0) {
        int st = partCancelar(tx);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "txId", tx);
        cJSON_AddStringToObject(o, "estado", "ABORTED");
        return responder(conn, st, o);
    }
    if (post && strcmp(acao, "terminacao") == 0) {
        const char *est = partTerminacao(tx);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "txId", tx);
        cJSON_AddStringToObject(o, "estado", est);
        return responder(conn, 200, o);
    }
    if (get && acao[0] == '\0') {
        const char *papel = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "papel");
        int ehCoord = papel ? strcmp(papel, "coordenador") == 0 : txEmpresaCoord(tx) == empresaId;
        if (ehCoord) {
            if (txEmpresaCoord(tx) != empresaId) return responder(conn, 404, jsonErro("NAO_E_COORDENADOR", "Esta empresa nao coordena a transacao."));
            return responder(conn, 200, coordStatusJson(tx));
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "txId", tx);
        cJSON_AddStringToObject(o, "papel", "participante");
        cJSON_AddStringToObject(o, "estado", partEstadoLeitura(tx));
        return responder(conn, 200, o);
    }
    return responder(conn, 404, jsonErro("ROTA_NAO_ENCONTRADA", "Endpoint nao encontrado."));
}

enum MHD_Result consorcioHandlerRest(void *cls, struct MHD_Connection *conn, const char *url, const char *method,
                                     const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;
    struct RestReq *req = *con_cls;
    if (!req) {
        req = calloc(1, sizeof(*req));
        if (!req) return MHD_NO;
        *con_cls = req;
        return MHD_YES;
    }
    if (*upload_data_size > 0) {
        if (req->size + *upload_data_size > REST_BODY_MAX) {
            *upload_data_size = 0;
            return responder(conn, 413, jsonErro("CORPO_MUITO_GRANDE", "Corpo acima de 64 KiB."));
        }
        char *novo = realloc(req->body, req->size + *upload_data_size + 1);
        if (!novo) { *upload_data_size = 0; return MHD_NO; }
        req->body = novo;
        memcpy(req->body + req->size, upload_data, *upload_data_size);
        req->size += *upload_data_size;
        req->body[req->size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* requisição completa: aplica a regra de recebimento do relógio de Lamport */
    const char *clk = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "X-Lamport-Clock");
    if (clk) lamportReceive(strtoull(clk, NULL, 10));

    int get = strcmp(method, "GET") == 0;
    if (get && strcmp(url, "/api/v1/health") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddNumberToObject(o, "empresaId", empresaId);
        cJSON_AddStringToObject(o, "api", "v1");
        cJSON_AddNumberToObject(o, "lamport", (double)lamportAtual());
        return responder(conn, 200, o);
    }
    if (get && strcmp(url, "/api/v1/trechos") == 0) {
        const char *fo = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "origem");
        const char *fd = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "destino");
        const char *fdata = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "data");
        const char *disp = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "disponiveis");
        const char *aud = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "auditoria");
        cJSON *arr = cJSON_CreateArray();
        int limite = lerLimiteIdTrechos();
        for (int i = 0; i < limite; i++) {
            cJSON *t = lerTrecho(i);
            if (!t) continue;
            const char *o = jstr(t, "origem"), *d = jstr(t, "destino");
            int ok = (!fo || (o && !strcasecmp(o, fo))) && (!fd || (d && !strcasecmp(d, fd))) && dataConfere(fdata, jstr(t, "data"))
                     && (!(disp && !strcmp(disp, "true")) || jnum(t, "capacidade", 0) > 0);
            if (ok) cJSON_AddItemToArray(arr, trechoPublico(t, aud != NULL));
            cJSON_Delete(t);
        }
        return responder(conn, 200, arr);
    }
    if (get && strncmp(url, "/api/v1/trechos/", 16) == 0) {
        char *fim = NULL;
        long id = strtol(url + 16, &fim, 10);
        if (fim == url + 16 || *fim != '\0' || id < 0 || id >= ID_STRIDE) return responder(conn, 400, jsonErro("ID_INVALIDO", "Id de trecho invalido."));
        cJSON *t = lerTrecho((int)id);
        if (!t) return responder(conn, 404, jsonErro("TRECHO_NAO_ENCONTRADO", "Nao existe trecho com esse id."));
        cJSON *pub = trechoPublico(t, MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "auditoria") != NULL);
        cJSON_Delete(t);
        return responder(conn, 200, pub);
    }
    if (strncmp(url, "/api/v1/transacoes/", 19) == 0) return rotearTransacao(conn, method, url + 19, req->body);
    return responder(conn, 404, jsonErro("ROTA_NAO_ENCONTRADA", "Endpoint REST nao encontrado."));
}

void consorcioLiberarRequisicao(void *cls, struct MHD_Connection *conn, void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)conn; (void)toe;
    struct RestReq *req = *con_cls;
    if (req) { free(req->body); free(req); *con_cls = NULL; }
}
