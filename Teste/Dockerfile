FROM gcc:latest

WORKDIR /app

# Copia o código fonte para dentro do container
COPY . /app

# Compila os dois binários
RUN gcc servidor.c -o servidor
RUN gcc cliente.c -o cliente

CMD ["./servidor"]