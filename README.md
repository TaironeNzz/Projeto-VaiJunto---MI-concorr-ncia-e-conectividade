# VaiJunto - Sistema de Caronas Compartilhadas

## 1. Visão geral

O **VaiJunto** é um protótipo de sistema distribuído para caronas compartilhadas, desenvolvido em C sobre **sockets TCP/IP nativos**, com um **servidor central** e dois tipos de clientes:

- **Cliente Motorista**: publica trechos/rotas, consulta seus trechos e cancela trechos.
- **Cliente Passageiro**: autentica, procura caronas, reserva trechos, consulta reservas e cancela reservas.
- **Servidor**: mantém o estado compartilhado das caronas e reservas, processa requisições concorrentes e persiste os dados em arquivos.

A solução utiliza **JSON** como representação intermediária das mensagens, processado pela biblioteca **cJSON**, e utiliza `pthread` para atender vários clientes simultaneamente.

A porta TCP utilizada pelo programa é **65432**.

---

## 2. Relação com o problema do PDF

O enunciado descreve uma plataforma na qual:

1. motoristas publicam uma rota como sequência ordenada de cidades;
2. cada trecho possui data, horário, quantidade de assentos e preço;
3. passageiros procuram itinerários entre origem e destino;
4. uma viagem pode usar uma única carona ou combinar trechos de motoristas diferentes;
5. a disponibilidade deve ser controlada por trecho;
6. reservas concorrentes não podem vender o mesmo assento duas vezes;
7. a comunicação deve usar TCP/IP e a API de socket nativa;
8. não devem ser usados frameworks de RPC/mensageria;
9. deve existir um único servidor central;
10. o servidor deve atender vários clientes simultaneamente;
11. mensagens precisam usar uma representação intermediária bem definida;
12. o servidor deve validar e descartar mensagens malformadas;
13. os componentes devem ser executados por meio de Docker;
14. os clientes e o servidor devem conseguir ser executados em computadores distintos;
15. o produto precisa de documentação do protocolo e de um teste automatizado de concorrência.

O projeto atende a maior parte dessa arquitetura. Os itens ainda incompletos ou parciais estão explicitados na seção **"Conformidade e pendências"** deste README.

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

A estrutura recomendada do repositório é:

```text
VaiJunto/
├── README.md
├── Dockerfile.Servidor
├── Dockerfile.Motorista
├── Dockerfile.Cliente
├── .dockerignore
│
├── servidor.c
├── motorista.c
├── cliente.c
├── formatos.h
│
├── cJSON.c
├── cJSON.h
│
├── grafomapa.c
├── grafomapa.h
├── mapa.txt
│
├── dados/
│   ├── loginCliente.json
│   └── loginMotorista.json
│
├── trechosCadastrados/
│   └── trechos.json
│
└── logs/
    └── servidor.log
```

### Função dos arquivos

| Arquivo | Função |
|---|---|
| `servidor.c` | Servidor central, sockets, concorrência, persistência e regras das caronas |
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

> Os arquivos enviados para esta documentação possuem nomes de versão no anexo, como `cliente(4).c` e `servidor(3).c`. No repositório recomenda-se usar os nomes canônicos `cliente.c`, `motorista.c` e `servidor.c`.

---

# 5. Dependências

## 5.1 Dependências do software

### Compilador

- GCC
- padrão C utilizado: C11/C compatível com o código atual

### Biblioteca de threads

- POSIX Threads (`pthread`)
- habilitada na compilação com `-pthread`

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

Não são utilizados:

- bancos de dados;
- frameworks RPC;
- brokers de mensagens;
- bibliotecas de comunicação de alto nível.

Isso mantém a comunicação dentro da restrição do problema: **socket TCP/IP nativo**.

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

### Limitação importante

A implementação atual não possui um cabeçalho de tamanho nem um delimitador explícito (`\n`, por exemplo) para cada mensagem.

Portanto, para uma implementação de produção ou para garantir interoperabilidade mais rigorosa entre linguagens, o protocolo deve evoluir para:

```text
[4 bytes: tamanho da mensagem][JSON]
```

ou:

```text
JSON\n
```

O README documenta o protocolo **existente no código entregue**, mas essa melhoria de framing é recomendada para eliminar ambiguidades de leitura/coalescência do TCP.

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
docker build -t vaijunto-servidor -f Dockerfile.Servidor .
```

Execução Linux/macOS:

```bash
docker run -d \
  --name vaijunto-servidor \
  -p 65432:65432 \
  -v "$(pwd)/dados:/app/dados" \
  -v "$(pwd)/trechosCadastrados:/app/trechosCadastrados" \
  -v "$(pwd)/logs:/app/logs" \
  vaijunto-servidor
```

PowerShell:

```powershell
docker run -d `
  --name vaijunto-servidor `
  -p 65432:65432 `
  -v "${PWD}/dados:/app/dados" `
  -v "${PWD}/trechosCadastrados:/app/trechosCadastrados" `
  -v "${PWD}/logs:/app/logs" `
  vaijunto-servidor
```

---

## 22.2 Imagem do motorista

```bash
docker build -t vaijunto-motorista -f Dockerfile.Motorista .
```

Executar:

```bash
docker run --rm -it vaijunto-motorista
```

Quando o programa solicitar:

```text
Digite o IP do servidor:
```

informe o IPv4 da máquina que executa o servidor.

---

## 22.3 Imagem do passageiro

```bash
docker build -t vaijunto-cliente -f Dockerfile.Cliente .
```

Executar:

```bash
docker run --rm -it vaijunto-cliente
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

Windows:

```powershell
ipconfig
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
gcc -std=c11 -Wall -Wextra -O2 \
    servidor.c grafomapa.c cJSON.c \
    -pthread -lm \
    -o servidor_bin
```

Motorista:

```bash
gcc -std=c11 -Wall -Wextra -O2 \
    motorista.c cJSON.c \
    -o motorista_bin
```

Cliente:

```bash
gcc -std=c11 -Wall -Wextra -O2 \
    cliente.c cJSON.c \
    -o cliente_bin
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

# 31. Teste básico de comunicação

Sequência mínima:

### Teste 1 - Servidor

Verificar:

```text
Servidor iniciado na porta 65432
```

### Teste 2 - Motorista

1. cadastrar/login;
2. cadastrar um trecho;
3. listar trecho.

Esperado:

```text
TRECHO_CADASTRADO
```

e o trecho aparecer na listagem.

### Teste 3 - Passageiro

1. cadastrar/login;
2. buscar a carona;
3. selecionar o ID;
4. verificar resposta.

Esperado:

```text
CARONA_RESERVADA
```

### Teste 4 - Concorrência

Executar dois ou mais passageiros tentando reservar o último assento simultaneamente.

Esperado:

```text
somente um cliente -> CARONA_RESERVADA
demais clientes -> ASSENTO_INDISPONIVEL
```

O arquivo não deve apresentar capacidade negativa.

---

# 32. Teste concorrente exigido pelo enunciado

O PDF determina um **teste automatizado** com múltiplos clientes disputando os mesmos trechos.

O teste deve verificar pelo menos:

1. vários clientes executando `selecionar_carona` simultaneamente;
2. nenhum assento vendido duas vezes;
3. capacidade nunca menor que zero;
4. estado consistente de `clientes[]`;
5. nenhum itinerário confirmado pela metade;
6. tempo de resposta sob carga.

## Estratégia recomendada

Preparar:

```text
capacidade = 1
```

e iniciar, por exemplo:

```text
10 clientes
```

tentando reservar o mesmo trecho.

Resultado esperado:

```text
1 sucesso
9 falhas por falta de assento
```

O teste deve registrar:

```text
tempo total
tempo médio
maior tempo
menor tempo
quantidade de reservas aceitas
quantidade de reservas recusadas
```

### Situação atual

Entre os arquivos enviados para elaboração deste README **não existe um programa de teste automatizado de concorrência**.

Portanto, este requisito do PDF deve ser tratado como pendência de implementação/teste, mesmo que o servidor já possua mutexes para proteger as operações concorrentes.

---

# 33. Conformidade com os requisitos do problema

| Requisito do PDF | Situação |
|---|---|
| Servidor central único | Implementado |
| Clientes motorista e passageiro | Implementado |
| Socket TCP/IP nativo | Implementado |
| Sem framework RPC/mensageria | Implementado |
| Docker | Implementado/documentado |
| Clientes em computadores diferentes | Suportado/documentado |
| Representação intermediária bem definida | JSON/cJSON |
| Atendimento simultâneo | Implementado com `pthread` |
| Controle concorrente de assentos | Implementado com `trechosMutex` |
| Cadastro/autenticação | Implementado |
| Publicação de trechos | Implementado |
| Publicação de rota | Implementado |
| Busca de caronas | Implementado |
| Reserva | Implementado |
| Consulta/cancelamento de reserva | Implementado |
| Combinação de trechos | Implementado parcialmente |
| Rollback de reservas em falha de finalização | Implementado |
| Proteção contra dois clientes no mesmo assento | Protegido pelo mutex |
| Expiração de trechos | Implementado |
| Protocolo de aplicação documentado | Documentado neste README |
| Exemplos de mensagens | Documentados neste README |
| Teste automatizado concorrente | **Ainda necessário** |
| Consulta dos passageiros confirmados pelo motorista | **Ainda necessário no cliente atual** |
| Atomicidade completa em caso de queda abrupta do cliente | **Parcial** |
| Framing explícito de mensagens TCP | **Melhoria recomendada** |

---

# 34. Pontos que precisam ser observados na apresentação

## 34.1 Atomicidade

O enunciado exige:

> ou todos os trechos do itinerário são reservados, ou nenhum.

O código atual reserva trechos durante a montagem da rota e, no final, executa uma validação. Se a validação falha, tenta desfazer as reservas.

Isso fornece um mecanismo de rollback.

Porém, existe uma diferença entre rollback por lógica de aplicação e uma transação realmente protegida contra falhas do processo.

Se o cliente cair no meio da montagem do itinerário antes de `finalizar_rota`, o estado pode exigir limpeza adicional.

Esse comportamento deve ser tratado como uma limitação conhecida ou corrigido em uma versão posterior.

---

## 34.2 Passageiros confirmados pelo motorista

O enunciado pede que o motorista consiga:

> "consultar as caronas já publicadas e os passageiros confirmados em cada trecho"

A estrutura do servidor já armazena os passageiros no campo:

```json
"clientes": [
    "ana@email.com"
]
```

Porém a função atual de listagem do motorista retorna principalmente:

```text
ID
Origem
Destino
Data
Hora
Capacidade
Preço
```

e não exibe, no menu atual, a lista completa de passageiros de cada trecho.

Para aderir integralmente ao enunciado, deve ser adicionada uma operação de consulta dos passageiros confirmados.

---

# 35. Segurança e validação

O sistema é um protótipo acadêmico e não implementa segurança de produção.

As senhas são persistidas nos arquivos de login.

Não há:

- TLS;
- hash de senha;
- token de sessão;
- criptografia;
- controle de acesso avançado.

A validação de mensagens é feita principalmente por:

```cJSON_Parse()
```

e pela análise dos campos esperados.

Mensagens que não formam JSON válido são rejeitadas e registradas no log.

---

# 36. Limitações atuais conhecidas

1. O protocolo não possui cabeçalho de tamanho nem delimitador explícito.
2. O código de teste concorrente exigido pelo enunciado ainda não está presente nos arquivos enviados.
3. O cliente motorista ainda não mostra a lista detalhada de passageiros por trecho.
4. A atomicidade do itinerário combinado depende do rollback da etapa de finalização.
5. O armazenamento é baseado em arquivos JSON, não em banco de dados.
6. As credenciais são armazenadas sem proteção criptográfica.
7. O sistema foi pensado para um único servidor central.
8. O servidor não possui mecanismo de replicação ou failover.

Essas limitações não devem ser escondidas durante a apresentação; devem ser tratadas como decisões do protótipo ou pontos de evolução.

---

# 37. Checklist de demonstração

Antes da apresentação, verificar:

```text
[ ] servidor inicia
[ ] mapa.txt está presente
[ ] cJSON.h/cJSON.c estão presentes
[ ] grafomapa.c/grafomapa.h estão presentes
[ ] diretórios de dados existem
[ ] porta TCP 65432 está liberada
[ ] motorista consegue conectar
[ ] passageiro consegue conectar
[ ] cadastro funciona
[ ] login funciona
[ ] motorista cadastra trecho
[ ] motorista lista trecho
[ ] passageiro encontra carona
[ ] passageiro reserva
[ ] capacidade diminui
[ ] reserva aparece em "minhas caronas"
[ ] passageiro cancela
[ ] capacidade aumenta
[ ] motorista cancela trecho
[ ] rota com múltiplos trechos funciona
[ ] containers estão em computadores distintos
[ ] logs estão sendo gerados
[ ] teste concorrente está preparado
```

---

# 38. Comandos úteis

Ver containers:

```bash
docker ps
```

Ver todos:

```bash
docker ps -a
```

Logs do servidor:

```bash
docker logs -f vaijunto-servidor
```

Entrar no container do servidor:

```bash
docker exec -it vaijunto-servidor bash
```

Parar:

```bash
docker stop vaijunto-servidor
```

Remover:

```bash
docker rm vaijunto-servidor
```

Reconstruir sem cache:

```bash
docker build --no-cache -t vaijunto-servidor -f Dockerfile.Servidor .
```

---

# 39. Reprodutibilidade

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

# 40. Observação sobre o relatório acadêmico

O enunciado separa o README do relatório em formato SBC.

O README deste projeto é destinado principalmente ao:

- funcionamento;
- instalação;
- execução;
- protocolo;
- estrutura;
- dependências;
- operação;
- testes;
- limitações.

O relatório SBC deve tratar, separadamente:

- fundamentação teórica;
- arquitetura distribuída;
- TCP/IP;
- sockets;
- concorrência;
- controle de assentos;
- persistência;
- decisões de projeto;
- resultados experimentais;
- referências.

O PDF estabelece que o relatório SBC deve ter no máximo **8 páginas**.

---

# 41. Resumo do protocolo

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

# 42. Referência do problema

**Problema 1 - VAIJUNTO: Sistema de Caronas Compartilhadas**, TEC502.

O enunciado determina, entre outros pontos, o uso de TCP/IP com API Socket nativa, containers Docker, servidor central único, tratamento concorrente das reservas, representação intermediária bem definida, execução em computadores distintos e documentação do protocolo.

Este README deve permanecer versionado junto ao código no GitHub.
