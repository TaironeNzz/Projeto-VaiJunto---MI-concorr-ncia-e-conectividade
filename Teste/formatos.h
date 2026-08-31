#ifndef formatos_h
#define formatos_h

typedef enum{
    DESCONECTADO,
    AUTENTICADO,
    EM_VIAGEM
} Status;

typedef struct lista_trechos {
    int ID_trecho_atual;
    struct lista_trechos *proximo_trecho;
} lista_trechos;

typedef struct {
    int id;
    char nome[50];
    char email[50];
    char senha[20];
    Status status;
} Cliente;

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