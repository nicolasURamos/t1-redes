# CLAUDE.md — t1-redes: Servidor HTTP com QoS (C + Pthreads)

Trabalho de Programação em Rede — Fundamentos e Avaliação de Redes de Computadores — UNIPAMPA 2026/2.
Equipe: Rodrigo Silveira, João Gabriel Oliveira da Silva, Nicolas Ramos.

Leia este arquivo e o README.md antes de qualquer alteração. Não reorganize pastas nem renomeie arquivos.

## Estado

- **Versão 1 (concorrência + HTTP/1.1 persistente): pronta.**
- **Versão 2 (QoS): implementada e testada** em 19/10/2026. `make` sem warnings, `make test` passando
  (6 testes v1 + 6 testes de QoS), ThreadSanitizer limpo.
- Próximas entregas: validação com o professor (até 26/10), slides (03/12), seminário (04/12).

## Mapa do código

| Arquivo | Papel |
|---|---|
| `servidor.c` | `main` (`[porta] [pasta] [vazao_max_kbps] [arquivo_taxas]`), accept, timeouts, `TCP_NODELAY`, thread por conexão; `atender` chama `qos_admitir` (NULL → `recusar`: 503 + espera o cliente fechar) e `qos_liberar` |
| `http.c` | `interpretar` (validação), `http_atender` (laço keep-alive; mede `t_req` com `CLOCK_MONOTONIC`); 503 leva `Retry-After: 5` |
| `arquivos.c` | `arquivos_responder`: caminho seguro (`openat` por componente), MIME, envio em blocos; não-HTML usa `qos_aguardar_envio` por bloco; ganchos `qos_requisicao_objeto` (antes) e `qos_html_enviado` (depois) |
| `qos.c` | Taxas do arquivo, tabela de IPs (vetor de 256 + 1 mutex), admissão, relógio virtual por IP, medidas RTT/atraso/banda, `qos_instantaneo` para o painel |
| `painel.c` | Thread que imprime o painel em stdout a cada 1 s (limpa a tela só se `isatty`) |
| `testes.py` | Integração; testes de QoS usam IPs de origem 127.0.0.1/.2/.3 |
| `scripts/experimentos.sh` | Cenários de avaliação (curl/wget), saída em kbps |

## Regras

- C11 + pthreads, flags do `Makefile`, zero warnings. Nomes e comentários em português, estilo do código existente.
- Mudanças mínimas; não mexer na validação HTTP nem na proteção de caminhos sem necessidade.
- Estado compartilhado só sob o mutex de `qos.c`; nunca segurar o mutex durante `send`/`recv`/`read`/sleep.
- **Citação de IA obrigatória:** todo arquivo alterado mantém no topo as linhas de apoio (ChatGPT na v1,
  Claude Code na v2) e o README registra o apoio.
- Depois de qualquer mudança: `make` sem warnings e `make test` passando.

## Decisões (manter coerência com o README e o relatório)

- Admissão por IP novo (`comprometida + taxa <= vazao_max`); conexões extras do mesmo IP sempre entram.
- IP não cadastrado: 1000 kbps, admitido somente se houver vazão livre.
- HTML sem limite de taxa.
- Divisão da taxa por relógio virtual compartilhado por IP (não `taxa/conexoes`).
- RTT = pedido do 1º objeto − fim do envio do HTML; atraso = RTT/2; banda = bits do HTML / ciclo.
- `TCP_NODELAY` evita o atraso de ~40 ms de Nagle + ACK atrasado.
