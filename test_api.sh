#!/bin/bash

HOST="${1:-localhost}"
PORT="${2:-3553}"
BASE="http://${HOST}:${PORT}"
DELAY=1

echo "=========================================="
echo "  AIMP API Test"
echo "  Target: $BASE"
echo "=========================================="

# Проверка сервера
echo ""
echo "--- Проверка сервера ---"
curl -s -o /dev/null -w "HTTP %{http_code}\n" "$BASE/api" --connect-timeout 3
sleep 1

# 1. Статус плеера
echo ""
echo "=========================================="
echo "  1. PLAYER STATUS"
echo "=========================================="
echo ""
echo "GET /api/player/status"
curl -s "$BASE/api/player/status"
echo ""
sleep $DELAY

# 2. Управление воспроизведением
echo ""
echo "=========================================="
echo "  2. PLAYBACK CONTROL"
echo "=========================================="

echo ""
echo "POST /api/player/play"
curl -s -X POST "$BASE/api/player/play"
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/pause"
curl -s -X POST "$BASE/api/player/pause"
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/next"
curl -s -X POST "$BASE/api/player/next"
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/prev"
curl -s -X POST "$BASE/api/player/prev"
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/stop"
curl -s -X POST "$BASE/api/player/stop"
echo ""
sleep $DELAY

# 3. Громкость
echo ""
echo "=========================================="
echo "  3. VOLUME & MUTE"
echo "=========================================="

echo ""
echo "GET /api/player/volume"
curl -s "$BASE/api/player/volume"
echo ""

echo ""
echo "PUT /api/player/volume (50%)"
curl -s -X PUT "$BASE/api/player/volume" -H "Content-Type: application/json" -d '{"volume":50}'
echo ""
sleep $DELAY

echo ""
echo "GET /api/player/volume (проверка)"
curl -s "$BASE/api/player/volume"
echo ""

echo ""
echo "PUT /api/player/volume (75%)"
curl -s -X PUT "$BASE/api/player/volume" -H "Content-Type: application/json" -d '{"volume":75}'
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/mute (ON)"
curl -s -X POST "$BASE/api/player/mute"
echo ""
sleep $DELAY

echo ""
echo "POST /api/player/mute (OFF)"
curl -s -X POST "$BASE/api/player/mute"
echo ""
sleep $DELAY

# 4. Позиция
echo ""
echo "=========================================="
echo "  4. POSITION"
echo "=========================================="

echo ""
echo "POST /api/player/play"
curl -s -X POST "$BASE/api/player/play"
echo ""
sleep 2

echo ""
echo "PUT /api/player/position (10s)"
curl -s -X PUT "$BASE/api/player/position" -H "Content-Type: application/json" -d '{"position":10.0}'
echo ""
sleep $DELAY

echo ""
echo "GET /api/player/status"
curl -s "$BASE/api/player/status"
echo ""

# 5. Плейлисты
echo ""
echo "=========================================="
echo "  5. PLAYLISTS"
echo "=========================================="

echo ""
echo "GET /api/playlists"
curl -s "$BASE/api/playlists"
echo ""

echo ""
echo "GET /api/playlists/0"
curl -s "$BASE/api/playlists/0"
echo ""

echo ""
echo "GET /api/playlists/0/tracks?limit=5"
curl -s "$BASE/api/playlists/0/tracks?limit=5"
echo ""

echo ""
echo "GET /api/playlists/0/tracks/0"
curl -s "$BASE/api/playlists/0/tracks/0"
echo ""

# 6. Действия с плейлистами
echo ""
echo "=========================================="
echo "  6. PLAYLIST ACTIONS"
echo "=========================================="

echo ""
echo "POST /api/playlists/0/select"
curl -s -X POST "$BASE/api/playlists/0/select"
echo ""
sleep $DELAY

echo ""
echo "POST /api/playlists/0/play"
curl -s -X POST "$BASE/api/playlists/0/play"
echo ""
sleep $DELAY

echo ""
echo "POST /api/playlists/0/tracks/0/play"
curl -s -X POST "$BASE/api/playlists/0/tracks/0/play"
echo ""
sleep $DELAY

# 7. Фокус (навигация)
echo ""
echo "=========================================="
echo "  7. FOCUS NAVIGATION"
echo "=========================================="

echo ""
echo "GET /api/focus"
curl -s "$BASE/api/focus"
echo ""

echo ""
echo "POST /api/focus/playlist/next"
curl -s -X POST "$BASE/api/focus/playlist/next"
echo ""
sleep $DELAY

echo ""
echo "POST /api/focus/playlist/next"
curl -s -X POST "$BASE/api/focus/playlist/next"
echo ""
sleep $DELAY

echo ""
echo "POST /api/focus/playlist/prev"
curl -s -X POST "$BASE/api/focus/playlist/prev"
echo ""
sleep $DELAY

echo ""
echo "GET /api/focus"
curl -s "$BASE/api/focus"
echo ""

echo ""
echo "POST /api/focus/track/next"
curl -s -X POST "$BASE/api/focus/track/next"
echo ""
sleep $DELAY

echo ""
echo "POST /api/focus/track/next"
curl -s -X POST "$BASE/api/focus/track/next"
echo ""
sleep $DELAY

echo ""
echo "POST /api/focus/track/prev"
curl -s -X POST "$BASE/api/focus/track/prev"
echo ""
sleep $DELAY

echo ""
echo "GET /api/focus"
curl -s "$BASE/api/focus"
echo ""

echo ""
echo "POST /api/focus/play"
curl -s -X POST "$BASE/api/focus/play"
echo ""
sleep 1

echo ""
echo "GET /api/player/status"
curl -s "$BASE/api/player/status"
echo ""

echo ""
echo "=========================================="
echo "  TEST COMPLETE"
echo "=========================================="
