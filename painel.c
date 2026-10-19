/* painel.c - Painel em tempo real dos clientes atendidos.
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
 *
 * A cada 1 s copia o estado do QoS (qos_instantaneo) e imprime em stdout.
 * Nenhum mutex fica travado durante a impressao. Em terminal, a tela e
 * limpa a cada quadro; redirecionado para arquivo, os quadros se acumulam
 * (util para registrar a evolucao no relatorio).
 */
#include "painel.h"
#include "qos.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINHAS 256

static int porta_servidor;

static void *desenhar(void *arg)
{
    (void)arg;
    static struct qos_linha linhas[MAX_LINHAS];
    int terminal = isatty(STDOUT_FILENO);
    struct timespec anterior, atual, espera = {.tv_sec = 1, .tv_nsec = 0};
    clock_gettime(CLOCK_MONOTONIC, &anterior);
    for (;;) {
        nanosleep(&espera, NULL);
        clock_gettime(CLOCK_MONOTONIC, &atual);
        double intervalo = (double)(atual.tv_sec - anterior.tv_sec) +
                           (double)(atual.tv_nsec - anterior.tv_nsec) / 1e9;
        anterior = atual;
        double vazao, comprometida;
        int conexoes;
        size_t n = qos_instantaneo(linhas, MAX_LINHAS, intervalo,
                                   &vazao, &comprometida, &conexoes);
        if (terminal) printf("\033[H\033[2J");
        time_t relogio = time(NULL);
        struct tm local;
        char hora[16] = "";
        if (localtime_r(&relogio, &local)) strftime(hora, sizeof hora, "%H:%M:%S", &local);
        printf("Servidor HTTP com QoS :%d  [%s]\n", porta_servidor, hora);
        printf("Vazao max: %.0f kbps | Comprometida: %.0f kbps | Livre: %.0f kbps | IPs: %zu | Conexoes: %d\n",
               vazao, comprometida, vazao - comprometida, n, conexoes);
        printf("%-15s %4s %11s %11s %9s %11s %12s\n",
               "IP", "Con", "Taxa(kbps)", "Real(kbps)", "RTT(ms)", "Atraso(ms)", "Banda(kbps)");
        if (n == 0) printf("(nenhum cliente sendo atendido)\n");
        for (size_t i = 0; i < n; ++i) {
            struct qos_linha *l = &linhas[i];
            printf("%-15s %4d %11.0f %11.1f ", l->ip, l->conexoes, l->taxa_kbps, l->real_kbps);
            if (l->tem_medida)
                printf("%9.3f %11.3f %12.1f\n", l->rtt_ms, l->atraso_ms, l->banda_kbps);
            else
                printf("%9s %11s %12s\n", "-", "-", "-");
        }
        if (!terminal) printf("\n");
        fflush(stdout);
    }
    return NULL;
}

int painel_iniciar(int porta)
{
    porta_servidor = porta;
    pthread_t thread;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int erro = pthread_create(&thread, &attr, desenhar, NULL);
    pthread_attr_destroy(&attr);
    return erro ? -1 : 0;
}
