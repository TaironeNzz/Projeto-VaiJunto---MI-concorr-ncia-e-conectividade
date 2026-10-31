# VaiJunto - Sistema de Caronas Compartilhadas

## 1. Visão geral

O **VaiJunto** é um protótipo de sistema distribuído para caronas compartilhadas, desenvolvido em C sobre **sockets TCP/IP nativos**, com um **servidor central** e dois tipos de clientes:

- **Cliente Motorista**: publica trechos/rotas, consulta seus trechos e cancela trechos.
- **Cliente Passageiro**: autentica, procura caronas, reserva trechos, consulta reservas e cancela reservas.
- **Servidor**: mantém o estado compartilhado das caronas e reservas, processa requisições concorrentes e persiste os dados em arquivos.

A solução utiliza **JSON** como representação intermediária das mensagens, processado pela biblioteca **cJSON**, e utiliza `pthread` para atender vários clientes simultaneamente.

A porta TCP utilizada pelo programa é **65432**.

---

## 2. Introdução

O VaiJunto é um sistema de caronas compartilhadas desenvolvido em linguagem C, com o objetivo de conectar motoristas e passageiros de forma simples e organizada. Por meio do sistema, motoristas podem cadastrar seus trechos de viagem, informando origem, destino, data, horário, quantidade de assentos e preço, enquanto passageiros podem pesquisar caronas disponíveis e realizar suas reservas.

A aplicação funciona no modelo cliente-servidor, utilizando sockets TCP/IP para a comunicação entre os usuários e o servidor central. O servidor é responsável por armazenar e gerenciar as informações de usuários, trechos e reservas, além de controlar o acesso concorrente aos assentos disponíveis.

O projeto possui dois tipos de clientes: o cliente motorista, utilizado para cadastrar, consultar e cancelar trechos, e o cliente passageiro, utilizado para buscar, reservar, visualizar e cancelar caronas. Também é possível montar uma viagem utilizando mais de um trecho, permitindo representar trajetos maiores por meio da combinação de diferentes caronas.

Para facilitar a execução e a reprodução do sistema, o projeto utiliza Docker, permitindo que o servidor e os clientes sejam executados em ambientes separados. Dessa forma, o VaiJunto reúne conceitos de programação em rede, arquitetura cliente-servidor, persistência de dados e concorrência em uma aplicação distribuída

---

# 3. Arquitetura do sistema

```text
                         REDE TCP/IP
                              |
               +--------------+--------------+
               |                             |
        +------v------+               +------v------+
        |  Servidor   |               |  Firewall   |
        | TCP :65432  |               | TCP :65432  |
        +------+------+               +-------------+
               |
        +------+------------------------------------+
        |                                           |
 +------v------+                              +-----v------+
 | Motorista   |                              | Passageiro |
 |  cliente C  |                              | cliente C  |
 +-------------+                              +------------+

             Estado centralizado no servidor
             --------------------------------
             dados/
             trechosCadastrados/
             logs/
```

O servidor:

- cria um socket IPv4/TCP;
- usa `bind()` na porta 65432;
- usa `listen()`;
- aceita conexões com `accept()`;
- cria uma thread (`pthread`) por conexão;
- recebe mensagens;
- interpreta o campo `classe`;
- encaminha para o tratamento de `Cliente` ou `Motorista`;
- protege o estado compartilhado dos trechos com `pthread_mutex_t`.

O cliente não acessa os arquivos do servidor diretamente. Toda operação passa pelo socket TCP.

---

# 4. Estrutura de diretórios
```text
PBL/
├── dados/                    # Ficheiros de dados dos utilizadores (loginCliente.json, loginMotorista.json)
├── trechosCadastrados/        # Persistência dos trechos e reservas (trechos.json)
├── logs/                     # Registos de auditoria e eventos do servidor (servidor.log)
├── TestesConcorrência        # arquivos de testes para concorrência
├── cJSON.c                   # Implementação do parser JSON em C
├── cJSON.h                   # Cabeçalho da biblioteca cJSON
├── cliente.c                 # Aplicação de terminal para o passageiro
├── motorista.c               # Aplicação de terminal para o motorista
├── servidor.c                # Servidor central e gestão de concorrência
├── formatos.h                # Estruturas de dados (Cliente, Motorista, Status)
├── grafomapa.c               # Módulo do grafo para validação e rotas
├── grafomapa.h               # Interface do módulo do grafo
├── mapa.txt                  # Grafo com as cidades e ligações geográficas
├── DockerFile.Servidor       # Ficheiro de compilação Docker para o servidor
├── Dockerfile.Motorista      # Ficheiro de compilação Docker para o motorista
├── Dockerfile.Cliente        # Ficheiro de compilação Docker para o passageiro
```
### Função dos arquivos

| Arquivo | Função |
|---|---|
| `servidor.c` | Servidor central, sockets, concorrência, persistência e regras das caronas |
| `teste_concorrencia.c` | Testes para a concorrência|
| `motorista.c` | Aplicação de terminal do motorista |
| `cliente.c` | Aplicação de terminal do passageiro |
| `formatos.h` | Estruturas `Cliente`, `Motorista`, `lista_trechos` e enum `Status` |
| `cJSON.c` | Implementação da biblioteca JSON usada no projeto |
| `cJSON.h` | Cabeçalho da biblioteca cJSON |
| `grafomapa.c` | Implementação do grafo/mapa utilizado na validação das rotas |
| `grafomapa.h` | Interface do módulo do grafo |
| `mapa.txt` | Dados do mapa de cidades |
| `dados/` | Usuários persistidos |
| `trechosCadastrados/` | Trechos e reservas persistidos |
| `logs/` | Registro das operações do servidor |
| `Dockerfile.*` | Construção dos containers |

---

# 5. Dependências

## 5.1 Dependências do software

### Compilador

- GCC
- padrão C utilizado: C11/C compatível com o código atual

### Biblioteca de threads

- POSIX Threads (`pthread`)
- habilitada na compilação com `-pthread`

### Plataforma utilizada

- Plataforma de Contentores: Docker instalado para execução isolada em ambiente multi-computador.

### Rede

A implementação utiliza a API nativa de sockets:

```c
socket()
bind()
listen()
accept()
connect()
read()
write()
send()
close()
```

Bibliotecas principais:

```c
sys/types.h
sys/socket.h
netinet/in.h
arpa/inet.h
netdb.h
unistd.h
pthread.h
time.h
stdarg.h
```

### JSON

O projeto usa **cJSON** como representação intermediária.

A biblioteca está incorporada ao próprio projeto por meio de:

```text
cJSON.c
cJSON.h
```

Não é necessário um servidor de JSON ou framework externo.

A versão do código enviado corresponde ao cJSON **1.7.19**.

### Grafo/mapa

O servidor depende de:

```text
grafomapa.c
grafomapa.h
mapa.txt
```

O código utiliza o grafo para verificar se existe caminho entre cidades e validar as rotas.

---

# 6. Dependências instaladas pelo Docker

Os Dockerfiles utilizam uma imagem baseada em GCC para facilitar a compilação.

O servidor também instala:

- `tzdata`, para o tratamento correto de data/hora utilizado por `localtime()` e `mktime()`;
- `ca-certificates`, para manter o ambiente padrão do container.

A biblioteca cJSON continua sendo compilada a partir dos arquivos locais do projeto.

---

# 7. Modelo de dados

## 7.1 Cliente

Definido em `formatos.h`:

```c
typedef struct {
    int id;
    char nome[50];
    char email[50];
    char senha[20];
    Status status;
} Cliente;
```

Status:

```text
DESCONECTADO
AUTENTICADO
EM_VIAGEM
```

## 7.2 Motorista

```c
typedef struct {
    int id;
    char nome[50];
    char email[50];
    char senha[20];
    int reservas;
    lista_trechos *trechos;
    Status status;
} Motorista;
```

## 7.3 Trecho persistido

Os trechos são armazenados como **um objeto JSON por linha** em:

```text
trechosCadastrados/trechos.json
```

Formato conceitual:

```json
{
  "idTrecho": 0,
  "nomeMotorista": "Joao",
  "emailMotorista": "joao@email.com",
  "origem": "Salvador",
  "destino": "Feira de Santana",
  "data": "18/09/2026",
  "hora": "08:00",
  "capacidade": 3,
  "preco": 35.50,
  "clientes": [
    "ana@email.com"
  ]
}
```

A capacidade representa os assentos ainda disponíveis para aquele trecho.

---

# 8. Persistência

O servidor utiliza arquivos locais em vez de banco de dados.

## Usuários

```text
dados/loginCliente.json
dados/loginMotorista.json
```

Os registros são armazenados em JSON, um por linha.

## Trechos e reservas

```text
trechosCadastrados/trechos.json
```

Cada linha representa um trecho.

O array:

```json
"clientes": []
```

contém os emails dos passageiros que reservaram aquele trecho.

## Logs

```text
logs/servidor.log
```

O servidor registra operações de:

- login;
- cadastro;
- reserva;
- cancelamento;
- erros;
- conexões e desconexões.

---

# 9. Protocolo de rede

## 9.1 Camada de transporte

O transporte é feito por:

```text
IPv4
TCP
porta 65432
```

No servidor:

```c
socket(AF_INET, SOCK_STREAM, 0);
```

O servidor associa o socket a:

```text
INADDR_ANY:65432
```

e fica aguardando conexões.

O cliente recebe o IP do servidor no terminal e executa:

```c
gethostbyname()
connect()
```

---

# 10. Protocolo de aplicação

O protocolo definido pelo projeto usa:

1. conexão TCP persistente durante a sessão;
2. mensagens de requisição codificadas em JSON;
3. respostas que podem ser:
   - JSON;
   - textos de status.

A mensagem possui, como campos básicos:

```json
{
  "classe": "Cliente",
  "acao": "buscar_carona"
}
```

### Campo `classe`

Identifica quem enviou:

```text
Cliente
Motorista
```

### Campo `acao`

Identifica a operação desejada.

---

# 11. Fluxo da conexão

## Cliente

```text
socket()
   |
connect(IP_SERVIDOR, 65432)
   |
login/cadastro
   |
operações
   |
DESCONECTADO
   |
close()
```

## Servidor

```text
socket()
   |
bind()
   |
listen()
   |
accept()
   |
pthread_create()
   |
receber mensagem
   |
validar JSON
   |
classe = Cliente/Motorista
   |
executar operação
   |
enviar resposta
   |
voltar a aguardar mensagens
```

A conexão pode permanecer aberta enquanto o usuário navega pelo menu.

---

# 12. Framing atual das mensagens TCP

O código atual recebe bytes TCP em um buffer e vai acumulando os dados até que:

```c
cJSON_Parse(buffer_mensagem)
```

consiga interpretar um JSON completo.

Isso é importante porque **TCP não preserva fronteiras de mensagens**.

Uma mensagem pode chegar em partes:

```text
pacote 1 -> {"classe":"Cliente","acao":"selec
pacote 2 -> ionar_carona","idSelecionado":3,...}
```

O servidor acumula os bytes até conseguir interpretar o objeto JSON.

---

# 13. Protocolo de autenticação

## 13.1 Login do cliente

Requisição:

```json
{
  "classe": "Cliente",
  "nome": "",
  "email": "ana@email.com",
  "senha": "1234",
  "status": "",
  "acao": "login"
}
```

Resposta de sucesso:

```text
AUTENTICADO
```

Resposta de falha:

```text
NAO_AUTENTICADO
```

---

## 13.2 Cadastro do cliente

Requisição:

```json
{
  "classe": "Cliente",
  "nome": "Ana",
  "email": "ana@email.com",
  "senha": "1234",
  "status": "",
  "acao": "cadastro"
}
```

Respostas:

```text
CADASTRO_REALIZADO
EMAIL_JA_CADASTRADO
```

---

## 13.3 Login do motorista

Requisição:

```json
{
  "classe": "Motorista",
  "nome": "",
  "email": "joao@email.com",
  "senha": "1234",
  "status": "",
  "acao": "login"
}
```

Resposta de sucesso:

```json
{
  "nome": "Joao"
}
```

Resposta de falha:

```text
NAO_AUTENTICADO
```

---

# 14. Protocolo do motorista

## 14.1 Cadastrar trecho

Ação:

```text
cadastrar_trecho
```

Exemplo:

```json
{
  "classe": "Motorista",
  "acao": "cadastrar_trecho",
  "emailMotorista": "joao@email.com",
  "origem": "Salvador",
  "destino": "Feira de Santana",
  "data": "18/09/2026",
  "hora": "08:00",
  "capacidade": 3,
  "preco": 35.5,
  "nome": "Joao",
  "clientes": []
}
```

Resposta:

```text
TRECHO_CADASTRADO
```

ou:

```text
FALHA_CADASTRO_TRECHO
```

ou:

```text
MAPA_NAO_CARREGADO
```

---

## 14.2 Listar trechos

Ação:

```text
listar_trechos
```

Exemplo:

```json
{
  "classe": "Motorista",
  "nome": "Joao",
  "email": "joao@email.com",
  "senha": "1234",
  "acao": "listar_trechos"
}
```

Resposta:

```json
[
  {
    "id": 0,
    "origem": "Salvador",
    "destino": "Feira de Santana",
    "capacidade": 3,
    "data": "18/09/2026",
    "hora": "08:00",
    "preco": 35.5
  }
]
```

---

## 14.3 Cancelar trecho

Ação:

```text
cancelar_trecho
```

Exemplo:

```json
{
  "classe": "Motorista",
  "acao": "cancelar_trecho",
  "nome": "Joao",
  "email": "joao@email.com",
  "idSelecionado": 0
}
```

Resposta:

```text
TRECHO_CANCELADO
```

ou:

```text
TRECHO_NAO_ENCONTRADO
```

---

## 14.4 Cadastrar rota com vários trechos

Ação:

```text
cadastrar_rota
```

Exemplo:

```json
{
  "classe": "Motorista",
  "acao": "cadastrar_rota",
  "nome": "Joao",
  "trechos": [
    {
      "origem": "Salvador",
      "destino": "Feira de Santana",
      "nome": "Joao",
      "emailMotorista": "joao@email.com",
      "data": "18/09/2026",
      "hora": "08:00",
      "capacidade": 3,
      "preco": 35.5
    },
    {
      "origem": "Feira de Santana",
      "destino": "Serrinha",
      "nome": "Joao",
      "emailMotorista": "joao@email.com",
      "data": "18/09/2026",
      "hora": "09:00",
      "capacidade": 3,
      "preco": 25.0
    }
  ]
}
```

O servidor valida:

1. existência de origem;
2. existência de destino;
3. existência de caminho no mapa;
4. ligação entre os trechos consecutivos.

Respostas:

```text
ROTA_CADASTRADA
ROTA_INVALIDA
ROTA_DESCONECTADA
FALHA_CADASTRO_ROTA
MAPA_NAO_CARREGADO
```

---

# 15. Protocolo do passageiro

## 15.1 Buscar carona direta

Ação:

```text
buscar_carona
```

Exemplo:

```json
{
  "classe": "Cliente",
  "acao": "buscar_carona",
  "origem": "Salvador",
  "destino": "Feira de Santana",
  "data": "18/09/2026",
  "hora": "08:00"
}
```

Também é permitido utilizar:

```text
dd/mm/aaaa
```

no campo de data para procurar sem restringir a data.

Resposta:

```json
[
  {
    "id": 0,
    "nomeMotorista": "Joao",
    "origem": "Salvador",
    "destino": "Feira de Santana",
    "capacidade": 3,
    "data": "18/09/2026",
    "hora": "08:00",
    "preco": 35.5
  }
]
```

---

## 15.2 Selecionar e reservar carona

Ação:

```text
selecionar_carona
```

Exemplo:

```json
{
  "classe": "Cliente",
  "acao": "selecionar_carona",
  "emailCliente": "ana@email.com",
  "idSelecionado": 0,
  "origem": "Salvador",
  "destino": "Feira de Santana"
}
```

Resposta:

```text
CARONA_RESERVADA
```

ou:

```text
ASSENTO_INDISPONIVEL
```

ou:

```text
TRECHO_NAO_ENCONTRADO
```

A atualização de capacidade e o acréscimo do email do passageiro são realizados pelo servidor sob `trechosMutex`, evitando duas threads alterarem o mesmo trecho simultaneamente.

---

# 16. Reserva de itinerário com vários motoristas

A sequência utilizada pelo cliente é:

```text
buscar_carona
      |
      +-- existe carona direta?
             |
          SIM -> selecionar_carona
             |
            FIM

          NAO
             |
             v
    buscar_trechos_partida
             |
             v
    selecionar_trecho
             |
             v
       muda origem
             |
             +----> repetir
             |
             v
       finalizar_rota
```

## 16.1 Buscar trecho para continuação

Ação:

```text
buscar_trechos_partida
```

Exemplo:

```json
{
  "classe": "Cliente",
  "acao": "buscar_trechos_partida",
  "origem": "Feira de Santana"
}
```

O servidor retorna os trechos que partem daquela cidade.

---

## 16.2 Reservar trecho intermediário

Ação:

```text
selecionar_trecho
```

Exemplo:

```json
{
  "classe": "Cliente",
  "acao": "selecionar_trecho",
  "idSelecionado": 4,
  "emailCliente": "ana@email.com",
  "origem": "Feira de Santana",
  "destino": "Serrinha"
}
```

Resposta normalmente é o próprio trecho serializado em JSON.

---

## 16.3 Finalizar itinerário

Ação:

```text
finalizar_rota
```

Exemplo conceitual:

```json
{
  "classe": "Cliente",
  "email": "ana@email.com",
  "acao": "finalizar_rota",
  "rota": [
    {
      "id": 1,
      "origem": "Salvador",
      "destino": "Feira de Santana"
    },
    {
      "id": 4,
      "origem": "Feira de Santana",
      "destino": "Serrinha"
    }
  ],
  "origemRota": "Salvador",
  "destinoRota": "Serrinha"
}
```

Resposta:

```text
CARONA_CADASTRADA
```

ou:

```text
CARONA_NAO_CADASTRADA
```

Quando a finalização falha, o código tenta retirar as reservas já realizadas por meio de `cancelarReservaInterna()`.

---

# 17. Consultar reservas do passageiro

Ação:

```text
listar_caronas
```

Exemplo:

```json
{
  "classe": "Cliente",
  "nome": "Ana",
  "email": "ana@email.com",
  "senha": "1234",
  "acao": "listar_caronas"
}
```

O servidor percorre `clientes[]` de cada trecho e retorna os trechos contendo o email do passageiro.

---

# 18. Cancelar reserva do passageiro

Ação:

```text
cancelar_carona
```

Exemplo:

```json
{
  "classe": "Cliente",
  "acao": "cancelar_carona",
  "emailCliente": "ana@email.com",
  "idSelecionado": 0
}
```

Resposta:

```text
CARONA_CANCELADA
```

ou:

```text
TRECHO_NAO_ENCONTRADO
```

Ao cancelar a reserva, o servidor:

1. procura o trecho;
2. procura o email dentro de `clientes[]`;
3. remove o email;
4. incrementa novamente a capacidade;
5. grava o arquivo atualizado.

---

# 19. Mensagens de erro

Principais códigos usados pelo protocolo:

| Mensagem | Significado |
|---|---|
| `AUTENTICADO` | Login do passageiro aceito |
| `NAO_AUTENTICADO` | Credenciais inválidas |
| `CADASTRO_REALIZADO` | Cadastro concluído |
| `EMAIL_JA_CADASTRADO` | Email já existente |
| `ACAO_DESCONHECIDA` | Campo `acao` não reconhecido |
| `TRECHO_CADASTRADO` | Trecho criado |
| `TRECHO_CANCELADO` | Trecho cancelado |
| `TRECHO_NAO_ENCONTRADO` | Trecho/registro não encontrado |
| `ROTA_CADASTRADA` | Rota aceita |
| `ROTA_INVALIDA` | Estrutura da rota inválida |
| `ROTA_DESCONECTADA` | Trechos não são contínuos |
| `FALHA_CADASTRO_ROTA` | Rota não pôde ser cadastrada |
| `CARONA_RESERVADA` | Reserva confirmada |
| `ASSENTO_INDISPONIVEL` | Não há assento disponível |
| `CARONA_CANCELADA` | Reserva cancelada |
| `CARONA_NAO_CADASTRADA` | Itinerário não foi confirmado |
| `MAPA_NAO_CARREGADO` | Grafo não foi carregado |

---

# 20. Concorrência

O servidor utiliza `pthread` para criar uma thread de tratamento por conexão.

Mutexes utilizados:

```c
trechosMutex
loginMotoristaMutex
loginClienteMutex
logMutex
```

## `trechosMutex`

Protege operações de leitura/modificação/escrita dos trechos.

Exemplo de reserva:

```text
lock
  ler trechos
  localizar trecho
  verificar capacidade
  decrementar capacidade
  adicionar passageiro
  substituir arquivo
unlock
```

Isso reduz o risco de dois clientes confirmarem o mesmo assento simultaneamente.

## `loginClienteMutex`

Protege o arquivo:

```text
dados/loginCliente.json
```

## `loginMotoristaMutex`

Protege:

```text
dados/loginMotorista.json
```

## `logMutex`

Evita que múltiplas threads escrevam no arquivo de log simultaneamente.

---

# 21. Expiração automática

O servidor cria uma thread de limpeza periódica.

A rotina:

```text
rotinaLimpezaTrechos()
```

executa aproximadamente a cada:

```text
60 segundos
```

Ela verifica `data + hora` de cada trecho e remove os trechos expirados.

Isso impede que caronas antigas continuem aparecendo indefinidamente.

---

# 22. Docker

## 22.1 Imagem do servidor

Build:

```bash
docker build -t app-servidor -f DockerFile.Servidor .
```

Execução Linux/macOS:

```bash
docker run -d \
  --name servidor \
  -p 65432:65432 \
  -v "$(pwd)/dados:/app/dados" \
  -v "$(pwd)/trechosCadastrados:/app/trechosCadastrados" \
  -v "$(pwd)/logs:/app/logs" \
  app-servidor
```

---

## 22.2 Imagem do motorista

```bash
docker build -t app-motorista -f Dockerfile.Motorista .
```

Executar:

```bash
docker run --rm -it app-motorista
```

Quando o programa solicitar:

```text
Digite o IP do servidor:
```

informe o IPv4 da máquina que executa o servidor.

---

## 22.3 Imagem do passageiro

```bash
docker build -t app-cliente -f Dockerfile.Cliente .
```

Executar:

```bash
docker run --rm -it app-cliente
```

Informe o mesmo IP do servidor.

---

# 23. Execução em computadores distintos

Esta é a forma recomendada para a demonstração do laboratório.

## Computador 1 - Servidor

1. iniciar o Docker;
2. construir `vaijunto-servidor`;
3. executar o container;
4. identificar o IPv4 da máquina;
5. liberar a porta TCP 65432 no firewall.

Linux:

```bash
ip addr
```


Exemplo:

```text
192.168.0.100
```

O servidor deve estar acessível em:

```text
192.168.0.100:65432
```

## Computador 2 - Motorista

Executar o container do motorista e informar:

```text
192.168.0.100
```

## Computador 3 - Passageiro

Executar o container do passageiro e informar:

```text
192.168.0.100
```

Todos os computadores precisam estar na mesma rede roteável.

---

# 24. Firewall

O servidor precisa aceitar conexões TCP na porta:

```text
65432
```

No Linux com `ufw`, por exemplo:

```bash
sudo ufw allow 65432/tcp
```

A configuração exata depende do sistema operacional e da política do laboratório.

---

# 25. Manual de uso - motorista

## Primeiro acesso

Menu inicial:

```text
1- LOGIN
2- CADASTRAR
3- SAIR
```

### Cadastro

1. selecionar `2`;
2. informar nome;
3. informar email;
4. informar senha;
5. selecionar envio do cadastro.

### Login

1. selecionar `1`;
2. informar email;
3. informar senha;
4. enviar login.

---

## Menu do motorista

```text
1- Cadastrar um Trecho
2- Cadastrar Trechos
3- Ver meus Trechos
4- Cancelar Trecho
5- Cadastrar Rota (varios trechos conectados)
6- Voltar
```

### Opção 1 - Cadastrar um trecho

Informar:

```text
Origem
Destino
Data
Hora
Capacidade
Preço
```

O servidor verifica se existe caminho no mapa.

### Opção 2 - Cadastrar vários trechos

Informar a quantidade desejada e preencher cada trecho.

### Opção 3 - Ver meus trechos

Exibe:

```text
ID
Origem
Destino
Data
Hora
Capacidade
Preço
```

### Opção 4 - Cancelar trecho

1. listar os trechos;
2. informar o ID;
3. o servidor verifica se o trecho pertence ao motorista;
4. remove o registro.

### Opção 5 - Cadastrar rota

A rota é informada como uma sequência:

```text
Salvador -> Feira de Santana
Feira de Santana -> Serrinha
Serrinha -> Araci
```

O sistema verifica a conexão entre trechos.

---

# 26. Manual de uso - passageiro

Menu inicial:

```text
1- LOGIN
2- CADASTRAR
3- SAIR
```

Após autenticação:

```text
1- Buscar Carona
2- Cancelar Reserva
3- Ver minhas Caronas
4- Sair da Conta
```

## Buscar carona

Informar:

```text
Origem
Destino
Data
Hora
```

O servidor apresenta as caronas encontradas.

Depois o usuário informa o ID.

Se houver disponibilidade:

```text
Carona reservada com sucesso!
```

## Ver minhas caronas

O servidor percorre o array `clientes[]` de cada trecho e mostra as reservas do usuário.

## Cancelar reserva

1. selecionar `2`;
2. listar reservas;
3. informar ID;
4. o servidor remove o email do passageiro e devolve o assento à capacidade.

---

# 27. Política de capacidade

A capacidade é controlada **por trecho**, e não pela rota inteira.

Exemplo:

```text
Trecho A -> B: capacidade 2
Trecho B -> C: capacidade 2
```

Se um passageiro ocupar:

```text
A -> B
```

o trecho:

```text
B -> C
```

continua com capacidade própria.

Essa característica corresponde ao requisito do enunciado de controlar disponibilidade por trecho.

---

# 28. Cancelamento

## Motorista

O cancelamento remove o trecho do arquivo central.

## Passageiro

O cancelamento remove o email do array `clientes[]` e devolve uma unidade à capacidade.

---

# 29. Logs

Os logs ficam em:

```text
logs/servidor.log
```

Exemplos de eventos:

```text
Servidor iniciado
Novo dispositivo conectado
Cliente logado
Motorista logado
Reserva realizada
Reserva cancelada
Trecho cadastrado
Trecho cancelado
Falha de autenticação
```

---

# 30. Compilação manual sem Docker

Servidor:

```bash
gcc servidor.c grafomapa.c cJSON.c -o servidor_bin -pthread 
```

Motorista:

```bash
gcc motorista.c cJSON.c -o motorista_bin
```

Cliente:

```bash
gcc cliente.c cJSON.c -o cliente_bin
```

Depois:

```bash
./servidor_bin
```

e, em outros terminais:

```bash
./motorista_bin
```

```bash
./cliente_bin
```
---
---
# 31 Execução do teste
Compilar o código do teste
```bash
gcc -Wall -O2 -o teste_concorrencia teste_concorrencia.c -lpthread
```
Executar o código do teste (servidor precisa estar rodando)
```bash
./teste_concorrencia <ip do servidor>
```
Saída dos testes
```bash
============================================================
 TESTE 1: Cadastro concorrente do MESMO email (cliente)
============================================================
Disparando 8 cadastros simultaneos para o email: corrida_cadastro_1789697806_111794_0@teste.com
Resultado: 1 sucesso(s) | 7 'ja cadastrado' | 0 outro/erro
>>> PASSOU: exatamente 1 cadastro foi aceito, os demais foram barrados.

============================================================
 TESTE 2: Reserva concorrente da MESMA carona (capacidade limitada)
============================================================
Cadastrando 1 trecho Feira_de_Santana -> Salvador com capacidade = 3 ...
Trecho cadastrado com ID = 26
Disparando 10 clientes tentando reservar os 3 assento(s) ao mesmo tempo...
Resultado: 3 reservada(s) | 7 sem assento | 0 outro/erro
>>> PASSOU: exatamente 3 reservas vingaram (nenhum overbooking).

============================================================
 TESTE 3: Cancelamento concorrente da MESMA reserva
============================================================
Duas threads tentando cancelar a MESMA reserva (id=26, cliente=cliente_reserva_1789697806_111794_3@teste.com) ao mesmo tempo...
Resultado: 1 cancelada(s) | 1 'nao encontrada' | 0 outro/erro
>>> PASSOU: apenas uma das duas tentativas cancelou a reserva.

============================================================
 TESTE 4: Cadastro concorrente de trechos (varios motoristas)
============================================================
Disparando 12 motoristas cadastrando um trecho cada, ao mesmo tempo...
Resultado: 12 trechos cadastrados com sucesso | 0 erro(s) | 0 ID(s) duplicado(s)
>>> PASSOU: todos os trechos foram cadastrados com IDs unicos.

============================================================
 TESTE 5: Cadastro concorrente do MESMO email (motorista)
============================================================
Disparando 8 cadastros simultaneos para o email: corrida_cadastro_motorista_1789697806_111794_24@teste.com
Resultado: 1 sucesso(s) | 7 'ja cadastrado' | 0 outro/erro
>>> PASSOU: exatamente 1 cadastro de motorista foi aceito, os demais foram barrados.

============================================================
 TESTE 6: Reservas simultaneas em trechos DIFERENTES (sem contencao / sem deadlock)
============================================================
Cadastrando 6 trechos independentes (Feira_de_Santana -> Salvador, capacidade=1)...
Disparando 6 clientes reservando 6 trechos DIFERENTES ao mesmo tempo...
Resultado: 6 reservada(s) de 6 | 0 outro/erro | tempo total: 0.006s
>>> PASSOU: todas as reservas em trechos distintos foram bem-sucedidas, sem travar.

============================================================
 TESTE 7: Leituras concorrentes durante escritas no mesmo arquivo (trechos.json)
============================================================
Preparando 1 motorista com um trecho ja cadastrado (alvo das leituras)...
Disparando 5 leitores (listar_trechos x5 cada) e 5 escritores (cadastrar_trecho) ao mesmo tempo...
Resultado: 5/5 leitores com respostas sempre validas | 5/5 escritas com sucesso
>>> PASSOU: nenhuma leitura recebeu resposta corrompida durante as escritas simultaneas.

============================================================
 TESTE 8: Motorista cancelando o trecho x Cliente reservando o MESMO trecho
============================================================
Trecho de teste cadastrado com ID = 51 (capacidade = 1)
Disparando o cancelamento do trecho (motorista) e a reserva (cliente) ao mesmo tempo...
Resposta do cancelamento (motorista): TRECHO_CANCELADO
Resposta da reserva (cliente):        TRECHO_NAO_ENCONTRADO
>>> OK (diagnostico): o servidor respondeu as duas operacoes concorrentes sem travar nem corromper a resposta. Confira manualmente se a combinacao acima faz sentido (ex.: reserva 'CARONA_RESERVADA' + cancelamento 'TRECHO_CANCELADO' significa que um cliente pode ficar com uma reserva 'orfa' de um trecho que o motorista cancelou).
```
---
---
# 32. Comandos úteis

Ver containers:

```bash
docker ps
```

Ver todos:

```bash
docker ps -a
```
```

Entrar no container do servidor:

```bash
docker exec -it app-servidor bash
```

Parar:

```bash
docker stop app-servidor
```

Remover:

```bash
docker rm app-servidor
```

Reconstruir sem cache:

```bash
docker build --no-cache -t app-servidor -f Dockerfile.Servidor .
```

---

# 33. Reprodutibilidade

Para reproduzir o ambiente:

1. instalar Docker;
2. clonar o repositório;
3. conferir os arquivos obrigatórios;
4. criar os diretórios de persistência;
5. construir as três imagens;
6. iniciar o servidor;
7. liberar TCP 65432;
8. iniciar motorista e passageiros;
9. informar o IP do servidor;
10. executar o fluxo de teste.

Os dados de execução não devem depender de arquivos internos do container. Por isso o diretório do servidor deve ser montado como volume/bind mount:

```text
dados/
trechosCadastrados/
logs/
```

---
---

# 34. Resumo do protocolo

```text
TRANSPORTE
    IPv4 + TCP
    Porta 65432

REPRESENTAÇÃO
    JSON / cJSON

CLASSES
    Cliente
    Motorista

REQUISIÇÕES DO CLIENTE
    login
    cadastro
    buscar_carona
    selecionar_carona
    buscar_trechos_partida
    selecionar_trecho
    finalizar_rota
    listar_caronas
    cancelar_carona

REQUISIÇÕES DO MOTORISTA
    login
    cadastro
    cadastrar_trecho
    listar_trechos
    cancelar_trecho
    cadastrar_rota

PERSISTÊNCIA
    dados/
    trechosCadastrados/
    logs/

CONCORRÊNCIA
    pthread
    mutexes

SERVIDOR
    um servidor central
    múltiplos clientes simultâneos
```

---

# 35. Referência

TANENBAUM, Andrew S.; FEAMSTER, Nick; WETHERALL, David J. *Redes de computadores*. 6. ed. São Paulo: Pearson / Porto Alegre: Bookman, 2021.
