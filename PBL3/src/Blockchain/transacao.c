#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "transacao.h"


int gravar_u32_be(unsigned char *destino, size_t capacidade, uint32_t valor) {

    // Verificar se o destino é válido
    if (destino == NULL || capacidade < sizeof(uint32_t)) {
        return -1;
    }

    // Gravar os quatro bytes em big-endian
    destino[0] = (unsigned char)((valor >> 24) & 0xFF);
    destino[1] = (unsigned char)((valor >> 16) & 0xFF);
    destino[2] = (unsigned char)((valor >> 8) & 0xFF);
    destino[3] = (unsigned char)(valor & 0xFF);

    return 0;
}


int gravar_u64_be(unsigned char *destino, size_t capacidade, uint64_t valor) {

    // Verificar se o destino é válido
    if (destino == NULL || capacidade < sizeof(uint64_t)) {
        return -1;
    }

    // Gravar os oito bytes em big-endian
    destino[0] = (unsigned char)((valor >> 56) & 0xFF);
    destino[1] = (unsigned char)((valor >> 48) & 0xFF);
    destino[2] = (unsigned char)((valor >> 40) & 0xFF);
    destino[3] = (unsigned char)((valor >> 32) & 0xFF);
    destino[4] = (unsigned char)((valor >> 24) & 0xFF);
    destino[5] = (unsigned char)((valor >> 16) & 0xFF);
    destino[6] = (unsigned char)((valor >> 8) & 0xFF);
    destino[7] = (unsigned char)(valor & 0xFF);

    return 0;
}

// VALIDAÇÃO DE ENDEREÇO
static int endereco_valido(const char *endereco) {

    if (endereco == NULL) {
        return -1;
    }

    // Verificar o terminador
    if (endereco[TAM_ENDERECO_HEX] != '\0') {
        return -1;
    }

    // Verificar os caracteres hexadecimais
    for (size_t i = 0; i < TAM_ENDERECO_HEX; i++) {

        char c = endereco[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f'))) {

            return -1;
        }
    }

    return 0;
}

// VALIDAÇÃO DOS CAMPOS DA TRANSAÇÃO
int transacao_validar_campos(const Transacao *transacao) {

    if (transacao == NULL) {
        return -1;
    }

    // Verificar versão
    if (transacao->versao != TRANSACAO_VERSAO_ATUAL) {
        return -1;
    }

    // Chain ID zero é reservado
    if (transacao->chain_id == 0) {
        return -1;
    }

    // Por enquanto, somente transferências estão implementadas
    if (transacao->tipo != TOKEN_TRANSFERENCIA) {
        return -1;
    }

    // Verificar valor
    if (transacao->valor == 0) {
        return -1;
    }

    // Verificar endereços
    if (endereco_valido(transacao->remetente) != 0 ||
        endereco_valido(transacao->destinatario) != 0) {

        return -1;
    }

    // Calcular endereço correspondente à chave pública
    unsigned char hash[crypto_hash_sha256_BYTES];
    char endereco_calculado[TAM_ENDERECO];

    if (crypto_hash_sha256(
            hash,
            transacao->chave_publica,
            crypto_sign_PUBLICKEYBYTES
        ) != 0) {

        return -1;
    }

    if (sodium_bin2hex(
            endereco_calculado,
            sizeof(endereco_calculado),
            hash,
            sizeof(hash)
        ) == NULL) {

        sodium_memzero(hash, sizeof(hash));
        return -1;
    }

    sodium_memzero(hash, sizeof(hash));

    // Verificar se a chave pública pertence ao remetente
    if (strcmp(
            endereco_calculado,
            transacao->remetente
        ) != 0) {

        return -1;
    }

    return 0;
}

// SERIALIZAÇÃO PARA ASSINATURA
int transacao_serializar_assinatura(const Transacao *transacao, unsigned char *destino, size_t capacidade, size_t *bytes_serializados) {

    // Verificar ponteiro do tamanho produzido
    if (bytes_serializados == NULL) {
        return -1;
    }

    *bytes_serializados = 0;

    // Verificar parâmetros
    if (transacao == NULL || destino == NULL) {
        return -1;
    }

    // Verificar capacidade
    if (capacidade < TRANSACAO_SERIALIZADA_BYTES) {
        return -1;
    }

    // Validar campos
    if (transacao_validar_campos(transacao) != 0) {
        return -1;
    }

    size_t offset = 0;


    // 1. Identificação do protocolo
    memcpy(
        destino + offset,
        TRANSACAO_DOMINIO,
        TRANSACAO_DOMINIO_TAMANHO
    );

    offset += TRANSACAO_DOMINIO_TAMANHO;


    // 2. Versão
    if (gravar_u32_be(
            destino + offset,
            capacidade - offset,
            transacao->versao
        ) != 0) {

        return -1;
    }

    offset += sizeof(uint32_t);


    // 3. Chain ID
    if (gravar_u32_be(
            destino + offset,
            capacidade - offset,
            transacao->chain_id
        ) != 0) {

        return -1;
    }

    offset += sizeof(uint32_t);


    // 4. Tipo da transação
    if (gravar_u32_be(
            destino + offset,
            capacidade - offset,
            (uint32_t)transacao->tipo
        ) != 0) {

        return -1;
    }

    offset += sizeof(uint32_t);


    // 5. Remetente
    memcpy(
        destino + offset,
        transacao->remetente,
        TAM_ENDERECO_HEX
    );

    offset += TAM_ENDERECO_HEX;


    // 6. Destinatário
    memcpy(
        destino + offset,
        transacao->destinatario,
        TAM_ENDERECO_HEX
    );

    offset += TAM_ENDERECO_HEX;


    // 7. Valor
    if (gravar_u64_be(
            destino + offset,
            capacidade - offset,
            transacao->valor
        ) != 0) {

        return -1;
    }

    offset += sizeof(uint64_t);


    // 8. Nonce
    if (gravar_u64_be(
            destino + offset,
            capacidade - offset,
            transacao->nonce
        ) != 0) {

        return -1;
    }

    offset += sizeof(uint64_t);


    // 9. Chave pública
    memcpy(
        destino + offset,
        transacao->chave_publica,
        crypto_sign_PUBLICKEYBYTES
    );

    offset += crypto_sign_PUBLICKEYBYTES;


    // Conferir tamanho final
    if (offset != TRANSACAO_SERIALIZADA_BYTES) {
        return -1;
    }

    *bytes_serializados = offset;

    return 0;
}

//Assinatura da transação
int transacao_assinar(Transacao *transacao, const unsigned char *chave_publica, size_t tamanho_chave_publica, const unsigned char *chave_privada, size_t tamanho_chave_privada) {

    // Verificar parâmetros
    if (transacao == NULL || chave_publica == NULL || chave_privada == NULL) {
        return -1;
    }

    // Verificar tamanho das chaves
    if (tamanho_chave_publica != crypto_sign_PUBLICKEYBYTES ||
        tamanho_chave_privada != crypto_sign_SECRETKEYBYTES) {

        return -1;
    }

    // Copiar a chave pública para a transação
    memcpy(transacao->chave_publica, chave_publica, crypto_sign_PUBLICKEYBYTES);

    // Serializar a transação para assinatura
    unsigned char buffer[TRANSACAO_SERIALIZADA_BYTES];
    size_t bytes_serializados = 0;

    if (transacao_serializar_assinatura(
            transacao,
            buffer,
            sizeof(buffer),
            &bytes_serializados
        ) != 0) {

        return -1;
    }

    // Assinar a transação
    unsigned long long assinatura_tamanho = 0;

    if (crypto_sign_detached(
            transacao->assinatura,
            &assinatura_tamanho,
            buffer,
            bytes_serializados,
            chave_privada
        ) != 0) {

        return -1;
    }

    // Conferir tamanho da assinatura
    if (assinatura_tamanho != crypto_sign_BYTES) {
        return -1;
    }

    if (crypto_sign_verify_detached(transacao->assinatura, buffer, bytes_serializados, chave_publica) != 0) {
        return -1;
    }

    // Calcular o ID da transação
    if (transacao_calcular_id(transacao, transacao->id, sizeof(transacao->id)) != 0) {
        return -1;
    }

    return 0;

}

int transacao_calcular_id(const Transacao *transacao, char *id_saida, size_t capacidade){
    if (transacao == NULL || id_saida == NULL) {
        return -1;
    }

    if (capacidade < TAM_ENDERECO) {
        return -1;
    }

    unsigned char hash[crypto_hash_sha256_BYTES];

    unsigned char buffer[TRANSACAO_SERIALIZADA_COM_ASSINATURA_BYTES];
    size_t bytes_serializados = 0;

    if (transacao_serializar_assinatura(transacao, buffer, sizeof(buffer), &bytes_serializados) != 0) {
        return -1;
    }

    if (bytes_serializados != TRANSACAO_SERIALIZADA_BYTES) {
        return -1;
    }

    // Acrescentar assinatura após os dados serializados
    memcpy(buffer + bytes_serializados, transacao->assinatura, crypto_sign_BYTES);

    bytes_serializados += crypto_sign_BYTES;

    // Calcular SHA-256 dos 264 bytes
    if (crypto_hash_sha256(hash, buffer, bytes_serializados) != 0) {
        sodium_memzero(buffer, sizeof(buffer));
        sodium_memzero(hash, sizeof(hash));
        return -1;
    }

    // Converter para hexadecimal
    if (sodium_bin2hex(id_saida, capacidade, hash, sizeof(hash)) == NULL) {
        sodium_memzero(hash, sizeof(hash));
        return -1;
    }

    return 0;
}

int transacao_verificar(const Transacao *transacao, uint32_t chain_id_esperado){
    if (transacao == NULL || chain_id_esperado == 0) {
        return -1;
    }


    // Verificar se pertence à rede esperada.
    if (transacao->chain_id != chain_id_esperado) {
        return -1;
    }


    // Validar campos.
    if (transacao_validar_campos(transacao) != 0) {
        return -1;
    }


    // Verificar formato do ID.

    if (endereco_valido(transacao->id) != 0) {
        return -1;
    }


    // Serializar novamente os dados.
    unsigned char buffer[TRANSACAO_SERIALIZADA_BYTES];

    size_t tamanho = 0;

    if (transacao_serializar_assinatura(
            transacao,
            buffer,
            sizeof(buffer),
            &tamanho
        ) != 0) {

        return -1;
    }


    // Verificar assinatura digital.
    if (crypto_sign_verify_detached(
            transacao->assinatura,
            buffer,
            tamanho,
            transacao->chave_publica
        ) != 0) {

        sodium_memzero(buffer, sizeof(buffer));
        return -1;
    }

    // Recalcular ID.
    char id_calculado[TAM_ENDERECO];

    if (transacao_calcular_id(
            transacao,
            id_calculado,
            sizeof(id_calculado)
        ) != 0) {

        sodium_memzero(buffer, sizeof(buffer));
        return -1;
    }


    // Comparar ID calculado com o recebido.
    if (sodium_memcmp(
            id_calculado,
            transacao->id,
            TAM_ENDERECO
        ) != 0) {

        sodium_memzero(buffer, sizeof(buffer));
        sodium_memzero(id_calculado, sizeof(id_calculado));

        return -1;
    }


    sodium_memzero(buffer, sizeof(buffer));
    sodium_memzero(id_calculado, sizeof(id_calculado));

    return 0;
}