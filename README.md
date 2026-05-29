# AIMP HTTP Control Plugin

Плагин для удалённого управления AIMP v6 через HTTP API. Доступен для Windows (`.dll`) и Linux (`.so`).

---

## Сборка

### Windows (cross-compile)

```bash
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -I. -Isdk -Ithird_party \
    aimp_http_plugin.cpp \
    -shared -o AimpHttpControl64.dll \
    -lws2_32 -luuid -lkernel32 -luser32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
```

### Linux

```bash
g++ -shared -fPIC -O2 -std=c++17 \
    -isystem sdk_linux -I sdk -I third_party -I . \
    aimp_http_plugin_linux.cpp -o aimp_httpcontrol.so -lpthread
```

---

## Установка

### Через `.aimppack` (рекомендуется)

1. Откройте AIMP → Настройки → Плагины
2. Нажмите «Установить» (или перетащите файл в окно AIMP)
3. Выберите `AimpHttpControl.aimppack`
4. Включите плагин в списке

`AimpHttpControl.aimppack` содержит:
- `AimpHttpControl/x64/AimpHttpControl64.dll` — Windows 64-bit
- `AimpHttpControl/aimp_httpcontrol.so` — Linux

### Вручную (Linux)

```bash
sudo mkdir -p /opt/aimp/Plugins/aimp_httpcontrol/
sudo cp aimp_httpcontrol.so /opt/aimp/Plugins/aimp_httpcontrol/
```

После установки перезапустите AIMP.

---

## Настройки

Настройки хранятся в файле конфигурации AIMP (`AIMP.ini`):

| Параметр | По умолчанию | Описание |
|----------|-------------|----------|
| Port | `3553` | Порт HTTP сервера |
| Allowed | `127.0.0.1` | Разрешённые IP (`*` — все) |

Изменить можно через меню AIMP: Настройки → Плагины → AIMP HTTP Control.

---

## API эндпоинты

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/api/ping` | Проверка работы |
| GET | `/api/status` | Полный статус плеера |
| GET | `/api/player` | Состояние плеера |
| GET | `/api/player/state` | Только состояние (playing/paused/stopped) |
| GET | `/api/player/track` | Текущий трек |
| GET | `/api/player/track/focused` | Трек на курсоре |
| GET | `/api/player/track/selected` | Выделенные треки |
| GET | `/api/player/position` | Позиция воспроизведения |
| POST | `/api/player/position?position=30` | Установить позицию |
| GET | `/api/player/volume` | Громкость |
| POST | `/api/player/volume?volume=0.5` | Установить громкость |
| POST | `/api/player/playpause` | Play/Pause |
| POST | `/api/player/play?track=5` | Play / Play трек |
| POST | `/api/player/pause` | Пауза |
| POST | `/api/player/stop` | Стоп |
| POST | `/api/player/next` | Следующий трек |
| POST | `/api/player/prev` | Предыдущий трек |
| GET | `/api/playlists` | Список плейлистов |
| GET | `/api/playlist/{id}` | Инфо о плейлисте |
| GET | `/api/playlist/{id}/tracks` | Треки в плейлисте |
| POST | `/api/playlist/{id}/play?track=5` | Запустить трек |

