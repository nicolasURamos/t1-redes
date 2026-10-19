/* qos.h - Qualidade de Servico da versao 2.
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
 *
 * Responsabilidades:
 *  - tabela de taxas lida do arquivo auxiliar (IP -> kbps);
 *  - tabela de IPs com conexoes ativas e controle de admissao pela vazao maxima;
 *  - ritmo de envio compartilhado por IP (as conexoes do mesmo IP dividem a taxa);
 *  - medidas de RTT, atraso e banda a partir do HTML e do primeiro objeto.
 * Todo o estado compartilhado e protegido por um unico mutex interno.
 */
#ifndef QOS_H
#define QOS_H
#include <netinet/in.h>
#include <stddef.h>
#include <time.h>

/* Registro de um IP com pelo menos uma conexao ativa (estrutura opaca). */
struct qos_ip;

/* Le o arquivo de taxas e define a vazao maxima do servidor, em kbps.
 * Se o arquivo nao puder ser aberto, avisa e usa 1000 kbps para todos. */
int qos_iniciar(const char *arquivo_taxas, double vazao_max_kbps);

/* Controle de admissao. Retorna NULL se o IP nao couber na vazao maxima. */
struct qos_ip *qos_admitir(struct in_addr ip);
/* Fim de uma conexao. Ao fechar a ultima do IP, libera a taxa reservada. */
void qos_liberar(struct qos_ip *c);

/* Tamanho de bloco para o envio com ritmo: cerca de 20 ms de dados. */
size_t qos_tamanho_bloco(const struct qos_ip *c);
/* Reserva um intervalo no relogio do IP e dorme ate o instante de envio. */
void qos_aguardar_envio(struct qos_ip *c, size_t bytes);
/* Soma bytes enviados (usado para a taxa real mostrada no painel). */
void qos_contabilizar(struct qos_ip *c, size_t bytes);

/* Medidas: fim do envio de um HTML e chegada do pedido do primeiro objeto. */
void qos_html_enviado(struct qos_ip *c, size_t bytes,
                      struct timespec t_req, struct timespec t_fim);
void qos_requisicao_objeto(struct qos_ip *c, struct timespec t_req);

/* Copia do estado para o painel, sem ponteiros internos. */
struct qos_linha {
    char ip[INET_ADDRSTRLEN];
    int conexoes;
    double taxa_kbps, real_kbps;
    double rtt_ms, atraso_ms, banda_kbps;
    int tem_medida;
};
/* Preenche ate 'max' linhas. 'intervalo_s' e o tempo desde a chamada
 * anterior, usado para calcular a taxa real. Retorna o numero de linhas. */
size_t qos_instantaneo(struct qos_linha *v, size_t max, double intervalo_s,
                       double *vazao_max, double *comprometida, int *conexoes);
#endif
