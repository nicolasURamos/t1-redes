/* Criado com apoio de ChatGPT/OpenAI em 05/10/2026. Ver README.md. */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>
#include <stdint.h>

/* Envia todos os bytes, mesmo quando send() envia apenas uma parte. */
int http_enviar(int socket_fd, const void *dados, size_t tamanho);
int http_cabecalho(int socket_fd, int status, const char *tipo,
                  uintmax_t tamanho, int fechar);
int http_erro(int socket_fd, int status, int fechar, int apenas_cabecalho);
/* Atende varias requisicoes na mesma conexao, ate erro, timeout ou close. */
void http_atender(int socket_fd, int raiz_fd);
#endif
