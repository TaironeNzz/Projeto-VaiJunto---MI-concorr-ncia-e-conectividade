#ifndef formatos_h
#define formatos_h
//enum de status
typedef enum{
    DESCONECTADO,
    AUTENTICADO,
    EM_VIAGEM
} Status;
//strcut para ter uma lista de trecho
typedef struct lista_trechos {
    int ID_trecho_atual;
    struct lista_trechos *proximo_trecho;
} lista_trechos;
//struct para o cliente
typedef struct {
    int id;
    char nome[50];
    char email[50];
    char senha[20];
    Status status;
} Cliente;
//struct para o motorista
typedef struct {
    int id;
    char nome[50];
    char email[50];
    char senha[20];
    int reservas;
    lista_trechos *trechos;
    Status status;
} Motorista;

#endif