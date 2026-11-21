/*
 * Interface gráfica (GTK3) do VaiJunto: cliente (passageiro) e motorista.
 *
 * Duas telas:
 *   1) Login / Cadastro  (escolha do perfil, IP do servidor, e-mail, senha)
 *   2) Operações         (abas diferentes para cliente e motorista)
 *
 * Os trechos devolvidos pelo servidor aparecem em listas: basta clicar na linha
 * (o ID vem junto) em vez de digitar o ID. Fala com o servidor da própria empresa
 * pelo mesmo protocolo TCP/JSON dos clientes de terminal (porta 65432).
 *
 * Compilação: veja o Makefile (usa -I cJSON, por isso o include abaixo é "cJSON.h").
 */
#include <gtk/gtk.h>
#include "cJSON.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define PORTA_PADRAO 65432
#define TAM_BUFFER   65536   /* tamanho máximo de uma resposta do servidor */
#define TIMEOUT_SEG  8       /* tempo máximo esperando o servidor (evita travar a janela) */

/* Lista (tabela) onde todas as colunas são texto; a coluna 0 é sempre o ID */
typedef struct {
    GtkWidget    *scroll, *view;
    GtkListStore *store;
    int           n;
} Lista;

/* Campos de um formulário de trecho (motorista) */
typedef struct {
    GtkWidget *origem, *destino, *data, *hora, *cap, *preco;
} FormTrecho;

typedef struct {
    GtkWidget *janela, *pilha, *pilha_perfil;

    /* tela de login */
    GtkWidget *l_perfil, *l_ip, *l_nome, *l_email, *l_senha, *l_msg;

    /* sessão */
    int  logado, motorista;
    char nome[128], email[128], senha[128], ip[256];
    GtkWidget *cab_usuario, *barra, *log, *nb_cliente, *nb_motorista;

    /* cliente */
    GtkWidget *c_origem, *c_destino, *c_data, *c_total;
    Lista busca, rota_cli, minhas, avisos;

    /* motorista */
    FormTrecho f_unico, f_rota;
    Lista meus, rota_mot;

    /* conexão TCP */
    int  socket_servidor;
    char ip_conectado[256];
} App;

/* ============================================================== utilidades de UI */

/* Texto do campo sem espaços nas pontas (quem chama libera com g_free / g_autofree) */
static char *limpo(GtkWidget *w) {
    return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(w))));
}

static void aviso(App *a, GtkMessageType tipo, const char *msg) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->janela), GTK_DIALOG_MODAL, tipo,
                                          GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static int confirmar(App *a, const char *msg) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->janela), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_YES_NO, "%s", msg);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_YES;
}

static void status_ops(App *a, const char *msg) {
    gtk_label_set_text(GTK_LABEL(a->barra), msg);
}

static void login_msg(App *a, const char *msg) {
    gtk_label_set_text(GTK_LABEL(a->l_msg), msg);
}

/* Registra o que foi enviado/recebido no painel "Log de comunicação" */
static void log_com(App *a, const char *rotulo, const char *conteudo) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log));
    if (gtk_text_buffer_get_char_count(b) > 200000) {
        GtkTextIter ini, meio;
        gtk_text_buffer_get_start_iter(b, &ini);
        gtk_text_buffer_get_iter_at_offset(b, &meio, 100000);
        gtk_text_buffer_delete(b, &ini, &meio);
    }
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    char hora[16];
    strftime(hora, sizeof(hora), "%H:%M:%S", &tm_);

    char *ok = g_utf8_make_valid(conteudo ? conteudo : "", -1);
    char *linha = g_strdup_printf("[%s] %s\n%s\n\n", hora, rotulo, ok);
    GtkTextIter fim;
    gtk_text_buffer_get_end_iter(b, &fim);
    gtk_text_buffer_insert(b, &fim, linha, -1);
    gtk_text_buffer_get_end_iter(b, &fim);
    GtkTextMark *marca = gtk_text_buffer_create_mark(b, NULL, &fim, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(a->log), marca);
    gtk_text_buffer_delete_mark(b, marca);
    g_free(ok);
    g_free(linha);
}

/* ============================================================== rede */

static void fechar_conexao(App *a) {
    if (a->socket_servidor >= 0) close(a->socket_servidor);
    a->socket_servidor = -1;
    a->ip_conectado[0] = '\0';
}

/* Abre a conexão TCP e define timeout de leitura/escrita. Devolve o socket ou -1 */
static int conectar(const char *host) {
    struct addrinfo dicas, *lista = NULL, *p;
    memset(&dicas, 0, sizeof(dicas));
    dicas.ai_family = AF_UNSPEC;
    dicas.ai_socktype = SOCK_STREAM;

    char porta[16];
    snprintf(porta, sizeof(porta), "%d", PORTA_PADRAO);
    if (getaddrinfo(host, porta, &dicas, &lista) != 0) return -1;

    int fd = -1;
    for (p = lista; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(lista);

    if (fd >= 0) {
        struct timeval tv = { TIMEOUT_SEG, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

static int enviar_tudo(int fd, const char *dados, size_t tamanho) {
    size_t enviados = 0;
    while (enviados < tamanho) {
        /* MSG_NOSIGNAL: se o servidor caiu, send() falha em vez de matar a janela com SIGPIPE */
        ssize_t n = send(fd, dados + enviados, tamanho - enviados, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        enviados += (size_t)n;
    }
    return 1;
}

typedef enum { RESP_OK, RESP_FECHADA, RESP_TIMEOUT, RESP_INCOMPLETA } EstadoResp;

/* O servidor não usa delimitador: lemos até formar um JSON válido. Respostas em texto
   simples (ex.: CARONA_CANCELADA) são aceitas no primeiro pacote. */
static EstadoResp receber_resposta(int fd, char **saida) {
    char *buf = g_malloc0(TAM_BUFFER + 1);
    size_t total = 0;
    EstadoResp estado = RESP_INCOMPLETA;

    while (total < TAM_BUFFER) {
        ssize_t n = recv(fd, buf + total, TAM_BUFFER - total, 0);
        if (n == 0) { estado = total ? RESP_INCOMPLETA : RESP_FECHADA; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            estado = (errno == EAGAIN || errno == EWOULDBLOCK) ? RESP_TIMEOUT : RESP_FECHADA;
            break;
        }
        total += (size_t)n;
        buf[total] = '\0';

        if (buf[0] != '{' && buf[0] != '[') { estado = RESP_OK; break; }
        cJSON *teste = cJSON_Parse(buf);
        if (teste != NULL) { cJSON_Delete(teste); estado = RESP_OK; break; }
    }
    *saida = buf;
    return estado;
}

/* Envia o pedido e devolve a resposta (alocada com g_malloc; quem chama usa g_free).
   *erro = 1 se não houve resposta utilizável (a string devolvida é então a mensagem de erro).
   Reaproveita a conexão e, se ela estiver velha (servidor reiniciou), reconecta e reenvia uma vez. */
static char *enviar_receber(App *a, cJSON *pedido, int *erro) {
    *erro = 1;
    const char *ip = a->ip;
    if (*ip == '\0') return g_strdup("Informe o IP do servidor.");

    char *json = cJSON_PrintUnformatted(pedido);
    if (json == NULL) return g_strdup("Erro ao montar a requisição JSON.");
    size_t len = strlen(json);

    /* trocou o IP: descarta a conexão anterior */
    if (a->socket_servidor >= 0 && strcmp(a->ip_conectado, ip) != 0) fechar_conexao(a);

    char *resultado = NULL;
    for (int tentativa = 0; tentativa < 2 && resultado == NULL; tentativa++) {
        int reutilizada = (a->socket_servidor >= 0);

        if (!reutilizada) {
            a->socket_servidor = conectar(ip);
            if (a->socket_servidor < 0) {
                resultado = g_strdup_printf("Não foi possível conectar a %s:%d.\n"
                    "Verifique o IP, a porta e se o servidor está ativo.", ip, PORTA_PADRAO);
                break;
            }
            g_strlcpy(a->ip_conectado, ip, sizeof(a->ip_conectado));
        }

        if (!enviar_tudo(a->socket_servidor, json, len)) {
            fechar_conexao(a);
            if (reutilizada) continue;               /* conexão velha: tenta de novo, já reconectando */
            resultado = g_strdup("Falha ao enviar a requisição ao servidor.");
            break;
        }

        char *resp = NULL;
        EstadoResp estado = receber_resposta(a->socket_servidor, &resp);

        if (estado == RESP_OK) {
            resultado = resp;
            *erro = 0;
        } else if (estado == RESP_FECHADA) {
            g_free(resp);
            fechar_conexao(a);
            if (reutilizada) continue;               /* servidor tinha fechado a conexão antiga */
            resultado = g_strdup("O servidor encerrou a conexão sem enviar resposta.");
        } else {
            /* timeout ou JSON incompleto: a conexão fica fora de sincronia, então é descartada */
            fechar_conexao(a);
            if (resp[0] == '\0') {
                g_free(resp);
                resultado = g_strdup_printf("O servidor não respondeu em %d segundos.", TIMEOUT_SEG);
            } else {
                resultado = g_strdup_printf("%s\n\n[AVISO] Resposta incompleta (%s).", resp,
                    estado == RESP_TIMEOUT ? "tempo esgotado" : "conexão encerrada");
                g_free(resp);
            }
        }
    }
    free(json);
    return resultado;
}

/* Cópia do pedido com a senha escondida, para mostrar no log */
static char *pedido_para_exibir(cJSON *p) {
    cJSON *copia = cJSON_Duplicate(p, 1);
    if (cJSON_GetObjectItemCaseSensitive(copia, "senha") != NULL)
        cJSON_ReplaceItemInObjectCaseSensitive(copia, "senha", cJSON_CreateString("***"));
    char *s = cJSON_Print(copia);
    cJSON_Delete(copia);
    return s;
}

static cJSON *pedido_base(int motorista, const char *acao) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "classe", motorista ? "Motorista" : "Cliente");
    cJSON_AddStringToObject(p, "acao", acao);
    return p;
}

/* Envia o pedido (liberado aqui), registra no log e devolve a resposta.
   Em caso de falha de comunicação mostra o erro ao usuário e devolve NULL. */
static char *chamar(App *a, cJSON *p) {
    char *exibir = pedido_para_exibir(p);
    log_com(a, "→ enviado", exibir);
    free(exibir);

    int erro = 0;
    char *resp = enviar_receber(a, p, &erro);
    cJSON_Delete(p);
    log_com(a, erro ? "✖ erro" : "← resposta", resp);

    if (erro) {
        status_ops(a, "Falha na comunicação com o servidor.");
        aviso(a, GTK_MESSAGE_ERROR, resp);
        g_free(resp);
        return NULL;
    }
    return resp;
}

/* Interpreta a resposta como array JSON; se for texto de erro do servidor, avisa e devolve NULL */
static cJSON *resp_array(App *a, const char *resp) {
    cJSON *j = (resp[0] == '[') ? cJSON_Parse(resp) : NULL;
    if (j == NULL) {
        char *msg = g_strdup_printf("O servidor respondeu: %s", resp);
        aviso(a, GTK_MESSAGE_WARNING, msg);
        g_free(msg);
    }
    return j;
}

/* ============================================================== listas (tabelas) */

static void lista_criar(Lista *l, const char **titulos, int n, int altura_min) {
    GType *tipos = g_new(GType, n);
    for (int i = 0; i < n; i++) tipos[i] = G_TYPE_STRING;
    l->store = gtk_list_store_newv(n, tipos);
    g_free(tipos);
    l->n = n;

    l->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(l->store));
    for (int i = 0; i < n; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titulos[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(l->view), c);
    }
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(l->view)), GTK_SELECTION_SINGLE);

    l->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(l->scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(l->scroll), altura_min);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(l->scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(l->scroll), l->view);
}

static void lista_limpar(Lista *l) {
    gtk_list_store_clear(l->store);
}

static int lista_tamanho(Lista *l) {
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(l->store), NULL);
}

/* Linha selecionada pelo usuário. Devolve 1 se há seleção */
static int lista_selecionada(Lista *l, GtkTreeIter *it) {
    GtkTreeModel *m;
    return gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(l->view)), &m, it);
}

static char *lista_texto(Lista *l, GtkTreeIter *it, int coluna) {
    char *s = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(l->store), it, coluna, &s, -1);
    return s ? s : g_strdup("");
}

/* Converte um valor JSON em texto para a célula */
static char *valor_texto(cJSON *v, const char *chave) {
    if (cJSON_IsString(v) && v->valuestring) return g_strdup(v->valuestring);
    if (cJSON_IsNumber(v)) {
        if (strcmp(chave, "preco") == 0) return g_strdup_printf("%.2f", v->valuedouble);
        return g_strdup_printf("%.0f", v->valuedouble);
    }
    if (cJSON_IsArray(v)) {
        GString *s = g_string_new(NULL);
        cJSON *e;
        cJSON_ArrayForEach(e, v) {
            if (cJSON_IsString(e) && e->valuestring) {
                if (s->len) g_string_append(s, ", ");
                g_string_append(s, e->valuestring);
            }
        }
        if (s->len == 0) g_string_assign(s, "-");
        return g_string_free(s, FALSE);
    }
    return g_strdup("");
}

/* Preenche a lista com um array JSON; chaves[c] diz qual campo vai na coluna c */
static void lista_preencher(Lista *l, cJSON *arr, const char **chaves, int anexar) {
    if (!anexar) lista_limpar(l);
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        GtkTreeIter it;
        gtk_list_store_append(l->store, &it);
        for (int c = 0; c < l->n; c++) {
            char *v = valor_texto(cJSON_GetObjectItemCaseSensitive(item, chaves[c]), chaves[c]);
            gtk_list_store_set(l->store, &it, c, v, -1);
            g_free(v);
        }
    }
}

/* ============================================================== construção de widgets */

static GtkWidget *rotulo(const char *t) {
    GtkWidget *l = gtk_label_new(t);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

/* Rótulo + campo de texto na grade */
static GtkWidget *campo(GtkWidget *grade, const char *r, int linha, int coluna, int max, int largura) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(e), largura);
    if (max > 0) gtk_entry_set_max_length(GTK_ENTRY(e), max);
    gtk_grid_attach(GTK_GRID(grade), rotulo(r), coluna, linha, 1, 1);
    gtk_grid_attach(GTK_GRID(grade), e, coluna + 1, linha, 1, 1);
    return e;
}

static GtkWidget *botao(const char *t, GCallback cb, gpointer dados) {
    GtkWidget *b = gtk_button_new_with_label(t);
    g_signal_connect(b, "clicked", cb, dados);
    return b;
}

static GtkWidget *grade_nova(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    return g;
}

/* Máscara de digitação: o usuário digita só números e o separador entra sozinho
   (ex.: 30122026 vira 30/12/2026; 0830 vira 08:30). Apagar funciona normalmente. */
typedef struct { char sep; int n; int tam[3]; } Mascara;
static const Mascara MASCARA_DATA = { '/', 3, {2, 2, 4} };
static const Mascara MASCARA_HORA = { ':', 2, {2, 2, 0} };

/* O GTK move o cursor logo depois do "changed", então o ajuste final é adiado para
   depois que ele terminar (senão os próximos números caem no lugar errado). */
static gboolean cursor_no_fim(gpointer dados) {
    GtkEditable *e = GTK_EDITABLE(dados);
    gtk_editable_set_position(e, -1);
    g_object_unref(e);
    return G_SOURCE_REMOVE;
}

static void ao_mascara(GtkEditable *e, gpointer dados) {
    const Mascara *m = dados;
    const char *atual = gtk_entry_get_text(GTK_ENTRY(e));
    char saida[32];
    int j = 0, grupo = 0, no_grupo = 0;

    for (const char *p = atual; *p && j < (int)sizeof(saida) - 2; p++) {
        if (*p < '0' || *p > '9') continue;              /* ignora tudo que não é dígito */
        if (no_grupo == m->tam[grupo]) {                  /* grupo cheio: abre o próximo com o separador */
            if (grupo + 1 >= m->n) break;                 /* passou do tamanho máximo */
            grupo++;
            no_grupo = 0;
            saida[j++] = m->sep;
        }
        saida[j++] = *p;
        no_grupo++;
    }
    saida[j] = '\0';

    if (strcmp(saida, atual) != 0) {
        g_signal_handlers_block_by_func(e, ao_mascara, dados);
        gtk_entry_set_text(GTK_ENTRY(e), saida);
        g_signal_handlers_unblock_by_func(e, ao_mascara, dados);
        g_idle_add(cursor_no_fim, g_object_ref(e));
    }
}

static void aplicar_mascara(GtkWidget *entry, const Mascara *m) {
    g_signal_connect(entry, "changed", G_CALLBACK(ao_mascara), (gpointer)m);
}

static GtkWidget *linha_h(void) {
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
}

static GtkWidget *aba_nova(void) {
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(b), 12);
    return b;
}

/* ============================================================== validação dos formulários do motorista */

typedef struct { char *origem, *destino, *data, *hora; int cap; double preco; } DadosTrecho;

static void dados_liberar(DadosTrecho *t) {
    g_free(t->origem); g_free(t->destino); g_free(t->data); g_free(t->hora);
    memset(t, 0, sizeof(*t));
}

static int data_valida(const char *s) {
    int d, m, y;
    if (strlen(s) != 10 || s[2] != '/' || s[5] != '/') return 0;
    if (sscanf(s, "%d/%d/%d", &d, &m, &y) != 3) return 0;
    return d >= 1 && d <= 31 && m >= 1 && m <= 12 && y >= 2000 && y <= 2100;
}

static int hora_valida(const char *s) {
    int h, mi;
    if (strlen(s) != 5 || s[2] != ':') return 0;
    if (sscanf(s, "%d:%d", &h, &mi) != 2) return 0;
    return h >= 0 && h <= 23 && mi >= 0 && mi <= 59;
}

/* O servidor apaga trechos cuja data/hora já passou, então avisamos antes */
static int ja_passou(const char *data, const char *hora) {
    int d, m, y, h, mi;
    sscanf(data, "%d/%d/%d", &d, &m, &y);
    sscanf(hora, "%d:%d", &h, &mi);
    struct tm tm_ = {0};
    tm_.tm_mday = d; tm_.tm_mon = m - 1; tm_.tm_year = y - 1900;
    tm_.tm_hour = h; tm_.tm_min = mi; tm_.tm_isdst = -1;
    time_t t = mktime(&tm_);
    return t != (time_t)-1 && t < time(NULL);
}

/* Lê e valida o formulário. Em caso de erro avisa o usuário e devolve 0 */
static int ler_form(App *a, FormTrecho *f, DadosTrecho *t) {
    memset(t, 0, sizeof(*t));
    t->origem = limpo(f->origem);
    t->destino = limpo(f->destino);
    t->data = limpo(f->data);
    t->hora = limpo(f->hora);
    char *cap_s = limpo(f->cap);
    char *preco_s = limpo(f->preco);
    const char *erro = NULL;
    char *fim = NULL;

    if (!*t->origem || !*t->destino) erro = "Informe a origem e o destino.";
    else if (strcmp(t->origem, t->destino) == 0) erro = "A origem e o destino devem ser diferentes.";
    else if (!data_valida(t->data)) erro = "Data inválida. Use o formato dd/mm/aaaa.";
    else if (!hora_valida(t->hora)) erro = "Hora inválida. Use o formato hh:mm.";
    else if (ja_passou(t->data, t->hora)) erro = "Essa data/hora já passou. O servidor removeria o trecho em seguida.";
    else {
        long c = strtol(cap_s, &fim, 10);
        if (!*cap_s || *fim || c < 1 || c > 100) erro = "A capacidade deve ser um número inteiro entre 1 e 100.";
        else t->cap = (int)c;
    }
    if (!erro) {
        g_strdelimit(preco_s, ",", '.');   /* aceita vírgula decimal */
        double p = g_ascii_strtod(preco_s, &fim);
        if (!*preco_s || *fim || p < 0) erro = "Preço inválido. Exemplo: 25.50";
        else t->preco = p;
    }
    g_free(cap_s);
    g_free(preco_s);

    if (erro) {
        aviso(a, GTK_MESSAGE_WARNING, erro);
        dados_liberar(t);
        return 0;
    }
    return 1;
}

/* ============================================================== sessão: login, cadastro e sair */

static const char *CHAVES_BUSCA[]  = {"id", "nomeMotorista", "origem", "destino", "data", "hora", "capacidade", "preco"};
static const char *TITULOS_BUSCA[] = {"ID", "Motorista", "Origem", "Destino", "Data", "Hora", "Vagas", "Preço"};
static const char *CHAVES_RESERVA[]  = {"id", "nomeMotorista", "origem", "destino", "data", "hora", "preco"};
static const char *TITULOS_RESERVA[] = {"ID", "Motorista", "Origem", "Destino", "Data", "Hora", "Preço"};
static const char *CHAVES_AVISO[]  = {"nomeMotorista", "origem", "destino", "data", "hora"};
static const char *CHAVES_MEUS[]   = {"id", "origem", "destino", "data", "hora", "capacidade", "preco", "nomesClientes"};

static int perfil_motorista(App *a) {
    return gtk_combo_box_get_active(GTK_COMBO_BOX(a->l_perfil)) == 1;
}

/* Copia o IP digitado para a sessão. Devolve 0 se estiver vazio */
static int definir_ip(App *a) {
    char *ip = limpo(a->l_ip);
    g_strlcpy(a->ip, ip, sizeof(a->ip));
    g_free(ip);
    return a->ip[0] != '\0';
}

static void limpar_listas(App *a) {
    lista_limpar(&a->busca); lista_limpar(&a->rota_cli); lista_limpar(&a->minhas); lista_limpar(&a->avisos);
    lista_limpar(&a->meus);  lista_limpar(&a->rota_mot);
    gtk_label_set_text(GTK_LABEL(a->c_total), "Total da rota: R$ 0.00");
}

static void iniciar_sessao(App *a, int motorista, const char *nome, const char *email, const char *senha) {
    a->logado = 1;
    a->motorista = motorista;
    g_strlcpy(a->nome, nome, sizeof(a->nome));
    g_strlcpy(a->email, email, sizeof(a->email));
    g_strlcpy(a->senha, senha, sizeof(a->senha));

    char *marcado = g_markup_printf_escaped("Logado como <b>%s</b> (%s) — servidor %s:%d",
                                            nome, motorista ? "Motorista" : "Cliente", a->ip, PORTA_PADRAO);
    gtk_label_set_markup(GTK_LABEL(a->cab_usuario), marcado);
    g_free(marcado);

    limpar_listas(a);
    gtk_stack_set_visible_child_name(GTK_STACK(a->pilha_perfil), motorista ? "motorista" : "cliente");
    gtk_notebook_set_current_page(GTK_NOTEBOOK(motorista ? a->nb_motorista : a->nb_cliente), 0);
    status_ops(a, "Pronto.");
    gtk_entry_set_text(GTK_ENTRY(a->l_senha), "");
    login_msg(a, "");
    gtk_stack_set_visible_child_name(GTK_STACK(a->pilha), "ops");
}

static void ao_entrar(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    g_autofree char *email = limpo(a->l_email);
    const char *senha = gtk_entry_get_text(GTK_ENTRY(a->l_senha));
    if (!*email || !*senha) { login_msg(a, "Informe o e-mail e a senha."); return; }
    if (!definir_ip(a)) { login_msg(a, "Informe o IP do servidor."); return; }

    int motorista = perfil_motorista(a);
    cJSON *p = pedido_base(motorista, "login");
    cJSON_AddStringToObject(p, "nome", "");
    cJSON_AddStringToObject(p, "email", email);
    cJSON_AddStringToObject(p, "senha", senha);
    cJSON_AddStringToObject(p, "status", "");

    g_autofree char *resp = chamar(a, p);
    if (!resp) { login_msg(a, "Não foi possível falar com o servidor."); return; }

    cJSON *j = (resp[0] == '{') ? cJSON_Parse(resp) : NULL;
    const char *nome = j ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "nome")) : NULL;
    if (nome != NULL) {
        iniciar_sessao(a, motorista, nome, email, senha);
    } else if (strcmp(resp, "NAO_AUTENTICADO") == 0) {
        login_msg(a, "E-mail ou senha incorretos (confira também o perfil escolhido).");
    } else if (strcmp(resp, "DADOS_INVALIDOS") == 0) {
        login_msg(a, "Preencha todos os campos.");
    } else {
        g_autofree char *msg = g_strdup_printf("Resposta inesperada do servidor: %s", resp);
        login_msg(a, msg);
    }
    cJSON_Delete(j);
}

/* Enter no campo de senha equivale a clicar em Entrar */
static void ao_enter_senha(GtkEntry *e, gpointer dados) {
    (void)e;
    ao_entrar(NULL, dados);
}

static void ao_cadastrar(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    g_autofree char *nome = limpo(a->l_nome);
    g_autofree char *email = limpo(a->l_email);
    const char *senha = gtk_entry_get_text(GTK_ENTRY(a->l_senha));
    if (!*nome || !*email || !*senha) { login_msg(a, "Para cadastrar, preencha nome, e-mail e senha."); return; }
    if (!definir_ip(a)) { login_msg(a, "Informe o IP do servidor."); return; }

    cJSON *p = pedido_base(perfil_motorista(a), "cadastro");
    cJSON_AddStringToObject(p, "nome", nome);
    cJSON_AddStringToObject(p, "email", email);
    cJSON_AddStringToObject(p, "senha", senha);
    cJSON_AddStringToObject(p, "status", "");

    g_autofree char *resp = chamar(a, p);
    if (!resp) { login_msg(a, "Não foi possível falar com o servidor."); return; }

    if (strcmp(resp, "CADASTRO_REALIZADO") == 0) login_msg(a, "Cadastro realizado! Agora clique em Entrar.");
    else if (strcmp(resp, "EMAIL_JA_CADASTRADO") == 0) login_msg(a, "Esse e-mail já está cadastrado neste perfil.");
    else if (strcmp(resp, "DADOS_INVALIDOS") == 0) login_msg(a, "Preencha todos os campos.");
    else {
        g_autofree char *msg = g_strdup_printf("Resposta inesperada do servidor: %s", resp);
        login_msg(a, msg);
    }
}

static void ao_sair(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    fechar_conexao(a);
    a->logado = 0;
    memset(a->senha, 0, sizeof(a->senha));
    limpar_listas(a);
    gtk_stack_set_visible_child_name(GTK_STACK(a->pilha), "login");
    login_msg(a, "Você saiu da conta.");
}

/* ============================================================== cliente: busca e reserva */

/* Mensagem amigável para as respostas de texto do servidor ao confirmar itinerário */
static const char *msg_itinerario(const char *resp) {
    if (strcmp(resp, "ASSENTO_INDISPONIVEL") == 0) return "Um dos trechos ficou sem assento disponível. Nenhuma reserva foi feita.";
    if (strcmp(resp, "TRECHO_NAO_ENCONTRADO") == 0) return "Um dos trechos não existe mais (cancelado pelo motorista) ou os dados não conferem. Nenhuma reserva foi feita.";
    if (strcmp(resp, "RESERVA_DUPLICADA") == 0) return "Você já possui reserva em um dos trechos do itinerário. Nenhuma reserva foi feita.";
    if (strcmp(resp, "ITINERARIO_INVALIDO") == 0) return "Itinerário inválido (os trechos não formam um caminho contínuo).";
    if (strcmp(resp, "FALHA_ITINERARIO") == 0) return "O servidor não conseguiu registrar o itinerário. Nenhuma reserva foi feita. Tente novamente.";
    return NULL;
}

/* Envia o itinerário (rota é consumida). Devolve 1 se foi confirmado */
static int enviar_itinerario(App *a, cJSON *rota, const char *origem, const char *destino) {
    cJSON *p = pedido_base(0, "confirmar_itinerario");
    cJSON_AddStringToObject(p, "emailCliente", a->email);
    cJSON_AddStringToObject(p, "nomeCliente", a->nome);
    cJSON_AddStringToObject(p, "origemRota", origem);
    cJSON_AddStringToObject(p, "destinoRota", destino);
    cJSON_AddItemToObject(p, "rota", rota);

    g_autofree char *resp = chamar(a, p);
    if (!resp) return 0;

    cJSON *j = (resp[0] == '{') ? cJSON_Parse(resp) : NULL;
    if (j != NULL) {
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "status"));
        if (st && strcmp(st, "ITINERARIO_CONFIRMADO") == 0) {
            int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "idItinerario"));
            g_autofree char *msg = g_strdup_printf("Itinerário %d confirmado com sucesso!\nTodos os trechos foram reservados.", id);
            cJSON_Delete(j);
            status_ops(a, "Itinerário confirmado.");
            aviso(a, GTK_MESSAGE_INFO, msg);
            return 1;
        }
        cJSON_Delete(j);
    }
    const char *amigavel = msg_itinerario(resp);
    g_autofree char *msg = amigavel ? g_strdup(amigavel) : g_strdup_printf("Resposta desconhecida do servidor: %s", resp);
    status_ops(a, "Reserva não realizada.");
    aviso(a, GTK_MESSAGE_WARNING, msg);
    return 0;
}

static void mostrar_resultado_busca(App *a, const char *resp, const char *vazio) {
    cJSON *arr = resp_array(a, resp);
    if (!arr) return;
    lista_preencher(&a->busca, arr, CHAVES_BUSCA, 0);
    int n = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    if (n == 0) {
        status_ops(a, vazio);
    } else {
        g_autofree char *msg = g_strdup_printf("%d trecho(s) encontrado(s). Clique em uma linha para escolher.", n);
        status_ops(a, msg);
    }
}

static void buscar_partida(App *a, const char *origem) {
    cJSON *p = pedido_base(0, "buscar_trechos_partida");
    cJSON_AddStringToObject(p, "origem", origem);
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    g_autofree char *vazio = g_strdup_printf("Nenhuma carona com vaga saindo de %s.", origem);
    mostrar_resultado_busca(a, resp, vazio);
}

static void ao_buscar_direta(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    g_autofree char *origem = limpo(a->c_origem);
    g_autofree char *destino = limpo(a->c_destino);
    g_autofree char *data = limpo(a->c_data);
    if (!*origem || !*destino) { aviso(a, GTK_MESSAGE_WARNING, "Informe a origem e o destino."); return; }
    if (*data && !data_valida(data)) {
        aviso(a, GTK_MESSAGE_WARNING, "Data incompleta ou inválida. Digite os 8 números (ddmmaaaa) ou deixe em branco para qualquer data.");
        return;
    }

    cJSON *p = pedido_base(0, "buscar_carona");
    cJSON_AddStringToObject(p, "origem", origem);
    cJSON_AddStringToObject(p, "destino", destino);
    /* o servidor só aceita "qualquer data" com o texto literal dd/mm/aaaa */
    cJSON_AddStringToObject(p, "data", *data ? data : "dd/mm/aaaa");
    cJSON_AddStringToObject(p, "hora", "");
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    mostrar_resultado_busca(a, resp,
        "Nenhuma carona direta. Use \"Ver trechos saindo da origem\" para montar uma rota com vários motoristas.");
}

static void ao_buscar_partida(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    g_autofree char *origem = limpo(a->c_origem);
    if (!*origem) { aviso(a, GTK_MESSAGE_WARNING, "Informe a origem."); return; }
    buscar_partida(a, origem);
}

/* Reserva o trecho selecionado como um itinerário de um único trecho */
static void ao_reservar_direto(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter it;
    if (!lista_selecionada(&a->busca, &it)) { aviso(a, GTK_MESSAGE_INFO, "Clique em um trecho da lista primeiro."); return; }

    g_autofree char *id_s = lista_texto(&a->busca, &it, 0);
    g_autofree char *mot = lista_texto(&a->busca, &it, 1);
    g_autofree char *o = lista_texto(&a->busca, &it, 2);
    g_autofree char *d = lista_texto(&a->busca, &it, 3);
    g_autofree char *data = lista_texto(&a->busca, &it, 4);
    g_autofree char *hora = lista_texto(&a->busca, &it, 5);
    g_autofree char *preco = lista_texto(&a->busca, &it, 7);

    g_autofree char *pergunta = g_strdup_printf("Reservar o trecho %s (%s → %s)\ncom %s em %s às %s por R$ %s?",
                                                id_s, o, d, mot, data, hora, preco);
    if (!confirmar(a, pergunta)) return;

    cJSON *rota = cJSON_CreateArray();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "idTrecho", atoi(id_s));
    cJSON_AddStringToObject(t, "origem", o);
    cJSON_AddStringToObject(t, "destino", d);
    cJSON_AddItemToArray(rota, t);

    if (enviar_itinerario(a, rota, o, d)) lista_limpar(&a->busca);
}

/* ---- rota em montagem (vários trechos) ---- */

static void atualizar_total_rota(App *a) {
    double total = 0;
    GtkTreeModel *m = GTK_TREE_MODEL(a->rota_cli.store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        g_autofree char *p = lista_texto(&a->rota_cli, &it, 6);
        total += g_ascii_strtod(p, NULL);
    }
    g_autofree char *txt = g_strdup_printf("Total da rota: R$ %.2f", total);
    gtk_label_set_text(GTK_LABEL(a->c_total), txt);
}

/* Devolve o iterador da última linha da rota. 0 se estiver vazia */
static int rota_ultima(App *a, GtkTreeIter *it) {
    int n = lista_tamanho(&a->rota_cli);
    return n > 0 && gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(a->rota_cli.store), it, NULL, n - 1);
}

static void ao_add_rota(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter sel;
    if (!lista_selecionada(&a->busca, &sel)) { aviso(a, GTK_MESSAGE_INFO, "Clique em um trecho da lista primeiro."); return; }

    g_autofree char *id_s = lista_texto(&a->busca, &sel, 0);
    g_autofree char *mot = lista_texto(&a->busca, &sel, 1);
    g_autofree char *o = lista_texto(&a->busca, &sel, 2);
    g_autofree char *d = lista_texto(&a->busca, &sel, 3);
    g_autofree char *data = lista_texto(&a->busca, &sel, 4);
    g_autofree char *hora = lista_texto(&a->busca, &sel, 5);
    g_autofree char *preco = lista_texto(&a->busca, &sel, 7);

    /* o trecho precisa continuar de onde o anterior parou, e não pode se repetir */
    GtkTreeModel *m = GTK_TREE_MODEL(a->rota_cli.store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        g_autofree char *outro = lista_texto(&a->rota_cli, &it, 0);
        if (strcmp(outro, id_s) == 0) { aviso(a, GTK_MESSAGE_WARNING, "Esse trecho já está na sua rota."); return; }
    }
    GtkTreeIter ult;
    if (rota_ultima(a, &ult)) {
        g_autofree char *ult_dest = lista_texto(&a->rota_cli, &ult, 3);
        if (strcmp(ult_dest, o) != 0) {
            g_autofree char *msg = g_strdup_printf("Esse trecho sai de %s, mas sua rota está em %s.\n"
                "Escolha um trecho que saia de %s.", o, ult_dest, ult_dest);
            aviso(a, GTK_MESSAGE_WARNING, msg);
            return;
        }
    }

    GtkTreeIter novo;
    gtk_list_store_append(a->rota_cli.store, &novo);
    gtk_list_store_set(a->rota_cli.store, &novo, 0, id_s, 1, mot, 2, o, 3, d, 4, data, 5, hora, 6, preco, -1);
    atualizar_total_rota(a);

    /* continua a busca a partir do destino do trecho, a menos que já tenha chegado */
    g_autofree char *destino_final = limpo(a->c_destino);
    gtk_entry_set_text(GTK_ENTRY(a->c_origem), d);
    if (*destino_final && strcmp(destino_final, d) == 0) {
        lista_limpar(&a->busca);
        status_ops(a, "Você chegou ao destino! Clique em \"Confirmar rota\" para reservar todos os trechos.");
    } else {
        buscar_partida(a, d);
    }
}

static void ao_remover_ultimo(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter ult;
    if (!rota_ultima(a, &ult)) return;
    gtk_list_store_remove(a->rota_cli.store, &ult);
    atualizar_total_rota(a);
    if (rota_ultima(a, &ult)) {
        g_autofree char *d = lista_texto(&a->rota_cli, &ult, 3);
        gtk_entry_set_text(GTK_ENTRY(a->c_origem), d);
    }
}

static void ao_limpar_rota(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    lista_limpar(&a->rota_cli);
    atualizar_total_rota(a);
}

static void ao_confirmar_rota(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    int n = lista_tamanho(&a->rota_cli);
    if (n == 0) { aviso(a, GTK_MESSAGE_INFO, "Sua rota está vazia. Adicione trechos primeiro."); return; }

    cJSON *rota = cJSON_CreateArray();
    char *origem = NULL, *destino = NULL;
    GtkTreeModel *m = GTK_TREE_MODEL(a->rota_cli.store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        g_autofree char *id_s = lista_texto(&a->rota_cli, &it, 0);
        char *o = lista_texto(&a->rota_cli, &it, 2);
        char *d = lista_texto(&a->rota_cli, &it, 3);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "idTrecho", atoi(id_s));
        cJSON_AddStringToObject(t, "origem", o);
        cJSON_AddStringToObject(t, "destino", d);
        cJSON_AddItemToArray(rota, t);
        if (!origem) origem = o; else g_free(o);
        g_free(destino);
        destino = d;
    }

    g_autofree char *total = g_strdup(gtk_label_get_text(GTK_LABEL(a->c_total)));
    g_autofree char *pergunta = g_strdup_printf("Confirmar a rota %s → %s com %d trecho(s)?\n%s\n\n"
        "Todos os trechos serão reservados de uma vez, ou nenhum.", origem, destino, n, total);
    int sim = confirmar(a, pergunta);

    int confirmado = 0;
    if (sim) confirmado = enviar_itinerario(a, rota, origem, destino);
    else cJSON_Delete(rota);
    g_free(origem);
    g_free(destino);

    if (confirmado) {
        lista_limpar(&a->rota_cli);
        lista_limpar(&a->busca);
        atualizar_total_rota(a);
    }
}

/* ---- minhas caronas e avisos ---- */

static void atualizar_minhas(App *a) {
    cJSON *p = pedido_base(0, "listar_caronas");
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddStringToObject(p, "email", a->email);
    cJSON_AddStringToObject(p, "senha", a->senha);
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    cJSON *arr = resp_array(a, resp);
    if (!arr) return;
    lista_preencher(&a->minhas, arr, CHAVES_RESERVA, 0);
    int n = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    status_ops(a, n == 0 ? "Você não tem caronas reservadas." : "Suas caronas reservadas foram atualizadas.");
}

static void ao_atualizar_minhas(GtkButton *b, gpointer dados) {
    (void)b;
    atualizar_minhas(dados);
}

static void ao_cancelar_reserva(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter it;
    if (!lista_selecionada(&a->minhas, &it)) { aviso(a, GTK_MESSAGE_INFO, "Clique em uma carona da lista primeiro."); return; }
    g_autofree char *id_s = lista_texto(&a->minhas, &it, 0);
    g_autofree char *o = lista_texto(&a->minhas, &it, 2);
    g_autofree char *d = lista_texto(&a->minhas, &it, 3);

    g_autofree char *pergunta = g_strdup_printf("Cancelar a reserva do trecho %s (%s → %s)?\n\n"
        "ATENÇÃO: se esse trecho fizer parte de um itinerário, TODO o itinerário será cancelado "
        "e os demais trechos dele serão liberados.", id_s, o, d);
    if (!confirmar(a, pergunta)) return;

    cJSON *p = pedido_base(0, "cancelar_carona");
    cJSON_AddStringToObject(p, "emailCliente", a->email);
    cJSON_AddStringToObject(p, "nomeCliente", a->nome);
    cJSON_AddNumberToObject(p, "idSelecionado", atoi(id_s));
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;

    if (strcmp(resp, "CARONA_CANCELADA") == 0) {
        aviso(a, GTK_MESSAGE_INFO, "Carona cancelada com sucesso!\nSe o trecho fazia parte de um itinerário, "
                                   "o itinerário inteiro foi cancelado.");
        atualizar_minhas(a);
    } else {
        g_autofree char *msg = g_strdup_printf("Carona não cancelada. Motivo: %s", resp);
        aviso(a, GTK_MESSAGE_WARNING, msg);
    }
}

static void ao_atualizar_avisos(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    cJSON *p = pedido_base(0, "listar_avisos");
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddStringToObject(p, "email", a->email);
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    cJSON *arr = resp_array(a, resp);
    if (!arr) return;
    /* o servidor entrega cada aviso uma única vez, então acumulamos na lista */
    lista_preencher(&a->avisos, arr, CHAVES_AVISO, 1);
    int n = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    g_autofree char *msg = g_strdup_printf("%d aviso(s) novo(s).", n);
    status_ops(a, msg);
}

static void ao_limpar_avisos(GtkButton *b, gpointer dados) {
    (void)b;
    lista_limpar(&((App *)dados)->avisos);
}

static void ao_trocar_aba_cliente(GtkNotebook *nb, GtkWidget *pagina, guint num, gpointer dados) {
    (void)nb; (void)pagina;
    App *a = dados;
    if (a->logado && num == 1) atualizar_minhas(a);   /* ao abrir "Minhas caronas" já carrega a lista */
}

static GtkWidget *montar_cliente(App *a) {
    a->nb_cliente = gtk_notebook_new();

    /* ---- aba 1: buscar e reservar ---- */
    GtkWidget *aba1 = aba_nova();
    GtkWidget *grade = grade_nova();
    a->c_origem = campo(grade, "Origem", 0, 0, 49, 16);
    a->c_destino = campo(grade, "Destino", 0, 2, 49, 16);
    a->c_data = campo(grade, "Data (opcional)", 0, 4, 10, 12);
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->c_data), "dd/mm/aaaa");
    aplicar_mascara(a->c_data, &MASCARA_DATA);
    gtk_box_pack_start(GTK_BOX(aba1), grade, FALSE, FALSE, 0);

    GtkWidget *bb = linha_h();
    gtk_box_pack_start(GTK_BOX(bb), botao("Buscar carona direta", G_CALLBACK(ao_buscar_direta), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb), botao("Ver trechos saindo da origem", G_CALLBACK(ao_buscar_partida), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba1), bb, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(aba1), rotulo("Trechos encontrados (clique em uma linha para escolher):"), FALSE, FALSE, 0);
    lista_criar(&a->busca, TITULOS_BUSCA, 8, 130);
    gtk_widget_set_vexpand(a->busca.scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(aba1), a->busca.scroll, TRUE, TRUE, 0);

    GtkWidget *bb2 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb2), botao("Reservar trecho selecionado", G_CALLBACK(ao_reservar_direto), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb2), botao("Adicionar à rota (vários trechos)", G_CALLBACK(ao_add_rota), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba1), bb2, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(aba1), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(aba1), rotulo("Rota em montagem (nada é reservado até você confirmar):"), FALSE, FALSE, 0);
    static const char *titulos_rota[] = {"ID", "Motorista", "Origem", "Destino", "Data", "Hora", "Preço"};
    lista_criar(&a->rota_cli, titulos_rota, 7, 90);
    gtk_box_pack_start(GTK_BOX(aba1), a->rota_cli.scroll, FALSE, FALSE, 0);

    GtkWidget *bb3 = linha_h();
    a->c_total = gtk_label_new("Total da rota: R$ 0.00");
    gtk_box_pack_start(GTK_BOX(bb3), a->c_total, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb3), botao("Confirmar rota", G_CALLBACK(ao_confirmar_rota), a), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb3), botao("Limpar rota", G_CALLBACK(ao_limpar_rota), a), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb3), botao("Remover último", G_CALLBACK(ao_remover_ultimo), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba1), bb3, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_cliente), aba1, gtk_label_new("Buscar e reservar"));

    /* ---- aba 2: minhas caronas ---- */
    GtkWidget *aba2 = aba_nova();
    lista_criar(&a->minhas, TITULOS_RESERVA, 7, 200);
    gtk_widget_set_vexpand(a->minhas.scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(aba2), rotulo("Caronas que você reservou (clique para selecionar):"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), a->minhas.scroll, TRUE, TRUE, 0);
    GtkWidget *bb4 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb4), botao("Atualizar", G_CALLBACK(ao_atualizar_minhas), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb4), botao("Cancelar reserva selecionada", G_CALLBACK(ao_cancelar_reserva), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), bb4, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_cliente), aba2, gtk_label_new("Minhas caronas"));

    /* ---- aba 3: avisos ---- */
    GtkWidget *aba3 = aba_nova();
    static const char *titulos_aviso[] = {"Motorista que cancelou", "Origem", "Destino", "Data", "Hora"};
    lista_criar(&a->avisos, titulos_aviso, 5, 200);
    gtk_widget_set_vexpand(a->avisos.scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(aba3), rotulo("Avisos de trechos cancelados pelo motorista "
        "(cada aviso é entregue uma única vez; a lista guarda os desta sessão):"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba3), a->avisos.scroll, TRUE, TRUE, 0);
    GtkWidget *bb5 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb5), botao("Buscar avisos novos", G_CALLBACK(ao_atualizar_avisos), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb5), botao("Limpar lista", G_CALLBACK(ao_limpar_avisos), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba3), bb5, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_cliente), aba3, gtk_label_new("Avisos"));

    g_signal_connect(a->nb_cliente, "switch-page", G_CALLBACK(ao_trocar_aba_cliente), a);
    return a->nb_cliente;
}

/* ============================================================== motorista */

static GtkWidget *form_criar(FormTrecho *f) {
    GtkWidget *g = grade_nova();
    f->origem = campo(g, "Origem", 0, 0, 49, 18);
    f->destino = campo(g, "Destino", 0, 2, 49, 18);
    f->data = campo(g, "Data", 1, 0, 10, 18);
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->data), "dd/mm/aaaa");
    aplicar_mascara(f->data, &MASCARA_DATA);
    f->hora = campo(g, "Hora", 1, 2, 5, 18);
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->hora), "hh:mm");
    aplicar_mascara(f->hora, &MASCARA_HORA);
    f->cap = campo(g, "Capacidade", 2, 0, 3, 18);
    f->preco = campo(g, "Preço (R$)", 2, 2, 12, 18);
    return g;
}

static void ao_cadastrar_trecho(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    DadosTrecho t;
    if (!ler_form(a, &a->f_unico, &t)) return;

    cJSON *p = pedido_base(1, "cadastrar_trecho");
    cJSON_AddStringToObject(p, "emailMotorista", a->email);
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddStringToObject(p, "origem", t.origem);
    cJSON_AddStringToObject(p, "destino", t.destino);
    cJSON_AddStringToObject(p, "data", t.data);
    cJSON_AddStringToObject(p, "hora", t.hora);
    cJSON_AddNumberToObject(p, "capacidade", t.cap);
    cJSON_AddNumberToObject(p, "preco", t.preco);
    cJSON_AddItemToObject(p, "clientes", cJSON_CreateArray());
    cJSON_AddItemToObject(p, "nomesClientes", cJSON_CreateArray());
    dados_liberar(&t);

    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    if (strcmp(resp, "TRECHO_CADASTRADO") == 0) {
        status_ops(a, "Trecho cadastrado.");
        aviso(a, GTK_MESSAGE_INFO, "Trecho cadastrado com sucesso!");
    } else if (strcmp(resp, "FALHA_CADASTRO_TRECHO") == 0) {
        aviso(a, GTK_MESSAGE_WARNING, "Falha ao cadastrar o trecho: não há caminho possível entre essas cidades no mapa "
                                      "(confira os nomes) ou o servidor não conseguiu gravar.");
    } else {
        g_autofree char *msg = g_strdup_printf("Resposta do servidor: %s", resp);
        aviso(a, GTK_MESSAGE_WARNING, msg);
    }
}

/* ---- rota do motorista (vários trechos conectados) ---- */

static int rota_mot_ultima(App *a, GtkTreeIter *it) {
    int n = lista_tamanho(&a->rota_mot);
    return n > 0 && gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(a->rota_mot.store), it, NULL, n - 1);
}

/* A origem do próximo trecho é o destino do anterior; só é editável com a rota vazia */
static void rota_mot_sincronizar(App *a) {
    GtkTreeIter ult;
    if (rota_mot_ultima(a, &ult)) {
        g_autofree char *d = lista_texto(&a->rota_mot, &ult, 1);
        gtk_entry_set_text(GTK_ENTRY(a->f_rota.origem), d);
        gtk_editable_set_editable(GTK_EDITABLE(a->f_rota.origem), FALSE);
    } else {
        gtk_editable_set_editable(GTK_EDITABLE(a->f_rota.origem), TRUE);
    }
}

static void ao_rota_add(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    DadosTrecho t;
    if (!ler_form(a, &a->f_rota, &t)) return;

    GtkTreeIter ult;
    if (rota_mot_ultima(a, &ult)) {
        g_autofree char *d = lista_texto(&a->rota_mot, &ult, 1);
        if (strcmp(d, t.origem) != 0) {
            aviso(a, GTK_MESSAGE_WARNING, "A origem deste trecho deve ser o destino do trecho anterior.");
            dados_liberar(&t);
            return;
        }
    }
    g_autofree char *cap_s = g_strdup_printf("%d", t.cap);
    g_autofree char *preco_s = g_strdup_printf("%.2f", t.preco);
    GtkTreeIter novo;
    gtk_list_store_append(a->rota_mot.store, &novo);
    gtk_list_store_set(a->rota_mot.store, &novo, 0, t.origem, 1, t.destino, 2, t.data, 3, t.hora,
                       4, cap_s, 5, preco_s, -1);
    dados_liberar(&t);

    rota_mot_sincronizar(a);
    gtk_entry_set_text(GTK_ENTRY(a->f_rota.destino), "");
    status_ops(a, "Trecho adicionado à rota (ainda não enviado).");
}

static void ao_rota_remover_ultimo(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter ult;
    if (!rota_mot_ultima(a, &ult)) return;
    gtk_list_store_remove(a->rota_mot.store, &ult);
    rota_mot_sincronizar(a);
}

static void ao_rota_limpar(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    lista_limpar(&a->rota_mot);
    rota_mot_sincronizar(a);
}

static void ao_rota_enviar(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    if (lista_tamanho(&a->rota_mot) == 0) { aviso(a, GTK_MESSAGE_INFO, "Adicione pelo menos um trecho à rota."); return; }

    cJSON *trechos = cJSON_CreateArray();
    GtkTreeModel *m = GTK_TREE_MODEL(a->rota_mot.store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        g_autofree char *o = lista_texto(&a->rota_mot, &it, 0);
        g_autofree char *d = lista_texto(&a->rota_mot, &it, 1);
        g_autofree char *data = lista_texto(&a->rota_mot, &it, 2);
        g_autofree char *hora = lista_texto(&a->rota_mot, &it, 3);
        g_autofree char *cap = lista_texto(&a->rota_mot, &it, 4);
        g_autofree char *preco = lista_texto(&a->rota_mot, &it, 5);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "origem", o);
        cJSON_AddStringToObject(t, "destino", d);
        cJSON_AddStringToObject(t, "nome", a->nome);
        cJSON_AddStringToObject(t, "emailMotorista", a->email);
        cJSON_AddStringToObject(t, "data", data);
        cJSON_AddStringToObject(t, "hora", hora);
        cJSON_AddNumberToObject(t, "capacidade", atoi(cap));
        cJSON_AddNumberToObject(t, "preco", g_ascii_strtod(preco, NULL));
        cJSON_AddItemToArray(trechos, t);
    }

    cJSON *p = pedido_base(1, "cadastrar_rota");
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddItemToObject(p, "trechos", trechos);

    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    if (strcmp(resp, "ROTA_CADASTRADA") == 0) {
        status_ops(a, "Rota cadastrada.");
        aviso(a, GTK_MESSAGE_INFO, "Rota cadastrada com sucesso!");
        ao_rota_limpar(NULL, a);
    } else if (strcmp(resp, "ROTA_DESCONECTADA") == 0) {
        aviso(a, GTK_MESSAGE_WARNING, "Os trechos não formam uma rota conectada.");
    } else if (strcmp(resp, "FALHA_CADASTRO_ROTA") == 0) {
        aviso(a, GTK_MESSAGE_WARNING, "Um ou mais trechos não têm caminho possível no mapa.");
    } else {
        g_autofree char *msg = g_strdup_printf("Resposta do servidor: %s", resp);
        aviso(a, GTK_MESSAGE_WARNING, msg);
    }
}

/* ---- meus trechos ---- */

static void atualizar_meus(App *a) {
    cJSON *p = pedido_base(1, "listar_trechos");
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddStringToObject(p, "email", a->email);
    cJSON_AddStringToObject(p, "senha", a->senha);
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    cJSON *arr = resp_array(a, resp);
    if (!arr) return;
    lista_preencher(&a->meus, arr, CHAVES_MEUS, 0);
    int n = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    status_ops(a, n == 0 ? "Você não tem trechos cadastrados." : "Seus trechos foram atualizados.");
}

static void ao_atualizar_meus(GtkButton *b, gpointer dados) {
    (void)b;
    atualizar_meus(dados);
}

static void ao_cancelar_trecho(GtkButton *b, gpointer dados) {
    (void)b;
    App *a = dados;
    GtkTreeIter it;
    if (!lista_selecionada(&a->meus, &it)) { aviso(a, GTK_MESSAGE_INFO, "Clique em um trecho da lista primeiro."); return; }
    g_autofree char *id_s = lista_texto(&a->meus, &it, 0);
    g_autofree char *o = lista_texto(&a->meus, &it, 1);
    g_autofree char *d = lista_texto(&a->meus, &it, 2);

    g_autofree char *pergunta = g_strdup_printf("Cancelar o trecho %s (%s → %s)?\n\n"
        "Os passageiros serão avisados e os itinerários que usam este trecho serão cancelados.", id_s, o, d);
    if (!confirmar(a, pergunta)) return;

    cJSON *p = pedido_base(1, "cancelar_trecho");
    cJSON_AddStringToObject(p, "nome", a->nome);
    cJSON_AddStringToObject(p, "email", a->email);
    cJSON_AddNumberToObject(p, "idSelecionado", atoi(id_s));
    g_autofree char *resp = chamar(a, p);
    if (!resp) return;
    if (strcmp(resp, "TRECHO_CANCELADO") == 0) {
        aviso(a, GTK_MESSAGE_INFO, "Trecho cancelado com sucesso!");
        atualizar_meus(a);
    } else {
        aviso(a, GTK_MESSAGE_WARNING, "Trecho não encontrado (ou não pertence a você).");
    }
}

static void ao_trocar_aba_motorista(GtkNotebook *nb, GtkWidget *pagina, guint num, gpointer dados) {
    (void)nb; (void)pagina;
    App *a = dados;
    if (a->logado && num == 2) atualizar_meus(a);   /* ao abrir "Meus trechos" já carrega a lista */
}

static GtkWidget *montar_motorista(App *a) {
    a->nb_motorista = gtk_notebook_new();

    /* ---- aba 1: trecho único ---- */
    GtkWidget *aba1 = aba_nova();
    gtk_box_pack_start(GTK_BOX(aba1), rotulo("Cadastre um trecho de carona. Use os nomes das cidades exatamente como estão no mapa."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba1), form_criar(&a->f_unico), FALSE, FALSE, 0);
    GtkWidget *bb = linha_h();
    gtk_box_pack_start(GTK_BOX(bb), botao("Cadastrar trecho", G_CALLBACK(ao_cadastrar_trecho), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba1), bb, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_motorista), aba1, gtk_label_new("Cadastrar trecho"));

    /* ---- aba 2: rota com vários trechos ---- */
    GtkWidget *aba2 = aba_nova();
    gtk_box_pack_start(GTK_BOX(aba2), rotulo("Monte uma rota: cada trecho começa onde o anterior terminou. "
        "Nada é enviado até você clicar em \"Enviar rota\"."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), form_criar(&a->f_rota), FALSE, FALSE, 0);
    GtkWidget *bb2 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb2), botao("Adicionar trecho à rota", G_CALLBACK(ao_rota_add), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), bb2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), rotulo("Trechos da rota:"), FALSE, FALSE, 0);
    static const char *titulos_rota[] = {"Origem", "Destino", "Data", "Hora", "Vagas", "Preço"};
    lista_criar(&a->rota_mot, titulos_rota, 6, 160);
    gtk_widget_set_vexpand(a->rota_mot.scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(aba2), a->rota_mot.scroll, TRUE, TRUE, 0);
    GtkWidget *bb3 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb3), botao("Enviar rota", G_CALLBACK(ao_rota_enviar), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb3), botao("Remover último", G_CALLBACK(ao_rota_remover_ultimo), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb3), botao("Limpar rota", G_CALLBACK(ao_rota_limpar), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba2), bb3, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_motorista), aba2, gtk_label_new("Cadastrar rota"));

    /* ---- aba 3: meus trechos ---- */
    GtkWidget *aba3 = aba_nova();
    static const char *titulos_meus[] = {"ID", "Origem", "Destino", "Data", "Hora", "Vagas", "Preço", "Passageiros"};
    lista_criar(&a->meus, titulos_meus, 8, 220);
    gtk_widget_set_vexpand(a->meus.scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(aba3), rotulo("Seus trechos (clique em uma linha para selecionar):"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba3), a->meus.scroll, TRUE, TRUE, 0);
    GtkWidget *bb4 = linha_h();
    gtk_box_pack_start(GTK_BOX(bb4), botao("Atualizar", G_CALLBACK(ao_atualizar_meus), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb4), botao("Cancelar trecho selecionado", G_CALLBACK(ao_cancelar_trecho), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(aba3), bb4, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->nb_motorista), aba3, gtk_label_new("Meus trechos"));

    g_signal_connect(a->nb_motorista, "switch-page", G_CALLBACK(ao_trocar_aba_motorista), a);
    return a->nb_motorista;
}

/* ============================================================== telas e janela */

static GtkWidget *montar_tela_login(App *a) {
    GtkWidget *centro = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(centro, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(centro, GTK_ALIGN_CENTER);

    GtkWidget *titulo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(titulo),
        "<span size='xx-large' weight='bold'>VaiJunto</span>\n"
        "Entre com sua conta ou cadastre-se");
    gtk_label_set_justify(GTK_LABEL(titulo), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(centro), titulo, FALSE, FALSE, 0);

    GtkWidget *g = grade_nova();
    gtk_grid_attach(GTK_GRID(g), rotulo("Perfil"), 0, 0, 1, 1);
    a->l_perfil = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->l_perfil), "Cliente (passageiro)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->l_perfil), "Motorista");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->l_perfil), 0);
    gtk_grid_attach(GTK_GRID(g), a->l_perfil, 1, 0, 1, 1);

    a->l_ip = campo(g, "IP do servidor", 1, 0, 100, 30);
    gtk_entry_set_text(GTK_ENTRY(a->l_ip), "127.0.0.1");
    a->l_nome = campo(g, "Nome (só no cadastro)", 2, 0, 49, 30);
    a->l_email = campo(g, "E-mail", 3, 0, 49, 30);
    a->l_senha = campo(g, "Senha", 4, 0, 49, 30);
    gtk_entry_set_visibility(GTK_ENTRY(a->l_senha), FALSE);
    gtk_box_pack_start(GTK_BOX(centro), g, FALSE, FALSE, 0);

    GtkWidget *bb = linha_h();
    gtk_box_set_homogeneous(GTK_BOX(bb), TRUE);
    gtk_box_pack_start(GTK_BOX(bb), botao("Entrar", G_CALLBACK(ao_entrar), a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bb), botao("Cadastrar", G_CALLBACK(ao_cadastrar), a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(centro), bb, FALSE, FALSE, 0);

    a->l_msg = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(a->l_msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->l_msg), 50);
    gtk_box_pack_start(GTK_BOX(centro), a->l_msg, FALSE, FALSE, 0);

    g_signal_connect(a->l_senha, "activate", G_CALLBACK(ao_enter_senha), a);   /* Enter na senha = Entrar */
    return centro;
}

static GtkWidget *montar_tela_ops(App *a) {
    GtkWidget *raiz = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(raiz), 10);

    GtkWidget *cab = linha_h();
    a->cab_usuario = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(cab), a->cab_usuario, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(cab), botao("Sair da conta", G_CALLBACK(ao_sair), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(raiz), cab, FALSE, FALSE, 0);

    a->pilha_perfil = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(a->pilha_perfil), montar_cliente(a), "cliente");
    gtk_stack_add_named(GTK_STACK(a->pilha_perfil), montar_motorista(a), "motorista");
    gtk_widget_set_vexpand(a->pilha_perfil, TRUE);
    gtk_box_pack_start(GTK_BOX(raiz), a->pilha_perfil, TRUE, TRUE, 0);

    a->barra = gtk_label_new("");
    gtk_widget_set_halign(a->barra, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(raiz), a->barra, FALSE, FALSE, 0);

    GtkWidget *exp = gtk_expander_new("Log de comunicação com o servidor");
    GtkWidget *rolagem = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(rolagem), 140);
    a->log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->log), TRUE);
    gtk_container_add(GTK_CONTAINER(rolagem), a->log);
    gtk_container_add(GTK_CONTAINER(exp), rolagem);
    gtk_box_pack_start(GTK_BOX(raiz), exp, FALSE, FALSE, 0);
    return raiz;
}

static void ao_fechar_janela(GtkWidget *w, gpointer dados) {
    (void)w;
    fechar_conexao((App *)dados);
    gtk_main_quit();
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);   /* servidor fechando a conexão não pode derrubar a interface */
    gtk_init(&argc, &argv);

    App *a = g_new0(App, 1);
    a->socket_servidor = -1;

    a->janela = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->janela), "VaiJunto — Caronas");
    gtk_window_set_default_size(GTK_WINDOW(a->janela), 1000, 780);
    g_signal_connect(a->janela, "destroy", G_CALLBACK(ao_fechar_janela), a);

    a->pilha = gtk_stack_new();
    gtk_container_add(GTK_CONTAINER(a->janela), a->pilha);

    /* a tela de operações é criada primeiro porque a tela de login usa o log e a barra de status */
    GtkWidget *ops = montar_tela_ops(a);
    GtkWidget *login = montar_tela_login(a);
    gtk_stack_add_named(GTK_STACK(a->pilha), login, "login");
    gtk_stack_add_named(GTK_STACK(a->pilha), ops, "ops");
    gtk_stack_set_visible_child_name(GTK_STACK(a->pilha), "login");

    gtk_widget_show_all(a->janela);
    gtk_stack_set_visible_child_name(GTK_STACK(a->pilha), "login");
    gtk_main();
    g_free(a);
    return 0;
}