#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
simulador.py - simulador de passageiros (sem interface gráfica) do consórcio VaiJunto.

Gera, de forma autônoma e concorrente, um grande volume de buscas de itinerários e reservas a partir dos
servidores de TODAS as empresas ao mesmo tempo. Cada passageiro conecta-se apenas à SUA empresa (protocolo TCP/JSON
dos clientes do Problema 1); os itinerários retornados pela busca podem misturar trechos de várias empresas.

Uso (3 empresas no docker-compose local):
  python3 simulador.py --empresas 1=localhost:65431:8081,2=localhost:65432:8082,3=localhost:65433:8083 \
         --semear --threads 32 --duracao 30 --passageiros-por-empresa 40

Uso em computadores diferentes do laboratório:
  python3 simulador.py --empresas 1=192.168.0.11:65432:8080,2=192.168.0.12:65432:8080,3=192.168.0.13:65432:8080 ...

Formato de --empresas:  <id>=<host>:<porta dos clientes>:<porta REST>[,...]
Saída: resumo no terminal e, com --saida, um JSON com todos os resultados (usado pelo teste automatizado).
"""
import argparse
import json
import os
import random
import sys
import threading
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vaijunto as vj  # noqa: E402


def rodar_carga(empresas, passageiros, threads, duracao, semente=None, pausa=0.0, parar=None, log=print):
    """Executa 'threads' passageiros virtuais durante 'duracao' segundos (ou até o evento 'parar').
    Retorna a lista de vj.Resultado."""
    resultados, trava = [], threading.Lock()
    fim = time.time() + duracao
    base = semente if semente is not None else int(time.time())

    def trabalhador(k):
        rng = random.Random(base * 1000 + k)
        while time.time() < fim and not (parar and parar.is_set()):
            emp, nome, email = rng.choice(passageiros)
            r = vj.uma_reserva(emp, nome, email, rng)
            with trava:
                resultados.append(r)
            if pausa:
                time.sleep(pausa)

    ths = [threading.Thread(target=trabalhador, args=(k,), daemon=True) for k in range(threads)]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    return resultados


def resumo(resultados, duracao):
    por_status = Counter(r.status.split(":")[0] if r.status.startswith(("REDE", "BUSCA")) else r.status for r in resultados)
    lat = [r.lat for r in resultados if r.status == "CONFIRMADO"]
    lat_all = [r.lat for r in resultados]
    linhas = [f"requisições: {len(resultados)} em {duracao:.1f}s ({len(resultados) / max(duracao, 1e-9):.1f}/s)"]
    for st, n in por_status.most_common():
        linhas.append(f"  {st:<28} {n}")
    if lat:
        linhas.append(f"latência da confirmação (s): p50={vj.percentil(lat, 50):.3f} p95={vj.percentil(lat, 95):.3f} máx={max(lat):.3f}")
    if lat_all:
        linhas.append(f"latência busca+confirmação (s): p50={vj.percentil(lat_all, 50):.3f} p95={vj.percentil(lat_all, 95):.3f}")
    por_emp = Counter(r.empresa for r in resultados if r.status == "CONFIRMADO")
    linhas.append("confirmados por empresa de origem do passageiro: " + ", ".join(f"E{k}={v}" for k, v in sorted(por_emp.items())))
    return "\n".join(linhas)


def confirmados_para_auditoria(resultados):
    return [{"tx": r.tx, "trechos": r.trechos, "email": r.email, "empresa": r.empresa}
            for r in resultados if r.status == "CONFIRMADO"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--empresas", required=True, help="1=host:portaClientes:portaRest,2=...")
    ap.add_argument("--semear", action="store_true", help="cadastra motoristas/caronas do cenário antes da carga")
    ap.add_argument("--passageiros-por-empresa", type=int, default=40)
    ap.add_argument("--threads", type=int, default=24)
    ap.add_argument("--duracao", type=float, default=30.0, help="segundos de carga")
    ap.add_argument("--pausa", type=float, default=0.0, help="pausa (s) entre ciclos de cada passageiro virtual")
    ap.add_argument("--semente", type=int, default=None)
    ap.add_argument("--auditar", action="store_true", help="ao final, audita as invariantes (servidores com MODO_TESTE=1)")
    ap.add_argument("--saida", help="grava todos os resultados em JSON")
    a = ap.parse_args()

    empresas = vj.parse_empresas(a.empresas)
    if not vj.aguardar_no_ar(empresas, 20):
        sys.exit("alguma empresa não respondeu em /api/v1/health")
    if a.semear:
        print("Semeando o cenário...")
        vj.semear(empresas)
    print("Cadastrando passageiros...")
    passageiros = vj.cadastrar_passageiros(empresas, a.passageiros_por_empresa)
    print(f"Carga: {a.threads} passageiros virtuais por {a.duracao:.0f}s em {len(empresas)} empresas")
    ini = time.time()
    res = rodar_carga(empresas, passageiros, a.threads, a.duracao, a.semente, a.pausa)
    dur = time.time() - ini
    print(resumo(res, dur))
    if a.saida:
        with open(a.saida, "w", encoding="utf-8") as f:
            json.dump([{"empresa": r.empresa, "email": r.email, "status": r.status, "tx": r.tx,
                        "trechos": r.trechos, "lat": r.lat, "t": r.t} for r in res], f)
    if a.auditar:
        ok, t, pend = vj.esperar_sem_bloqueios(empresas, 30)
        viol = vj.auditar(empresas, confirmados_para_auditoria(res))
        for v in viol:
            print("  VIOLAÇÃO:", v)
        print("AUDITORIA:", "OK" if not viol else "FALHOU")
        sys.exit(0 if not viol else 1)


if __name__ == "__main__":
    main()
