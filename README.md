
# t1-redes
=======
# Redes — servidor HTTP, versão 1

## Equipe

- Rodrigo Silveira
- João Gabriel Oliveira da Silva
- Nicolas Ramos

## Compilar e executar no Linux

Dependências: GCC, Make e biblioteca POSIX Pthreads. Python 3 é necessário somente para os testes automatizados.
No Ubuntu/Debian, se necessário: `sudo apt install build-essential python3 curl`.

```sh
make
./servidor 8080 www
```

Abra http://localhost:8080. Em outro computador da mesma rede, use
`http://IP_DO_SERVIDOR:8080`. O servidor escuta em todas as interfaces IPv4.
Porta padrão: 8080. Pasta padrão: `www`. Execute a partir desta pasta do projeto.
Encerre com Ctrl+C. `make clean` remove apenas o executável e os arquivos objeto.

Sem Make:

```sh
gcc -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread servidor.c http.c arquivos.c -o servidor
```

## Organização

| Arquivo | Responsabilidade |
|---|---|
| servidor.c | Socket TCP, accept, timeout e uma pthread destacada por conexão |
| http.c / http.h | Leitura incremental, cabeçalhos, persistência e respostas |
| arquivos.c / arquivos.h | Caminhos, tipos MIME e envio de arquivos em blocos |
| Makefile | Compilação, limpeza e testes |
| www/index.html | Página que referencia três imagens |
| www/imagem1.bmp a imagem3.bmp | Imagens de teste de 3.145.782 bytes cada |
| testes.py | Testes de integração com a biblioteca padrão do Python |

## Comportamento implementado

- Requisições HTTP/1.1 GET e HEAD de arquivos estáticos; Host obrigatório.
- Vários clientes simultâneos, usando Pthreads.
- Persistência por padrão; `Connection: close` encerra após a resposta.
- Content-Length delimita respostas; requisições fragmentadas e pipelining são tratados.
- Timeout de inatividade de 15 segundos em cada operação de socket.
- Arquivos binários enviados em blocos, tratando envios parciais.
- Respostas de erro (por exemplo, 400, 403, 404 e 405).
- Bloqueio de `..` e de links simbólicos nos caminhos servidos.
- Cache-Control: no-store para facilitar os experimentos de transferência.

Uma thread trata toda a vida de uma conexão; ela não é recriada a cada requisição.
Dados já recebidos da próxima requisição são preservados no buffer.
Os arquivos de `www` devem permanecer estáveis durante os testes.

## Escopo e limites

É um servidor didático com o subconjunto HTTP/1.1 necessário para os testes de arquivos
estáticos, não uma implementação completa do padrão. Não há TLS, upload, CGI,
HTTP/1.0, Range, cache condicional ou decodificação de corpos chunked.
Requisições com corpo não vazio são rejeitadas e a conexão é encerrada;
Transfer-Encoding é rejeitado com 501 (ou 400 se combinado com Content-Length).
Alvos aceitos usam `/caminho`, como os enviados normalmente ao servidor de origem.
Não há limite configurável de threads nem timeout absoluto contra clientes que enviam
bytes muito lentamente. Não é destinado à exposição pública em produção.

QoS por IP, controle de admissão por vazão e estimativas de RTT/banda pertencem à
segunda versão e não estão implementados aqui.

## Testes automatizados

```sh
make test
```

O teste inicia e encerra seu próprio servidor em porta local temporária. Confere HTML,
HEAD, persistência real no mesmo socket, close, fragmentação, pipelining, erros,
travessia de diretórios e integridade de 24 downloads concorrentes com 12 workers.
Também mantém um cliente incompleto conectado enquanto outros fazem downloads.

**Estado de validação na criação:** o ambiente de geração é Windows, sem compilador
Linux acessível; a consulta ao WSL foi negada. A compilação e os testes de integração
não foram executados aqui. Executar `make` e `make test` em Linux antes da entrega.
Não apresentar os testes descritos como resultados obtidos até executá-los.

## Experimento manual para o relatório

Com o servidor rodando em outro terminal:

```sh
# Duas requisições: o verbose deve indicar reutilização da conexão.
curl --http1.1 -v -o /dev/null http://127.0.0.1:8080/ -o /dev/null http://127.0.0.1:8080/imagem1.bmp

# Baixar também os objetos referenciados no HTML.
wget --page-requisites --directory-prefix=/tmp/redes-download http://127.0.0.1:8080/

# Doze clientes requisitando simultaneamente.
seq 1 12 | xargs -P 12 -I '{}' curl --http1.1 -sS -o /dev/null -w '%{http_code} %{size_download} %{time_total} %{speed_download}\n' http://127.0.0.1:8080/imagem1.bmp
```

No último comando: status HTTP, bytes recebidos, segundos e bytes/segundo.
Para taxa decimal em kbps, multiplique bytes/segundo por 8 e divida por 1000.

No Wireshark, capture na interface do experimento (loopback `lo` para localhost;
Ethernet/Wi-Fi para clientes remotos). Use filtro `tcp.port == 8080` e, se necessário,
Decode As → HTTP. Confirme várias requisições/respostas no mesmo TCP stream e
conexões diferentes simultâneas. Registre captura, número de clientes, tamanhos,
tempos, ambiente e observações reais. Com IPTraf/IPTraf-ng, monitore a mesma interface.
Um teste local verifica funcionalidade; medidas da rede devem usar clientes remotos.

O relatório separado deve seguir o modelo SBC e incluir Resumo, Introdução,
Desenvolvimento, Avaliação Experimental, Conclusões e Referências.
Este pacote não contém relatório com resultados nem publica um repositório no GitHub.

## Referências e declaração de apoio

- Especificação do Trabalho de Programação em Rede, 2026/2, fornecida na disciplina.
- RFC 9112 — HTTP/1.1: https://www.rfc-editor.org/rfc/rfc9112.html
- Pthreads: https://www.man7.org/linux/man-pages/man7/pthreads.7.html
- OpenAI, ChatGPT, assistência na geração e organização deste código, documentação
  e testes, em 05/10/2026: https://chatgpt.com/

O código deste pacote foi gerado com apoio de IA. A equipe deve revisar, compreender,
testar e adaptar a implementação, além de citar esse apoio no relatório da versão,
conforme a especificação. Os arquivos de código também registram esse apoio.
As imagens BMP são padrões sintéticos de teste produzidos programaticamente.
>>>>>>> 4a1e0cc (entrega 1)
