#!/bin/bash
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# =============================================================================
# AIMP HTTP Plugin — полный тест всех API ручек
# Совместим с macOS (bash + curl, без grep/sed/awk)
#
# Использование:
#   ./test_api.sh                     → localhost:3553
#   ./test_api.sh 192.168.2.49        → 192.168.2.49:3553
#   ./test_api.sh 192.168.2.49 8080
# =============================================================================

HOST="${1:-localhost}"
PORT="${2:-3553}"
BASE="http://${HOST}:${PORT}"

# Задержка между изменяющими состояние запросами (секунды)
DELAY=1

# Цвета
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
RESET='\033[0m'

PASS=0
FAIL=0

# =============================================================================
# Вспомогательные функции
# =============================================================================

header() {
    echo ""
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${RESET}"
    echo -e "${BOLD}${CYAN}  $1${RESET}"
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${RESET}"
}

subheader() {
    echo ""
    echo -e "${YELLOW}  ── $1${RESET}"
}

# call METHOD PATH [body]
# Возвращает HTTP код и тело ответа, увеличивает PASS/FAIL
call() {
    local METHOD="$1"
    local PATH="$2"
    local BODY="$3"
    local URL="${BASE}${PATH}"

    echo ""
    echo -e "${BOLD}  ${METHOD} ${PATH}${RESET}"
    echo -e "${DIM}  ${URL}${RESET}"

    if [ -n "$BODY" ]; then
        echo -e "${DIM}  body: ${BODY}${RESET}"
    fi

    local TMP_BODY TMP_CODE
    TMP_BODY=$(mktemp)
    TMP_CODE=$(mktemp)

    if [ -n "$BODY" ]; then
        curl -s -o "$TMP_BODY" -w "%{http_code}" \
            -X "$METHOD" \
            -H "Content-Type: application/json" \
            --data "$BODY" \
            "$URL" > "$TMP_CODE" 2>&1
    else
        curl -s -o "$TMP_BODY" -w "%{http_code}" \
            -X "$METHOD" \
            -H "Content-Type: application/json" \
            "$URL" > "$TMP_CODE" 2>&1
    fi

    local HTTP_CODE
    HTTP_CODE=$(cat "$TMP_CODE")
    local RESPONSE
    RESPONSE=$(cat "$TMP_BODY")

    rm -f "$TMP_BODY" "$TMP_CODE"

    if [ -z "$HTTP_CODE" ] || [ "$HTTP_CODE" = "000" ]; then
        echo -e "  ${RED}НЕТ ОТВЕТА (сервер недоступен?)${RESET}"
        FAIL=$((FAIL + 1))
        return
    elif [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "201" ]; then
        echo -e "  ${GREEN}HTTP ${HTTP_CODE}${RESET}"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}HTTP ${HTTP_CODE}${RESET}"
        FAIL=$((FAIL + 1))
    fi

    local PRETTY
    if command -v jq > /dev/null 2>&1; then
        PRETTY=$(echo "$RESPONSE" | jq . 2>/dev/null)
        [ -z "$PRETTY" ] && PRETTY="$RESPONSE"
    elif command -v python3 > /dev/null 2>&1; then
        PRETTY=$(echo "$RESPONSE" | python3 -m json.tool 2>/dev/null)
        [ -z "$PRETTY" ] && PRETTY="$RESPONSE"
    else
        PRETTY="$RESPONSE"
    fi

    while IFS= read -r line; do
        echo "    $line"
    done <<< "$PRETTY"
}

# Проверка доступности сервера
check_server() {
    echo -e "${BOLD}Проверка ${BASE} ...${RESET}"
    local TMP
    TMP=$(mktemp)
    local CODE
    CODE=$(curl -s -o "$TMP" -w "%{http_code}" --connect-timeout 3 "${BASE}/api" 2>&1)
    rm -f "$TMP"
    if [ "$CODE" = "200" ] || [ "$CODE" = "404" ]; then
        echo -e "${GREEN}Сервер доступен (HTTP ${CODE})${RESET}"
        return 0
    else
        echo -e "${RED}Сервер недоступен: ${BASE} (код: ${CODE})${RESET}"
        echo -e "${RED}Убедитесь что AIMP запущен и плагин загружен${RESET}"
        exit 1
    fi
}

# =============================================================================
# MAIN
# =============================================================================

echo -e "${BOLD}${CYAN}"
echo "  ╔══════════════════════════════════════════╗"
echo "  ║     AIMP HTTP Plugin — API Test Suite    ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${RESET}"
echo -e "  Target: ${BOLD}${BASE}${RESET}"
echo -e "  Задержка между действиями: ${DELAY} сек"

check_server

# =============================================================================
header "0. API — список маршрутов"
# =============================================================================
call GET "/api"

# =============================================================================
header "1. PLAYER — статус (полный)"
# =============================================================================
call GET "/api/player/status"

# =============================================================================
header "2. PLAYER — управление воспроизведением"
# =============================================================================

subheader "Play (resume)"
call POST "/api/player/play"
sleep $DELAY

subheader "Статус после play"
call GET "/api/player/status"

subheader "Pause"
call POST "/api/player/pause"
sleep $DELAY

subheader "Статус после pause"
call GET "/api/player/status"

subheader "Play снова"
call POST "/api/player/play"
sleep $DELAY

subheader "Next track"
call POST "/api/player/next"
sleep $DELAY

subheader "Prev track"
call POST "/api/player/prev"
sleep $DELAY

subheader "Stop"
call POST "/api/player/stop"
sleep $DELAY

subheader "Статус после stop"
call GET "/api/player/status"

# =============================================================================
header "3. PLAYER — громкость, mute, позиция"
# =============================================================================

subheader "Получить текущую громкость"
call GET "/api/player/volume"

subheader "Установить громкость 50%"
call PUT "/api/player/volume" '{"volume": 50}'
sleep $DELAY

subheader "Проверить громкость"
call GET "/api/player/volume"

subheader "Вернуть громкость 75%"
call PUT "/api/player/volume" '{"volume": 75}'
sleep $DELAY

subheader "Toggle mute (включить)"
call POST "/api/player/mute"
sleep $DELAY

subheader "Громкость после mute"
call GET "/api/player/volume"

subheader "Toggle mute (выключить)"
call POST "/api/player/mute"
sleep $DELAY

subheader "Play для теста позиции"
call POST "/api/player/play"
sleep 1   # даём треку начать играть

subheader "Перемотать на 10 секунд"
call PUT "/api/player/position" '{"position": 10.0}'
sleep $DELAY

subheader "Статус (проверяем position ~10)"
call GET "/api/player/status"

# =============================================================================
header "4. PLAYLISTS — список и данные"
# =============================================================================

subheader "Все плейлисты"
call GET "/api/playlists"

subheader "Плейлист 0"
call GET "/api/playlists/0"

subheader "Плейлист 1"
call GET "/api/playlists/1"

subheader "Треки плейлиста 0 — первые 5"
call GET "/api/playlists/0/tracks?limit=5&offset=0"

subheader "Треки плейлиста 0 — следующие 5"
call GET "/api/playlists/0/tracks?limit=5&offset=5"

subheader "Трек 0 плейлиста 0"
call GET "/api/playlists/0/tracks/0"

subheader "Трек 1 плейлиста 0"
call GET "/api/playlists/0/tracks/1"

subheader "Несуществующий плейлист → ожидаем ошибку"
call GET "/api/playlists/999"

subheader "Несуществующий трек → ожидаем ошибку"
call GET "/api/playlists/0/tracks/99999"

# =============================================================================
header "5. PLAYLISTS — действия"
# =============================================================================

subheader "Активировать плейлист 0 (select tab)"
call POST "/api/playlists/0/select"
sleep $DELAY

subheader "Воспроизвести плейлист 0 с начала"
call POST "/api/playlists/0/play"
sleep 1

subheader "Статус (должен играть плейлист 0)"
call GET "/api/player/status"

subheader "Воспроизвести трек 0 из плейлиста 0"
call POST "/api/playlists/0/tracks/0/play"
sleep 1

subheader "Установить AIMP-фокус на трек 1"
call POST "/api/playlists/0/tracks/1/select"
sleep $DELAY

# =============================================================================
header "6. FOCUS — навигация кнопками Bitfocus"
# =============================================================================

subheader "Начальный фокус"
call GET "/api/focus"

subheader "→ Следующий плейлист"
call POST "/api/focus/playlist/next"
sleep $DELAY

subheader "→ Следующий плейлист"
call POST "/api/focus/playlist/next"
sleep $DELAY

subheader "← Предыдущий плейлист"
call POST "/api/focus/playlist/prev"
sleep $DELAY

subheader "Текущий фокус (плейлист)"
call GET "/api/focus"

subheader "→ Следующий трек"
call POST "/api/focus/track/next"
sleep $DELAY

subheader "→ Следующий трек"
call POST "/api/focus/track/next"
sleep $DELAY

subheader "→ Следующий трек"
call POST "/api/focus/track/next"
sleep $DELAY

subheader "← Предыдущий трек"
call POST "/api/focus/track/prev"
sleep $DELAY

subheader "Текущий фокус (трек)"
call GET "/api/focus"

subheader "▶ Воспроизвести трек в фокусе"
call POST "/api/focus/play"
sleep 1

subheader "Статус — playing должен совпадать с focus"
call GET "/api/player/status"

subheader "Смена плейлиста → трек сбрасывается на 0"
call POST "/api/focus/playlist/next"
sleep $DELAY

subheader "Финальное состояние фокуса"
call GET "/api/focus"

# =============================================================================
header "7. ИТОГ"
# =============================================================================
echo ""
TOTAL=$((PASS + FAIL))
echo -e "  Запросов выполнено: ${BOLD}${TOTAL}${RESET}"
echo -e "  ${GREEN}${BOLD}Успешных (2xx): ${PASS}${RESET}"
if [ "$FAIL" -gt 0 ]; then
    echo -e "  ${RED}${BOLD}Ошибок:         ${FAIL}${RESET}"
else
    echo -e "  ${GREEN}${BOLD}Ошибок:         ${FAIL}${RESET}"
fi
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}✓ Все тесты прошли успешно!${RESET}"
else
    echo -e "  ${YELLOW}${BOLD}⚠ Есть ошибки — проверьте вывод выше${RESET}"
fi
echo ""
