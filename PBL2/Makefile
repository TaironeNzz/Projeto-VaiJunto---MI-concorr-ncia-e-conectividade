# ============================================================================
# Makefile do VaiJunto (Problema 2 - Consorcio de Caronas)
#
# Estrutura esperada (rode o make na RAIZ do projeto):
#   src/            cliente.c, motorista.c, interface.c (opcional), formatos.h
#   src/Consorcio/  servidor.c servidor.h consorcio.c consorcio.h lamport.c lamport.h
#   cJSON/          cJSON.c, cJSON.h
#   GrafoMapa/      grafomapa.c, grafomapa.h, mapa.txt
#   DockerFile/     DockerFile.Servidor, DockerFile.Cliente, DockerFile.Motorista
#   simulador/      simulador.py, vaijunto.py
#   testes/         teste_consorcio.py
#   relatorio/      relatorio.tex, refs.bib, sbc-template.sty, sbc.bst
#
# Uso rapido:
#   make deps          instala as dependencias (gcc, make, libmicrohttpd, GTK3, python3)
#   make               compila servidor, cliente, motorista (e interface, se houver GTK3)
#
# Consorcio SEM Docker (3 empresas como processos locais, cada uma com disco proprio em run/empresaN):
#   make cluster-up    sobe as 3 empresas em segundo plano    (clientes: 17001..17003 | REST: 18001..18003)
#   make cluster-status | cluster-logs | cluster-down
#   make cluster-kill EMPRESA=2     derruba (kill -9) uma empresa
#   make cluster-start EMPRESA=2    religa a empresa (recupera o estado do disco)
#   make run-empresa1 | run-empresa2 | run-empresa3    uma empresa em primeiro plano (terminais separados)
#   make run-cliente | run-motorista | run-interface   (informe o IP 127.0.0.1; os clientes usam a porta 65432,
#                                                       entao use `make run-servidor` para a empresa na porta padrao)
#
# Consorcio COM Docker:
#   make docker-build  gera as 3 imagens | make docker-up / docker-down / docker-kill EMPRESA=2 / docker-start EMPRESA=2
#
# Simulador e testes:
#   make simulador     carga de passageiros no cluster local (cluster-up antes; MODO_TESTE=1 para --auditar)
#   make teste         teste automatizado (carga + injecao de falhas + auditoria), modo local
#   make teste-docker  o mesmo, nos conteineres do docker-compose
#   make relatorio     compila o relatorio (pdflatex + bibtex)
#   make clean | make distclean
# ============================================================================

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2

SRC_DIR    = src
CONS_DIR   = src/Consorcio
CJSON_DIR  = cJSON
GRAFO_DIR  = GrafoMapa
BIN_DIR    = bin
RUN_DIR    = run
DOCKER_DIR = DockerFile

# -I evita ter que alterar os #include dos fontes (grafomapa.c inclui consorcio.h; servidor.c inclui grafomapa.h...)
INCLUDES = -I$(CJSON_DIR) -I$(GRAFO_DIR) -I$(SRC_DIR) -I$(CONS_DIR)

CJSON_SRC = $(CJSON_DIR)/cJSON.c
GRAFO_SRC = $(GRAFO_DIR)/grafomapa.c
MAPA      = $(GRAFO_DIR)/mapa.txt

# Servidor = servidor + consorcio (2PC, API REST) + relogio de Lamport + grafo do mapa + cJSON
SERVIDOR_SRC  = $(CONS_DIR)/servidor.c $(CONS_DIR)/consorcio.c $(CONS_DIR)/lamport.c
CLIENTE_SRC   = $(SRC_DIR)/cliente.c
MOTORISTA_SRC = $(SRC_DIR)/motorista.c
INTERFACE_SRC = $(SRC_DIR)/interface.c

# Cabecalhos: qualquer mudanca neles recompila (nao entram na linha do gcc)
HEADERS = $(wildcard $(SRC_DIR)/*.h) $(wildcard $(CONS_DIR)/*.h) $(wildcard $(CJSON_DIR)/*.h) $(wildcard $(GRAFO_DIR)/*.h)

# So compila a interface grafica se o GTK3 estiver instalado e o fonte existir
TEM_GTK := $(shell pkg-config --exists gtk+-3.0 2>/dev/null && echo sim)
TEM_INTERFACE := $(wildcard $(INTERFACE_SRC))

ALVOS = $(BIN_DIR)/servidor $(BIN_DIR)/cliente $(BIN_DIR)/motorista
ifeq ($(TEM_GTK),sim)
ifneq ($(TEM_INTERFACE),)
ALVOS += $(BIN_DIR)/interface
endif
endif

# Configuracao do cluster local (3 empresas)
EMPRESAS    = 1 2 3
PORTA_CLI   = 1700
PORTA_REST  = 1800
EMPRESA    ?= 2
DURACAO    ?= 30
THREADS    ?= 24
PY         ?= python3
SIM_EMPRESAS = 1=127.0.0.1:17001:18001,2=127.0.0.1:17002:18002,3=127.0.0.1:17003:18003

.PHONY: all deps dirs clean distclean help docker-build docker-up docker-down docker-kill docker-start \
        run-servidor run-cliente run-motorista run-interface run-empresa1 run-empresa2 run-empresa3 \
        cluster-up cluster-down cluster-status cluster-logs cluster-kill cluster-start \
        simulador teste teste-docker relatorio

all: $(ALVOS)
ifneq ($(TEM_GTK),sim)
	@echo "Aviso: GTK3 nao encontrado, a interface grafica nao foi compilada (rode: make deps)."
endif

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/servidor: $(SERVIDOR_SRC) $(GRAFO_SRC) $(CJSON_SRC) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ -lmicrohttpd -pthread -lm

$(BIN_DIR)/cliente: $(CLIENTE_SRC) $(CJSON_SRC) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ -lm

$(BIN_DIR)/motorista: $(MOTORISTA_SRC) $(CJSON_SRC) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ -lm

$(BIN_DIR)/interface: $(INTERFACE_SRC) $(CJSON_SRC) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $$(pkg-config --cflags --libs gtk+-3.0) -lm

# Instala as dependencias conforme o gerenciador de pacotes da distribuicao
deps:
	@if command -v apt-get >/dev/null 2>&1; then \
		echo ">> Debian/Ubuntu"; \
		sudo apt-get update && \
		sudo apt-get install -y build-essential pkg-config libmicrohttpd-dev libgtk-3-dev python3 curl; \
	elif command -v dnf >/dev/null 2>&1; then \
		echo ">> Fedora/RHEL"; \
		sudo dnf install -y gcc make pkgconf-pkg-config libmicrohttpd-devel gtk3-devel python3 curl; \
	elif command -v pacman >/dev/null 2>&1; then \
		echo ">> Arch"; \
		sudo pacman -S --needed --noconfirm base-devel pkgconf libmicrohttpd gtk3 python curl; \
	else \
		echo "Gerenciador de pacotes nao suportado. Instale manualmente: gcc, make, pkg-config, libmicrohttpd (dev), GTK3 (dev), python3 e curl."; \
		exit 1; \
	fi
	@echo ">> Dependencias instaladas. (Docker e opcional: https://docs.docker.com/engine/install/)"

# Cria as pastas que o servidor usa em tempo de execucao (para rodar o servidor direto da raiz)
dirs:
	mkdir -p logs trechosCadastrados dados ArquivoID Avisos Itinerarios transacoes
	@test -f mapa.txt || cp $(MAPA) mapa.txt

# ------------------------------------------------------------ execucao individual
# Empresa na porta padrao dos clientes (65432) e REST 8080, sem pares (para testar cliente/motorista)
run-servidor: $(BIN_DIR)/servidor dirs
	EMPRESA_ID=1 ./$(BIN_DIR)/servidor

run-cliente: $(BIN_DIR)/cliente
	./$(BIN_DIR)/cliente

run-motorista: $(BIN_DIR)/motorista
	./$(BIN_DIR)/motorista

run-interface: $(BIN_DIR)/interface
	./$(BIN_DIR)/interface

# ------------------------------------------------------------ cluster local: 3 empresas, disco proprio em run/empresaN
# Uma empresa em primeiro plano (rode cada uma em um terminal). Ex.: make run-empresa2
define EMPRESA_RULES
run-empresa$(1): $(BIN_DIR)/servidor
	@mkdir -p $(RUN_DIR)/empresa$(1) && cp -f $(MAPA) $(RUN_DIR)/empresa$(1)/mapa.txt
	cd $(RUN_DIR)/empresa$(1) && EMPRESA_ID=$(1) PORTA_CLIENTES=$(PORTA_CLI)$(1) PORTA_REST=$(PORTA_REST)$(1) \
		MODO_TESTE=$$(MODO_TESTE) PEERS="$$(call PEERS_DE,$(1))" $(CURDIR)/$(BIN_DIR)/servidor
endef
COMMA := ,
SPACE := $(subst ,, )
# lista "id=http://host:porta,id=..." das OUTRAS empresas (separada por virgula, como o servidor espera)
PEERS_DE = $(subst $(SPACE),$(COMMA),$(strip $(foreach e,$(filter-out $(1),$(EMPRESAS)),$(e)=http://127.0.0.1:$(PORTA_REST)$(e))))
MODO_TESTE ?= 1
$(foreach e,$(EMPRESAS),$(eval $(call EMPRESA_RULES,$(e))))

# Sobe/derruba as tres em segundo plano (pid em run/empresaN/pid, log em run/empresaN/stdout.log)
cluster-up: $(BIN_DIR)/servidor
	@for e in $(EMPRESAS); do $(MAKE) --no-print-directory cluster-start EMPRESA=$$e; done
	@sleep 1; $(MAKE) --no-print-directory cluster-status

cluster-start: $(BIN_DIR)/servidor
	@mkdir -p $(RUN_DIR)/empresa$(EMPRESA) && cp -f $(MAPA) $(RUN_DIR)/empresa$(EMPRESA)/mapa.txt
	@if [ -f $(RUN_DIR)/empresa$(EMPRESA)/pid ] && kill -0 $$(cat $(RUN_DIR)/empresa$(EMPRESA)/pid) 2>/dev/null; then \
		echo "empresa $(EMPRESA) ja esta no ar (pid $$(cat $(RUN_DIR)/empresa$(EMPRESA)/pid))"; \
	else \
		cd $(RUN_DIR)/empresa$(EMPRESA) && \
		EMPRESA_ID=$(EMPRESA) PORTA_CLIENTES=$(PORTA_CLI)$(EMPRESA) PORTA_REST=$(PORTA_REST)$(EMPRESA) MODO_TESTE=$(MODO_TESTE) \
		PEERS="$(call PEERS_DE,$(EMPRESA))" setsid nohup $(CURDIR)/$(BIN_DIR)/servidor >> stdout.log 2>&1 < /dev/null & \
		echo $$! > $(CURDIR)/$(RUN_DIR)/empresa$(EMPRESA)/pid; \
		echo "empresa $(EMPRESA) iniciada (clientes :$(PORTA_CLI)$(EMPRESA), REST :$(PORTA_REST)$(EMPRESA))"; \
	fi

cluster-kill:
	@if [ -f $(RUN_DIR)/empresa$(EMPRESA)/pid ]; then kill -9 $$(cat $(RUN_DIR)/empresa$(EMPRESA)/pid) 2>/dev/null && echo "empresa $(EMPRESA) derrubada (kill -9)"; rm -f $(RUN_DIR)/empresa$(EMPRESA)/pid; else echo "empresa $(EMPRESA) nao esta rodando"; fi

cluster-down:
	@for e in $(EMPRESAS); do $(MAKE) --no-print-directory cluster-kill EMPRESA=$$e; done

cluster-status:
	@for e in $(EMPRESAS); do \
		if curl -fs --max-time 1 http://127.0.0.1:$(PORTA_REST)$$e/api/v1/health >/dev/null 2>&1; then echo "empresa $$e: NO AR  (clientes :$(PORTA_CLI)$$e, REST :$(PORTA_REST)$$e)"; else echo "empresa $$e: fora do ar"; fi; \
	done

cluster-logs:
	@tail -n 20 $(RUN_DIR)/empresa*/stdout.log 2>/dev/null || echo "sem logs (rode: make cluster-up)"

# ------------------------------------------------------------ simulador e testes
# Carga de passageiros no cluster local (suba antes com: make cluster-up). Semeia as caronas e audita no final.
simulador:
	$(PY) simulador/simulador.py --empresas $(SIM_EMPRESAS) --semear --threads $(THREADS) --duracao $(DURACAO) --auditar

# Teste automatizado: carga + queda + falhas injetadas + controle negativo (usa bin/servidor; nao use junto com cluster-up)
teste: $(BIN_DIR)/servidor
	$(PY) TestesConcorrência/teste_consorcio.py --modo local --binario $(BIN_DIR)/servidor

teste-docker:
	$(PY) TestesConcorrência/teste_consorcio.py --modo docker

# ------------------------------------------------------------ docker
# Contexto de build = raiz do projeto (o "." no final)
docker-build:
	docker build -t app-servidor  -f $(DOCKER_DIR)/DockerFile.Servidor .
	docker build -t app-cliente   -f $(DOCKER_DIR)/DockerFile.Cliente .
	docker build -t app-motorista -f $(DOCKER_DIR)/DockerFile.Motorista .

docker-up:
	docker compose up -d --build

docker-down:
	docker compose down

docker-kill:
	docker kill empresa$(EMPRESA)

docker-start:
	docker start empresa$(EMPRESA)

# ------------------------------------------------------------ relatorio
relatorio:
	cd relatorio && pdflatex -interaction=nonstopmode relatorio.tex && bibtex relatorio && pdflatex -interaction=nonstopmode relatorio.tex && pdflatex -interaction=nonstopmode relatorio.tex

clean:
	rm -rf $(BIN_DIR)

# clean + estado de execucao (CUIDADO: apaga os dados do cluster local em run/ e as pastas de dados da raiz)
distclean: clean
	-$(MAKE) --no-print-directory cluster-down
	rm -rf $(RUN_DIR) logs trechosCadastrados dados ArquivoID Avisos Itinerarios transacoes
	rm -f relatorio/relatorio.aux relatorio/relatorio.log relatorio/relatorio.out relatorio/relatorio.bbl relatorio/relatorio.blg

help:
	@echo "Compilar:   make deps | make | make clean | make distclean"
	@echo "Local:      make run-servidor | run-cliente | run-motorista | run-interface"
	@echo "Cluster:    make cluster-up | cluster-status | cluster-logs | cluster-down | cluster-kill EMPRESA=2 | cluster-start EMPRESA=2"
	@echo "            make run-empresa1 | run-empresa2 | run-empresa3   (um terminal por empresa)"
	@echo "Docker:     make docker-build | docker-up | docker-down | docker-kill EMPRESA=2 | docker-start EMPRESA=2"
	@echo "Testes:     make simulador [DURACAO=30 THREADS=24] | make teste | make teste-docker"
	@echo "Relatorio:  make relatorio"