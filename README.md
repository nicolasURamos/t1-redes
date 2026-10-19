# Redes — servidor HTTP com QoS, versão 2

Trabalho de Programação em Rede — Fundamentos e Avaliação de Redes de Computadores — UNIPAMPA 2026/2.

## Equipe

- Rodrigo Silveira
- João Gabriel Oliveira da Silva
- Nicolas Ramos

## Compilar e executar no Linux

Dependências: GCC, Make e biblioteca POSIX Pthreads. Python 3 é necessário somente para os testes automatizados.
No Ubuntu/Debian, se necessário: `sudo apt install build-essential python3 curl wget`.

```sh
make
./servidor [porta] [pasta] [vazao_max_kbps] [arquivo_taxas]
./servidor 8080 www 12000 taxas.txt 2> servidor.log
```

| Parâmetro | Padrão | Significado |
|---|---|---|
| `porta` | 8080 | Porta TCP (o servidor escuta em todas as interfaces IPv4) |
| `pasta` | `www` | Pasta com os arquivos servidos |
| `vazao_max_kbps` | 100000 | Vazão máxima do servidor, usada no controle de admissão |
| `arquivo_taxas` | `taxas.txt` | Taxa máxima por IP de origem |

Abra http://localhost:8080 ou, de outro computador, `http://IP_DO_SERVIDOR:8080`.
Encerre com Ctrl+C. `make clean` remove o executável e os arquivos objeto.

O **painel** é impresso em `stdout` e os **logs** (conexões, recusas, medidas de RTT) em `stderr`;
por isso o exemplo redireciona `2> servidor.log`. Redirecionando também o `stdout`
(`> painel.txt`), os quadros do painel ficam registrados em sequência.

### Arquivo de taxas

```
# IP            taxa_kbps
127.0.0.1       8000
127.0.0.2       4000
192.168.0.10    2000
```

Linhas vazias e linhas iniciadas por `#` são ignoradas; linhas inválidas geram aviso.
IP não listado: **1000 kbps**, se houver vazão livre. Se o arquivo não abrir, o servidor avisa
e atende todos os IPs com 1000 kbps. 1 kbps = 1000 bit/s.

### Painel

Quadro real, com três `wget -p` simultâneos a partir de 127.0.0.1:

```
Servidor HTTP com QoS :18101  [12:14:07]
Vazao max: 10000 kbps | Comprometida: 8000 kbps | Livre: 2000 kbps | IPs: 1 | Conexoes: 3
IP               Con  Taxa(kbps)  Real(kbps)   RTT(ms)  Atraso(ms)  Banda(kbps)
127.0.0.1          3        8000      7998.3     0.737       0.368       6597.5
```

`Taxa` é a taxa configurada para o IP; `Real` é a taxa medida no último segundo
(as três conexões somadas ficam nos 8000 kbps do IP).
`RTT`, `Atraso` e `Banda` aparecem depois que o cliente pede o HTML e, em seguida, o primeiro objeto.

## Organização

| Arquivo | Responsabilidade |
|---|---|
| servidor.c | Argumentos, socket TCP, accept, timeout, `TCP_NODELAY`, uma pthread por conexão; admissão no início de cada thread |
| http.c / http.h | Leitura incremental, cabeçalhos, persistência, respostas; instante de cada requisição |
| arquivos.c / arquivos.h | Caminhos, tipos MIME, envio em blocos; ritmo de envio (exceto HTML) e ganchos da medida de RTT |
| qos.c / qos.h | **V2:** tabela de taxas, tabela de IPs ativos, admissão, ritmo compartilhado por IP, RTT/atraso/banda |
| painel.c / painel.h | **V2:** thread que redesenha o painel a cada 1 s |
| taxas.txt | **V2:** taxas de exemplo |
| scripts/experimentos.sh | **V2:** cenários de avaliação com curl e wget |
| testes.py | Testes de integração (v1 + QoS) com a biblioteca padrão do Python |
| www/ | `index.html` com três imagens BMP de 3.145.782 bytes cada |

## Comportamento implementado

### Versão 1
- Requisições HTTP/1.1 GET e HEAD de arquivos estáticos; Host obrigatório.
- Vários clientes simultâneos, usando Pthreads (uma thread por conexão, durante toda a conexão).
- Persistência por padrão; `Connection: close` encerra após a resposta.
- Content-Length delimita respostas; requisições fragmentadas e pipelining são tratados.
- Timeout de inatividade de 15 segundos em cada operação de socket.
- Respostas de erro (400, 403, 404, 405, 413, 431, 501, 505) e bloqueio de `..` e links simbólicos.
- Cache-Control: no-store para facilitar os experimentos de transferência.

### Versão 2 (QoS)
- **Taxa controlada por IP de origem** para todos os objetos, **exceto HTML**, conforme `taxas.txt`.
- **Divisão da taxa** entre conexões simultâneas do mesmo IP.
- **Controle de admissão:** um IP novo só é admitido se a taxa dele couber na vazão máxima livre;
  caso contrário recebe `503 Service Unavailable` com `Retry-After: 5`.
- **RTT, atraso fim-a-fim e banda** por cliente, a partir do pedido do HTML e do pedido do primeiro objeto.
- **Painel em tempo real** e registro das medidas e recusas no log.

### Como funciona o controle de taxa
Cada IP tem um **relógio virtual** (`proximo_envio`). Antes de enviar um bloco de `n` bytes,
a conexão reserva o intervalo `n·8 / taxa` nesse relógio e dorme até a sua vez:

```
inicio        = max(agora, proximo_envio)
proximo_envio = inicio + n*8 / (taxa_kbps*1000)
dormir até inicio; enviar o bloco
```

Como todas as conexões do mesmo IP usam o mesmo relógio, a soma das taxas nunca passa da taxa
do IP e a divisão entre elas é automática, inclusive quando conexões abrem ou fecham no meio
de uma transferência. O `max(agora, ...)` impede acumular crédito enquanto o IP fica ocioso.
Os blocos têm cerca de 20 ms de dados na taxa do IP (entre 1460 e 65536 bytes).

### Como funcionam as medidas
- `RTT = t(pedido do 1º objeto) − t(fim do envio do HTML)`
- `atraso fim-a-fim ≈ RTT / 2`
- `banda = bits do HTML / (t(pedido do 1º objeto) − t(pedido do HTML))`

## Decisões de projeto

- Admissão por **IP novo**: conexões adicionais de um IP já admitido sempre entram e dividem a taxa dele.
- "1000 kbps se viável": um IP não cadastrado só entra se houver 1000 kbps livres.
- A admissão é decidida no início da thread da conexão, e não no laço de `accept`. Ao recusar,
  o servidor envia o 503, fecha o lado de escrita e espera o cliente fechar (até 1 s), para que
  o kernel não envie RST antes de o cliente ler a resposta. Assim essa espera nunca atrasa o `accept`.
- **`TCP_NODELAY`** (Nagle desligado): o cabeçalho e o corpo de uma resposta pequena saem em
  envios separados. Com Nagle, o corpo esperava o ACK atrasado do cliente (~40 ms), o que atrasava
  a página e fazia o RTT medido em loopback dar ~41 ms em vez de < 1 ms.
- O RTT é medido pela aplicação e inclui o tempo de o cliente processar o HTML; o atraso RTT/2
  é, portanto, uma estimativa superior. Navegadores podem atrasar ou paralelizar os pedidos de
  imagens; por isso os experimentos usam `wget -p` e `curl`.
- A banda é a vazão efetiva de um ciclo HTML → primeiro objeto; com HTML pequeno, ela é dominada
  pelo RTT e subestima a capacidade do enlace.
- Todo o estado compartilhado do QoS fica sob um único mutex, nunca mantido durante envio, leitura ou espera.

## Testes automatizados

```sh
make test
```

O teste sobe seus próprios servidores em portas livres (cerca de 15 s no total).
Versão 1: HTML, HEAD, persistência, close, fragmentação, pipelining, erros, travessia de
diretórios e 24 downloads concorrentes com um cliente lento conectado.
Versão 2, usando IPs de origem diferentes no loopback (127.0.0.1, 127.0.0.2, 127.0.0.3):

| Teste | Resultado esperado |
|---|---|
| HTML sem limite | < 0,3 s |
| 1 download de 3 MB a 8000 kbps | ≈ 3,15 s |
| 2 downloads simultâneos do mesmo IP | ≈ 6,3 s cada (dividem a taxa) |
| IP não cadastrado, 125 kB | ≈ 1 s (1000 kbps) |
| Admissão com vazão 10000 | 503 + Retry-After para o IP que não cabe; aceito após liberar |
| RTT no painel | valor numérico < 50 ms em loopback |

**Estado de validação:** em 19/10/2026, em Linux, `make` compilou sem warnings, `make test`
passou em todos os testes e uma execução com ThreadSanitizer não apontou condições de corrida.
Esses testes verificam funcionalidade; as medidas do relatório vêm dos experimentos abaixo.

## Experimentos para o relatório

Com o servidor rodando em outro terminal (`./servidor 8080 www 12000 taxas.txt 2> servidor.log`):

```sh
scripts/experimentos.sh                      # local, inclui cenários com vários IPs
scripts/experimentos.sh http://IP:8080       # a partir de outro computador
sudo tcpdump -i lo -w captura.pcap tcp port 8080   # 'lo' local; Ethernet/Wi-Fi para clientes remotos
sudo iptraf-ng                                     # vazão por conexão em tempo real
```

No Wireshark: filtro `tcp.port == 8080` (Decode As → HTTP se necessário), Statistics → IO Graph
para ver a vazão de cada conexão próxima da taxa configurada, e Statistics → Conversations.
Taxa em kbps = bytes/s × 8 / 1000.

**WSL2:** por padrão o WSL2 fica atrás de NAT e outros computadores não alcançam o servidor.
Para clientes externos (cabeado × Wi-Fi), use o modo espelhado (`networkingMode=mirrored` em
`%UserProfile%\.wslconfig`, Windows 11) ou um Linux nativo. Com `netsh portproxy`, todos os
clientes aparecem com o mesmo IP, o que invalida o teste de taxa por IP.

## Escopo e limites

É um servidor didático com o subconjunto HTTP/1.1 necessário para os testes de arquivos
estáticos, não uma implementação completa do padrão. Não há TLS, upload, CGI,
HTTP/1.0, Range, cache condicional ou decodificação de corpos chunked.
Requisições com corpo não vazio são rejeitadas; Transfer-Encoding é rejeitado com 501.
Até 256 IPs simultâneos e 1024 linhas no arquivo de taxas. Não há limite configurável de
threads nem timeout absoluto contra clientes que enviam bytes muito lentamente.
Não é destinado à exposição pública em produção.

## Referências e declaração de apoio

- Especificação do Trabalho de Programação em Rede, 2026/2, fornecida na disciplina.
- RFC 9112 — HTTP/1.1: https://www.rfc-editor.org/rfc/rfc9112.html
- Pthreads: https://www.man7.org/linux/man-pages/man7/pthreads.7.html
- clock_nanosleep(2): https://www.man7.org/linux/man-pages/man2/clock_nanosleep.2.html
- tcp(7), opção TCP_NODELAY: https://www.man7.org/linux/man-pages/man7/tcp.7.html
- OpenAI, ChatGPT: assistência na geração e organização do código, documentação e testes
  da versão 1, em 05/10/2026: https://chatgpt.com/
- Anthropic, Claude (Claude Code): revisão do repositório da versão 1 e implementação da
  versão 2 (qos.c, painel.c, alterações em servidor.c, http.c e arquivos.c, testes de QoS,
  scripts/experimentos.sh e este README), em 18–19/10/2026: https://claude.ai/

O código foi produzido com apoio de IA, como indicado acima e no comentário do topo de cada
arquivo. A equipe deve revisar, compreender e testar a implementação e citar esse apoio no
relatório da versão, conforme a especificação.
As imagens BMP são padrões sintéticos de teste produzidos programaticamente.
