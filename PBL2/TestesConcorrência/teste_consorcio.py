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
import os
import shutil
import subprocess
import sys
import threading
import time

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
        self.empresas = [vj.Empresa(1, "127.0.0.1", 65431, 8081), vj.Empresa(2, "127.0.0.1", 65432, 8082),
                         vj.Empresa(3, "127.0.0.1", 65433, 8083)]

    def _run(self, args, env=None):
        return subprocess.run(self.compose + args, env=dict(os.environ, **AJUSTES, **(env or {})),
                              capture_output=True, text=True)

    def preparar(self):
        self._run(["down", "-v"])
        r = self._run(["up", "-d", "--build"])
        if r.returncode != 0:
            sys.exit("docker compose up falhou:\n" + r.stderr)

    def iniciar(self, eid, crash_at=None):
        # --force-recreate recria o contêiner (mesmo volume => mesmo disco) com/sem CRASH_AT
        self._run(["up", "-d", "--no-deps", "--force-recreate", f"empresa{eid}"],
                  env={f"CRASH_AT_E{eid}": crash_at or ""})

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
    ap.add_argument("--cenarios", default="carga,queda,pontos,controle",
                    help="lista separada por vírgulas: carga, queda, pontos (ou ponto:<nome>), controle")
    ap.add_argument("--duracao", type=float, default=15.0)
    ap.add_argument("--threads", type=int, default=24)
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
            elif c == "controle":
                todas += cenario_controle(be, args)
            elif c.startswith("ponto:"):
                todas += cenario_ponto(be, args, c.split(":", 1)[1])
    finally:
        be.encerrar()

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
