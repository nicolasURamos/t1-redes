#!/usr/bin/env bash
# experimentos.sh - cenarios de avaliacao da versao 2 (QoS).
# Versao 2 desenvolvida com apoio do Claude Code (Anthropic), 2026. Ver README.md.
#
# Uso:   scripts/experimentos.sh [url_base]
#        url_base padrao: http://127.0.0.1:8080
# Em execucao local (url com 127.x), os cenarios com varios IPs usam
# enderecos de loopback diferentes (127.0.0.1, .2, .3) via curl --interface.
# Deixe o servidor rodando em outro terminal, por exemplo:
#   ./servidor 8080 www 12000 taxas.txt 2> servidor.log
# e, se quiser, capture em paralelo:  sudo tcpdump -i lo -w captura.pcap tcp port 8080
# Com o taxas.txt do repositorio e vazao 12000: 127.0.0.1 (8000) + 127.0.0.2 (4000)
# cabem juntos (cenario 4); 127.0.0.1 + 127.0.0.3 (2000) + 127.0.0.2 nao (cenario 5).
set -u
BASE="${1:-http://127.0.0.1:8080}"
OBJ="${OBJ:-/imagem1.bmp}"
FORMATO='%{http_code}  %{size_download} B  %{time_total} s  %{speed_download}\n'

kbps() {  # converte a saida do curl (bytes/s no 4o campo) para kbps
    awk '{ printf "  status %s | %s %s | %s s | %.0f kbps\n", $1, $2, $3, $4, $6 * 8 / 1000 }'
}

baixar() {  # baixar <ip_origem|-> <caminho>
    local origem=()
    [ "$1" != "-" ] && origem=(--interface "$1")
    curl --http1.1 -s -o /dev/null "${origem[@]}" -w "$FORMATO" "$BASE$2"
}

local_=0
case "$BASE" in http://127.*|http://localhost*) local_=1 ;; esac

echo "== Servidor: $BASE   objeto: $OBJ"
echo
echo "== 1. Pagina completa com keep-alive (wget -p): gera medida de RTT no painel"
inicio=$(date +%s.%N)
wget -p -q --delete-after -P /tmp/redes-wget "$BASE/"
awk -v a="$inicio" -v b="$(date +%s.%N)" 'BEGIN { printf "  concluido em %.2f s\n", b - a }'
echo
echo "== 2. Um download (taxa do IP de origem)"
baixar - "$OBJ" | kbps
echo
echo "== 3. Tres downloads simultaneos do mesmo IP (devem dividir a taxa)"
for _ in 1 2 3; do baixar - "$OBJ" > "/tmp/redes-exp-$_.txt" & done; wait
cat /tmp/redes-exp-*.txt | kbps; rm -f /tmp/redes-exp-*.txt

if [ "$local_" = 1 ]; then
    echo
    echo "== 4. IPs diferentes ao mesmo tempo (cada um com a sua taxa)"
    for ip in 127.0.0.1 127.0.0.2; do
        (echo "  origem $ip:"; baixar "$ip" "$OBJ" | kbps) > "/tmp/redes-ip-$ip.txt" &
    done; wait
    cat /tmp/redes-ip-*.txt; rm -f /tmp/redes-ip-*.txt
    echo
    echo "== 5. Controle de admissao: enquanto 127.0.0.1 e 127.0.0.3 baixam, 127.0.0.2 tenta entrar"
    baixar 127.0.0.1 "$OBJ" > /dev/null &
    baixar 127.0.0.3 "$OBJ" > /dev/null &
    sleep 0.5
    curl --http1.1 -s -o /dev/null --interface 127.0.0.2 -w '  127.0.0.2 recebeu status %{http_code}\n' "$BASE/"
    wait
fi
echo
echo "Taxa em kbps = bytes/s x 8 / 1000. Compare com taxas.txt e com o painel do servidor."
