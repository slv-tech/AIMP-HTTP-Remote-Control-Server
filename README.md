# AIMP HTTP Remote Control

[Русский](#русский) | [English](#english)

---

## Русский

Плагин для удалённого управления AIMP через HTTP API. Работает с версией плеера 5.40.2716 и старше. Доступен для Windows (`.dll`). Создан для управления плеером в Bitfocus Companion через соответствующий модуль. Работает с Companion 4.3.2 и старше. Модуль для Companion доступен в каталоге модулей.

### Сборка

#### Windows (x64)

```bash
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -I. -isystem sdk -isystem third_party \
    aimp_http_plugin.cpp \
    -shared -o AimpHttpControl64.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
```

#### Windows (x32)

```bash
i686-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -I. -isystem sdk -isystem third_party \
    aimp_http_plugin.cpp \
    -shared -o AimpHttpControl32.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
```

### Настройки

Открываются через меню AIMP: **Настройки → Плагины → HTTP Remote Control**. Все настройки сохраняются при перезапуске плеера.

#### Сервер

| Параметр | По умолчанию | Описание |
|----------|-------------|----------|
| Port | `19122` | Порт HTTP сервера |
| Listen interface | `All interfaces (0.0.0.0)` | `Localhost (127.0.0.1)` — только локальные подключения; `All interfaces (0.0.0.0)` — все интерфейсы; далее — список реальных сетевых интерфейсов системы (имя адаптера + IP) |

#### Политика доступа

| Параметр | По умолчанию | Описание |
|----------|-------------|----------|
| Allow access only from | выключено | Если включено, принимаются подключения только с адресов из списка ниже |
| IP / CIDR list | — | Список разрешённых адресов через запятую. Поддерживаются точные адреса и CIDR-диапазоны, например: `192.168.1.5, 192.168.2.0/24, 10.0.0.0/8` |

Если галочка снята — подключения принимаются с любых адресов (whitelist выключен).

### API эндпоинты

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

---

## English

A plugin for remote control of AIMP via HTTP API. Compatible with AIMP version 5.40.2716 and above. Available for Windows (`.dll`). Designed for controlling the player in Bitfocus Companion via the corresponding module. Works with Companion 4.3.2 and above. The Companion module is available in the module catalog.

### Build

#### Windows (x64)

```bash
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -I. -isystem sdk -isystem third_party \
    aimp_http_plugin.cpp \
    -shared -o AimpHttpControl64.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
```

#### Windows (x32)

```bash
i686-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -I. -isystem sdk -isystem third_party \
    aimp_http_plugin.cpp \
    -shared -o AimpHttpControl32.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
```

### Settings

Accessible via the AIMP menu: **Settings → Plugins → HTTP Remote Control**. All settings are preserved across player restarts.

#### Server

| Parameter | Default | Description |
|-----------|---------|-------------|
| Port | `19122` | HTTP server port |
| Listen interface | `All interfaces (0.0.0.0)` | `Localhost (127.0.0.1)` — local connections only; `All interfaces (0.0.0.0)` — all interfaces; followed by a list of real network interfaces on the system (adapter name + IP) |

#### Access policy

| Parameter | Default | Description |
|-----------|---------|-------------|
| Allow access only from | disabled | When enabled, only connections from addresses in the list below are accepted |
| IP / CIDR list | — | Comma-separated list of allowed addresses. Supports exact addresses and CIDR ranges, e.g.: `192.168.1.5, 192.168.2.0/24, 10.0.0.0/8` |

When the checkbox is unchecked, connections are accepted from any address (whitelist disabled).

### API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/ping` | Health check |
| GET | `/api/status` | Full player status |
| GET | `/api/player` | Player state |
| GET | `/api/player/state` | State only (playing/paused/stopped) |
| GET | `/api/player/track` | Current track |
| GET | `/api/player/track/focused` | Track under cursor |
| GET | `/api/player/track/selected` | Selected tracks |
| GET | `/api/player/position` | Playback position |
| POST | `/api/player/position?position=30` | Set position |
| GET | `/api/player/volume` | Volume |
| POST | `/api/player/volume?volume=0.5` | Set volume |
| POST | `/api/player/playpause` | Play/Pause |
| POST | `/api/player/play?track=5` | Play / Play track |
| POST | `/api/player/pause` | Pause |
| POST | `/api/player/stop` | Stop |
| POST | `/api/player/next` | Next track |
| POST | `/api/player/prev` | Previous track |
| GET | `/api/playlists` | List of playlists |
| GET | `/api/playlist/{id}` | Playlist info |
| GET | `/api/playlist/{id}/tracks` | Tracks in playlist |
| POST | `/api/playlist/{id}/play?track=5` | Play track |
