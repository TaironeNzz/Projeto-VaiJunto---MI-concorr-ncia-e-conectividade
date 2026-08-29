FROM gcc:latest

# 1. Instalar a biblioteca cJSON no sistema
RUN apt-get update && apt-get install -y \
    libcjson-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copia o código fonte para dentro do container
COPY . /app

# 2. Compila os dois binários vinculando pthreads e cJSON
RUN gcc -D_XOPEN_SOURCE=600 servidor.c -o servidor -lpthread -lcjson
RUN gcc cliente.c -o cliente -lcjson

CMD ["./servidor"]