/* Criado com apoio de ChatGPT/OpenAI em 05/10/2026. Ver README.md.
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
 * Modelo: uma pthread destacada por conexao; nenhuma thread compartilha
 * buffers de requisicao. O descritor da pasta raiz e somente leitura.
 * V2: controle de admissao por IP (qos.c) e thread de painel (painel.c).
 */
#include "http.h"
#include "painel.h"
#include "qos.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct cliente {
    int socket_fd;
    int raiz_fd;
    struct in_addr endereco;
    char ip[INET_ADDRSTRLEN];
};

/* Recusa por falta de vazao: envia 503 e espera o cliente fechar (ate 1 s).
 * Fechar com a requisicao ainda nao lida faria o kernel enviar RST, e o
 * cliente poderia perder a resposta 503. */
static void recusar(int fd)
{
    char descarte[4096];
    struct timeval curto = {.tv_sec = 1, .tv_usec = 0};
    http_erro(fd, 503, 1, 0);
    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &curto, sizeof curto);
    while (recv(fd, descarte, sizeof descarte, 0) > 0) {}
}

static void *atender(void *arg)
{
    struct cliente *c = arg;
    /* Controle de admissao: feito aqui (e nao no laco de accept) para que a
     * espera de recusar() nunca atrase a aceitacao de outros clientes. */
    struct qos_ip *qos = qos_admitir(c->endereco);
    if (!qos) {
        fprintf(stderr, "Recusado (vazao maxima atingida): %s (socket %d)\n",
                c->ip, c->socket_fd);
        recusar(c->socket_fd);
    } else {
        fprintf(stderr, "Conectado: %s (socket %d)\n", c->ip, c->socket_fd);
        http_atender(c->socket_fd, c->raiz_fd, qos);
        qos_liberar(qos);
        fprintf(stderr, "Encerrado: %s (socket %d)\n", c->ip, c->socket_fd);
    }
    close(c->socket_fd);
    free(c);
    return NULL;
}

int main(int argc, char **argv)
{
    char *fim;
    long porta = 8080;
    double vazao_max = 100000.0;
    if (argc > 5) {
        fprintf(stderr, "Uso: %s [porta] [pasta_publica] [vazao_max_kbps] [arquivo_taxas]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc > 1) {
        errno = 0;
        porta = strtol(argv[1], &fim, 10);
        if (errno || !*argv[1] || *fim || porta < 1 || porta > 65535) {
            fprintf(stderr, "Porta invalida. Use 1 a 65535.\n");
            return EXIT_FAILURE;
        }
    }
    const char *pasta = argc > 2 ? argv[2] : "www";
    if (argc > 3) {
        errno = 0;
        vazao_max = strtod(argv[3], &fim);
        if (errno || !*argv[3] || *fim || !isfinite(vazao_max) || vazao_max <= 0) {
            fprintf(stderr, "Vazao maxima invalida. Use um numero de kbps maior que zero.\n");
            return EXIT_FAILURE;
        }
    }
    const char *arquivo_taxas = argc > 4 ? argv[4] : "taxas.txt";
    int raiz = open(pasta, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (raiz < 0) { perror("Pasta publica"); return EXIT_FAILURE; }
    qos_iniciar(arquivo_taxas, vazao_max);
    /* Cliente desconectado nao deve encerrar o processo em send(). */
    signal(SIGPIPE, SIG_IGN);
    int servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor < 0) { perror("socket"); close(raiz); return EXIT_FAILURE; }
    int sim = 1;
    if (setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, &sim, sizeof sim) < 0) {
        perror("SO_REUSEADDR"); close(servidor); close(raiz); return EXIT_FAILURE;
    }
    struct sockaddr_in endereco = {0};
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = htonl(INADDR_ANY);
    endereco.sin_port = htons((unsigned short)porta);
    if (bind(servidor, (struct sockaddr *)&endereco, sizeof endereco) < 0 ||
        listen(servidor, 128) < 0) {
        perror("bind/listen"); close(servidor); close(raiz); return EXIT_FAILURE;
    }
    pthread_attr_t attr;
    int erro = pthread_attr_init(&attr);
    if (erro) { fprintf(stderr, "pthread_attr_init: %s\n", strerror(erro)); return EXIT_FAILURE; }
    erro = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (erro) { fprintf(stderr, "pthread_attr_setdetachstate: %s\n", strerror(erro)); return EXIT_FAILURE; }
    fprintf(stderr, "HTTP: http://localhost:%ld | pasta: %s | vazao max: %.0f kbps | Ctrl+C encerra\n",
            porta, pasta, vazao_max);
    if (painel_iniciar((int)porta) < 0) fprintf(stderr, "Aviso: painel nao iniciado\n");
    for (;;) {
        struct sockaddr_in remoto;
        socklen_t tamanho = sizeof remoto;
        int fd = accept(servidor, (struct sockaddr *)&remoto, &tamanho);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0) {
            perror("timeout"); close(fd); continue;
        }
        /* Desliga o algoritmo de Nagle: sem isso, o corpo de uma resposta
         * pequena (enviado apos o cabecalho) espera o ACK atrasado do cliente
         * (~40 ms), o que atrasa a pagina e distorce a medida de RTT. */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &sim, sizeof sim);
        struct cliente *c = malloc(sizeof *c);
        if (!c) { http_erro(fd, 503, 1, 0); close(fd); continue; }
        c->socket_fd = fd;
        c->raiz_fd = raiz;
        c->endereco = remoto.sin_addr;
        if (!inet_ntop(AF_INET, &remoto.sin_addr, c->ip, sizeof c->ip))
            strcpy(c->ip, "desconhecido");
        pthread_t thread;
        erro = pthread_create(&thread, &attr, atender, c);
        if (erro) {
            fprintf(stderr, "pthread_create: %s\n", strerror(erro));
            http_erro(fd, 503, 1, 0); close(fd); free(c);
        }
    }
    pthread_attr_destroy(&attr);
    close(servidor);
    /* O processo encerra todas as threads. Nao reutilizar raiz enquanto ativas. */
    return EXIT_FAILURE;
}
