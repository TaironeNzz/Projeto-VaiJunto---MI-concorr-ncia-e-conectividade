#ifndef formatos_h
#define formatos_h

typedef enum{
    DESCONECTADO,
    AUTENTICADO,
    EM_VIAGEM
} Status_Cliente;

typedef struct{
    int ID_trecho_atual;
    lista_trechos *proximo_trecho;
} lista_trechos;

typedef struct {
    int id;
    char nome[30];
    Status_Cliente status;
} Cliente;

typedef struct {
    int id;
    char nome[30];
    int reservas;
    lista_trechos *trechos;
    Status_Cliente status;
} Motorista;

#endif