#!/usr/bin/env python3
"""Testes de integracao. Apoio: ChatGPT/OpenAI, 05/10/2026; ver README."""
import concurrent.futures
import hashlib
import http.client
from pathlib import Path
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent

def executar():
    # Porta local livre; o servidor e um subprocesso encerrado ao terminar.
    with socket.socket() as reserva:
        reserva.bind(('127.0.0.1', 0))
        porta = reserva.getsockname()[1]
    processo = subprocess.Popen([str(ROOT / 'servidor'), str(porta), str(ROOT / 'www')],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def conectar():
        return socket.create_connection(('127.0.0.1', porta), timeout=5)
    def ler(sock, head=False):
        r = http.client.HTTPResponse(sock, method='HEAD' if head else 'GET')
        r.begin()
        corpo = r.read()
        return r.status, dict(r.getheaders()), corpo
    def pedir(caminho, metodo='GET'):
        with conectar() as s:
            s.sendall(f'{metodo} {caminho} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode())
            return ler(s, metodo == 'HEAD')
    try:
        for _ in range(100):
            if processo.poll() is not None:
                raise RuntimeError('O servidor encerrou antes dos testes.')
            try:
                with conectar():
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError('Servidor nao iniciou.')
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

        original = (ROOT / 'www/imagem1.bmp').read_bytes()
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
        print('Todos os testes passaram. Isso nao substitui Wireshark/IPTraf.')
    finally:
        processo.terminate()
        try:
            processo.wait(timeout=5)
        except subprocess.TimeoutExpired:
            processo.kill()
            processo.wait()

if __name__ == '__main__':
    executar()
