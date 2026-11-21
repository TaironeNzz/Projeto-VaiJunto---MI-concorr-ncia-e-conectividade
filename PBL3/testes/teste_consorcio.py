#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
teste_consorcio.py - teste automatizado do consórcio VaiJunto: carga + injeção de falhas + auditoria.

Verifica, sob carga concorrente vinda das três empresas e com quedas abruptas de uma empresa, que:
  I1  nenhum assento é confirmado para mais de um passageiro;
  I2  nenhum itinerário é confirmado pela metade (atomicidade entre empresas);
  I3  nenhum assento permanece bloqueado depois da queda e da recuperação do servidor;
  D   as demais empresas continuam operando enquanto uma está fora do ar (disponibilidade).

Cenários:
  1. carga        - carga pura (sem falhas);
  2. queda        - `kill -9` / `docker kill` da empresa 2 NO MEIO da carga, com sondas das outras empresas durante a
                    queda; depois a empresa volta (mesmo disco) e o estado converge;
  4. controle     - controle negativo: corrompe de propósito um itinerário e exige que a auditoria o detecte;
  3. ponto:<nome> - falha injetada (CRASH_AT) em pontos críticos do 2PC: part_apos_prepare, part_antes_commit,
                    coord_antes_decisao, coord_apos_decisao (o processo morre com _exit(137) exatamente ali).

Backends:
  --modo local   (padrão) sobe 3 processos do binário em diretórios separados (precisa do binário compilado:
                  gcc ... -o servidor_bin, ver README) - não precisa de Docker;
  --modo docker  usa o docker-compose.yml (contêineres empresa1/2/3 com volumes), derrubando com `docker kill`.

Uso:
  python3 testes/teste_consorcio.py --modo local  --binario ./servidor_bin
  python3 testes/teste_consorcio.py --modo docker
  python3 testes/teste_consorcio.py --cenarios carga,queda --duracao 20
Código de saída 0 = todas as invariantes valeram em todos os cenários.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
sys.path.insert(0, os.path.join(RAIZ, "simulador"))
import vaijunto as vj          # noqa: E402
import simulador as sim        # noqa: E402

# tempos reduzidos para o teste terminar rápido (valores de produção estão em consorcio.c / README)
AJUSTES = {"PREPARE_TIMEOUT_MS": "3000", "DECISAO_TIMEOUT_MS": "1500", "INDOUBT_MS": "1500",
           "SUSPEITA_MS": "1500", "PREPARED_TIMEOUT_MS": "10000", "ESPERA_CONFLITO_MS": "1500", "MODO_TESTE": "1"}
PONTOS_DE_FALHA = ["part_apos_prepare", "part_antes_commit", "coord_antes_decisao", "coord_apos_decisao"]


# ------------------------------------------------------------------------------------------ backends
def achar_mapa():
    """mapa.txt fica em GrafoMapa/ (estrutura do repositório); aceita também a raiz."""
    for c in (os.path.join(RAIZ, "GrafoMapa", "mapa.txt"), os.path.join(RAIZ, "mapa.txt")):
        if os.path.exists(c):
            return c
    sys.exit("mapa.txt não encontrado (esperado em GrafoMapa/mapa.txt)")


class BackendLocal:
    """3 processos do servidor, cada um num diretório próprio (armazenamento NÃO compartilhado)."""
    def __init__(self, binario, base="/tmp/vaijunto_teste"):
        self.binario = os.path.abspath(binario)
        self.base = base
        self.pause_before_decision_ms = 0
        self.procs = {}
        self.empresas = [vj.Empresa(i, "127.0.0.1", 17000 + i, 18000 + i) for i in (1, 2, 3)]

    def preparar(self):
        shutil.rmtree(self.base, ignore_errors=True)
        for e in self.empresas:
            d = os.path.join(self.base, f"e{e.id}")
            os.makedirs(d)
            shutil.copy(achar_mapa(), d)

    def iniciar(self, eid, crash_at=None):
        e = next(x for x in self.empresas if x.id == eid)
        env = dict(os.environ, **AJUSTES, EMPRESA_ID=str(eid), PORTA_CLIENTES=str(e.porta_clientes),
                   PORTA_REST=str(e.porta_rest),
                   TEST_PAUSE_BEFORE_DECISION_MS=str(self.pause_before_decision_ms if eid == 1 else 0),
                   PEERS=",".join(f"{p.id}=http://127.0.0.1:{p.porta_rest}" for p in self.empresas if p.id != eid))
        env.pop("CRASH_AT", None)
        if crash_at:
            env["CRASH_AT"] = crash_at
        log = open(os.path.join(self.base, f"e{eid}", "stdout.log"), "ab")
        self.procs[eid] = subprocess.Popen([self.binario], cwd=os.path.join(self.base, f"e{eid}"), env=env,
                                           stdout=log, stderr=log)

    def matar(self, eid):
        p = self.procs.get(eid)
        if p and p.poll() is None:
            p.kill()          # SIGKILL: queda abrupta, sem nenhuma chance de limpeza
            p.wait()

    def morto(self, eid):
        p = self.procs.get(eid)
        return p is None or p.poll() is not None

    def encerrar(self):
        for eid in list(self.procs):
            self.matar(eid)


class BackendDocker:
    """Contêineres do docker-compose.yml (um volume por empresa). Queda = `docker kill`."""
    def __init__(self):
        self.compose = ["docker", "compose", "-f", os.path.join(RAIZ, "docker-compose.yml")]
        self.pause_before_decision_ms = 0
        self.empresas = [vj.Empresa(1, "127.0.0.1", 65431, 8081), vj.Empresa(2, "127.0.0.1", 65432, 8082),
                         vj.Empresa(3, "127.0.0.1", 65433, 8083)]

    def _run(self, args, env=None):
        padrao = {"TEST_PAUSE_BEFORE_DECISION_MS": str(self.pause_before_decision_ms)}
        padrao.update(env or {})
        return subprocess.run(self.compose + args, env=dict(os.environ, **AJUSTES, **padrao),
                              capture_output=True, text=True)

    def preparar(self):
        self._run(["down", "-v"])
        r = self._run(["up", "-d", "--build"])
        if r.returncode != 0:
            sys.exit("docker compose up falhou:\n" + r.stderr)

    def iniciar(self, eid, crash_at=None):
        # --force-recreate recria o contêiner (mesmo volume => mesmo disco) com/sem CRASH_AT
        self._run(["up", "-d", "--no-deps", "--force-recreate", f"empresa{eid}"],
                  env={f"CRASH_AT_E{eid}": crash_at or "",
                       "TEST_PAUSE_BEFORE_DECISION_MS": str(self.pause_before_decision_ms if eid == 1 else 0)})

    def matar(self, eid):
        subprocess.run(["docker", "kill", f"empresa{eid}"], capture_output=True)

    def morto(self, eid):
        r = subprocess.run(["docker", "inspect", "-f", "{{.State.Running}}", f"empresa{eid}"],
                           capture_output=True, text=True)
        return r.stdout.strip() != "true"

    def encerrar(self):
        self._run(["down", "-v"])


# ------------------------------------------------------------------------------------------ utilidades
def titulo(txt):
    print("\n" + "=" * 100 + f"\n{txt}\n" + "=" * 100, flush=True)


def reiniciar_ambiente(be):
    be.encerrar()
    be.preparar()
    for e in be.empresas:
        be.iniciar(e.id)
    if not vj.aguardar_no_ar(be.empresas, 60):
        sys.exit("as empresas não subiram")
    vj.semear(be.empresas, log=lambda *_: None)
    return vj.cadastrar_passageiros(be.empresas, 30)


def verificar(be, resultados, nome, esperar=90):
    """Espera a convergência (assentos desbloqueados) e audita. Retorna lista de violações."""
    ini = time.time()
    if not vj.aguardar_no_ar(be.empresas, 60):
        return [f"{nome}: alguma empresa não voltou"]
    ok, t, pend = vj.esperar_sem_bloqueios(be.empresas, esperar)
    t = time.time() - ini
    print(f"  convergência: {'ok' if ok else 'NÃO convergiu'} em {t:.1f}s" + ("" if ok else f" pendentes={pend[:3]}"))
    return [f"{nome}: {v}" for v in vj.auditar(be.empresas, sim.confirmados_para_auditoria(resultados))]


def carga_em_thread(be, passageiros, threads, duracao, saida):
    parar = threading.Event()
    def alvo():
        saida.extend(sim.rodar_carga(be.empresas, passageiros, threads, duracao, parar=parar))
    th = threading.Thread(target=alvo, daemon=True)
    th.start()
    return th, parar


# ------------------------------------------------------------------------------------------ cenários
def cenario_carga(be, args):
    titulo("CENÁRIO 1 - carga concorrente sem falhas")
    pas = reiniciar_ambiente(be)
    t0 = time.time()
    res = sim.rodar_carga(be.empresas, pas, args.threads, args.duracao, semente=1)
    print(sim.resumo(res, time.time() - t0))
    viol = verificar(be, res, "carga")
    ok_conf = sum(1 for r in res if r.status == "CONFIRMADO")
    if ok_conf == 0:
        viol.append("carga: nenhuma reserva foi confirmada (teste inconclusivo)")
    cross = sum(1 for r in res if r.status == "CONFIRMADO" and len({g // vj.ID_STRIDE for g in r.trechos}) > 1)
    print(f"  itinerários confirmados que envolvem mais de uma empresa: {cross}")
    return viol


def cenario_queda(be, args):
    titulo("CENÁRIO 2 - queda abrupta da empresa 2 no meio da carga (+ sondas das demais empresas)")
    pas = reiniciar_ambiente(be)
    resultados = []
    th, parar = carga_em_thread(be, pas, args.threads, args.duracao * 2 + 25, resultados)
    time.sleep(args.duracao * 0.4)
    print(f"  [{time.strftime('%H:%M:%S')}] derrubando a empresa 2 (kill -9)")
    be.matar(2)
    viol = []
    # --- disponibilidade: itinerário E1+E3 (sem trechos da empresa 2) deve continuar sendo confirmado
    e1 = next(e for e in be.empresas if e.id == 1)
    sondas = []
    for i in range(6):
        nome, email = f"sonda{i}", f"sonda{i}@empresa1.com"
        vj.cadastrar_cliente(e1, nome, email)
        r = vj.uma_reserva(e1, nome, email, __import__("random").Random(i), pares=[vj.PAR_SONDA])
        sondas.append(r)
        time.sleep(0.3)
    ok_s = sum(1 for r in sondas if r.status == "CONFIRMADO")
    print(f"  sondas durante a queda (itinerário E1+E3, não depende da E2): {ok_s}/{len(sondas)} confirmadas")
    if ok_s < len(sondas):
        viol.append(f"queda: apenas {ok_s}/{len(sondas)} sondas E1+E3 foram confirmadas com a empresa 2 fora do ar "
                    f"({[r.status for r in sondas if r.status != 'CONFIRMADO'][:3]})")
    resultados.extend(sondas)
    time.sleep(args.duracao * 0.4)
    print(f"  [{time.strftime('%H:%M:%S')}] reiniciando a empresa 2 (mesmo disco)")
    be.iniciar(2)
    time.sleep(1.0)
    parar.set()                          # para a carga logo após a volta: mede a convergência da recuperação
    th.join(timeout=90)
    print(sim.resumo(resultados, args.duracao * 1.3))
    viol += verificar(be, resultados, "queda")
    return viol


def cenario_ponto(be, args, ponto):
    titulo(f"CENÁRIO 3 - falha injetada no ponto '{ponto}' da empresa 2")
    pas = reiniciar_ambiente(be)
    be.matar(2)
    be.iniciar(2, crash_at=ponto)
    vj.aguardar_no_ar([be.empresas[1]], 30)
    resultados = []
    th, parar = carga_em_thread(be, pas, args.threads, 60, resultados)
    fim = time.time() + 45
    while time.time() < fim and not be.morto(2):
        time.sleep(0.2)
    caiu = be.morto(2)
    print(f"  empresa 2 {'caiu no ponto injetado' if caiu else 'NÃO chegou ao ponto de falha'}")
    time.sleep(1.5)                      # as outras empresas ficam um tempo sem a E2, sob carga
    be.iniciar(2)                        # volta sem a falha injetada
    parar.set()                          # para a carga JÁ: a convergência medida abaixo é só da recuperação
    th.join(timeout=90)
    viol = [] if caiu else [f"ponto {ponto}: a falha injetada não foi alcançada (teste inconclusivo)"]
    viol += verificar(be, resultados, f"ponto:{ponto}")
    return viol


def cenario_cancelamento_cliente(be, args):
    """Valida que o cliente vê os trechos de todas as empresas e cancela o itinerário completo."""
    titulo("CENÁRIO 5 - listagem e cancelamento pelo cliente em itinerário E1+E2")
    reiniciar_ambiente(be)
    e1 = be.empresas[0]
    nome, email = "ClienteCancelamento", "cliente.cancelamento@empresa1.com"
    vj.cadastrar_cliente(e1, nome, email)

    rotas = vj.buscar_rotas(e1, "Salvador", "Feira_de_Santana")
    rota = next((r["trechos"] for r in rotas
                 if any(t.get("empresaId") == 1 for t in r.get("trechos", []))
                 and any(t.get("empresaId") == 2 for t in r.get("trechos", []))), None)
    if not rota:
        return ["cancelamento-cliente: não encontrou itinerário que envolvesse E1 e E2"]

    confirmado = vj.confirmar_itinerario(e1, nome, email, "Salvador", "Feira_de_Santana", rota)
    if not isinstance(confirmado, dict) or confirmado.get("status") != "ITINERARIO_CONFIRMADO":
        return [f"cancelamento-cliente: confirmação inicial falhou ({confirmado!r})"]
    tx = confirmado.get("idTransacao")

    reservas = vj.tcp_requisicao(e1, {
        "classe": "Cliente", "acao": "listar_caronas", "email": email, "emailCliente": email
    })
    empresas_listadas = {int(r.get("empresaId", -1)) for r in reservas if isinstance(r, dict)} if isinstance(reservas, list) else set()
    viol = []
    if not {1, 2}.issubset(empresas_listadas):
        viol.append(f"cancelamento-cliente: listagem deveria conter E1 e E2, recebeu empresas {sorted(empresas_listadas)}")

    id_remoto = next(int(t["id"]) for t in rota if t.get("empresaId") == 2)
    resposta = vj.tcp_requisicao(e1, {
        "classe": "Cliente", "acao": "cancelar_carona", "emailCliente": email,
        "nomeCliente": nome, "idSelecionado": id_remoto
    })
    print(f"  listagem: empresas {sorted(empresas_listadas)} | cancelamento: {resposta}")
    if resposta not in ("CARONA_CANCELADA", "CANCELAMENTO_PENDENTE"):
        viol.append(f"cancelamento-cliente: resposta inesperada ao cancelar trecho remoto: {resposta!r}")

    # Se a resposta for 202, espera a fila de retransmissão; em operação local saudável deve concluir logo.
    fim = time.time() + 20
    while time.time() < fim:
        try:
            estado = vj.auditar(be.empresas)
            if not estado:
                break
        except (OSError, RuntimeError):
            pass
        time.sleep(0.5)
    else:
        viol.append("cancelamento-cliente: auditoria não convergiu após cancelamento")

    # Confirma diretamente que nenhum dos participantes mantém a reserva da transação.
    for e in be.empresas[:2]:
        st, trechos = vj.rest(e, "/api/v1/trechos?auditoria=1")
        if st != 200:
            viol.append(f"cancelamento-cliente: não conseguiu auditar empresa {e.id}")
            continue
        for trecho in trechos:
            if any(r.get("tx") == tx for r in trecho.get("reservas", [])):
                viol.append(f"cancelamento-cliente: empresa {e.id}, trecho {trecho.get('idTrecho')} ainda mantém tx {tx}")
    return viol


def cenario_cancelamento_prepare(be, args):
    """Cancela, pelo motorista remoto, um trecho que está em PREPARED antes da decisão do coordenador.

    É o caso de corrida que antes não era coberto: o assento aparece em `bloqueios`, não em `reservas`.
    A pausa artificial só é habilitada com MODO_TESTE=1 e abre uma janela determinística para o teste.
    """
    titulo("CENÁRIO 6 - motorista cancela trecho remoto durante PREPARED")
    reiniciar_ambiente(be)
    e1, e2 = be.empresas[0], be.empresas[1]

    # Reinicia apenas a coordenadora (com os mesmos dados) com uma janela controlada após os votos SIM.
    be.pause_before_decision_ms = 2500
    be.matar(1)
    be.iniciar(1)
    if not vj.aguardar_no_ar([e1], 30):
        be.pause_before_decision_ms = 0
        return ["cancelamento/PREPARED: coordenadora E1 não voltou"]

    nome_cliente, email_cliente = "PassageiroCorrida", "corrida.cancelamento@empresa1.com"
    vj.cadastrar_cliente(e1, nome_cliente, email_cliente)
    rotas = vj.buscar_rotas(e1, "Salvador", "Feira_de_Santana")
    rota = next((r["trechos"] for r in rotas
                 if any(t.get("empresaId") == 1 for t in r.get("trechos", []))
                 and any(t.get("empresaId") == 2 for t in r.get("trechos", []))), None)
    if not rota:
        be.pause_before_decision_ms = 0
        be.matar(1)
        be.iniciar(1)
        return ["cancelamento/PREPARED: não encontrou itinerário E1+E2"]

    trecho_e2 = next(t for t in rota if t.get("empresaId") == 2)
    id_global = int(trecho_e2.get("id", trecho_e2.get("idTrecho", -1)))
    id_local = id_global % vj.ID_STRIDE
    saida = {}
    erro = {}

    def confirmar():
        try:
            saida["resposta"] = vj.confirmar_itinerario(
                e1, nome_cliente, email_cliente, "Salvador", "Feira_de_Santana", rota, timeout=30.0)
        except Exception as ex:  # mantemos a exceção para o relatório do cenário
            erro["ex"] = repr(ex)

    th = threading.Thread(target=confirmar, daemon=True)
    th.start()

    # Aguarda o bloqueio PREPARED do trecho da empresa 2.
    preparado = False
    limite = time.time() + 12
    while time.time() < limite:
        try:
            st, trechos = vj.rest(e2, "/api/v1/trechos?auditoria=1", timeout=2)
            if st == 200:
                trecho = next((t for t in trechos if t.get("idTrecho") == id_local), None)
                if trecho and trecho.get("bloqueios"):
                    preparado = True
                    break
        except OSError:
            pass
        time.sleep(0.05)

    viol = []
    if not preparado:
        viol.append("cancelamento/PREPARED: o trecho remoto não entrou em bloqueios dentro do prazo")
    else:
        resp = vj.tcp_requisicao(e2, {
            "classe": "Motorista", "acao": "cancelar_trecho", "nome": "Motorista2",
            "email": "motorista2@empresa2.com", "idSelecionado": id_local
        }, timeout=20.0)
        print(f"  resposta do motorista E2: {resp}")
        if resp != "TRECHO_CANCELADO":
            viol.append(f"cancelamento/PREPARED: motorista recebeu {resp!r}, esperado TRECHO_CANCELADO")

    th.join(timeout=35)
    if th.is_alive():
        viol.append("cancelamento/PREPARED: confirmação do passageiro não terminou")
    if "ex" in erro:
        viol.append("cancelamento/PREPARED: erro na confirmação: " + erro["ex"])
    resposta = saida.get("resposta")
    if isinstance(resposta, dict) and resposta.get("status") == "ITINERARIO_CONFIRMADO":
        viol.append("cancelamento/PREPARED: itinerário foi confirmado mesmo após o ABORT do motorista")
    print(f"  resposta da confirmação concorrente: {resposta}")

    # Restaura E1 sem a pausa antes de a próxima etapa iniciar.
    be.pause_before_decision_ms = 0
    be.matar(1)
    be.iniciar(1)
    if not vj.aguardar_no_ar(be.empresas, 30):
        viol.append("cancelamento/PREPARED: empresas não ficaram disponíveis após recuperação")
    ok, _, pend = vj.esperar_sem_bloqueios(be.empresas, 30)
    if not ok:
        viol.append(f"cancelamento/PREPARED: bloqueios permaneceram após cancelamento: {pend[:3]}")
    viol += vj.auditar(be.empresas)

    # A primeira perna da E1 deve estar livre novamente; a perna E2 deve ter sido removida.
    st, lista_e1 = vj.rest(e1, "/api/v1/trechos?auditoria=1")
    st2, lista_e2 = vj.rest(e2, "/api/v1/trechos?auditoria=1")
    if st != 200 or st2 != 200:
        viol.append("cancelamento/PREPARED: não foi possível consultar os trechos após a operação")
    else:
        primeira_e1 = next((t for t in rota if t.get("empresaId") == 1), None)
        id_e1 = int(primeira_e1.get("id", primeira_e1.get("idTrecho", -1))) % vj.ID_STRIDE
        estado_e1 = next((t for t in lista_e1 if t.get("idTrecho") == id_e1), None)
        if estado_e1 and (estado_e1.get("reservas") or estado_e1.get("bloqueios")):
            viol.append("cancelamento/PREPARED: trecho E1 ainda mantém reserva/bloqueio do itinerário cancelado")
        if any(t.get("idTrecho") == id_local for t in lista_e2):
            viol.append("cancelamento/PREPARED: trecho E2 cancelado pelo motorista ainda aparece publicado")
    return viol


def cenario_controle(be, args):
    """CONTROLE NEGATIVO: prova que a auditoria não é complacente. Reserva um itinerário E1+E2 e depois apaga à mão
    a reserva do trecho da empresa 2 (como se o COMMIT tivesse sido aplicado só pela metade). A auditoria PRECISA
    acusar a violação; se não acusar, o teste inteiro seria inútil. (Só no modo local: edita o disco da empresa.)"""
    import json
    titulo("CENÁRIO 4 - controle negativo (a auditoria tem que detectar um itinerário pela metade)")
    if not hasattr(be, "base"):
        print("  (apenas no modo local; ignorado)")
        return []
    reiniciar_ambiente(be)
    e1 = be.empresas[0]
    rotas = vj.buscar_rotas(e1, "Salvador", "Feira_de_Santana")
    rota = next(r["trechos"] for r in rotas if len(r["trechos"]) == 2)
    r = vj.confirmar_itinerario(e1, "Ana", "ana@empresa1.com", "Salvador", "Feira_de_Santana", rota)
    if not (isinstance(r, dict) and r.get("status") == "ITINERARIO_CONFIRMADO"):
        return [f"controle: não foi possível reservar o itinerário de teste ({r})"]
    limpo = vj.auditar(be.empresas)
    be.matar(2)
    caminho = os.path.join(be.base, "e2", "trechosCadastrados", "trecho0.json")
    j = json.load(open(caminho))
    j["reservas"], j["clientes"], j["nomesClientes"], j["capacidade"] = [], [], [], j["capacidade"] + 1
    json.dump(j, open(caminho, "w"))
    be.iniciar(2)
    vj.aguardar_no_ar(be.empresas, 30)
    sujo = vj.auditar(be.empresas)
    print(f"  antes da corrupção: {len(limpo)} violações | depois: {len(sujo)} violação(ões) detectada(s)")
    erros = []
    if limpo:
        erros.append(f"controle: auditoria acusou violações em estado íntegro: {limpo}")
    if not any(v.startswith("I2") for v in sujo):
        erros.append("controle: a auditoria NÃO detectou o itinerário pela metade")
    return erros


# ------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--modo", choices=["local", "docker"], default="local")
    ap.add_argument("--binario", default=os.path.join(RAIZ, "servidor_bin"), help="binário do servidor (modo local)")
    ap.add_argument("--cenarios", default="carga,queda,pontos,cancelamento-cliente,cancelamento,controle",
                    help="lista: carga, queda, pontos (ou ponto:<nome>), cancelamento-cliente, cancelamento, controle")
    ap.add_argument("--duracao", type=float, default=15.0)
    ap.add_argument("--threads", type=int, default=24)
    ap.add_argument("--relatorio-json", default=None, help="salva o resultado agregado dos cenários em JSON")
    args = ap.parse_args()

    be = BackendDocker() if args.modo == "docker" else BackendLocal(args.binario)
    todas = []
    try:
        for c in args.cenarios.split(","):
            if c == "carga":
                todas += cenario_carga(be, args)
            elif c == "queda":
                todas += cenario_queda(be, args)
            elif c == "pontos":
                for p in PONTOS_DE_FALHA:
                    todas += cenario_ponto(be, args, p)
            elif c == "cancelamento-cliente":
                todas += cenario_cancelamento_cliente(be, args)
            elif c == "cancelamento":
                todas += cenario_cancelamento_prepare(be, args)
            elif c == "controle":
                todas += cenario_controle(be, args)
            elif c.startswith("ponto:"):
                todas += cenario_ponto(be, args, c.split(":", 1)[1])
    finally:
        be.encerrar()

    aprovado = not bool(todas)
    if args.relatorio_json:
        relatorio = {
            "executado_em": datetime.now().astimezone().isoformat(),
            "modo": args.modo,
            "cenarios": args.cenarios.split(","),
            "duracao_carga_s": args.duracao,
            "threads": args.threads,
            "aprovado": aprovado,
            "violacoes": todas,
            "observacao": "O relatório registra o resultado desta execução; não substitui os logs detalhados do simulador."
        }
        pasta = os.path.dirname(os.path.abspath(args.relatorio_json))
        os.makedirs(pasta, exist_ok=True)
        with open(args.relatorio_json, "w", encoding="utf-8") as f:
            json.dump(relatorio, f, ensure_ascii=False, indent=2)
        print(f"  relatório JSON salvo em: {args.relatorio_json}")

    titulo("RESULTADO")
    if todas:
        for v in todas:
            print("  FALHOU:", v)
        print(f"\n{len(todas)} violação(ões)")
        sys.exit(1)
    print("  OK - I1 (nenhum assento duplicado), I2 (nenhum itinerário pela metade), I3 (nenhum assento bloqueado) "
          "e disponibilidade das demais empresas valeram em todos os cenários.")


if __name__ == "__main__":
    main()
