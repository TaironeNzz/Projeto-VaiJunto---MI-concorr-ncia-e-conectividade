# ============================================================================
# Makefile do VaiJunto
#
# Estrutura esperada (rode o make na RAIZ do projeto):
#   src/         servidor.c, cliente.c, motorista.c, interface.c, formatos.h
#   cJSON/       cJSON.c, cJSON.h
#   GrafoMapa/   grafomapa.c, grafomapa.h
#   DockerFile/  DockerFile.Servidor, DockerFile.Cliente, DockerFile.Motorista
#   mapa.txt
#
# Uso rapido:
#   make deps        instala as dependencias (gcc, make, libmicrohttpd, GTK3)
#   make             compila servidor, cliente, motorista (e interface, se houver GTK3)
#   make run-servidor | run-cliente | run-motorista | run-interface
#   make docker-build   gera as 3 imagens Docker
#   make clean
# ============================================================================

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2

SRC_DIR   = src
CJSON_DIR = cJSON
GRAFO_DIR = GrafoMapa
BIN_DIR   = bin
DOCKER_DIR = DockerFile

# -I evita ter que alterar os #include dos fontes
INCLUDES = -I$(CJSON_DIR) -I$(GRAFO_DIR) -I$(SRC_DIR)

CJSON_SRC = $(CJSON_DIR)/cJSON.c
GRAFO_SRC = $(GRAFO_DIR)/grafomapa.c

# Ajuste aqui se os nomes dos seus arquivos forem diferentes
SERVIDOR_SRC  = $(SRC_DIR)/servidor.c
CLIENTE_SRC   = $(SRC_DIR)/cliente.c
MOTORISTA_SRC = $(SRC_DIR)/motorista.c
INTERFACE_SRC = $(SRC_DIR)/interface.c

# So compila a interface grafica se o GTK3 estiver instalado
TEM_GTK := $(shell pkg-config --exists gtk+-3.0 2>/dev/null && echo sim)

ALVOS = $(BIN_DIR)/servidor $(BIN_DIR)/cliente $(BIN_DIR)/motorista
ifeq ($(TEM_GTK),sim)
ALVOS += $(BIN_DIR)/interface
endif

.PHONY: all deps dirs clean help docker-build run-servidor run-cliente run-motorista run-interface

all: $(ALVOS)
ifneq ($(TEM_GTK),sim)
	@echo "Aviso: GTK3 nao encontrado, a interface grafica nao foi compilada (rode: make deps)."
endif

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/servidor: $(SERVIDOR_SRC) $(GRAFO_SRC) $(CJSON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lmicrohttpd -pthread -lm

$(BIN_DIR)/cliente: $(CLIENTE_SRC) $(CJSON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm

$(BIN_DIR)/motorista: $(MOTORISTA_SRC) $(CJSON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm

$(BIN_DIR)/interface: $(INTERFACE_SRC) $(CJSON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $$(pkg-config --cflags --libs gtk+-3.0) -lm

# Instala as dependencias conforme o gerenciador de pacotes da distribuicao
deps:
	@if command -v apt-get >/dev/null 2>&1; then \
		echo ">> Debian/Ubuntu"; \
		sudo apt-get update && \
		sudo apt-get install -y build-essential pkg-config libmicrohttpd-dev libgtk-3-dev; \
	elif command -v dnf >/dev/null 2>&1; then \
		echo ">> Fedora/RHEL"; \
		sudo dnf install -y gcc make pkgconf-pkg-config libmicrohttpd-devel gtk3-devel; \
	elif command -v pacman >/dev/null 2>&1; then \
		echo ">> Arch"; \
		sudo pacman -S --needed --noconfirm base-devel pkgconf libmicrohttpd gtk3; \
	else \
		echo "Gerenciador de pacotes nao suportado. Instale manualmente: gcc, make, pkg-config, libmicrohttpd (dev) e GTK3 (dev)."; \
		exit 1; \
	fi
	@echo ">> Dependencias instaladas. (Docker e opcional: https://docs.docker.com/engine/install/)"

# Cria as pastas que o servidor usa em tempo de execucao
dirs:
	mkdir -p logs trechosCadastrados dados ArquivoID Avisos Itinerarios

run-servidor: $(BIN_DIR)/servidor dirs
	./$(BIN_DIR)/servidor

run-cliente: $(BIN_DIR)/cliente
	./$(BIN_DIR)/cliente

run-motorista: $(BIN_DIR)/motorista
	./$(BIN_DIR)/motorista

run-interface: $(BIN_DIR)/interface
	./$(BIN_DIR)/interface

# Contexto de build = raiz do projeto (o "." no final)
docker-build:
	docker build -t app-servidor  -f $(DOCKER_DIR)/DockerFile.Servidor .
	docker build -t app-cliente   -f $(DOCKER_DIR)/DockerFile.Cliente .
	docker build -t app-motorista -f $(DOCKER_DIR)/DockerFile.Motorista .

clean:
	rm -rf $(BIN_DIR)

help:
	@echo "make deps | make | make dirs | make run-servidor | make run-cliente | make run-motorista | make run-interface | make docker-build | make clean"