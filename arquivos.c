/* Criado com apoio de ChatGPT/OpenAI em 05/10/2026. Ver README.md. */
#include "arquivos.h"
#include "http.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int hexadecimal(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *tipo_mime(const char *nome)
{
    const char *ext = strrchr(nome, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".bmp")) return "image/bmp";
    if (!strcasecmp(ext, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

int arquivos_responder(int socket_fd, int raiz_fd, const char *alvo,
                      int fechar, int apenas_cabecalho)
{
    char caminho[4096];
    size_t j = 0;
    if (*alvo != '/') return http_erro(socket_fd, 400, fechar, apenas_cabecalho);
    /* Decodifica %HH; consulta ?x=y nao pertence ao nome do arquivo. */
    for (size_t i = 1; alvo[i] && alvo[i] != '?'; ++i) {
        unsigned char c = (unsigned char)alvo[i];
        if (c == '%') {
            if (!alvo[i+1] || !alvo[i+2]) return http_erro(socket_fd, 400, fechar, apenas_cabecalho);
            int a = hexadecimal((unsigned char)alvo[i+1]);
            int b = hexadecimal((unsigned char)alvo[i+2]);
            if (a < 0 || b < 0) return http_erro(socket_fd, 400, fechar, apenas_cabecalho);
            c = (unsigned char)(a * 16 + b); i += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || c == '#')
            return http_erro(socket_fd, 400, fechar, apenas_cabecalho);
        if (j + 12 >= sizeof caminho) return http_erro(socket_fd, 414, fechar, apenas_cabecalho);
        caminho[j++] = (char)c;
    }
    caminho[j] = '\0';
    if (j == 0 || caminho[j-1] == '/') strcpy(caminho + j, "index.html");
    const char *mime = tipo_mime(caminho);
    /* openat por componente impede '..' e links simbolicos de escapar da raiz.
       O_NONBLOCK evita bloquear uma thread ao encontrar FIFO no diretorio. */
    int atual = dup(raiz_fd);
    if (atual < 0) return http_erro(socket_fd, 500, fechar, apenas_cabecalho);
    char *estado = NULL;
    char *parte = strtok_r(caminho, "/", &estado);
    while (parte) {
        if (!strcmp(parte, "..") || !strcmp(parte, ".")) {
            close(atual); return http_erro(socket_fd, 403, fechar, apenas_cabecalho);
        }
        char *proxima = strtok_r(NULL, "/", &estado);
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (proxima) flags |= O_DIRECTORY;
        int novo = openat(atual, parte, flags);
        int erro = errno;
        close(atual);
        if (novo < 0) {
            int status = (erro == ENOENT || erro == ENOTDIR) ? 404 :
                         (erro == EACCES || erro == ELOOP) ? 403 : 500;
            return http_erro(socket_fd, status, fechar, apenas_cabecalho);
        }
        atual = novo; parte = proxima;
    }
    struct stat st;
    if (fstat(atual, &st) < 0) {
        close(atual); return http_erro(socket_fd, 500, fechar, apenas_cabecalho);
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        close(atual); return http_erro(socket_fd, 403, fechar, apenas_cabecalho);
    }
    int resultado = http_cabecalho(socket_fd, 200, mime, (uintmax_t)st.st_size, fechar);
    uintmax_t restante = apenas_cabecalho ? 0 : (uintmax_t)st.st_size;
    char bloco[65536];
    while (resultado == 0 && restante) {
        size_t quantidade = restante < sizeof bloco ? (size_t)restante : sizeof bloco;
        ssize_t lidos = read(atual, bloco, quantidade);
        if (lidos < 0 && errno == EINTR) continue;
        if (lidos <= 0) { resultado = -1; break; }
        resultado = http_enviar(socket_fd, bloco, (size_t)lidos);
        restante -= (uintmax_t)lidos;
    }
    close(atual);
    return resultado;
}
