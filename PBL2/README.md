# VaiJunto — Consórcio de Caronas Compartilhadas (Problema 2, TEC502)

Cada empresa do consórcio executa o **seu próprio servidor** (contêiner Docker, armazenamento próprio). Passageiros e
motoristas falam só com o servidor da própria empresa (TCP + JSON, como no Problema 1); os servidores cooperam entre si
por uma **API REST** para que um passageiro monte itinerários com caronas de qualquer empresa e os confirme de forma
**atômica**, sem servidor central, coordenador fixo ou banco compartilhado.

| Requisito do enunciado | Como é atendido | Onde |
|---|---|---|
| Atomicidade entre empresas | Two-Phase Commit com **coordenador por transação** (a empresa do passageiro), log write-ahead com fsync | `consorcio.c` |
| Nenhum assento duplicado | Cada empresa é dona dos seus assentos; bloqueio sob mutex do trecho, durável e idempotente | `consorcio.c`, `servidor.c` |
| Precedência na disputa | Relógio lógico de **Lamport** persistido + prioridade `(ts, empresa)` + **wait-die** (sem deadlock) | `lamport.c`, `consorcio.c` |
| Falhas e recuperação | Log do coordenador/participante, recuperação na partida, terminação cooperativa, lease do voto SIM, retransmissão | `consorcio.c` |
| API REST entre servidores | libmicrohttpd, 10 endpoints, `X-Lamport-Clock` | `docs/API.md`, `consorcio.c` |
| Docker, uma empresa por contêiner | `DockerFile.Servidor`, `docker-compose.yml`, `docker-compose.empresa.yml` | raiz |
| Simulador de passageiros | `simulador/simulador.py` (carga concorrente sem GUI) | `simulador/` |
| Teste automatizado (carga + falhas) | `testes/teste_consorcio.py` (I1, I2, I3 + disponibilidade + controle negativo) | `testes/` |
| Relatório SBC | `relatorio/relatorio.tex` (+ PDF) | `relatorio/` |

## Estrutura do repositório

```
cJSON/                 cJSON.c cJSON.h
GrafoMapa/             grafomapa.c grafomapa.h mapa.txt          (mapa + busca de itinerários)
DockerFile/            DockerFile.Servidor  DockerFile.Cliente  DockerFile.Motorista
src/                   cliente.c motorista.c formatos.h          (clientes do Problema 1)
src/Consórcio/         servidor.c servidor.h consorcio.c consorcio.h lamport.c lamport.h
docker-compose.yml     3 empresas na mesma máquina          docker-compose.empresa.yml  1 empresa por computador
docs/API.md            especificação da API REST + diagramas de sequência
simulador/             vaijunto.py (biblioteca), simulador.py (gerador de carga)
testes/                teste_consorcio.py (carga + injeção de falhas + auditoria)
relatorio/             relatorio.tex, refs.bib, sbc-template.sty, sbc.bst, relatorio.pdf
```

O **contexto de build do Docker é a raiz do repositório** (os `COPY` usam `cJSON/`, `GrafoMapa/` e `src/`). Dentro da imagem a pasta
`Consórcio` vira `consorcio` (sem acento) para evitar problemas de codificação de nomes de arquivo.

## Atalhos com o Makefile (na raiz do repositório)
```bash
make deps                  # instala gcc, libmicrohttpd, python3, curl (e GTK3 para a interface)
make                       # compila bin/servidor, bin/cliente, bin/motorista
make cluster-up            # 3 empresas locais em segundo plano (clientes :17001-17003, REST :18001-18003)
make cluster-kill EMPRESA=2   # derruba a empresa 2 (kill -9)      make cluster-start EMPRESA=2   # religa (recupera do disco)
make cluster-status | cluster-logs | cluster-down
make simulador DURACAO=30 THREADS=24   # carga + auditoria no cluster local
make teste                 # teste automatizado completo (modo local)
make docker-up | docker-kill EMPRESA=2 | docker-start EMPRESA=2 | docker-down | teste-docker
make relatorio             # compila o relatório SBC
```
Os clientes (`make run-cliente`) usam a porta fixa 65432: para usá-los com o cluster local, use `make run-servidor`
(empresa na porta padrão) ou o Docker (`docker compose run --rm cliente`, host `empresa1`).

## Executando

### A. Três empresas na mesma máquina (desenvolvimento)
```bash
docker compose up -d --build            # (na raiz do repositório) empresa1:65431/8081  empresa2:65432/8082  empresa3:65433/8083
docker compose run --rm motorista       # IP do servidor: empresa1  -> cadastre-se e publique caronas
docker compose run --rm cliente         # IP do servidor: empresa1  -> busque e confirme itinerários
docker compose kill empresa2            # derruba uma empresa; as outras continuam
docker compose start empresa2           # ela volta e se recupera sozinha
```

### B. Apresentação: uma empresa por computador do laboratório
Em cada máquina (ajuste os IPs; libere `65432/tcp` e `8080/tcp` no firewall):
```bash
EMPRESA_ID=1 PEERS="2=http://IP2:8080,3=http://IP3:8080" docker compose -f docker-compose.empresa.yml up -d --build
EMPRESA_ID=2 PEERS="1=http://IP1:8080,3=http://IP3:8080" docker compose -f docker-compose.empresa.yml up -d --build
EMPRESA_ID=3 PEERS="1=http://IP1:8080,2=http://IP2:8080" docker compose -f docker-compose.empresa.yml up -d --build
```
O tutor pode interromper qualquer uma (`docker kill empresaN`); religue com `docker start empresaN` (o volume preserva o disco).

### C. Sem Docker (compilação local, a partir da raiz do repositório)
```bash
sudo apt-get install -y libmicrohttpd-dev
gcc -O2 -Wall -I cJSON -I GrafoMapa -I src -I "src/Consórcio" -o servidor_bin \
    "src/Consórcio/servidor.c" "src/Consórcio/consorcio.c" "src/Consórcio/lamport.c" GrafoMapa/grafomapa.c cJSON/cJSON.c \
    -lmicrohttpd -pthread
gcc -Wall -I cJSON -I src -o cliente_bin   src/cliente.c   cJSON/cJSON.c
gcc -Wall -I cJSON -I src -o motorista_bin src/motorista.c cJSON/cJSON.c
# o servidor lê "mapa.txt" do diretório corrente:
mkdir -p /tmp/empresa1 && cp GrafoMapa/mapa.txt /tmp/empresa1 && cd /tmp/empresa1
EMPRESA_ID=1 PORTA_CLIENTES=17001 PORTA_REST=18001 PEERS="2=http://127.0.0.1:18002,3=http://127.0.0.1:18003" /caminho/para/servidor_bin
```

### Variáveis de ambiente do servidor
| Variável | Padrão | Significado |
|---|---|---|
| `EMPRESA_ID` | 1 | id da empresa (≥ 1, único no consórcio) |
| `PEERS` | vazio | `id=http://host:porta,...` das **outras** empresas |
| `PORTA_CLIENTES` / `PORTA_REST` | 65432 / 8080 | portas TCP dos clientes e HTTP entre empresas |
| `PREPARE_TIMEOUT_MS` / `DECISAO_TIMEOUT_MS` / `BUSCA_TIMEOUT_MS` | 5000 / 2000 / 1500 | timeouts de rede (detector de falhas) |
| `SUSPEITA_MS` | 3000 | tempo em que um par que falhou é tratado como fora do ar |
| `ESPERA_CONFLITO_MS` | 1500 | quanto uma transação mais velha espera por um assento disputado (wait-die) |
| `INDOUBT_MS` / `PREPARED_TIMEOUT_MS` | 3000 / 30000 | quando o participante começa a perguntar o resultado / lease do voto SIM |
| `MODO_TESTE` | 0 | 1 = expõe campos de auditoria na API (só para o teste automatizado) |
| `CRASH_AT` | — | falha injetada: `part_apos_prepare`, `part_antes_commit`, `coord_antes_decisao`, `coord_apos_decisao` |

## Simulador de passageiros
```bash
python3 simulador/simulador.py \
  --empresas 1=localhost:65431:8081,2=localhost:65432:8082,3=localhost:65433:8083 \
  --semear --threads 32 --duracao 30 --passageiros-por-empresa 40 --auditar
```
Em computadores diferentes: `--empresas 1=IP1:65432:8080,2=IP2:65432:8080,3=IP3:65432:8080`.
`--auditar` exige servidores com `MODO_TESTE=1`. Só usa a biblioteca padrão do Python 3.

## Teste automatizado (carga + injeção de falhas)
```bash
# sem Docker (3 processos, cada um com disco próprio) — precisa do binário compilado (opção C)
python3 testes/teste_consorcio.py --modo local --binario ./servidor_bin
# com Docker (usa o docker-compose.yml; derruba com `docker kill`)
python3 testes/teste_consorcio.py --modo docker
# subconjuntos
python3 testes/teste_consorcio.py --cenarios carga,queda --duracao 20
```
Verifica, lendo o estado de todas as empresas pela API REST:

- **I1** nenhum assento confirmado para dois passageiros (reservas ≤ assentos, sem passageiro repetido, `livres + reservados + bloqueados = total`);
- **I2** nenhum itinerário pela metade (toda transação com reserva foi decidida COMMIT e está reservada em **todos** os seus trechos; todo itinerário confirmado ao passageiro idem);
- **I3** nenhum assento bloqueado depois da queda e da recuperação;
- **D** com a empresa 2 fora do ar, itinerários E1+E3 continuam sendo confirmados.

Cenários: carga pura · `kill -9` da empresa 2 no meio da carga · falha injetada em 4 pontos críticos do 2PC · **controle
negativo** (corrompe um itinerário de propósito e exige que a auditoria o detecte). Código de saída 0 = tudo OK.

## Mudanças em relação aos arquivos originais
- `DockerFile/DockerFile.Servidor`: compilava só `servidor.c cJSON.c grafomapa.c` (o link falhava) e não instalava `libmicrohttpd`;
  agora compila `servidor.c consorcio.c lamport.c grafomapa.c cJSON.c` com `-I` para as pastas, instala as dependências, expõe a
  porta REST, guarda o estado em volume e traz healthcheck. `DockerFile.Cliente` e `DockerFile.Motorista` foram adaptados às pastas.
- `GrafoMapa/grafomapa.c/.h`: `buscar_rotas` só lia os trechos **locais**; agora usa `listarTrechosConsorcio()` e devolve itinerários
  que misturam empresas, com `id` global, `empresaId`, `data`, `hora` e `preco` (limite de 6 trechos / 200 rotas). Passou a incluir
  `consorcio.h` (por isso a compilação precisa de `-I "src/Consórcio"`).
- `src/Consórcio/servidor.c`: `fsync` na gravação dos trechos (durabilidade).
- `src/cliente.c`: leitura das respostas com buffer dinâmico (`realloc`), mensagem para `EMPRESA_INDISPONIVEL` e correção do login
  (resposta truncada com nomes longos).
- Novos: `docker-compose.yml`, `docker-compose.empresa.yml`, `docs/API.md`, `simulador/`, `testes/`, `relatorio/`.