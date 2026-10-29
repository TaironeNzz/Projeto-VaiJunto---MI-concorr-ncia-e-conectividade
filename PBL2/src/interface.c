/*
 * Interface gráfica (GTK3) que unifica o cliente (passageiro) e o motorista.
 * Fala com o servidor da própria empresa pelo mesmo protocolo TCP/JSON dos
 * clientes de terminal (porta 65432).
 *
 * Compilação: veja o Makefile (usa -I cJSON, então o include abaixo é "cJSON.h").
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

#define PORTA_PADRAO 65432
#define TAM_BUFFER   65536   /* tamanho máximo de uma resposta do servidor */
#define TIMEOUT_SEG  8       /* tempo máximo esperando o servidor (evita travar a janela) */

typedef struct {
    GtkWidget *janela, *modo, *acao, *ip, *nome, *email, *senha;
    GtkWidget *origem, *destino, *data, *hora, *capacidade, *preco, *id;
    GtkWidget *resposta, *status;
    int  socket_servidor;
    char ip_conectado[256];
} App;

/* ---------------------------------------------------------------- helpers de UI */

static const char *texto(GtkWidget *w) {
    return gtk_entry_get_text(GTK_ENTRY(w));
}

/* Substitui todo o conteúdo da caixa de resposta (garante UTF-8 válido para o GTK) */
static void mostrar(App *a, const char *s) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->resposta));
    char *ok = g_utf8_make_valid(s ? s : "(resposta vazia)", -1);
    gtk_text_buffer_set_text(b, ok, -1);
    g_free(ok);
}

/* Acrescenta texto ao final da caixa de resposta */
static void acrescentar(App *a, const char *s) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->resposta));
    GtkTextIter fim;
    gtk_text_buffer_get_end_iter(b, &fim);
    char *ok = g_utf8_make_valid(s ? s : "", -1);
    gtk_text_buffer_insert(b, &fim, ok, -1);
    g_free(ok);
}

static void status(App *a, const char *s) {
    gtk_label_set_text(GTK_LABEL(a->status), s);
}

/* ---------------------------------------------------------------- rede */

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
   simples (ex.: CARONA_RESERVADA) são aceitas no primeiro pacote. */
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

/* Envia o pedido e devolve a resposta (string alocada com g_malloc; quem chama usa g_free).
   *erro = 1 se não houve resposta utilizável. Reaproveita a conexão e, se ela estiver
   velha (servidor reiniciou), reconecta e reenvia uma vez. */
static char *enviar_receber(App *a, cJSON *pedido, int *erro) {
    *erro = 1;
    const char *ip = texto(a->ip);
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

/* ---------------------------------------------------------------- montagem dos pedidos */

static void add_str(cJSON *o, const char *chave, const char *valor) {
    cJSON_AddStringToObject(o, chave, valor ? valor : "");
}

static void preencher_operacoes(App *a, const char *modo) {
    GtkComboBoxText *cb = GTK_COMBO_BOX_TEXT(a->acao);
    gtk_combo_box_text_remove_all(cb);

    const char *motorista[] = {
        "Login", "Cadastro", "Cadastrar trecho", "Listar meus trechos",
        "Cancelar trecho", "Cadastrar rota (um trecho)", NULL
    };
    const char *cliente[] = {
        "Login", "Cadastro", "Buscar carona direta", "Reservar carona (ID)",
        "Confirmar itinerário (um trecho)", "Listar minhas caronas",
        "Cancelar carona", "Listar avisos", "Buscar trechos por origem", NULL
    };
    const char **lista = (modo && strcmp(modo, "Motorista") == 0) ? motorista : cliente;
    for (int i = 0; lista[i] != NULL; i++) gtk_combo_box_text_append_text(cb, lista[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->acao), 0);
}

static void ao_mudar_perfil(GtkComboBox *box, gpointer dados) {
    App *a = dados;
    char *modo = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(box));
    preencher_operacoes(a, modo);
    g_free(modo);
}

static void alternar_perfil(GtkButton *btn, gpointer dados) {
    (void)btn;
    App *a = dados;
    int atual = gtk_combo_box_get_active(GTK_COMBO_BOX(a->modo));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->modo), atual == 0 ? 1 : 0);
}

/* Cria rótulo + campo de texto na grade */
static void campo(GtkWidget *grade, const char *rotulo, int linha, int coluna, GtkWidget **saida) {
    GtkWidget *l = gtk_label_new(rotulo);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(e), 20);
    gtk_grid_attach(GTK_GRID(grade), l, coluna, linha, 1, 1);
    gtk_grid_attach(GTK_GRID(grade), e, coluna + 1, linha, 1, 1);
    *saida = e;
}

/* Devolve uma cópia do pedido com a senha escondida, para exibir no log da janela */
static char *pedido_para_exibir(cJSON *p) {
    cJSON *copia = cJSON_Duplicate(p, 1);
    if (cJSON_GetObjectItemCaseSensitive(copia, "senha") != NULL)
        cJSON_ReplaceItemInObjectCaseSensitive(copia, "senha", cJSON_CreateString("***"));
    char *s = cJSON_Print(copia);
    cJSON_Delete(copia);
    return s;
}

/* Operações que precisam do campo "ID do trecho/carona" preenchido */
static int precisa_id(const char *acao) {
    return strcmp(acao, "Cancelar trecho") == 0 || strcmp(acao, "Reservar carona (ID)") == 0 ||
           strcmp(acao, "Confirmar itinerário (um trecho)") == 0 || strcmp(acao, "Cancelar carona") == 0;
}

static void ao_enviar(GtkButton *btn, gpointer dados) {
    (void)btn;
    App *a = dados;
    char *modo = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(a->modo));
    char *acao = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(a->acao));
    cJSON *p = NULL;
    char *resp = NULL, *exibir = NULL, *cabecalho = NULL;

    if (!modo || !acao) { mostrar(a, "Selecione o perfil e a operação."); goto fim; }
    if (precisa_id(acao) && *texto(a->id) == '\0') {
        mostrar(a, "Preencha o campo \"ID do trecho/carona\" para esta operação.");
        goto fim;
    }

    int motorista = strcmp(modo, "Motorista") == 0;
    const char *email = texto(a->email), *nome = texto(a->nome), *senha = texto(a->senha);
    int id = atoi(texto(a->id)), capacidade = atoi(texto(a->capacidade));
    double preco = g_ascii_strtod(texto(a->preco), NULL);

    p = cJSON_CreateObject();
    add_str(p, "classe", motorista ? "Motorista" : "Cliente");

    if (strcmp(acao, "Login") == 0 || strcmp(acao, "Cadastro") == 0) {
        add_str(p, "nome", nome); add_str(p, "email", email); add_str(p, "senha", senha); add_str(p, "status", "");
        add_str(p, "acao", strcmp(acao, "Login") == 0 ? "login" : "cadastro");

    } else if (motorista && strcmp(acao, "Cadastrar trecho") == 0) {
        add_str(p, "acao", "cadastrar_trecho"); add_str(p, "emailMotorista", email); add_str(p, "nome", nome);
        add_str(p, "origem", texto(a->origem)); add_str(p, "destino", texto(a->destino));
        add_str(p, "data", texto(a->data)); add_str(p, "hora", texto(a->hora));
        cJSON_AddNumberToObject(p, "capacidade", capacidade); cJSON_AddNumberToObject(p, "preco", preco);
        cJSON_AddItemToObject(p, "clientes", cJSON_CreateArray());
        cJSON_AddItemToObject(p, "nomesClientes", cJSON_CreateArray());

    } else if (motorista && strcmp(acao, "Listar meus trechos") == 0) {
        add_str(p, "acao", "listar_trechos"); add_str(p, "nome", nome); add_str(p, "email", email); add_str(p, "senha", senha);

    } else if (motorista && strcmp(acao, "Cancelar trecho") == 0) {
        add_str(p, "acao", "cancelar_trecho"); add_str(p, "nome", nome); add_str(p, "email", email);
        cJSON_AddNumberToObject(p, "idSelecionado", id);

    } else if (motorista && strcmp(acao, "Cadastrar rota (um trecho)") == 0) {
        add_str(p, "acao", "cadastrar_rota"); add_str(p, "nome", nome);
        cJSON *arr = cJSON_CreateArray(), *t = cJSON_CreateObject();
        add_str(t, "origem", texto(a->origem)); add_str(t, "destino", texto(a->destino));
        add_str(t, "nome", nome); add_str(t, "emailMotorista", email);
        add_str(t, "data", texto(a->data)); add_str(t, "hora", texto(a->hora));
        cJSON_AddNumberToObject(t, "capacidade", capacidade); cJSON_AddNumberToObject(t, "preco", preco);
        cJSON_AddItemToArray(arr, t); cJSON_AddItemToObject(p, "trechos", arr);

    } else if (!motorista && strcmp(acao, "Buscar carona direta") == 0) {
        /* o servidor só aceita "qualquer data" com o texto literal dd/mm/aaaa */
        const char *data = *texto(a->data) ? texto(a->data) : "dd/mm/aaaa";
        add_str(p, "acao", "buscar_carona"); add_str(p, "origem", texto(a->origem)); add_str(p, "destino", texto(a->destino));
        add_str(p, "data", data); add_str(p, "hora", texto(a->hora));

    } else if (!motorista && strcmp(acao, "Reservar carona (ID)") == 0) {
        add_str(p, "acao", "selecionar_carona"); add_str(p, "emailCliente", email); add_str(p, "nomeCliente", nome);
        add_str(p, "origem", texto(a->origem)); add_str(p, "destino", texto(a->destino));
        cJSON_AddNumberToObject(p, "idSelecionado", id);

    } else if (!motorista && strcmp(acao, "Confirmar itinerário (um trecho)") == 0) {
        add_str(p, "acao", "confirmar_itinerario"); add_str(p, "emailCliente", email); add_str(p, "nomeCliente", nome);
        add_str(p, "origemRota", texto(a->origem)); add_str(p, "destinoRota", texto(a->destino));
        cJSON *rota = cJSON_CreateArray(), *trecho = cJSON_CreateObject();
        cJSON_AddNumberToObject(trecho, "idTrecho", id);
        add_str(trecho, "origem", texto(a->origem)); add_str(trecho, "destino", texto(a->destino));
        cJSON_AddItemToArray(rota, trecho); cJSON_AddItemToObject(p, "rota", rota);

    } else if (!motorista && strcmp(acao, "Listar minhas caronas") == 0) {
        add_str(p, "acao", "listar_caronas"); add_str(p, "nome", nome); add_str(p, "email", email); add_str(p, "senha", senha);

    } else if (!motorista && strcmp(acao, "Cancelar carona") == 0) {
        add_str(p, "acao", "cancelar_carona"); add_str(p, "emailCliente", email); add_str(p, "nomeCliente", nome);
        cJSON_AddNumberToObject(p, "idSelecionado", id);

    } else if (!motorista && strcmp(acao, "Listar avisos") == 0) {
        add_str(p, "acao", "listar_avisos"); add_str(p, "nome", nome); add_str(p, "email", email);

    } else if (!motorista && strcmp(acao, "Buscar trechos por origem") == 0) {
        add_str(p, "acao", "buscar_trechos_partida"); add_str(p, "origem", texto(a->origem));

    } else {
        mostrar(a, "Operação não reconhecida para o perfil selecionado.");
        goto fim;
    }

    exibir = pedido_para_exibir(p);
    cabecalho = g_strdup_printf("Perfil: %s\nOperação: %s\nServidor: %s:%d\n\nRequisição enviada:\n%s\n\nResposta do servidor:\n",
                                modo, acao, texto(a->ip), PORTA_PADRAO, exibir ? exibir : "(erro)");
    mostrar(a, cabecalho);

    int erro = 0;
    resp = enviar_receber(a, p, &erro);
    acrescentar(a, resp ? resp : "(sem resposta)");

    /* Após o login, usa o nome devolvido pelo servidor para preencher o campo Nome */
    if (!erro && resp && strcmp(acao, "Login") == 0) {
        cJSON *obj = cJSON_Parse(resp);
        if (obj != NULL) {
            const char *nome_resp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "nome"));
            if (nome_resp && *nome_resp) gtk_entry_set_text(GTK_ENTRY(a->nome), nome_resp);
            cJSON_Delete(obj);
        }
    }
    status(a, erro ? "Falha na comunicação com o servidor." : "Resposta recebida; confira abaixo.");

fim:
    g_free(resp); g_free(cabecalho); free(exibir);
    cJSON_Delete(p);
    g_free(modo); g_free(acao);
}

static void ao_limpar(GtkButton *btn, gpointer dados) {
    (void)btn;
    mostrar((App *)dados, "");
}

static void ao_desconectar(GtkButton *btn, gpointer dados) {
    (void)btn;
    App *a = dados;
    fechar_conexao(a);
    mostrar(a, "Conexão TCP encerrada. A próxima operação abrirá uma nova conexão.");
    status(a, "Desconectado.");
}

static void ao_fechar_janela(GtkWidget *w, gpointer dados) {
    (void)w;
    fechar_conexao((App *)dados);
    gtk_main_quit();
}

/* ---------------------------------------------------------------- janela */

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);   /* servidor fechando a conexão não pode derrubar a interface */
    gtk_init(&argc, &argv);

    App a = {0};
    a.socket_servidor = -1;

    a.janela = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a.janela), "Sistema de Caronas — Cliente e Motorista");
    gtk_window_set_default_size(GTK_WINDOW(a.janela), 1000, 760);
    gtk_container_set_border_width(GTK_CONTAINER(a.janela), 16);
    g_signal_connect(a.janela, "destroy", G_CALLBACK(ao_fechar_janela), &a);

    GtkWidget *raiz = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(a.janela), raiz);

    GtkWidget *titulo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(titulo),
        "<span size='x-large' weight='bold'>Sistema de Caronas</span>\n"
        "Escolha o perfil, preencha os dados e envie uma operação ao servidor.");
    gtk_label_set_justify(GTK_LABEL(titulo), GTK_JUSTIFY_LEFT);
    gtk_widget_set_halign(titulo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(raiz), titulo, FALSE, FALSE, 0);

    GtkWidget *grade = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grade), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grade), 10);
    gtk_box_pack_start(GTK_BOX(raiz), grade, FALSE, FALSE, 0);

    GtkWidget *l_modo = gtk_label_new("Perfil");
    gtk_widget_set_halign(l_modo, GTK_ALIGN_START);
    a.modo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a.modo), "Cliente");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a.modo), "Motorista");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a.modo), 0);

    GtkWidget *l_acao = gtk_label_new("Operação");
    gtk_widget_set_halign(l_acao, GTK_ALIGN_START);
    a.acao = gtk_combo_box_text_new();

    gtk_grid_attach(GTK_GRID(grade), l_modo, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grade), a.modo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grade), l_acao, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grade), a.acao, 3, 0, 2, 1);

    campo(grade, "IP do servidor", 1, 0, &a.ip);
    gtk_entry_set_text(GTK_ENTRY(a.ip), "127.0.0.1");
    campo(grade, "Nome", 1, 2, &a.nome);
    campo(grade, "E-mail", 2, 0, &a.email);
    campo(grade, "Senha", 2, 2, &a.senha);
    gtk_entry_set_visibility(GTK_ENTRY(a.senha), FALSE);
    campo(grade, "Origem", 3, 0, &a.origem);
    campo(grade, "Destino", 3, 2, &a.destino);
    campo(grade, "Data (dd/mm/aaaa)", 4, 0, &a.data);
    campo(grade, "Hora (hh:mm)", 4, 2, &a.hora);
    campo(grade, "Capacidade", 5, 0, &a.capacidade);
    campo(grade, "Preço", 5, 2, &a.preco);
    campo(grade, "ID do trecho/carona", 6, 0, &a.id);

    GtkWidget *botoes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(raiz), botoes, FALSE, FALSE, 0);
    GtkWidget *btn_enviar      = gtk_button_new_with_label("Enviar operação");
    GtkWidget *btn_alternar    = gtk_button_new_with_label("Alternar Cliente/Motorista");
    GtkWidget *btn_limpar      = gtk_button_new_with_label("Limpar resposta");
    GtkWidget *btn_desconectar = gtk_button_new_with_label("Desconectar");
    gtk_box_pack_start(GTK_BOX(botoes), btn_enviar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(botoes), btn_alternar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(botoes), btn_limpar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(botoes), btn_desconectar, FALSE, FALSE, 0);

    a.status = gtk_label_new("Pronto. Porta TCP usada: 65432.");
    gtk_widget_set_halign(a.status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(raiz), a.status, FALSE, FALSE, 0);

    GtkWidget *l_resp = gtk_label_new("Resposta / log de comunicação");
    gtk_widget_set_halign(l_resp, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(raiz), l_resp, FALSE, FALSE, 0);

    GtkWidget *rolagem = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(rolagem, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rolagem), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    a.resposta = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a.resposta), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(a.resposta), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a.resposta), TRUE);
    gtk_container_add(GTK_CONTAINER(rolagem), a.resposta);
    gtk_box_pack_start(GTK_BOX(raiz), rolagem, TRUE, TRUE, 0);

    g_signal_connect(a.modo, "changed", G_CALLBACK(ao_mudar_perfil), &a);
    g_signal_connect(btn_enviar, "clicked", G_CALLBACK(ao_enviar), &a);
    g_signal_connect(btn_alternar, "clicked", G_CALLBACK(alternar_perfil), &a);
    g_signal_connect(btn_limpar, "clicked", G_CALLBACK(ao_limpar), &a);
    g_signal_connect(btn_desconectar, "clicked", G_CALLBACK(ao_desconectar), &a);

    ao_mudar_perfil(GTK_COMBO_BOX(a.modo), &a);   /* preenche a lista de operações inicial */
    gtk_widget_show_all(a.janela);
    gtk_main();
    return 0;
}