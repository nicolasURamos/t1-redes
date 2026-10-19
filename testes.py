#!/usr/bin/env python3
"""Testes de integracao.
Versao 1: apoio ChatGPT/OpenAI, 05/10/2026.
Versao 2 (testes de QoS): apoio Claude Code (Anthropic), 2026. Ver README.
"""
import concurrent.futures
import hashlib
import http.client
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
IMAGEM = ROOT / 'www/imagem1.bmp'


def porta_livre():
    with socket.socket() as reserva:
        reserva.bind(('127.0.0.1', 0))
        return reserva.getsockname()[1]


def iniciar(pasta, vazao, taxas, saida=subprocess.DEVNULL):
    """Sobe o servidor em porta livre com um arquivo de taxas temporario."""
    porta = porta_livre()
    arquivo = Path(tempfile.mkstemp(prefix='taxas_', suffix='.txt')[1])
    arquivo.write_text(taxas)
    processo = subprocess.Popen(
        [str(ROOT / 'servidor'), str(porta), str(pasta), str(vazao), str(arquivo)],
        stdout=saida, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if processo.poll() is not None:
            raise RuntimeError('O servidor encerrou antes dos testes.')
        try:
            with socket.create_connection(('127.0.0.1', porta), timeout=1):
                break
        except OSError:
            time.sleep(0.05)
    else:
        raise RuntimeError('Servidor nao iniciou.')
    time.sleep(0.1)  # a conexao de sondagem acima precisa ser liberada
    return processo, porta, arquivo


def encerrar(processo, arquivo):
    processo.terminate()
    try:
        processo.wait(timeout=5)
    except subprocess.TimeoutExpired:
        processo.kill()
        processo.wait()
    arquivo.unlink(missing_ok=True)


def ler(sock, head=False):
    r = http.client.HTTPResponse(sock, method='HEAD' if head else 'GET')
    r.begin()
    corpo = r.read()
    return r.status, dict(r.getheaders()), corpo


def testes_v1():
    # Taxa muito alta para 127.0.0.1: os testes da v1 nao sentem o QoS.
    processo, porta, arquivo = iniciar(ROOT / 'www', 100000000, '127.0.0.1 10000000\n')

    def conectar():
        return socket.create_connection(('127.0.0.1', porta), timeout=5)

    def pedir(caminho, metodo='GET'):
        with conectar() as s:
            s.sendall(f'{metodo} {caminho} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode())
            return ler(s, metodo == 'HEAD')
    try:
        html = (ROOT / 'www/index.html').read_bytes()
        status, headers, corpo = pedir('/')
        assert status == 200 and corpo == html
        assert int(headers['Content-Length']) == len(html)
        print('OK: HTML e Content-Length')

        # O mesmo socket envia duas requisicoes sequenciais, sem reconectar.
        with conectar() as s:
            s.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')
            assert ler(s)[2] == html
            s.sendall(b'GET /ausente HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            assert ler(s)[0] == 404
            assert s.recv(1) == b''
        print('OK: persistencia na mesma conexao e Connection: close')

        # Requisicao fragmentada e duas requisicoes no mesmo envio TCP.
        with conectar() as s:
            s.sendall(b'GET / HTTP/1.1\r\nHo')
            time.sleep(0.02)
            s.sendall(b'st: localhost\r\n\r\nGET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            # Leitura propria mantem os bytes que ja pertencem a segunda resposta.
            recebido = bytearray()
            while True:
                bloco = s.recv(65536)
                if not bloco:
                    break
                recebido.extend(bloco)
            dados = bytes(recebido)
            for _ in range(2):
                cab, dados = dados.split(b'\r\n\r\n', 1)
                assert cab.startswith(b'HTTP/1.1 200 ')
                n = int(next(l.split(b':', 1)[1] for l in cab.split(b'\r\n') if l.lower().startswith(b'content-length:')))
                assert dados[:n] == html
                dados = dados[n:]
            assert dados == b''
        print('OK: fragmentacao e pipelining')

        status, headers, corpo = pedir('/', 'HEAD')
        assert status == 200 and not corpo and int(headers['Content-Length']) == len(html)
        assert pedir('/nao-existe')[0] == 404
        assert pedir('/../README.md')[0] == 403
        assert pedir('/%2e%2e/README.md')[0] == 403
        assert pedir('/%00')[0] == 400
        assert pedir('/', 'POST')[0] == 405
        assert pedir('/?teste=1')[2] == html
        print('OK: HEAD, 404, metodo e protecao de caminhos')

        invalidas = [
            (b'GET / HTTP/1.1\r\n\r\n', 400),
            (b'GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n', 400),
            (b'GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nxx', 413),
            (b'GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n', 501),
            (b'GET / HTTP/1.0\r\n\r\n', 505),
            (b'GET / HTTP/1.1\r\nHost: x\r\nX: ' + b'a' * 17000, 431),
        ]
        for requisicao, esperado in invalidas:
            with conectar() as s:
                s.sendall(requisicao)
                assert ler(s)[0] == esperado
        print('OK: rejeicao de requisicoes fora do escopo')

        original = IMAGEM.read_bytes()
        esperado = hashlib.sha256(original).digest()

        def download(_):
            status, _, corpo = pedir('/imagem1.bmp')
            assert status == 200 and hashlib.sha256(corpo).digest() == esperado
        # Uma conexao incompleta nao pode bloquear o atendimento das demais.
        with conectar() as lento:
            lento.sendall(b'GET / HTTP/1.1\r\nHost:')
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                list(pool.map(download, range(24)))
        print('OK: 24 downloads, 12 workers, integridade binaria e cliente lento')
    finally:
        encerrar(processo, arquivo)


def testes_qos():
    # Pasta temporaria: pagina, imagem de 3 MB e um objeto pequeno de 125 kB.
    pasta = Path(tempfile.mkdtemp(prefix='www_qos_'))
    shutil.copy(ROOT / 'www/index.html', pasta / 'index.html')
    shutil.copy(IMAGEM, pasta / 'imagem1.bmp')
    (pasta / 'pequeno.bin').write_bytes(bytes(125000))
    painel = open(pasta / 'painel.txt', 'w')
    # Vazao maxima 10000 kbps; 127.0.0.1 = 8000, 127.0.0.2 = 4000;
    # demais IPs (ex.: 127.0.0.3) = 1000 por padrao.
    processo, porta, arquivo = iniciar(pasta, 10000, '127.0.0.1 8000\n127.0.0.2 4000\n', painel)
    imagem = IMAGEM.read_bytes()
    bits_imagem = len(imagem) * 8

    def conectar(origem):
        # Qualquer 127.x.x.x e loopback no Linux; o servidor ve IPs distintos.
        return socket.create_connection(('127.0.0.1', porta), timeout=30,
                                        source_address=(origem, 0))

    def pedir(origem, caminho, metodo='GET'):
        with conectar(origem) as s:
            s.sendall(f'{metodo} {caminho} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'.encode())
            return ler(s, metodo == 'HEAD')

    def cronometrar(origem, caminho):
        inicio = time.monotonic()
        status, _, corpo = pedir(origem, caminho)
        return status, corpo, time.monotonic() - inicio

    try:
        # 1. HTML nao sofre limitacao de taxa.
        status, _, segundos = cronometrar('127.0.0.1', '/')
        assert status == 200 and segundos < 0.3, segundos
        print(f'OK QoS: HTML sem limite de taxa ({segundos * 1000:.1f} ms)')

        # 2. Um download segue a taxa do IP (8000 kbps).
        status, corpo, segundos = cronometrar('127.0.0.1', '/imagem1.bmp')
        esperado = bits_imagem / 8000e3
        assert status == 200 and corpo == imagem
        assert 0.85 * esperado < segundos < 1.25 * esperado, (segundos, esperado)
        print(f'OK QoS: 1 download a 8000 kbps em {segundos:.2f} s '
              f'(esperado {esperado:.2f} s, medido {bits_imagem / segundos / 1000:.0f} kbps)')

        # 3. Duas conexoes do mesmo IP dividem a taxa (cada uma ~4000 kbps).
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            tempos = list(pool.map(lambda _: cronometrar('127.0.0.1', '/imagem1.bmp')[2], range(2)))
        esperado = 2 * bits_imagem / 8000e3
        for t in tempos:
            assert 0.85 * esperado < t < 1.25 * esperado, (tempos, esperado)
        print(f'OK QoS: 2 conexoes do mesmo IP dividem a taxa ({tempos[0]:.2f} s e {tempos[1]:.2f} s, '
              f'esperado {esperado:.2f} s)')

        # 4. IP nao cadastrado usa 1000 kbps (125 kB = 1 Mbit -> ~1 s).
        status, corpo, segundos = cronometrar('127.0.0.3', '/pequeno.bin')
        assert status == 200 and len(corpo) == 125000
        assert 0.75 < segundos < 1.3, segundos
        print(f'OK QoS: IP nao cadastrado a 1000 kbps ({segundos:.2f} s para 1 Mbit)')

        # 5. Controle de admissao: 8000 (127.0.0.1) + 1000 (127.0.0.3) ocupados;
        #    127.0.0.2 (4000) nao cabe em 10000 -> 503 com Retry-After.
        a = conectar('127.0.0.1')
        b = conectar('127.0.0.3')
        for s in (a, b):  # garante que as duas conexoes ja foram admitidas
            s.sendall(b'HEAD / HTTP/1.1\r\nHost: x\r\n\r\n')
            assert ler(s, True)[0] == 200
        status, headers, _ = pedir('127.0.0.2', '/')
        assert status == 503 and headers.get('Retry-After') == '5', (status, headers)
        # Mais uma conexao de um IP ja admitido entra (divide a taxa dele).
        assert pedir('127.0.0.1', '/', 'HEAD')[0] == 200
        a.close()
        b.close()
        time.sleep(0.3)  # o servidor percebe o fechamento e libera a vazao
        assert pedir('127.0.0.2', '/', 'HEAD')[0] == 200
        print('OK QoS: admissao (503 + Retry-After) e liberacao da vazao ao fechar')

        # 6. RTT: HTML seguido do primeiro objeto na mesma conexao (como wget -p).
        with conectar('127.0.0.2') as s:
            s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
            assert ler(s)[0] == 200
            s.sendall(b'GET /pequeno.bin HTTP/1.1\r\nHost: x\r\n\r\n')
            status, _, _ = ler(s)  # ~250 ms a 4000 kbps
            assert status == 200
            time.sleep(1.3)  # um quadro do painel com a conexao ainda aberta
        painel.flush()
        linhas = [l.split() for l in (pasta / 'painel.txt').read_text().splitlines()
                  if l.startswith('127.0.0.2')]
        medidas = [float(l[4]) for l in linhas if len(l) == 7 and l[4] != '-']
        assert medidas and 0 < medidas[-1] < 50, linhas[-3:]
        print(f'OK QoS: painel mostra RTT de 127.0.0.2 ({medidas[-1]:.3f} ms em loopback)')
    finally:
        encerrar(processo, arquivo)
        painel.close()
        shutil.rmtree(pasta, ignore_errors=True)


if __name__ == '__main__':
    testes_v1()
    testes_qos()
    print('Todos os testes passaram. Isso nao substitui Wireshark/IPTraf.')
