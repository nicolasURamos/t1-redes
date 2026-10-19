/* Criado com apoio de ChatGPT/OpenAI em 05/10/2026. Ver README.md.
 * Referencia: RFC 9112, secoes 2, 5, 6 e 9 (HTTP/1.1).
 * Subconjunto didatico: GET/HEAD de arquivos estaticos, sem corpo na requisicao.
 */
#include "http.h"
#include "arquivos.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#define LIMITE 16384

int http_enviar(int fd, const void *dados, size_t tamanho)
{
    const char *p = dados;
    while (tamanho) {
        ssize_t n = send(fd, p, tamanho, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; tamanho -= (size_t)n;
    }
    return 0;
}

static const char *motivo(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Error";
    }
}

int http_cabecalho(int fd, int status, const char *tipo, uintmax_t tamanho, int fechar)
{
    char cabecalho[1024], data[64];
    time_t agora = time(NULL);
    struct tm utc;
    if (!gmtime_r(&agora, &utc) ||
        !strftime(data, sizeof data, "%a, %d %b %Y %H:%M:%S GMT", &utc)) return -1;
    int n = snprintf(cabecalho, sizeof cabecalho,
        "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: Redes-V1\r\n"
        "Content-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
        "Connection: %s\r\nCache-Control: no-store\r\n%s\r\n",
        status, motivo(status), data, tipo, tamanho, fechar ? "close" : "keep-alive",
        status == 405 ? "Allow: GET, HEAD\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof cabecalho) return -1;
    return http_enviar(fd, cabecalho, (size_t)n);
}

int http_erro(int fd, int status, int fechar, int apenas_cabecalho)
{
    char corpo[128];
    int n = snprintf(corpo, sizeof corpo, "%d %s\n", status, motivo(status));
    if (n < 0 || (size_t)n >= sizeof corpo) return -1;
    if (http_cabecalho(fd, status, "text/plain; charset=utf-8", (uintmax_t)n, fechar) < 0) return -1;
    return apenas_cabecalho ? 0 : http_enviar(fd, corpo, (size_t)n);
}

static int token(unsigned char c)
{
    return isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static char *aparar(char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    char *fim = s + strlen(s);
    while (fim > s && (fim[-1] == ' ' || fim[-1] == '\t')) --fim;
    *fim = '\0'; return s;
}

/* Altera somente a copia do cabecalho, preservando bytes da proxima requisicao. */
static int interpretar(char *cab, char **alvo, int *fechar, int *head)
{
    char *linha = strstr(cab, "\r\n");
    if (!linha) return 400;
    *linha = '\0';
    char *a = strchr(cab, ' ');
    if (!a) return 400;
    *a++ = '\0';
    char *b = strchr(a, ' ');
    if (!b) return 400;
    *b++ = '\0';
    *head = !strcmp(cab, "HEAD");
    if (!*cab || !*a || strchr(b, ' ') || strchr(b, '\t')) return 400;
    for (char *p = cab; *p; ++p) if (!token((unsigned char)*p)) return 400;
    for (char *p = a; *p; ++p) if ((unsigned char)*p <= 32 || (unsigned char)*p == 127) return 400;
    if (strcmp(b, "HTTP/1.1")) return 505;
    if (*a != '/') return 400;
    *alvo = a;
    int hosts = 0, comprimentos = 0, transferencia = 0, expectativa = 0, corpo = 0;
    char *inicio = linha + 2;
    while (*inicio) {
        char *fim = strstr(inicio, "\r\n");
        if (!fim) return 400;
        *fim = '\0';
        if (!*inicio) break;
        char *dois_pontos = strchr(inicio, ':');
        if (!dois_pontos || dois_pontos == inicio) return 400;
        *dois_pontos = '\0';
        for (char *p = inicio; *p; ++p) if (!token((unsigned char)*p)) return 400;
        char *valor = aparar(dois_pontos + 1);
        for (char *p = valor; *p; ++p)
            if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127) return 400;
        if (!strcasecmp(inicio, "Host")) {
            if (++hosts > 1 || !*valor || strpbrk(valor, " /\\,@\t")) return 400;
        } else if (!strcasecmp(inicio, "Connection")) {
            char *estado = NULL;
            for (char *v = strtok_r(valor, ",", &estado); v; v = strtok_r(NULL, ",", &estado))
                if (!strcasecmp(aparar(v), "close")) *fechar = 1;
        } else if (!strcasecmp(inicio, "Content-Length")) {
            if (++comprimentos > 1 || !*valor) return 400;
            for (char *p = valor; *p; ++p) {
                if (*p < '0' || *p > '9') return 400;
                if (*p != '0') corpo = 1;
            }
        } else if (!strcasecmp(inicio, "Transfer-Encoding")) transferencia = 1;
        else if (!strcasecmp(inicio, "Expect")) expectativa = 1;
        inicio = fim + 2;
    }
    if (hosts != 1 || (transferencia && comprimentos)) return 400;
    if (transferencia) return 501;
    if (expectativa) return 417;
    if (strcmp(cab, "GET") && strcmp(cab, "HEAD")) return 405;
    if (corpo) return 413;
    return 0;
}

void http_atender(int fd, int raiz_fd)
{
    char buffer[LIMITE + 1], cabecalho[LIMITE + 1];
    size_t usados = 0;
    for (;;) {
        buffer[usados] = '\0';
        char *fim = strstr(buffer, "\r\n\r\n");
        if (!fim) {
            if (usados == LIMITE) { http_erro(fd, 431, 1, 0); return; }
            ssize_t n = recv(fd, buffer + usados, LIMITE - usados, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            if (memchr(buffer + usados, '\0', (size_t)n)) { http_erro(fd, 400, 1, 0); return; }
            usados += (size_t)n;
            continue;
        }
        size_t tamanho = (size_t)(fim - buffer) + 4;
        memcpy(cabecalho, buffer, tamanho);
        cabecalho[tamanho] = '\0';
        memmove(buffer, buffer + tamanho, usados - tamanho);
        usados -= tamanho;
        int fechar = 0, head = 0;
        char *alvo = NULL;
        int status = interpretar(cabecalho, &alvo, &fechar, &head);
        /* Erros de sintaxe/corpo encerram a conexao para nao perder enquadramento. */
        if (status) { http_erro(fd, status, 1, head); return; }
        if (arquivos_responder(fd, raiz_fd, alvo, fechar, head) < 0 || fechar) return;
    }
}
