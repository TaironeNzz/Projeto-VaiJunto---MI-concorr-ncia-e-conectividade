#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vaijunto.py - biblioteca usada pelo simulador de passageiros e pelo teste automatizado do consórcio VaiJunto.

Contém:
  * Empresa           - endereço de uma empresa (porta dos clientes + porta da API REST);
  * cliente TCP/JSON  - o MESMO protocolo dos clientes cliente.c / motorista.c (cada empresa só é acessada
                        pelos seus próprios usuários, como exige o enunciado);
  * semear()          - cadastra motoristas e caronas de um cenário com disputa de assentos entre empresas;
  * auditar()         - verifica as invariantes do consórcio lendo o estado de TODAS as empresas pela API REST
                        (requer MODO_TESTE=1 nos servidores, que expõe os campos de auditoria).

Apenas biblioteca padrão do Python 3.8+.
"""
import http.client
import json
import random
import socket
import time
from datetime import datetime, timedelta

ID_STRIDE = 1_000_000          # mesmo valor de consorcio.h


class Empresa:
    def __init__(self, eid, host, porta_clientes, porta_rest):
        self.id = int(eid)
        self.host = host
        self.porta_clientes = int(porta_clientes)
        self.porta_rest = int(porta_rest)

    def __repr__(self):
        return f"Empresa({self.id} {self.host} tcp={self.porta_clientes} rest={self.porta_rest})"


def parse_empresas(texto):
    """'1=host:65432:8080,2=host:65432:8080' -> [Empresa,...]"""
    out = []
    for item in texto.split(","):
        eid, resto = item.split("=")
        host, pc, pr = resto.split(":")
        out.append(Empresa(eid, host, pc, pr))
    return out


# ------------------------------------------------------------------------------------------ TCP (clientes)
def tcp_requisicao(emp, obj, timeout=15.0):
    """Envia um JSON ao servidor da empresa e devolve a resposta (objeto JSON ou texto puro).
    Lança OSError/socket.timeout se a empresa estiver fora do ar."""
    with socket.create_connection((emp.host, emp.porta_clientes), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall(json.dumps(obj, ensure_ascii=False).encode("utf-8"))
        dados = s.recv(65536)
        if not dados:
            raise ConnectionError("conexão fechada pelo servidor")
        txt = dados.decode("utf-8", "replace")
        if txt[:1] in "[{":                       # resposta JSON pode vir em vários segmentos TCP
            while True:
                try:
                    return json.loads(txt)
                except json.JSONDecodeError:
                    mais = s.recv(65536)
                    if not mais:
                        raise ConnectionError("JSON incompleto")
                    txt += mais.decode("utf-8", "replace")
        return txt.strip()


def cadastrar_cliente(emp, nome, email, senha="123"):
    return tcp_requisicao(emp, {"classe": "Cliente", "acao": "cadastro", "nome": nome, "email": email,
                                "senha": senha, "status": ""})


def cadastrar_motorista(emp, nome, email, senha="123"):
    return tcp_requisicao(emp, {"classe": "Motorista", "acao": "cadastro", "nome": nome, "email": email,
                                "senha": senha, "status": ""})


def cadastrar_trecho(emp, nome, email, origem, destino, data, hora, capacidade, preco):
    return tcp_requisicao(emp, {"classe": "Motorista", "acao": "cadastrar_trecho", "emailMotorista": email,
                                "nome": nome, "origem": origem, "destino": destino, "data": data, "hora": hora,
                                "capacidade": capacidade, "preco": preco})


def buscar_rotas(emp, origem, destino, data=None, timeout=15.0):
    req = {"classe": "Cliente", "acao": "buscar_rotas", "origem": origem, "destino": destino}
    if data:
        req["data"] = data
    return tcp_requisicao(emp, req, timeout)


def confirmar_itinerario(emp, nome, email, origem, destino, trechos, timeout=30.0):
    """trechos: lista de dicts {id|idTrecho, origem, destino} exatamente como devolvidos por buscar_rotas.
    Retorna a resposta crua: dict {"status":"ITINERARIO_CONFIRMADO",...} ou texto de erro."""
    rota = [{"idTrecho": t.get("idTrecho", t.get("id")), "origem": t["origem"], "destino": t["destino"]}
            for t in trechos]
    return tcp_requisicao(emp, {"classe": "Cliente", "acao": "confirmar_itinerario", "emailCliente": email,
                                "nomeCliente": nome, "origemRota": origem, "destinoRota": destino,
                                "rota": rota}, timeout)


# ------------------------------------------------------------------------------------------ REST (auditoria)
def rest(emp, caminho, metodo="GET", corpo=None, timeout=5.0):
    """Chamada à API REST de uma empresa. Retorna (status, json|None). Lança OSError se estiver fora do ar."""
    c = http.client.HTTPConnection(emp.host, emp.porta_rest, timeout=timeout)
    try:
        c.request(metodo, caminho, body=json.dumps(corpo) if corpo is not None else None,
                  headers={"Content-Type": "application/json"})
        r = c.getresponse()
        raw = r.read().decode("utf-8", "replace")
        try:
            return r.status, json.loads(raw) if raw else None
        except json.JSONDecodeError:
            return r.status, raw
    finally:
        c.close()


def empresa_no_ar(emp, timeout=1.5):
    try:
        st, _ = rest(emp, "/api/v1/health", timeout=timeout)
        return st == 200
    except OSError:
        return False


def aguardar_no_ar(empresas, limite=30.0):
    fim = time.time() + limite
    while time.time() < fim:
        if all(empresa_no_ar(e) for e in empresas):
            return True
        time.sleep(0.3)
    return False


# ------------------------------------------------------------------------------------------ cenário
def data_futura(dias=7):
    return (datetime.now() + timedelta(days=dias)).strftime("%d/%m/%Y")


# (empresa, origem, destino, capacidade). Os trechos formam itinerários que atravessam 2 e 3 empresas e
# competem pelos mesmos últimos assentos (T2, T5 e T6 são gargalos). T9/T10 formam um itinerário E1+E3 que NÃO
# depende da empresa 2: serve de "sonda" para provar que as demais empresas seguem operando quando ela cai.
CENARIO = [
    (1, "Salvador", "Simoes_Filho", 40),             # T1
    (2, "Simoes_Filho", "Feira_de_Santana", 25),      # T2  (gargalo)
    (3, "Feira_de_Santana", "Serrinha", 30),          # T3
    (3, "Salvador", "Feira_de_Santana", 10),          # T4  (direto, concorre com T1+T2)
    (2, "Feira_de_Santana", "Serrinha", 12),          # T5  (concorre com T3)
    (1, "Feira_de_Santana", "Serrinha", 8),           # T6  (poucos assentos, muita disputa)
    (1, "Serrinha", "Alagoinhas", 20),                # T7
    (3, "Simoes_Filho", "Serrinha", 10),              # T8
    (1, "Lauro_de_Freitas", "Salvador", 60),          # T9  (sonda E1+E3)
    (3, "Salvador", "Camacari", 60),                  # T10 (sonda E1+E3)
]
PARES_BUSCA = [("Salvador", "Serrinha"), ("Salvador", "Feira_de_Santana"), ("Feira_de_Santana", "Serrinha"),
               ("Simoes_Filho", "Serrinha"), ("Salvador", "Alagoinhas")]
PAR_SONDA = ("Lauro_de_Freitas", "Camacari")


def semear(empresas, cenario=None, log=print):
    """Cadastra um motorista por empresa e as caronas do cenário (cada carona só na empresa do motorista)."""
    cenario = cenario or CENARIO
    data = data_futura()
    por_id = {e.id: e for e in empresas}
    for e in empresas:
        r = cadastrar_motorista(e, f"Motorista{e.id}", f"motorista{e.id}@empresa{e.id}.com")
        log(f"  empresa {e.id}: motorista -> {r}")
    for (eid, o, d, cap) in cenario:
        e = por_id.get(eid)
        if e is None:
            continue
        r = cadastrar_trecho(e, f"Motorista{eid}", f"motorista{eid}@empresa{eid}.com", o, d, data, "23:00", cap, 25.0)
        log(f"  empresa {eid}: {o} -> {d} (cap {cap}) -> {r}")
        if r != "TRECHO_CADASTRADO":
            raise RuntimeError(f"falha ao cadastrar trecho {o}->{d} na empresa {eid}: {r}")
    return data


def cadastrar_passageiros(empresas, por_empresa, prefixo="p"):
    """Cria passageiros por empresa. Retorna lista de (Empresa, nome, email)."""
    out = []
    for e in empresas:
        for i in range(por_empresa):
            nome, email = f"{prefixo}{e.id}_{i}", f"{prefixo}{e.id}_{i}@empresa{e.id}.com"
            cadastrar_cliente(e, nome, email)
            out.append((e, nome, email))
    return out


# ------------------------------------------------------------------------------------------ auditoria
def coletar_estado(empresas, timeout=10.0):
    """Lê todos os trechos (com campos de auditoria) de todas as empresas. {(empresa, idTrecho): trecho}"""
    estado = {}
    for e in empresas:
        st, arr = rest(e, "/api/v1/trechos?auditoria=1", timeout=timeout)
        if st != 200 or not isinstance(arr, list):
            raise RuntimeError(f"empresa {e.id}: GET /trechos devolveu {st}")
        for t in arr:
            if "reservas" not in t:
                raise RuntimeError("campos de auditoria ausentes: suba os servidores com MODO_TESTE=1")
            estado[(e.id, t["idTrecho"])] = t
    return estado


def esperar_sem_bloqueios(empresas, limite=60.0, log=print):
    """Espera até que nenhum assento esteja bloqueado (PREPARED/em dúvida). Retorna (ok, tempo_gasto, pendentes)."""
    ini = time.time()
    pend = []
    while time.time() - ini < limite:
        try:
            est = coletar_estado(empresas)
            pend = [(k, t["bloqueios"]) for k, t in est.items() if t["bloqueios"]]
            if not pend:
                return True, time.time() - ini, []
        except (OSError, RuntimeError) as ex:
            pend = [("erro", str(ex))]
        time.sleep(0.5)
    return False, time.time() - ini, pend


def auditar(empresas, confirmados=None, log=print):
    """Verifica as invariantes. 'confirmados' = lista de dicts do simulador com status CONFIRMADO
    ({"tx":..., "trechos":[idGlobal,...], "email":...}). Retorna lista de violações (vazia = tudo certo).

    I1  nenhum assento vendido duas vezes: reservas <= capacidade total, sem passageiro repetido no trecho,
        e capacidade + reservas + bloqueios == capacidade total (conservação de assentos);
    I2  nenhum itinerário pela metade: toda transação com reserva em algum trecho foi decidida COMMIT pelo seu
        coordenador e tem reserva em TODOS os seus trechos; todo itinerário confirmado ao passageiro idem;
    I3  nenhum assento bloqueado: nenhum trecho com bloqueio ('PREPARED') pendente."""
    viol = []
    est = coletar_estado(empresas)
    por_id = {e.id: e for e in empresas}

    # ---- I1
    for (eid, tid), t in est.items():
        n_res = len(t["reservas"])
        pas = t["passageiros"]
        if n_res != t["clientes"] or len(pas) != t["clientes"]:
            viol.append(f"I1 trecho {eid}/{tid}: contagem inconsistente (reservas={n_res}, clientes={t['clientes']}, passageiros={len(pas)})")
        if len(set(pas)) != len(pas):
            viol.append(f"I1 trecho {eid}/{tid}: passageiro repetido {sorted(pas)}")
        if t["clientes"] > t["capacidadeTotal"]:
            viol.append(f"I1 trecho {eid}/{tid}: {t['clientes']} reservas para {t['capacidadeTotal']} assentos")
        if t["capacidade"] < 0:
            viol.append(f"I1 trecho {eid}/{tid}: capacidade negativa ({t['capacidade']})")
        if t["capacidade"] + t["clientes"] + len(t["bloqueios"]) != t["capacidadeTotal"]:
            viol.append(f"I1 trecho {eid}/{tid}: livres({t['capacidade']}) + reservados({t['clientes']}) + bloqueados({len(t['bloqueios'])}) != total({t['capacidadeTotal']})")
        txs = [r["tx"] for r in t["reservas"]]
        if len(set(txs)) != len(txs):
            viol.append(f"I1 trecho {eid}/{tid}: a mesma transação reservou dois assentos")

    # ---- I2
    reservas_por_tx = {}
    for (eid, tid), t in est.items():
        for r in t["reservas"]:
            reservas_por_tx.setdefault(r["tx"], set()).add((eid, tid))
    cache_coord = {}

    def decisao(tx):
        if tx in cache_coord:
            return cache_coord[tx]
        try:
            coord = int(tx[1:].split("-")[0])
            st, j = rest(por_id[coord], f"/api/v1/transacoes/{tx}?papel=coordenador")
            cache_coord[tx] = j if st == 200 else None
        except (OSError, KeyError, ValueError):
            cache_coord[tx] = None
        return cache_coord[tx]

    for tx, onde in reservas_por_tx.items():
        j = decisao(tx)
        if not j:
            viol.append(f"I2 tx {tx}: tem reserva mas o coordenador não respondeu")
            continue
        if j.get("decisao") != "COMMIT":
            viol.append(f"I2 tx {tx}: reserva existe mas a decisão do coordenador é {j.get('decisao')}")
            continue
        esperados = {(x["empresaId"], x["idTrecho"]) for x in j.get("trechos", [])}
        if esperados != onde:
            viol.append(f"I2 tx {tx}: itinerário pela metade (esperado {sorted(esperados)}, reservado {sorted(onde)})")

    for c in (confirmados or []):
        onde = reservas_por_tx.get(c["tx"], set())
        esperados = {((g // ID_STRIDE) or c["empresa"], g % ID_STRIDE) for g in c["trechos"]}
        if esperados != onde:
            viol.append(f"I2 itinerário confirmado {c['tx']} ao passageiro {c['email']}: esperado {sorted(esperados)}, reservado {sorted(onde)}")

    # ---- I3
    for (eid, tid), t in est.items():
        if t["bloqueios"]:
            viol.append(f"I3 trecho {eid}/{tid}: assento(s) ainda bloqueado(s) por {t['bloqueios']}")

    total_assentos = sum(t["capacidadeTotal"] for t in est.values())
    total_vendidos = sum(t["clientes"] for t in est.values())
    log(f"  auditoria: {len(est)} trechos | {total_vendidos}/{total_assentos} assentos vendidos | "
        f"{len(reservas_por_tx)} transações com reserva | {len(viol)} violação(ões)")
    return viol


# ------------------------------------------------------------------------------------------ gerador de carga
class Resultado:
    __slots__ = ("empresa", "email", "status", "tx", "trechos", "lat", "t")

    def __init__(self, empresa, email, status, tx, trechos, lat, t):
        self.empresa, self.email, self.status, self.tx, self.trechos, self.lat, self.t = empresa, email, status, tx, trechos, lat, t


def uma_reserva(emp, nome, email, rng, pares=None, data=None):
    """Um ciclo de passageiro: busca itinerários e tenta confirmar um deles. Retorna Resultado."""
    o, d = rng.choice(pares or PARES_BUSCA)
    t0 = time.time()
    try:
        rotas = buscar_rotas(emp, o, d, data)
        if not isinstance(rotas, list):
            return Resultado(emp.id, email, f"BUSCA_ERRO:{rotas}", None, [], time.time() - t0, t0)
        if not rotas:
            time.sleep(0.05)       # tudo esgotado: não faz busy-loop
            return Resultado(emp.id, email, "SEM_ROTAS", None, [], time.time() - t0, t0)
        rota = rng.choice(rotas)["trechos"]
        r = confirmar_itinerario(emp, nome, email, o, d, rota)
    except (OSError, ConnectionError, json.JSONDecodeError) as ex:
        return Resultado(emp.id, email, f"REDE_ERRO:{type(ex).__name__}", None, [], time.time() - t0, t0)
    lat = time.time() - t0
    if isinstance(r, dict) and r.get("status") == "ITINERARIO_CONFIRMADO":
        return Resultado(emp.id, email, "CONFIRMADO", r.get("idTransacao"), [t["id"] for t in rota], lat, t0)
    return Resultado(emp.id, email, str(r), None, [t["id"] for t in rota], lat, t0)


def percentil(valores, p):
    if not valores:
        return 0.0
    v = sorted(valores)
    return v[min(len(v) - 1, int(round((p / 100.0) * (len(v) - 1))))]
