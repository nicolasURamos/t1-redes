/* Criado com apoio de ChatGPT/OpenAI em 05/10/2026. Ver README.md.
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md. */
#ifndef ARQUIVOS_H
#define ARQUIVOS_H
#include <time.h>
struct qos_ip; /* definido em qos.c */
/* Responde um GET/HEAD. Objetos que nao sao HTML seguem a taxa do IP (qos);
 * t_req e o instante da requisicao, usado na medida de RTT. */
int arquivos_responder(int socket_fd, int raiz_fd, const char *alvo,
                      int fechar, int apenas_cabecalho,
                      struct qos_ip *qos, struct timespec t_req);
#endif
