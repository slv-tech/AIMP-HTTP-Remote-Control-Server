# AIMP HTTP Control Plugin

Плагин для удалённого управления AIMP через HTTP API. Доступен для Windows (`.dll`).

---

## Сборка

### Windows

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


## Настройки

| Параметр | По умолчанию | Описание |
|----------|-------------|----------|
| Port | `19122` | Порт HTTP сервера |
| Allowed | `127.0.0.1` | Разрешённые IP (`*` — все) |

Изменить можно через меню AIMP: Настройки → Плагины → AIMP HTTP Control.

 // TODO: Сохранение настроек

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

