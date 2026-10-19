/* qos.c - Qualidade de Servico da versao 2 (ver qos.h).
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
 */
#include "qos.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TAXAS 1024      /* linhas aceitas no arquivo de taxas */
#define MAX_IPS 256         /* IPs atendidos ao mesmo tempo */
#define TAXA_PADRAO 1000.0  /* kbps para IP nao cadastrado */

struct taxa { struct in_addr ip; double kbps; };

struct qos_ip {
    int em_uso;
    struct in_addr ip;
    double taxa_kbps;               /* fixa enquanto o registro existir */
    int conexoes;                   /* conexoes abertas deste IP */
    struct timespec proximo_envio;  /* relogio virtual do ritmo */
    uint64_t bytes_enviados, bytes_ultimo;
    double real_kbps;
    /* medidas de RTT */
    struct timespec t_req_html, t_fim_html;
    size_t bytes_html;
    int aguardando_objeto, tem_medida;
    double rtt_ms, atraso_ms, banda_kbps;
};

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static struct taxa taxas[MAX_TAXAS];
static size_t total_taxas;
static struct qos_ip tabela[MAX_IPS];
static double vazao_max, comprometida;

/* ---------- tempo ---------- */

static struct timespec agora(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static double diferenca_s(struct timespec fim, struct timespec inicio)
{
    return (double)(fim.tv_sec - inicio.tv_sec) +
           (double)(fim.tv_nsec - inicio.tv_nsec) / 1e9;
}

static int antes(struct timespec a, struct timespec b)
{
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

static struct timespec somar_s(struct timespec t, double s)
{
    long long ns = (long long)(s * 1e9);
    t.tv_sec += (time_t)(ns / 1000000000LL);
    t.tv_nsec += (long)(ns % 1000000000LL);
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

/* ---------- arquivo de taxas ---------- */

int qos_iniciar(const char *arquivo_taxas, double vazao_max_kbps)
{
    vazao_max = vazao_max_kbps;
    FILE *f = fopen(arquivo_taxas, "r");
    if (!f) {
        fprintf(stderr, "Aviso: nao abri '%s' (%s). Todos os IPs: %.0f kbps.\n",
                arquivo_taxas, strerror(errno), TAXA_PADRAO);
        return 0;
    }
    char linha[256];
    int numero = 0;
    while (fgets(linha, sizeof linha, f)) {
        ++numero;
        char ip[64], resto[8];
        double kbps;
        char *p = linha;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        int lidos = sscanf(p, "%63s %lf %7s", ip, &kbps, resto);
        struct in_addr endereco;
        if (lidos != 2 || kbps <= 0 || inet_pton(AF_INET, ip, &endereco) != 1) {
            fprintf(stderr, "Aviso: %s linha %d ignorada (use: IP taxa_kbps)\n",
                    arquivo_taxas, numero);
            continue;
        }
        if (total_taxas == MAX_TAXAS) {
            fprintf(stderr, "Aviso: limite de %d taxas atingido\n", MAX_TAXAS);
            break;
        }
        taxas[total_taxas].ip = endereco;
        taxas[total_taxas].kbps = kbps;
        ++total_taxas;
    }
    fclose(f);
    fprintf(stderr, "Taxas carregadas de %s: %zu IP(s); demais IPs: %.0f kbps\n",
            arquivo_taxas, total_taxas, TAXA_PADRAO);
    for (size_t i = 0; i < total_taxas; ++i) {
        char texto[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &taxas[i].ip, texto, sizeof texto);
        fprintf(stderr, "  %-15s %.0f kbps\n", texto, taxas[i].kbps);
    }
    return 0;
}

static double taxa_do_ip(struct in_addr ip)
{
    /* Ultima ocorrencia vence, caso o IP apareca repetido no arquivo. */
    double kbps = TAXA_PADRAO;
    for (size_t i = 0; i < total_taxas; ++i)
        if (taxas[i].ip.s_addr == ip.s_addr) kbps = taxas[i].kbps;
    return kbps;
}

/* ---------- admissao ---------- */

struct qos_ip *qos_admitir(struct in_addr ip)
{
    struct qos_ip *livre = NULL, *resultado = NULL;
    pthread_mutex_lock(&trava);
    for (int i = 0; i < MAX_IPS; ++i) {
        if (tabela[i].em_uso && tabela[i].ip.s_addr == ip.s_addr) {
            /* IP ja admitido: a nova conexao divide a taxa existente. */
            tabela[i].conexoes++;
            resultado = &tabela[i];
            break;
        }
        if (!tabela[i].em_uso && !livre) livre = &tabela[i];
    }
    if (!resultado && livre) {
        double taxa = taxa_do_ip(ip);
        /* IP novo so entra se a taxa dele couber na vazao livre. */
        if (comprometida + taxa <= vazao_max + 1e-9) {
            memset(livre, 0, sizeof *livre);
            livre->em_uso = 1;
            livre->ip = ip;
            livre->taxa_kbps = taxa;
            livre->conexoes = 1;
            comprometida += taxa;
            resultado = livre;
        }
    }
    pthread_mutex_unlock(&trava);
    return resultado;
}

void qos_liberar(struct qos_ip *c)
{
    if (!c) return;
    pthread_mutex_lock(&trava);
    if (--c->conexoes == 0) {
        comprometida -= c->taxa_kbps;
        if (comprometida < 0) comprometida = 0;
        c->em_uso = 0;
    }
    pthread_mutex_unlock(&trava);
}

/* ---------- ritmo de envio ---------- */

size_t qos_tamanho_bloco(const struct qos_ip *c)
{
    if (!c) return 65536;
    /* taxa_kbps nao muda enquanto o registro existe: leitura sem trava. */
    double bytes = c->taxa_kbps * 1000.0 / 8.0 / 50.0;   /* ~20 ms */
    if (bytes < 1460) bytes = 1460;
    if (bytes > 65536) bytes = 65536;
    return (size_t)bytes;
}

void qos_aguardar_envio(struct qos_ip *c, size_t bytes)
{
    if (!c || !bytes) return;
    struct timespec t = agora(), inicio;
    pthread_mutex_lock(&trava);
    /* Relogio virtual do IP: cada bloco ocupa bytes*8/taxa segundos.
     * max(agora, proximo) evita acumular credito quando o IP fica ocioso. */
    inicio = antes(c->proximo_envio, t) ? t : c->proximo_envio;
    c->proximo_envio = somar_s(inicio, (double)bytes * 8.0 / (c->taxa_kbps * 1000.0));
    pthread_mutex_unlock(&trava);
    /* Dorme fora da trava. clock_nanosleep retorna o erro (nao usa errno). */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &inicio, NULL) == EINTR) {}
}

void qos_contabilizar(struct qos_ip *c, size_t bytes)
{
    if (!c) return;
    pthread_mutex_lock(&trava);
    c->bytes_enviados += bytes;
    pthread_mutex_unlock(&trava);
}

/* ---------- RTT, atraso e banda ---------- */

void qos_html_enviado(struct qos_ip *c, size_t bytes,
                      struct timespec t_req, struct timespec t_fim)
{
    if (!c) return;
    pthread_mutex_lock(&trava);
    c->t_req_html = t_req;
    c->t_fim_html = t_fim;
    c->bytes_html = bytes;
    c->aguardando_objeto = 1;
    pthread_mutex_unlock(&trava);
}

void qos_requisicao_objeto(struct qos_ip *c, struct timespec t_req)
{
    if (!c) return;
    int nova = 0;
    double rtt_ms = 0, atraso_ms = 0, banda_kbps = 0;
    char ip[INET_ADDRSTRLEN];
    pthread_mutex_lock(&trava);
    if (c->aguardando_objeto) {
        /* RTT: do fim do envio do HTML ate o pedido do primeiro objeto. */
        double rtt = diferenca_s(t_req, c->t_fim_html);
        /* Ciclo completo: do pedido do HTML ate o pedido do objeto. */
        double ciclo = diferenca_s(t_req, c->t_req_html);
        if (rtt > 0 && ciclo > 0) {
            c->rtt_ms = rtt * 1000.0;
            c->atraso_ms = c->rtt_ms / 2.0;
            c->banda_kbps = (double)c->bytes_html * 8.0 / ciclo / 1000.0;
            c->tem_medida = 1;
            nova = 1;
            rtt_ms = c->rtt_ms; atraso_ms = c->atraso_ms; banda_kbps = c->banda_kbps;
            inet_ntop(AF_INET, &c->ip, ip, sizeof ip);
        }
        c->aguardando_objeto = 0;
    }
    pthread_mutex_unlock(&trava);
    /* Registro no log (fora da trava), util como evidencia no relatorio. */
    if (nova)
        fprintf(stderr, "Medida %s: RTT %.3f ms | atraso %.3f ms | banda %.1f kbps\n",
                ip, rtt_ms, atraso_ms, banda_kbps);
}

/* ---------- painel ---------- */

size_t qos_instantaneo(struct qos_linha *v, size_t max, double intervalo_s,
                       double *vazao, double *comp, int *conexoes)
{
    size_t n = 0;
    int total = 0;
    pthread_mutex_lock(&trava);
    for (int i = 0; i < MAX_IPS; ++i) {
        struct qos_ip *c = &tabela[i];
        if (!c->em_uso) continue;
        total += c->conexoes;
        if (intervalo_s > 0)
            c->real_kbps = (double)(c->bytes_enviados - c->bytes_ultimo) * 8.0 /
                           intervalo_s / 1000.0;
        c->bytes_ultimo = c->bytes_enviados;
        if (n == max) continue;
        inet_ntop(AF_INET, &c->ip, v[n].ip, sizeof v[n].ip);
        v[n].conexoes = c->conexoes;
        v[n].taxa_kbps = c->taxa_kbps;
        v[n].real_kbps = c->real_kbps;
        v[n].rtt_ms = c->rtt_ms;
        v[n].atraso_ms = c->atraso_ms;
        v[n].banda_kbps = c->banda_kbps;
        v[n].tem_medida = c->tem_medida;
        ++n;
    }
    *vazao = vazao_max;
    *comp = comprometida;
    *conexoes = total;
    pthread_mutex_unlock(&trava);
    return n;
}
