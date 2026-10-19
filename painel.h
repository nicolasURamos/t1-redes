/* painel.h - Painel em tempo real dos clientes atendidos.
 * Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
 */
#ifndef PAINEL_H
#define PAINEL_H
/* Inicia uma thread que redesenha o painel em stdout a cada 1 segundo. */
int painel_iniciar(int porta);
#endif
