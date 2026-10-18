/*
 * teste_vaijunto.c - Teste automatizado do servidor VaiJunto (Problema 1, TEC502)
 *
 * Fala com o servidor SOMENTE pela rede (sockets TCP puros + JSON), como os clientes reais, então serve para
 * o servidor em qualquer máquina/contêiner (use --host). Cada passageiro/motorista simulado é uma conexão.
 *
 * COMPILAR:  gcc -O2 -pthread -o teste_vaijunto teste_vaijunto.c cJSON.c -lm
 * EXECUTAR:  ./teste_vaijunto [--host 127.0.0.1] [--port 65432] [--clientes 50] [--assentos 10]
 *                             [--origem A --meio B --destino C] [--estendido] [--lento]
 *   --origem/--meio/--destino : cidades que EXISTEM no mapa do servidor (o servidor valida o caminho ao publicar).
 *   --estendido : liga os testes que dependem do servidor novo (camada de protocolo com validação/sessão,
 *                 buscar_itinerarios e cancelar_rota). Sem a flag, só se usa o protocolo comum às duas versões.
 *   --lento     : inclui o teste de tempo limite de mensagem incompleta (espera ~11 s).
 * Código de saída: 0 = todas as verificações passaram; 1 = alguma falhou.
 *
 * gcc -O2 -pthread -I/home/nonyz7/redes/pbl2/Projeto-VaiJunto---MI-concorr-ncia-e-conectividade/PBL2 \
    -o teste_vaijunto teste02.c \
    /home/nonyz7/redes/pbl2/Projeto-VaiJunto---MI-concorr-ncia-e-conectividade/PBL2/cJSON.c -lm
    
    ./teste_vaijunto --host 127.0.0.1 --clientes 50 --assentos 10 \
    --origem "Santa_Barbara" --meio "Feira_de_Santana" --destino "Salvador"
 * 
 * 
 * O QUE O ENUNCIADO PEDE  ->  ONDE ESTÁ TESTADO
 *   Cliente motorista: autenticar, publicar carona (rota, data, assentos, preço por trecho), consultar caronas
 *     publicadas e passageiros confirmados por trecho, cancelar carona ........ T1, T2, T10, (T13 estendido)
 *   Cliente passageiro: autenticar, buscar itinerários, confirmar reserva de 1+ trechos, consultar e cancelar
 *     reservas ................................................................ T1, T4, T10, T11
 *   Disponibilidade controlada POR TRECHO (assento ocupado num trecho segue livre nos outros) ... T3
 *   Itinerário com trechos de MOTORISTAS DIFERENTES ............................ T4, T11
 *   Confirmação ATÔMICA (tudo ou nada; último trecho indisponível; trecho inexistente) ........ T5
 *   Várias pessoas disputando os mesmos trechos: nenhum assento vendido duas vezes ........... T6, T8
 *   Nenhum itinerário confirmado pela metade .................................. T5, T6, T7, T8, T9
 *   Sem deadlock entre reservas dos mesmos trechos em ordens diferentes ..................... T7
 *   Queda/encerramento abrupto de cliente não derruba o servidor nem corrompe reservas, e
 *     nenhum assento fica preso por reserva não concluída ....................... T9, T10
 *   Dados em representação intermediária (JSON); receptor valida e descarta malformadas ... T12 (estendido)
 *   Tempo de resposta sob carga (média, p50, p95, máx) e vazão ................... T6 (e demais disputas)
 *   Execução em computadores distintos (basta apontar --host para o servidor) ........... uso
 * (Contêineres Docker, relatório SBC e README não são verificáveis por este programa.)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "cJSON.h"

/* ============================== configuração ============================== */
static char HOST[128] = "127.0.0.1";
static int PORTA = 65432;
static int N_CLIENTES = 50;
static int ASSENTOS = 10;
static char CID_A[64] = "A", CID_B[64] = "B", CID_C[64] = "C";
static int ESTENDIDO = 0, LENTO = 0;
static char UID[32];
static int SEQ = 0;                 /* numeração de usuários, só usada pela thread principal */

#define RESP_MAX (1 << 18)   /* 256 KB por conexão: sobra para as respostas do protocolo e cabe em centenas de clientes */
#define MAX_THREADS 600

/* ============================== verificações ============================== */
static int total_ok = 0, total_falha = 0, n_falhas = 0;
static char lista_falhas[300][200];

static void checar(int cond, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  [%s] %s\n", cond ? "ok" : "FALHA", msg);
    fflush(stdout);
    if (cond) {
        total_ok++;
    } else {
        total_falha++;
        if (n_falhas < 300) snprintf(lista_falhas[n_falhas++], sizeof(lista_falhas[0]), "%s", msg);
    }
}

static void titulo(const char *t) { printf("\n%s\n", t); fflush(stdout); }

/* ============================== tempo e estatística ============================== */
static double agora_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct { double *v; int n, cap; pthread_mutex_t m; } Amostras;
#define AMOSTRAS_INIT { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER }

static void amostra_add(Amostras *a, double x) {
    pthread_mutex_lock(&a->m);
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->v = realloc(a->v, sizeof(double) * a->cap);
    }
    a->v[a->n++] = x;
    pthread_mutex_unlock(&a->m);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void amostra_resumo(const char *nome, Amostras *a) {
    if (a->n == 0) { printf("  %-24s sem amostras\n", nome); return; }
    qsort(a->v, a->n, sizeof(double), cmp_double);
    double soma = 0;
    for (int i = 0; i < a->n; i++) soma += a->v[i];
    printf("  %-24s n=%-4d media=%8.2f ms  p50=%8.2f  p95=%8.2f  max=%8.2f\n", nome, a->n, soma / a->n,
           a->v[a->n / 2], a->v[(int)((a->n - 1) * 0.95)], a->v[a->n - 1]);
}

static void amostra_limpar(Amostras *a) { free(a->v); a->v = NULL; a->n = a->cap = 0; }

static Amostras LAT_LOGIN = AMOSTRAS_INIT, LAT_BUSCA = AMOSTRAS_INIT, LAT_CONF = AMOSTRAS_INIT;

/* ============================== conexão TCP ============================== */
typedef struct { int fd; char *buf; size_t len; } Conn;

static int conn_abrir(Conn *c) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    struct addrinfo dica, *res = NULL;
    memset(&dica, 0, sizeof(dica));
    dica.ai_family = AF_INET;
    dica.ai_socktype = SOCK_STREAM;
    char porta[16];
    snprintf(porta, sizeof(porta), "%d", PORTA);
    if (getaddrinfo(HOST, porta, &dica, &res) != 0 || res == NULL) return 0;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return 0; }
    struct timeval tv = { 20, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int um = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &um, sizeof(um));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); freeaddrinfo(res); return 0; }
    freeaddrinfo(res);
    c->fd = fd;
    c->buf = malloc(RESP_MAX);
    c->len = 0;
    return c->buf != NULL;
}

/* educado=1: manda DESCONECTADO antes de fechar; abrupto=1: fecha com RST (SO_LINGER 0) */
static void conn_fechar(Conn *c, int educado, int abrupto) {
    if (c->fd >= 0) {
        if (educado) send(c->fd, "DESCONECTADO", 12, MSG_NOSIGNAL);
        if (abrupto) {
            struct linger l = { 1, 0 };
            setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        }
        close(c->fd);
    }
    free(c->buf);
    c->fd = -1;
    c->buf = NULL;
}

static int conn_enviar(Conn *c, const char *dados, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = send(c->fd, dados + off, n - off, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        off += (size_t)r;
    }
    return 1;
}

/* tira uma resposta completa do começo do buffer: JSON (delimitado por chaves/colchetes) ou texto simples */
static int extrair_resposta(Conn *c, char *txt, size_t ntxt, cJSON **json) {
    size_t i = 0;
    while (i < c->len && (c->buf[i] == '\0' || isspace((unsigned char)c->buf[i]))) i++;
    if (i == c->len) { c->len = 0; return 0; }

    if (c->buf[i] == '{' || c->buf[i] == '[') {
        int prof = 0, em_str = 0, esc = 0;
        for (size_t j = i; j < c->len; j++) {
            char ch = c->buf[j];
            if (em_str) {
                if (esc) esc = 0;
                else if (ch == '\\') esc = 1;
                else if (ch == '"') em_str = 0;
                continue;
            }
            if (ch == '"') em_str = 1;
            else if (ch == '{' || ch == '[') prof++;
            else if (ch == '}' || ch == ']') {
                if (--prof == 0) {
                    char *copia = malloc(j - i + 2);
                    memcpy(copia, c->buf + i, j - i + 1);
                    copia[j - i + 1] = '\0';
                    *json = cJSON_Parse(copia);
                    free(copia);
                    memmove(c->buf, c->buf + j + 1, c->len - j - 1);
                    c->len -= j + 1;
                    txt[0] = '\0';
                    return 1;
                }
            }
        }
        return 0; /* JSON ainda incompleto */
    }

    /* código em texto simples (o servidor pode mandar o '\0' final junto) */
    size_t n = c->len - i;
    if (n >= ntxt) n = ntxt - 1;
    memcpy(txt, c->buf + i, n);
    txt[n] = '\0';
    c->len = 0;
    return 1;
}

static int conn_ler(Conn *c, char *txt, size_t ntxt, cJSON **json) {
    *json = NULL;
    txt[0] = '\0';
    for (;;) {
        if (c->len > 0 && extrair_resposta(c, txt, ntxt, json)) return 1;
        if (c->len >= RESP_MAX - 1) return 0;
        ssize_t r = recv(c->fd, c->buf + c->len, RESP_MAX - 1 - c->len, 0);
        if (r > 0) { c->len += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return 0; /* fechou, deu tempo esgotado ou erro */
    }
}

/* lê exatamente n bytes (respostas em texto coladas no mesmo segmento) */
static int conn_ler_exato(Conn *c, size_t n, char *txt, size_t ntxt) {
    while (c->len < n) {
        ssize_t r = recv(c->fd, c->buf + c->len, RESP_MAX - 1 - c->len, 0);
        if (r > 0) { c->len += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return 0;
    }
    size_t m = n < ntxt - 1 ? n : ntxt - 1;
    memcpy(txt, c->buf, m);
    txt[m] = '\0';
    memmove(c->buf, c->buf + n, c->len - n);
    c->len -= n;
    return 1;
}

/* envia a requisição (consome req) e lê a resposta. Devolve 1 se houve resposta. */
static int chamar(Conn *c, cJSON *req, cJSON **json, char *txt, size_t ntxt, Amostras *lat) {
    char *msg = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (msg == NULL) return 0;
    double t0 = agora_ms();
    int ok = conn_enviar(c, msg, strlen(msg));
    free(msg);
    if (!ok) { *json = NULL; txt[0] = '\0'; return 0; }
    ok = conn_ler(c, txt, ntxt, json);
    if (ok && lat) amostra_add(lat, agora_ms() - t0);
    return ok;
}

/* só interessa o código em texto */
static int chamar_txt(Conn *c, cJSON *req, char *txt, size_t ntxt, Amostras *lat) {
    cJSON *j = NULL;
    int ok = chamar(c, req, &j, txt, ntxt, lat);
    cJSON_Delete(j);
    return ok;
}

/* só interessa o JSON (NULL se vier texto ou falhar) */
static cJSON *chamar_json(Conn *c, cJSON *req, Amostras *lat) {
    cJSON *j = NULL;
    char txt[256];
    if (!chamar(c, req, &j, txt, sizeof(txt), lat)) return NULL;
    return j;
}

/* ============================== usuários e requisições ============================== */
typedef struct { Conn c; char classe[16]; char email[96]; char nome[64]; int ok; } Usuario;

static cJSON *req_base(const char *classe, const char *acao) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "classe", classe);
    cJSON_AddStringToObject(r, "acao", acao);
    return r;
}

/* requisição com a identidade do usuário (nomes de campo aceitos por qualquer versão do servidor) */
static cJSON *req_id(const Usuario *u, const char *acao) {
    cJSON *r = req_base(u->classe, acao);
    cJSON_AddStringToObject(r, "email", u->email);
    cJSON_AddStringToObject(r, "nome", u->nome);
    if (strcmp(u->classe, "Cliente") == 0) {
        cJSON_AddStringToObject(r, "emailCliente", u->email);
        cJSON_AddStringToObject(r, "nomeCliente", u->nome);
    } else {
        cJSON_AddStringToObject(r, "emailMotorista", u->email);
    }
    return r;
}

static int usuario_login(Usuario *u, Amostras *lat) {
    cJSON *r = req_base(u->classe, "login");
    cJSON_AddStringToObject(r, "email", u->email);
    cJSON_AddStringToObject(r, "senha", "123");
    cJSON *j = NULL;
    char txt[256];
    int ok = chamar(&u->c, r, &j, txt, sizeof(txt), lat) && j != NULL && cJSON_GetObjectItem(j, "nome") != NULL;
    cJSON_Delete(j);
    return ok;
}

/* conecta, cadastra e autentica. Os nomes são únicos por execução (UID) para poder repetir o teste. */
static int usuario_criar(Usuario *u, const char *classe, const char *prefixo, int idx, Amostras *lat_login) {
    memset(u, 0, sizeof(*u));
    snprintf(u->classe, sizeof(u->classe), "%s", classe);
    snprintf(u->nome, sizeof(u->nome), "%s%d_%s", prefixo, idx, UID);
    snprintf(u->email, sizeof(u->email), "%s%d_%s@teste.com", prefixo, idx, UID);
    if (!conn_abrir(&u->c)) return 0;

    cJSON *r = req_base(classe, "cadastro");
    cJSON_AddStringToObject(r, "nome", u->nome);
    cJSON_AddStringToObject(r, "email", u->email);
    cJSON_AddStringToObject(r, "senha", "123");
    char txt[256];
    if (!chamar_txt(&u->c, r, txt, sizeof(txt), NULL) || strcmp(txt, "CADASTRO_REALIZADO") != 0) return 0;
    u->ok = usuario_login(u, lat_login);
    return u->ok;
}

static void usuario_fechar(Usuario *u) { conn_fechar(&u->c, 1, 0); }

/* ============================== operações de domínio ============================== */
typedef struct { const char *o, *d, *data, *hora; int cap; double preco; } TDef;
typedef struct { int id; const char *o, *d; } Par;

static int publicar(Usuario *m, int n, const TDef *t, char *txt, size_t ntxt) {
    cJSON *r = req_base("Motorista", "cadastrar_rota");
    cJSON_AddStringToObject(r, "nome", m->nome);
    cJSON *arr = cJSON_AddArrayToObject(r, "trechos");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "origem", t[i].o);
        cJSON_AddStringToObject(o, "destino", t[i].d);
        cJSON_AddStringToObject(o, "nome", m->nome);
        cJSON_AddStringToObject(o, "emailMotorista", m->email);
        cJSON_AddStringToObject(o, "data", t[i].data);
        cJSON_AddStringToObject(o, "hora", t[i].hora);
        cJSON_AddNumberToObject(o, "capacidade", t[i].cap);
        cJSON_AddNumberToObject(o, "preco", t[i].preco);
        cJSON_AddItemToArray(arr, o);
    }
    if (!chamar_txt(&m->c, r, txt, ntxt, NULL)) { snprintf(txt, ntxt, "SEM_RESPOSTA"); return 0; }
    return strcmp(txt, "ROTA_CADASTRADA") == 0;
}

static cJSON *listar_trechos(Usuario *m) { return chamar_json(&m->c, req_id(m, "listar_trechos"), NULL); }

static cJSON *achar_trecho(cJSON *lista, const char *o, const char *d) {
    cJSON *it;
    cJSON_ArrayForEach(it, lista) {
        char *to = cJSON_GetStringValue(cJSON_GetObjectItem(it, "origem"));
        char *td = cJSON_GetStringValue(cJSON_GetObjectItem(it, "destino"));
        if (to && td && strcmp(to, o) == 0 && strcmp(td, d) == 0) return it;
    }
    return NULL;
}

static int numero(cJSON *it, const char *k) { return it ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, k)) : -1; }
static int npass(cJSON *it) {
    cJSON *a = it ? cJSON_GetObjectItem(it, "nomesClientes") : NULL;
    return a ? cJSON_GetArraySize(a) : -1;
}

/* capacidade livre e nº de passageiros de um trecho do motorista (ou -1/-1 se o trecho não existe) */
static int estado_trecho(Usuario *m, const char *o, const char *d, int *cap, int *pas, int *id) {
    cJSON *lista = listar_trechos(m);
    cJSON *t = lista ? achar_trecho(lista, o, d) : NULL;
    *cap = numero(t, "capacidade");
    *pas = npass(t);
    if (id) *id = numero(t, "id");
    int achou = t != NULL;
    cJSON_Delete(lista);
    return achou;
}

static int num_reservas(Usuario *p) {
    cJSON *j = chamar_json(&p->c, req_id(p, "listar_caronas"), NULL);
    int n = (j && cJSON_IsArray(j)) ? cJSON_GetArraySize(j) : -1;
    cJSON_Delete(j);
    return n;
}

/* 1 = confirmado, 0 = recusado (codigo traz o motivo), -1 = falha de comunicação */
static int confirmar(Usuario *p, const char *orig, const char *dest, int n, const Par *rota,
                     char *codigo, size_t ncod, Amostras *lat) {
    cJSON *r = req_id(p, "confirmar_itinerario");
    cJSON_AddStringToObject(r, "origemRota", orig);
    cJSON_AddStringToObject(r, "destinoRota", dest);
    cJSON *arr = cJSON_AddArrayToObject(r, "rota");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "idTrecho", rota[i].id);
        cJSON_AddStringToObject(o, "origem", rota[i].o);
        cJSON_AddStringToObject(o, "destino", rota[i].d);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON *j = NULL;
    char txt[256];
    if (!chamar(&p->c, r, &j, txt, sizeof(txt), lat)) { snprintf(codigo, ncod, "ERRO_COMUNICACAO"); return -1; }
    if (j != NULL) {
        char *st = cJSON_GetStringValue(cJSON_GetObjectItem(j, "status"));
        int ok = st && strcmp(st, "ITINERARIO_CONFIRMADO") == 0;
        snprintf(codigo, ncod, "%s", ok ? "ITINERARIO_CONFIRMADO" : "RESPOSTA_INESPERADA");
        cJSON_Delete(j);
        return ok ? 1 : 0;
    }
    snprintf(codigo, ncod, "%.*s", (int)ncod - 1, txt);
    return 0;
}

static int cancelar_reserva(Usuario *p, int id_trecho, char *txt, size_t ntxt) {
    cJSON *r = req_id(p, "cancelar_carona");
    cJSON_AddNumberToObject(r, "idSelecionado", id_trecho);
    return chamar_txt(&p->c, r, txt, ntxt, NULL);
}

/* invariante central: assentos livres + passageiros == capacidade inicial, e nenhum passageiro repetido.
 * Se isso vale, nenhum assento foi vendido duas vezes nem ficou bloqueado sem dono. */
static int invariante_motorista(Usuario *m, int cap_inicial, char *detalhe, size_t nd) {
    cJSON *lista = listar_trechos(m);
    int ok = lista != NULL && cJSON_IsArray(lista), n = 0;
    detalhe[0] = '\0';
    cJSON *it;
    cJSON_ArrayForEach(it, lista) {
        n++;
        int cap = numero(it, "capacidade"), pas = npass(it);
        if (cap < 0 || pas < 0 || cap + pas != cap_inicial) {
            ok = 0;
            snprintf(detalhe, nd, "trecho %d: livres=%d + passageiros=%d != %d", numero(it, "id"), cap, pas, cap_inicial);
        }
        cJSON *nomes = cJSON_GetObjectItem(it, "nomesClientes");
        int tam = nomes ? cJSON_GetArraySize(nomes) : 0;
        for (int a = 0; a < tam; a++)
            for (int b = a + 1; b < tam; b++) {
                char *x = cJSON_GetStringValue(cJSON_GetArrayItem(nomes, a));
                char *y = cJSON_GetStringValue(cJSON_GetArrayItem(nomes, b));
                if (x && y && strcmp(x, y) == 0) { ok = 0; snprintf(detalhe, nd, "passageiro repetido no trecho %d", numero(it, "id")); }
            }
    }
    cJSON_Delete(lista);
    return ok && n > 0;
}

/* ============================== cenário padrão: 2 motoristas, A->B e B->C ============================== */
typedef struct { Usuario m1, m2; int id1, id2, cap; } Cenario2;

static int cenario2_criar(Cenario2 *c, const char *data, int cap) {
    memset(c, 0, sizeof(*c));
    c->cap = cap;
    int idx = ++SEQ;
    char txt[128];
    if (!usuario_criar(&c->m1, "Motorista", "m1_", idx, NULL) || !usuario_criar(&c->m2, "Motorista", "m2_", idx, NULL)) return 0;
    TDef t1 = { CID_A, CID_B, data, "08:00", cap, 20.0 };
    TDef t2 = { CID_B, CID_C, data, "10:00", cap, 30.0 };
    if (!publicar(&c->m1, 1, &t1, txt, sizeof(txt))) { fprintf(stderr, "    (publicar A->B: %s)\n", txt); return 0; }
    if (!publicar(&c->m2, 1, &t2, txt, sizeof(txt))) { fprintf(stderr, "    (publicar B->C: %s)\n", txt); return 0; }
    int cap1, p1, cap2, p2;
    if (!estado_trecho(&c->m1, CID_A, CID_B, &cap1, &p1, &c->id1) || !estado_trecho(&c->m2, CID_B, CID_C, &cap2, &p2, &c->id2)) return 0;
    return 1;
}

static void cenario2_fechar(Cenario2 *c) { usuario_fechar(&c->m1); usuario_fechar(&c->m2); }

static void rota_ac(const Cenario2 *c, Par *rota) {
    rota[0] = (Par){ c->id1, CID_A, CID_B };
    rota[1] = (Par){ c->id2, CID_B, CID_C };
}

/* ============================== threads ============================== */
/* espera as threads por até 'seg' segundos no total; devolve quantas NÃO terminaram (travadas) */
static int juntar(pthread_t *th, int n, int seg) {
    struct timespec limite;
    clock_gettime(CLOCK_REALTIME, &limite);
    limite.tv_sec += seg;
    int travadas = 0;
    for (int i = 0; i < n; i++) {
        if (pthread_timedjoin_np(th[i], NULL, &limite) != 0) { travadas++; pthread_detach(th[i]); }
    }
    return travadas;
}

typedef struct {
    Usuario u;
    int idx, variante, ok;
    int resultado;            /* 1 confirmado, 0 recusado, -1 erro */
    char codigo[64];
    Cenario2 *c;
    pthread_barrier_t *bar;
    int id_extra;             /* para cenários com mais de dois trechos */
} ArgP;

/* ---------- T6: todos disputam o MESMO itinerário A->B->C (2 motoristas) ---------- */
static void *thr_disputa_itinerario(void *p) {
    ArgP *a = p;
    a->ok = usuario_criar(&a->u, "Cliente", "p", a->idx, &LAT_LOGIN);
    pthread_barrier_wait(a->bar);          /* todos largam juntos para maximizar a disputa */
    if (!a->ok) { a->resultado = -1; snprintf(a->codigo, sizeof(a->codigo), "SEM_LOGIN"); return NULL; }
    Par rota[2];
    rota_ac(a->c, rota);
    a->resultado = confirmar(&a->u, CID_A, CID_C, 2, rota, a->codigo, sizeof(a->codigo), &LAT_CONF);
    return NULL;
}

/* ---------- T7: dois itinerários com os mesmos trechos em ordens opostas ---------- */
static void *thr_ordens_opostas(void *p) {
    ArgP *a = p;
    a->ok = usuario_criar(&a->u, "Cliente", "o", a->idx, NULL);
    pthread_barrier_wait(a->bar);
    if (!a->ok) { a->resultado = -1; snprintf(a->codigo, sizeof(a->codigo), "SEM_LOGIN"); return NULL; }
    Par ida = { a->c->id1, CID_A, CID_B }, volta = { a->c->id2, CID_B, CID_A };
    if (a->variante == 0) {
        Par rota[2] = { ida, volta };
        a->resultado = confirmar(&a->u, CID_A, CID_A, 2, rota, a->codigo, sizeof(a->codigo), NULL);
    } else {
        Par rota[2] = { volta, ida };
        a->resultado = confirmar(&a->u, CID_B, CID_B, 2, rota, a->codigo, sizeof(a->codigo), NULL);
    }
    return NULL;
}

/* ---------- T8: itinerários diferentes disputando o mesmo trecho (X = só t1; Y = t1+t2) ---------- */
static void *thr_sobreposto(void *p) {
    ArgP *a = p;
    a->ok = usuario_criar(&a->u, "Cliente", "s", a->idx, NULL);
    pthread_barrier_wait(a->bar);
    if (!a->ok) { a->resultado = -1; snprintf(a->codigo, sizeof(a->codigo), "SEM_LOGIN"); return NULL; }
    Par rota[2];
    rota_ac(a->c, rota);
    if (a->variante == 0) a->resultado = confirmar(&a->u, CID_A, CID_C, 2, rota, a->codigo, sizeof(a->codigo), NULL);
    else a->resultado = confirmar(&a->u, CID_A, CID_B, 1, rota, a->codigo, sizeof(a->codigo), NULL);
    return NULL;
}

typedef void *(*FnThr)(void *);

/* cria N passageiros, solta todos juntos e espera; devolve nº de threads travadas */
static int disputa(int n, Cenario2 *c, FnThr fn, ArgP *args, int usa_variante) {
    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, (unsigned)n);
    pthread_t *th = calloc((size_t)n, sizeof(pthread_t));
    for (int i = 0; i < n; i++) {
        memset(&args[i], 0, sizeof(ArgP));
        args[i].idx = ++SEQ;
        args[i].variante = usa_variante ? (i % 2) : 0;
        args[i].c = c;
        args[i].bar = &bar;
        args[i].resultado = -1;
        if (pthread_create(&th[i], NULL, fn, &args[i]) != 0) {
            fprintf(stderr, "  (falha ao criar a thread %d: reduza --clientes)\n", i);
            exit(2);
        }
    }
    int travadas = juntar(th, n, 120);
    free(th);
    if (travadas == 0) pthread_barrier_destroy(&bar);
    return travadas;
}

/* ============================== T1 ============================== */
static void t1_autenticacao(void) {
    titulo("[T1] Cadastro e autenticação de motorista e passageiro");
    Usuario m, p, outro;
    int idx = ++SEQ;
    checar(usuario_criar(&m, "Motorista", "t1m", idx, NULL), "motorista cadastrou e autenticou");
    checar(usuario_criar(&p, "Cliente", "t1p", idx, NULL), "passageiro cadastrou e autenticou");

    char txt[128];
    cJSON *r = req_base("Cliente", "login");
    cJSON_AddStringToObject(r, "email", p.email);
    cJSON_AddStringToObject(r, "senha", "senha-errada");
    int ok = chamar_txt(&p.c, r, txt, sizeof(txt), NULL);
    checar(ok && strcmp(txt, "NAO_AUTENTICADO") == 0, "senha errada -> NAO_AUTENTICADO (%s)", txt);

    r = req_base("Cliente", "cadastro");
    cJSON_AddStringToObject(r, "nome", p.nome);
    cJSON_AddStringToObject(r, "email", p.email);
    cJSON_AddStringToObject(r, "senha", "123");
    ok = chamar_txt(&p.c, r, txt, sizeof(txt), NULL);
    checar(ok && strcmp(txt, "EMAIL_JA_CADASTRADO") == 0, "cadastro repetido -> EMAIL_JA_CADASTRADO (%s)", txt);

    /* o mesmo usuário pode autenticar de outra conexão (sessão por conexão) */
    memset(&outro, 0, sizeof(outro));
    snprintf(outro.classe, sizeof(outro.classe), "Cliente");
    snprintf(outro.email, sizeof(outro.email), "%s", p.email);
    snprintf(outro.nome, sizeof(outro.nome), "%s", p.nome);
    checar(conn_abrir(&outro.c) && usuario_login(&outro, NULL), "segunda conexão do mesmo usuário autentica");
    usuario_fechar(&outro);
    usuario_fechar(&m);
    usuario_fechar(&p);
}

/* ============================== T2 ============================== */
static void t2_publicar_e_consultar(void) {
    titulo("[T2] Motorista publica carona (rota, data, assentos, preço por trecho) e consulta o que publicou");
    Usuario m;
    char txt[128];
    checar(usuario_criar(&m, "Motorista", "t2m", ++SEQ, NULL), "motorista autenticado");
    TDef rota[2] = { { CID_A, CID_B, "02/02/2099", "08:00", 4, 20.0 }, { CID_B, CID_C, "02/02/2099", "10:00", 3, 30.0 } };
    checar(publicar(&m, 2, rota, txt, sizeof(txt)), "rota A->B->C publicada (%s)", txt);

    cJSON *lista = listar_trechos(&m);
    cJSON *t1 = lista ? achar_trecho(lista, CID_A, CID_B) : NULL, *t2 = lista ? achar_trecho(lista, CID_B, CID_C) : NULL;
    checar(lista && cJSON_GetArraySize(lista) == 2, "consulta mostra exatamente os 2 trechos publicados");
    checar(t1 && t2 && numero(t1, "capacidade") == 4 && numero(t2, "capacidade") == 3, "assentos controlados por trecho (4 e 3)");
    checar(t1 && t2 && cJSON_GetNumberValue(cJSON_GetObjectItem(t1, "preco")) == 20.0 &&
                       cJSON_GetNumberValue(cJSON_GetObjectItem(t2, "preco")) == 30.0, "preço por trecho preservado (20 e 30)");
    checar(t1 && t2 && npass(t1) == 0 && npass(t2) == 0, "sem passageiros confirmados ainda");
    cJSON_Delete(lista);

    /* trechos que não se encontram (B->C e depois A->B) não formam uma rota */
    TDef solta[2] = { { CID_B, CID_C, "03/03/2099", "08:00", 2, 10.0 }, { CID_A, CID_B, "03/03/2099", "09:00", 2, 10.0 } };
    int ok = publicar(&m, 2, solta, txt, sizeof(txt));
    checar(!ok && strcmp(txt, "ROTA_DESCONECTADA") == 0, "rota descontínua rejeitada (%s)", txt);
    usuario_fechar(&m);
}

/* ============================== T3 ============================== */
static void t3_controle_por_trecho(void) {
    titulo("[T3] Disponibilidade por trecho: assento ocupado no 1º trecho segue livre no 2º");
    Cenario2 c;
    if (!cenario2_criar(&c, "04/04/2099", 2)) { checar(0, "preparação do cenário"); return; }
    Usuario p1, p2;
    checar(usuario_criar(&p1, "Cliente", "t3a", ++SEQ, NULL) && usuario_criar(&p2, "Cliente", "t3b", ++SEQ, NULL), "2 passageiros autenticados");
    char cod[64];
    Par so1[1] = { { c.id1, CID_A, CID_B } }, so2[1] = { { c.id2, CID_B, CID_C } };
    int r1 = confirmar(&p1, CID_A, CID_B, 1, so1, cod, sizeof(cod), NULL);
    checar(r1 == 1, "passageiro 1 reserva só A->B (%s)", cod);
    int cap1, pas1, cap2, pas2;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(cap1 == 1 && pas1 == 1, "A->B ficou com 1 assento livre e 1 passageiro (livres=%d pass=%d)", cap1, pas1);
    checar(cap2 == 2 && pas2 == 0, "B->C continuou com os 2 assentos livres (livres=%d pass=%d)", cap2, pas2);
    int r2 = confirmar(&p2, CID_B, CID_C, 1, so2, cod, sizeof(cod), NULL);
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(r2 == 1 && cap2 == 1 && cap1 == 1, "passageiro 2 reserva só B->C sem mexer em A->B (A->B=%d, B->C=%d)", cap1, cap2);
    usuario_fechar(&p1); usuario_fechar(&p2); cenario2_fechar(&c);
}

/* ============================== T4 ============================== */
static void t4_itinerario_dois_motoristas(void) {
    titulo("[T4] Itinerário composto por trechos de motoristas diferentes");
    Cenario2 c;
    if (!cenario2_criar(&c, "05/05/2099", 3)) { checar(0, "preparação do cenário"); return; }
    Usuario p;
    usuario_criar(&p, "Cliente", "t4p", ++SEQ, NULL);
    Par rota[2];
    rota_ac(&c, rota);
    char cod[64];
    int r = confirmar(&p, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL);
    checar(r == 1, "itinerário A->B (motorista 1) + B->C (motorista 2) confirmado (%s)", cod);
    checar(num_reservas(&p) == 2, "passageiro consulta suas reservas: 2 trechos");
    int cap1, pas1, cap2, pas2;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(cap1 == 2 && pas1 == 1 && cap2 == 2 && pas2 == 1, "cada motorista vê o passageiro no seu trecho (livres %d/%d, pass %d/%d)", cap1, cap2, pas1, pas2);
    cJSON *lista = listar_trechos(&c.m1);
    cJSON *t = lista ? achar_trecho(lista, CID_A, CID_B) : NULL;
    char *nome = t ? cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(t, "nomesClientes"), 0)) : NULL;
    checar(nome && strcmp(nome, p.nome) == 0, "motorista 1 enxerga o NOME do passageiro confirmado no trecho");
    cJSON_Delete(lista);
    r = confirmar(&p, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL);   /* ainda há assentos, mas o passageiro já está nos trechos */
    checar(r == 0 && strcmp(cod, "RESERVA_DUPLICADA") == 0, "mesmo passageiro não reserva os mesmos trechos 2 vezes (%s)", cod);
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    checar(cap1 == 2 && pas1 == 1, "a tentativa duplicada não consumiu assento (livres=%d pass=%d)", cap1, pas1);
    usuario_fechar(&p); cenario2_fechar(&c);
}

/* ============================== T5 ============================== */
static void t5_atomicidade_basica(void) {
    titulo("[T5] Atomicidade: se um trecho falha, NENHUM é reservado");
    Cenario2 c;
    if (!cenario2_criar(&c, "06/06/2099", 1)) { checar(0, "preparação do cenário"); return; }
    Usuario ocupa, p;
    usuario_criar(&ocupa, "Cliente", "t5o", ++SEQ, NULL);
    usuario_criar(&p, "Cliente", "t5p", ++SEQ, NULL);
    char cod[64];
    Par so2[1] = { { c.id2, CID_B, CID_C } };
    checar(confirmar(&ocupa, CID_B, CID_C, 1, so2, cod, sizeof(cod), NULL) == 1, "outro passageiro lota o último trecho (B->C)");

    Par rota[2];
    rota_ac(&c, rota);
    int r = confirmar(&p, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL);
    checar(r == 0 && strcmp(cod, "ASSENTO_INDISPONIVEL") == 0, "último trecho lotado -> itinerário recusado (%s)", cod);
    int cap1, pas1;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    checar(cap1 == 1 && pas1 == 0, "o 1º trecho NÃO ficou reservado pela metade (livres=%d pass=%d)", cap1, pas1);
    checar(num_reservas(&p) == 0, "passageiro recusado não tem nenhuma reserva");

    Par inexistente[2] = { { c.id1, CID_A, CID_B }, { 999999, CID_B, CID_C } };
    r = confirmar(&p, CID_A, CID_C, 2, inexistente, cod, sizeof(cod), NULL);
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    checar(r == 0 && cap1 == 1 && pas1 == 0, "trecho inexistente no meio -> recusado e nada reservado (%s)", cod);

    Par descontinuo[2] = { { c.id1, CID_A, CID_B }, { c.id2, CID_C, CID_B } };
    r = confirmar(&p, CID_A, CID_B, 2, descontinuo, cod, sizeof(cod), NULL);
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    checar(r == 0 && cap1 == 1, "itinerário que não forma caminho contínuo -> recusado (%s)", cod);

    Par so1[1] = { { c.id1, CID_A, CID_B } };
    checar(confirmar(&p, CID_A, CID_B, 1, so1, cod, sizeof(cod), NULL) == 1, "reserva do 1º trecho sozinho funciona (assento realmente livre)");
    usuario_fechar(&ocupa); usuario_fechar(&p); cenario2_fechar(&c);
}

/* ============================== T6 ============================== */
static void t6_concorrencia_mesmo_itinerario(void) {
    int N = N_CLIENTES, K = ASSENTOS;
    titulo("[T6] Concorrência: passageiros disputando os mesmos assentos de um itinerário de 2 motoristas");
    printf("  %d passageiros simultâneos x %d assentos por trecho\n", N, K);
    Cenario2 c;
    if (!cenario2_criar(&c, "07/07/2099", K)) { checar(0, "preparação do cenário"); return; }
    ArgP *args = calloc((size_t)N, sizeof(ArgP));
    double t0 = agora_ms();
    int travadas = disputa(N, &c, thr_disputa_itinerario, args, 0);
    double dur = (agora_ms() - t0) / 1000.0;

    int conf = 0, recus = 0, erros = 0;
    for (int i = 0; i < N; i++) {
        if (args[i].resultado == 1) conf++;
        else if (args[i].resultado == 0 && strcmp(args[i].codigo, "ASSENTO_INDISPONIVEL") == 0) recus++;
        else { erros++; if (erros <= 3) printf("    resposta inesperada de um passageiro: %s\n", args[i].codigo); }
    }
    int esperado = N < K ? N : K;
    printf("  confirmados=%d recusados=%d erros=%d\n", conf, recus, erros);
    checar(travadas == 0, "nenhuma thread travada (sem deadlock)");
    checar(conf == esperado, "exatamente %d itinerários confirmados (nenhum assento vendido duas vezes)", esperado);
    checar(conf + recus == N, "todos os demais recusados com ASSENTO_INDISPONIVEL (sem erro de protocolo)");

    int cap1, pas1, cap2, pas2;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(pas1 == esperado && pas2 == esperado && cap1 == K - esperado && cap2 == K - esperado,
           "motoristas veem %d passageiros por trecho e %d assentos livres (A->B: %d/%d, B->C: %d/%d)", esperado, K - esperado, pas1, cap1, pas2, cap2);
    char det[160];
    checar(invariante_motorista(&c.m1, K, det, sizeof(det)) && invariante_motorista(&c.m2, K, det, sizeof(det)),
           "invariante: livres + passageiros == capacidade, sem passageiro repetido %s", det);

    /* nada pela metade: quem confirmou tem as 2 reservas; quem foi recusado, nenhuma */
    int meia = 0;
    for (int i = 0; i < N; i++) {
        if (!args[i].ok) continue;
        int n = num_reservas(&args[i].u);
        if ((args[i].resultado == 1 && n != 2) || (args[i].resultado == 0 && n != 0)) meia++;
    }
    checar(meia == 0, "nenhum itinerário confirmado pela metade (%d inconsistente(s))", meia);

    printf("  --- desempenho sob carga (%d clientes simultâneos) ---\n", N);
    amostra_resumo("login", &LAT_LOGIN);
    amostra_resumo("confirmar_itinerario", &LAT_CONF);
    printf("  tempo total da disputa: %.2f s (%.1f clientes/s, cada um com cadastro+login+confirmação)\n", dur, N / dur);
    amostra_limpar(&LAT_LOGIN); amostra_limpar(&LAT_CONF);

    for (int i = 0; i < N; i++) if (args[i].ok) usuario_fechar(&args[i].u);
    free(args);
    cenario2_fechar(&c);
}

/* ============================== T7 ============================== */
static void t7_ordens_opostas(void) {
    int K = 5, N = N_CLIENTES < 20 ? 20 : N_CLIENTES;
    titulo("[T7] Mesmos trechos pedidos em ordens opostas ao mesmo tempo (risco de deadlock)");
    Usuario m;
    char txt[128];
    usuario_criar(&m, "Motorista", "t7m", ++SEQ, NULL);
    TDef rota[2] = { { CID_A, CID_B, "08/08/2099", "09:00", K, 5.0 }, { CID_B, CID_A, "08/08/2099", "11:00", K, 5.0 } };
    if (!publicar(&m, 2, rota, txt, sizeof(txt))) { checar(0, "publicar ida e volta (%s) - o mapa precisa permitir B->A", txt); usuario_fechar(&m); return; }
    Cenario2 c;
    memset(&c, 0, sizeof(c));
    int cap, pas;
    estado_trecho(&m, CID_A, CID_B, &cap, &pas, &c.id1);
    estado_trecho(&m, CID_B, CID_A, &cap, &pas, &c.id2);

    ArgP *args = calloc((size_t)N, sizeof(ArgP));
    double t0 = agora_ms();
    int travadas = disputa(N, &c, thr_ordens_opostas, args, 1);
    int conf = 0, recus = 0;
    for (int i = 0; i < N; i++) {
        if (args[i].resultado == 1) conf++;
        else if (args[i].resultado == 0 && strcmp(args[i].codigo, "ASSENTO_INDISPONIVEL") == 0) recus++;
    }
    checar(travadas == 0, "nenhuma thread travada em %.2f s (ordem global de travas evita deadlock)", (agora_ms() - t0) / 1000.0);
    checar(conf == K && conf + recus == N, "exatamente %d confirmados e %d recusados (confirmados=%d recusados=%d)", K, N - K, conf, recus);
    char det[160];
    int cap1, pas1, cap2, pas2;
    estado_trecho(&m, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&m, CID_B, CID_A, &cap2, &pas2, NULL);
    checar(cap1 == 0 && cap2 == 0 && pas1 == K && pas2 == K && invariante_motorista(&m, K, det, sizeof(det)),
           "os dois trechos lotaram com exatamente %d passageiros cada %s", K, det);
    for (int i = 0; i < N; i++) if (args[i].ok) usuario_fechar(&args[i].u);
    free(args);
    usuario_fechar(&m);
}

/* ============================== T8 ============================== */
static void t8_itinerarios_sobrepostos(void) {
    int K = ASSENTOS, N = N_CLIENTES < 2 * K ? 2 * K : N_CLIENTES;
    titulo("[T8] Itinerários diferentes disputando o mesmo trecho (só A->B  x  A->B->C)");
    Cenario2 c;
    if (!cenario2_criar(&c, "09/09/2099", K)) { checar(0, "preparação do cenário"); return; }
    ArgP *args = calloc((size_t)N, sizeof(ArgP));
    int travadas = disputa(N, &c, thr_sobreposto, args, 1);
    int confX = 0, confY = 0, meia = 0;
    for (int i = 0; i < N; i++) {
        if (args[i].resultado == 1) { if (args[i].variante == 0) confY++; else confX++; }
    }
    int cap1, pas1, cap2, pas2;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    printf("  confirmados: só A->B=%d, A->B->C=%d | passageiros em A->B=%d, em B->C=%d\n", confX, confY, pas1, pas2);
    checar(travadas == 0, "nenhuma thread travada");
    checar(pas1 == K && confX + confY == K, "A->B vendeu exatamente %d assentos entre os dois tipos de itinerário", K);
    checar(pas2 == confY && cap2 == K - confY, "B->C vendeu apenas para quem escolheu o itinerário completo (%d)", confY);
    char det[160];
    checar(invariante_motorista(&c.m1, K, det, sizeof(det)) && invariante_motorista(&c.m2, K, det, sizeof(det)),
           "invariante: livres + passageiros == capacidade nos dois trechos %s", det);
    for (int i = 0; i < N; i++) {
        if (!args[i].ok) continue;
        int n = num_reservas(&args[i].u);
        int esperado = args[i].resultado == 1 ? (args[i].variante == 0 ? 2 : 1) : 0;
        if (n != esperado) meia++;
    }
    checar(meia == 0, "cada passageiro tem exatamente as reservas do que confirmou (%d divergência(s))", meia);
    for (int i = 0; i < N; i++) if (args[i].ok) usuario_fechar(&args[i].u);
    free(args);
    cenario2_fechar(&c);
}

/* ============================== T10 ============================== */
static void t10_cancelamentos(void) {
    titulo("[T10] Cancelamentos: assentos liberados e reutilizáveis, nada preso");
    char cod[64], txt[128];
    int cap1, pas1, cap2, pas2;

    /* (a) passageiro cancela um trecho do itinerário -> o itinerário inteiro é cancelado */
    Cenario2 c;
    if (!cenario2_criar(&c, "10/10/2099", 1)) { checar(0, "preparação do cenário (a)"); return; }
    Usuario p1, p2;
    usuario_criar(&p1, "Cliente", "t10a", ++SEQ, NULL);
    usuario_criar(&p2, "Cliente", "t10b", ++SEQ, NULL);
    Par rota[2];
    rota_ac(&c, rota);
    checar(confirmar(&p1, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL) == 1, "passageiro 1 confirma o itinerário (lotando os dois trechos)");
    checar(confirmar(&p2, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL) == 0, "passageiro 2 é recusado enquanto está lotado");
    int ok = cancelar_reserva(&p1, c.id1, txt, sizeof(txt));
    checar(ok && strcmp(txt, "CARONA_CANCELADA") == 0, "passageiro 1 cancela o trecho A->B (%s)", txt);
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(cap1 == 1 && pas1 == 0 && cap2 == 1 && pas2 == 0, "os DOIS trechos foram liberados (itinerário cancelado por inteiro: %d/%d livres)", cap1, cap2);
    checar(num_reservas(&p1) == 0, "passageiro 1 ficou sem reservas");
    checar(confirmar(&p2, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL) == 1, "os assentos liberados são reutilizados pelo passageiro 2");
    usuario_fechar(&p1); usuario_fechar(&p2); cenario2_fechar(&c);

    /* (b) motorista cancela um trecho -> passageiro afetado perde o itinerário e é avisado */
    if (!cenario2_criar(&c, "11/10/2099", 2)) { checar(0, "preparação do cenário (b)"); return; }
    Usuario p;
    usuario_criar(&p, "Cliente", "t10c", ++SEQ, NULL);
    rota_ac(&c, rota);
    checar(confirmar(&p, CID_A, CID_C, 2, rota, cod, sizeof(cod), NULL) == 1, "passageiro confirma o itinerário");
    cJSON *r = req_id(&c.m1, "cancelar_trecho");
    cJSON_AddNumberToObject(r, "idSelecionado", c.id1);
    ok = chamar_txt(&c.m1.c, r, txt, sizeof(txt), NULL);
    checar(ok && strcmp(txt, "TRECHO_CANCELADO") == 0, "motorista 1 cancela o trecho dele (%s)", txt);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    checar(cap2 == 2 && pas2 == 0, "o trecho do outro motorista foi liberado (livres=%d pass=%d)", cap2, pas2);
    checar(num_reservas(&p) == 0, "passageiro afetado ficou sem reservas");
    cJSON *avisos = chamar_json(&p.c, req_id(&p, "listar_avisos"), NULL);
    checar(avisos && cJSON_IsArray(avisos) && cJSON_GetArraySize(avisos) >= 1, "passageiro recebe aviso do trecho cancelado");
    cJSON_Delete(avisos);
    usuario_fechar(&p); cenario2_fechar(&c);
}

/* ============================== T11 ============================== */
static int contem_id(cJSON *arr, int id) {
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (numero(it, "id") == id) return 1;
    }
    return 0;
}

static void t11_busca(void) {
    titulo("[T11] Busca de caronas pelo passageiro");
    Usuario m, p;
    char txt[128];
    usuario_criar(&m, "Motorista", "t11m", ++SEQ, NULL);
    usuario_criar(&p, "Cliente", "t11p", ++SEQ, NULL);
    TDef direto = { CID_A, CID_C, "12/12/2099", "07:00", 2, 100.0 };
    checar(publicar(&m, 1, &direto, txt, sizeof(txt)), "carona direta A->C publicada (%s)", txt);
    int cap, pas, id;
    estado_trecho(&m, CID_A, CID_C, &cap, &pas, &id);

    cJSON *r = req_id(&p, "buscar_carona");
    cJSON_AddStringToObject(r, "origem", CID_A);
    cJSON_AddStringToObject(r, "destino", CID_C);
    cJSON_AddStringToObject(r, "data", "12/12/2099");
    cJSON *j = chamar_json(&p.c, r, &LAT_BUSCA);
    checar(j && cJSON_IsArray(j) && contem_id(j, id), "buscar_carona encontra a carona direta publicada");
    cJSON_Delete(j);

    if (!ESTENDIDO) {
        printf("  (busca de itinerários combinados no servidor: use --estendido com o servidor novo)\n");
        usuario_fechar(&m); usuario_fechar(&p);
        return;
    }

    /* combinação entre motoristas: A->B (08:00) + B->C (10:00) sai mais barata que o direto */
    Usuario mb, mc, md;
    usuario_criar(&mb, "Motorista", "t11b", ++SEQ, NULL);
    usuario_criar(&mc, "Motorista", "t11c", ++SEQ, NULL);
    usuario_criar(&md, "Motorista", "t11d", ++SEQ, NULL);
    TDef ab = { CID_A, CID_B, "12/12/2099", "08:00", 2, 20.0 };
    TDef bc = { CID_B, CID_C, "12/12/2099", "10:00", 2, 30.0 };
    TDef bc_antes = { CID_B, CID_C, "12/12/2099", "06:00", 2, 5.0 };      /* parte ANTES do embarque em B: não combina */
    TDef bc_outro_dia = { CID_B, CID_C, "13/12/2099", "10:00", 2, 5.0 };  /* outra data */
    checar(publicar(&mb, 1, &ab, txt, sizeof(txt)) && publicar(&mc, 1, &bc, txt, sizeof(txt)) &&
           publicar(&md, 1, &bc_antes, txt, sizeof(txt)) && publicar(&md, 1, &bc_outro_dia, txt, sizeof(txt)),
           "mais 4 caronas de 3 motoristas publicadas");

    r = req_id(&p, "buscar_itinerarios");
    cJSON_AddStringToObject(r, "origem", CID_A);
    cJSON_AddStringToObject(r, "destino", CID_C);
    cJSON_AddStringToObject(r, "data", "12/12/2099");
    j = chamar_json(&p.c, r, &LAT_BUSCA);
    int n = (j && cJSON_IsArray(j)) ? cJSON_GetArraySize(j) : -1;
    checar(n == 2, "apenas 2 itinerários válidos (sem o trecho que parte antes nem o de outra data): %d", n);
    cJSON *i0 = n > 0 ? cJSON_GetArrayItem(j, 0) : NULL, *i1 = n > 1 ? cJSON_GetArrayItem(j, 1) : NULL;
    checar(i0 && i1 && numero(i0, "numTrechos") == 1 && numero(i1, "numTrechos") == 2, "ordenação: menos trechos primeiro (direto, depois a combinação)");
    checar(i1 && cJSON_GetNumberValue(cJSON_GetObjectItem(i1, "precoTotal")) == 50.0, "combinação de 2 motoristas custa 20 + 30 = 50");
    cJSON_Delete(j);
    amostra_resumo("buscar_itinerarios", &LAT_BUSCA);

    r = req_id(&p, "buscar_itinerarios");
    cJSON_AddStringToObject(r, "origem", CID_A);
    cJSON_AddStringToObject(r, "destino", CID_C);
    cJSON_AddStringToObject(r, "data", "20/12/2099");
    j = chamar_json(&p.c, r, &LAT_BUSCA);
    checar(j && cJSON_IsArray(j) && cJSON_GetArraySize(j) == 0, "data sem oferta -> nenhum itinerário");
    cJSON_Delete(j);

    usuario_fechar(&mb); usuario_fechar(&mc); usuario_fechar(&md);
    usuario_fechar(&m); usuario_fechar(&p);
}

/* ============================== T9 ============================== */
static void t9_quedas_abruptas(void) {
    int K = 40, N = 30;
    titulo("[T9] Queda abrupta de clientes: servidor segue de pé, reservas íntegras, nada preso");
    Cenario2 c;
    if (!cenario2_criar(&c, "14/12/2099", K)) { checar(0, "preparação do cenário"); return; }

    /* (a) clientes que enviam JSON pela metade ou nada e somem */
    for (int i = 0; i < 25; i++) {
        Conn x;
        if (!conn_abrir(&x)) continue;
        if (i % 2) { const char *metade = "{\"classe\":\"Cliente\",\"acao\":\"login\",\"email\":\"x@y\",\"sen"; conn_enviar(&x, metade, strlen(metade)); }
        conn_fechar(&x, 0, i % 3 == 0);
    }

    /* (b) clientes que mandam a confirmação e somem sem ler a resposta (metade com RST) */
    Usuario *u = calloc((size_t)N, sizeof(Usuario));
    Par rota[2];
    rota_ac(&c, rota);
    int criados = 0;
    for (int i = 0; i < N; i++) {
        if (!usuario_criar(&u[i], "Cliente", "t9", ++SEQ, NULL)) continue;
        criados++;
        cJSON *r = req_id(&u[i], "confirmar_itinerario");
        cJSON_AddStringToObject(r, "origemRota", CID_A);
        cJSON_AddStringToObject(r, "destinoRota", CID_C);
        cJSON *arr = cJSON_AddArrayToObject(r, "rota");
        for (int k = 0; k < 2; k++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "idTrecho", rota[k].id);
            cJSON_AddStringToObject(o, "origem", rota[k].o);
            cJSON_AddStringToObject(o, "destino", rota[k].d);
            cJSON_AddItemToArray(arr, o);
        }
        char *msg = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        conn_enviar(&u[i].c, msg, strlen(msg));
        free(msg);
        conn_fechar(&u[i].c, 0, i % 2);          /* some sem ler a resposta */
    }
    usleep(800 * 1000);

    /* o servidor continua atendendo? */
    Usuario v;
    checar(usuario_criar(&v, "Cliente", "t9v", ++SEQ, NULL), "servidor continua atendendo novos clientes após as quedas abruptas");

    /* cada passageiro que caiu tem 0 ou 2 reservas, nunca 1 */
    int com2 = 0, com1 = 0;
    for (int i = 0; i < N; i++) {
        if (!u[i].ok) continue;
        Usuario re;
        memcpy(&re, &u[i], sizeof(re));
        if (!conn_abrir(&re.c) || !usuario_login(&re, NULL)) continue;
        int n = num_reservas(&re);
        if (n == 2) com2++;
        else if (n != 0) com1++;
        usuario_fechar(&re);
    }
    int cap1, pas1, cap2, pas2;
    estado_trecho(&c.m1, CID_A, CID_B, &cap1, &pas1, NULL);
    estado_trecho(&c.m2, CID_B, CID_C, &cap2, &pas2, NULL);
    printf("  (%d dos %d que caíram tiveram a confirmação processada)\n", com2, criados);
    checar(com1 == 0, "nenhum passageiro que caiu ficou com reserva pela metade (%d com 1 trecho)", com1);
    checar(pas1 == pas2 && pas1 == com2, "os dois trechos têm o mesmo nº de passageiros (%d / %d) = quem confirmou (%d)", pas1, pas2, com2);
    char det[160];
    checar(invariante_motorista(&c.m1, K, det, sizeof(det)) && invariante_motorista(&c.m2, K, det, sizeof(det)),
           "nenhum assento preso: livres + passageiros == capacidade %s", det);

    /* assentos continuam reserváveis por quem está vivo */
    Par r2[2];
    rota_ac(&c, r2);
    char cod[64];
    checar(confirmar(&v, CID_A, CID_C, 2, r2, cod, sizeof(cod), NULL) == 1, "passageiro novo ainda consegue reservar (%s)", cod);
    for (int i = 0; i < N; i++) free(u[i].c.buf);   /* conn_fechar já liberou; evita uso de ponteiro antigo */
    free(u);
    usuario_fechar(&v);
    cenario2_fechar(&c);
}

/* ============================== T12 (estendido): robustez do protocolo ============================== */
static void esperar_codigo(Conn *c, const char *esperado, const char *descricao) {
    char txt[256];
    cJSON *j = NULL;
    int ok = conn_ler(c, txt, sizeof(txt), &j);
    cJSON_Delete(j);
    checar(ok && strcmp(txt, esperado) == 0, "%s -> %s", descricao, ok ? txt : "(sem resposta)");
}

static void t12_robustez_protocolo(void) {
    titulo("[T12] Robustez do protocolo (servidor novo): validar e descartar mensagens malformadas");
    Conn c;
    char txt[256];
    if (!conn_abrir(&c)) { checar(0, "conectar"); return; }
    cJSON *r;

    r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "classe", "Cliente");
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "REQUISICAO_INVALIDA") == 0, "mensagem sem 'acao' -> REQUISICAO_INVALIDA (%s)", txt);
    r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "classe", "Cliente"); cJSON_AddNumberToObject(r, "acao", 123);
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "REQUISICAO_INVALIDA") == 0, "'acao' com tipo errado -> REQUISICAO_INVALIDA (%s)", txt);
    r = req_base("Cliente", "inventada");
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "ACAO_DESCONHECIDA") == 0, "ação inexistente -> ACAO_DESCONHECIDA (%s)", txt);
    r = req_base("Cliente", "buscar_itinerarios"); cJSON_AddStringToObject(r, "origem", CID_A); cJSON_AddStringToObject(r, "destino", CID_C);
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "NAO_AUTENTICADO") == 0, "ação protegida sem login -> NAO_AUTENTICADO (%s)", txt);
    r = req_base("Cliente", "cadastro"); cJSON_AddStringToObject(r, "nome", "x"); cJSON_AddStringToObject(r, "email", "y"); cJSON_AddStringToObject(r, "senha", "z");
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "REQUISICAO_INVALIDA") == 0, "e-mail curto demais -> REQUISICAO_INVALIDA (%s)", txt);
    r = req_base("Cliente", "selecionar_trecho");
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "ACAO_OBSOLETA") == 0, "fluxo antigo que prendia assentos -> ACAO_OBSOLETA (%s)", txt);

    conn_enviar(&c, "isto nao e json", 15);
    esperar_codigo(&c, "MENSAGEM_INVALIDA", "bytes fora do protocolo");
    const char *p1 = "{\"classe\":\"Cliente\",\"acao\":", *p2 = "\"inventada\"}";
    conn_enviar(&c, p1, strlen(p1));
    usleep(200 * 1000);
    conn_enviar(&c, p2, strlen(p2));
    esperar_codigo(&c, "ACAO_DESCONHECIDA", "mensagem dividida em 2 segmentos TCP");
    const char *dupla = "{\"classe\":\"Cliente\",\"acao\":\"inventada\"}{\"classe\":\"Cliente\",\"acao\":\"inventada\"}";
    conn_enviar(&c, dupla, strlen(dupla));
    int ok = conn_ler_exato(&c, 2 * strlen("ACAO_DESCONHECIDA"), txt, sizeof(txt));
    checar(ok && strcmp(txt, "ACAO_DESCONHECIDAACAO_DESCONHECIDA") == 0, "duas mensagens no mesmo segmento -> as duas tratadas");
    const char *um = "{\"classe\":\"Cliente\",\"acao\":\"inventada\"}";
    for (size_t i = 0; i < strlen(um); i++) conn_enviar(&c, um + i, 1);
    esperar_codigo(&c, "ACAO_DESCONHECIDA", "mensagem enviada byte a byte");

    /* identidade falsificada e classe errada */
    Usuario cli, mot;
    usuario_criar(&cli, "Cliente", "t12c", ++SEQ, NULL);
    usuario_criar(&mot, "Motorista", "t12m", ++SEQ, NULL);
    r = req_base("Cliente", "cancelar_carona");
    cJSON_AddNumberToObject(r, "idSelecionado", 0);
    cJSON_AddStringToObject(r, "emailCliente", "vitima@teste.com");
    checar(chamar_txt(&cli.c, r, txt, sizeof(txt), NULL) && strcmp(txt, "NAO_AUTORIZADO") == 0, "agir em nome de outro e-mail -> NAO_AUTORIZADO (%s)", txt);
    r = req_base("Motorista", "listar_trechos");
    checar(chamar_txt(&cli.c, r, txt, sizeof(txt), NULL) && strcmp(txt, "NAO_AUTORIZADO") == 0, "passageiro chamando ação de motorista -> NAO_AUTORIZADO (%s)", txt);

    /* publicação com campos inválidos */
    struct { const char *campo; double num; const char *str; } ruins[] = {
        { "capacidade", -5, NULL }, { "capacidade", 2.5, NULL }, { "preco", -1, NULL }, { "data", 0, "31-12-2099" }, { "hora", 0, "25:99" } };
    for (size_t k = 0; k < sizeof(ruins) / sizeof(ruins[0]); k++) {
        r = req_id(&mot, "cadastrar_trecho");
        cJSON_AddStringToObject(r, "origem", CID_A); cJSON_AddStringToObject(r, "destino", CID_B);
        cJSON_AddStringToObject(r, "data", "01/01/2099"); cJSON_AddStringToObject(r, "hora", "10:00");
        cJSON_AddNumberToObject(r, "capacidade", 3); cJSON_AddNumberToObject(r, "preco", 10);
        cJSON_DeleteItemFromObject(r, ruins[k].campo);
        if (ruins[k].str) cJSON_AddStringToObject(r, ruins[k].campo, ruins[k].str);
        else cJSON_AddNumberToObject(r, ruins[k].campo, ruins[k].num);
        int got = chamar_txt(&mot.c, r, txt, sizeof(txt), NULL);
        checar(got && (strcmp(txt, "REQUISICAO_INVALIDA") == 0 || strcmp(txt, "DADOS_INVALIDOS") == 0), "%s inválido rejeitado (%s)", ruins[k].campo, txt);
    }

    /* mensagem gigante */
    Conn g;
    if (conn_abrir(&g)) {
        char bloco[4096];
        memset(bloco, 'a', sizeof(bloco));
        { const char *ini = "{\"classe\":\"Cliente\",\"acao\":\"login\",\"email\":\""; conn_enviar(&g, ini, strlen(ini)); }
        for (int i = 0; i < 18; i++) if (!conn_enviar(&g, bloco, sizeof(bloco))) break;
        cJSON *j = NULL;
        int got = conn_ler(&g, txt, sizeof(txt), &j);
        cJSON_Delete(j);
        checar(!got || strcmp(txt, "MENSAGEM_MUITO_GRANDE") == 0, "mensagem acima do limite -> %s", got ? txt : "conexão encerrada");
        conn_fechar(&g, 0, 0);
    }

    if (LENTO) {
        Conn l;
        conn_abrir(&l);
        { const char *ini = "{\"classe\":\"Cliente\",\"acao\":"; conn_enviar(&l, ini, strlen(ini)); }
        printf("  (aguardando o tempo limite de mensagem incompleta, ~11 s...)\n");
        cJSON *j = NULL;
        int got = conn_ler(&l, txt, sizeof(txt), &j);
        cJSON_Delete(j);
        checar(got && strcmp(txt, "TEMPO_ESGOTADO") == 0, "mensagem incompleta além do prazo -> TEMPO_ESGOTADO (%s)", got ? txt : "sem resposta");
        conn_fechar(&l, 0, 0);
    }

    /* o servidor continua respondendo depois de tudo isso */
    r = req_base("Cliente", "inventada");
    checar(chamar_txt(&c, r, txt, sizeof(txt), NULL) && strcmp(txt, "ACAO_DESCONHECIDA") == 0, "a mesma conexão continua utilizável depois das mensagens ruins");
    conn_fechar(&c, 1, 0);
    usuario_fechar(&cli); usuario_fechar(&mot);
}

/* ============================== T13 (estendido): cancelar carona inteira ============================== */
static void t13_cancelar_rota(void) {
    titulo("[T13] Motorista cancela a carona inteira (servidor novo)");
    Usuario m, p;
    char txt[128], cod[64];
    usuario_criar(&m, "Motorista", "t13m", ++SEQ, NULL);
    usuario_criar(&p, "Cliente", "t13p", ++SEQ, NULL);
    TDef rota[2] = { { CID_A, CID_B, "15/12/2099", "08:00", 2, 10.0 }, { CID_B, CID_C, "15/12/2099", "10:00", 2, 10.0 } };
    checar(publicar(&m, 2, rota, txt, sizeof(txt)), "carona de 2 trechos publicada (%s)", txt);
    cJSON *lista = listar_trechos(&m);
    cJSON *t1 = lista ? achar_trecho(lista, CID_A, CID_B) : NULL, *t2 = lista ? achar_trecho(lista, CID_B, CID_C) : NULL;
    int id1 = numero(t1, "id"), id2 = numero(t2, "id"), idCarona = numero(t1, "idCarona");
    checar(t1 && t2 && idCarona >= 0 && idCarona == numero(t2, "idCarona"), "os 2 trechos compartilham o mesmo idCarona (%d)", idCarona);
    cJSON_Delete(lista);
    Par par[2] = { { id1, CID_A, CID_B }, { id2, CID_B, CID_C } };
    checar(confirmar(&p, CID_A, CID_C, 2, par, cod, sizeof(cod), NULL) == 1, "passageiro confirma a carona inteira");
    cJSON *r = req_id(&m, "cancelar_rota");
    cJSON_AddNumberToObject(r, "idCarona", idCarona);
    int ok = chamar_txt(&m.c, r, txt, sizeof(txt), NULL);
    checar(ok && strcmp(txt, "ROTA_CANCELADA") == 0, "cancelar_rota -> ROTA_CANCELADA (%s)", txt);
    lista = listar_trechos(&m);
    checar(lista && cJSON_GetArraySize(lista) == 0, "motorista não tem mais nenhum trecho dessa carona");
    cJSON_Delete(lista);
    checar(num_reservas(&p) == 0, "reserva do passageiro cancelada junto");
    usuario_fechar(&m); usuario_fechar(&p);
}

/* ============================== principal ============================== */
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) snprintf(HOST, sizeof(HOST), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) PORTA = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clientes") && i + 1 < argc) N_CLIENTES = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--assentos") && i + 1 < argc) ASSENTOS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--origem") && i + 1 < argc) snprintf(CID_A, sizeof(CID_A), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--meio") && i + 1 < argc) snprintf(CID_B, sizeof(CID_B), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--destino") && i + 1 < argc) snprintf(CID_C, sizeof(CID_C), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--estendido")) ESTENDIDO = 1;
        else if (!strcmp(argv[i], "--lento")) LENTO = 1;
        else { fprintf(stderr, "uso: %s [--host H] [--port P] [--clientes N] [--assentos K] [--origem A --meio B --destino C] [--estendido] [--lento]\n", argv[0]); return 2; }
    }
    if (N_CLIENTES < 2 || N_CLIENTES > MAX_THREADS || ASSENTOS < 1 || ASSENTOS > 100) {
        fprintf(stderr, "--clientes deve estar entre 2 e %d e --assentos entre 1 e 100\n", MAX_THREADS);
        return 2;
    }
    snprintf(UID, sizeof(UID), "%lx%x", (unsigned long)time(NULL) & 0xffffff, (unsigned)getpid() & 0xffff);

    Conn sonda;
    if (!conn_abrir(&sonda)) {
        fprintf(stderr, "Não foi possível conectar em %s:%d. O servidor está rodando?\n", HOST, PORTA);
        return 2;
    }
    conn_fechar(&sonda, 1, 0);

    printf("VaiJunto - teste automatizado | servidor %s:%d | %d clientes, %d assentos | cidades %s -> %s -> %s%s\n",
           HOST, PORTA, N_CLIENTES, ASSENTOS, CID_A, CID_B, CID_C, ESTENDIDO ? " | modo estendido" : "");

    t1_autenticacao();
    t2_publicar_e_consultar();
    t3_controle_por_trecho();
    t4_itinerario_dois_motoristas();
    t5_atomicidade_basica();
    t6_concorrencia_mesmo_itinerario();
    t7_ordens_opostas();
    t8_itinerarios_sobrepostos();
    t10_cancelamentos();
    t11_busca();
    t9_quedas_abruptas();            /* por último dos básicos: se o servidor cair, os demais já rodaram */
    if (ESTENDIDO) {
        t12_robustez_protocolo();
        t13_cancelar_rota();
    }

    printf("\n==================== RESULTADO ====================\n");
    printf("verificações ok: %d | falhas: %d\n", total_ok, total_falha);
    if (total_falha) {
        printf("Falhas:\n");
        for (int i = 0; i < n_falhas; i++) printf("  - %s\n", lista_falhas[i]);
        printf("\nALGUMA VERIFICAÇÃO FALHOU\n");
        return 1;
    }
    printf("TODAS AS VERIFICAÇÕES PASSARAM\n");
    return 0;
}