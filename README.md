# AIMP HTTP Remote Control Server

[Русский](#русский) | [English](#english)

---

## Русский

Плагин для удалённого управления AIMP через HTTP API. Работает с версией плеера 5.40.2716 и старше. Доступен для Windows (`.dll`). Создан для управления плеером в Bitfocus Companion через соответствующий модуль. Работает с Companion 4.3.2 и старше. Модуль для Companion доступен в каталоге модулей.

### Установка

#### Через пакет `.aimppack` (рекомендуется)

Скачайте `AimpHttpControl.aimppack` из releases и дважды кликните по файлу — AIMP установит плагин автоматически, выбрав нужную разрядность (x64 или x32).

#### Вручную

Скопируйте нужный `.dll` файл напрямую в папку плагинов AIMP и переименуйте в `AimpHttpControl.dll`:

```
%APPDATA%\AIMP\Plugins\AimpHttpControl.dll
```

Используйте `AimpHttpControl64.dll` для 64-бит AIMP, `AimpHttpControl32.dll` для 32-бит.

### Сборка

#### Быстрая сборка (x64 + x32 + пакет)

```bash
bash build_package.sh
```

Создаёт `AimpHttpControl64.dll`, `AimpHttpControl32.dll` и `AimpHttpControl.aimppack`.

#### Windows (x64)

```bash
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -Isrc -isystem sdk -isystem third_party \
    src/globals.cpp src/utils.cpp src/network.cpp src/focus_sync.cpp \
    src/settings.cpp src/player_api.cpp src/http_server.cpp \
    src/options_frame.cpp src/plugin.cpp \
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
    -Isrc -isystem sdk -isystem third_party \
    src/globals.cpp src/utils.cpp src/network.cpp src/focus_sync.cpp \
    src/settings.cpp src/player_api.cpp src/http_server.cpp \
    src/options_frame.cpp src/plugin.cpp \
    -shared -o AimpHttpControl32.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic \
    src/plugin.def
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

#### Плеер

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/api/player/status` | Полный статус плеера (состояние, трек, громкость, позиция, плейлист, фокус) |
| POST | `/api/player/play` | Возобновить воспроизведение |
| POST | `/api/player/pause` | Пауза |
| POST | `/api/player/stop` | Стоп |
| POST | `/api/player/next` | Следующий трек |
| POST | `/api/player/prev` | Предыдущий трек |
| GET | `/api/player/volume` | Получить громкость |
| PUT | `/api/player/volume` | Установить громкость (тело: `{"volume": 75}` или `?volume=75`) |
| POST | `/api/player/mute` | Переключить mute |
| PUT | `/api/player/position` | Установить позицию в секундах (тело: `{"position": 30}`) |
| GET | `/api/player/shuffle` | Получить состояние shuffle |
| POST | `/api/player/shuffle` | Переключить shuffle |
| GET | `/api/player/repeat` | Получить состояние repeat |
| POST | `/api/player/repeat` | Переключить repeat |
| GET | `/api/player/auto-jump` | Получить состояние auto-jump |
| POST | `/api/player/auto-jump` | Переключить auto-jump |

#### Фокус (навигация по плейлистам без воспроизведения)

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/api/focus` | Текущий фокус (плейлист + трек) |
| POST | `/api/focus/playlist/next` | Фокус на следующий плейлист |
| POST | `/api/focus/playlist/prev` | Фокус на предыдущий плейлист |
| POST | `/api/focus/track/next` | Фокус на следующий трек |
| POST | `/api/focus/track/prev` | Фокус на предыдущий трек |
| POST | `/api/focus/play` | Воспроизвести трек в фокусе |

#### Плейлисты

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/api/playlists` | Список всех плейлистов |
| GET | `/api/playlists/{id}` | Информация о плейлисте |
| GET | `/api/playlists/{id}/tracks` | Треки плейлиста (`?limit=50&offset=0`) |
| GET | `/api/playlists/{id}/tracks/{tid}` | Информация о треке |
| POST | `/api/playlists/{id}/play` | Воспроизвести плейлист с первого трека |
| POST | `/api/playlists/{id}/resume` | Воспроизвести плейлист с последней позиции (трек в фокусе) |
| POST | `/api/playlists/{id}/select` | Сделать плейлист активным |
| POST | `/api/playlists/{id}/tracks/{tid}/play` | Воспроизвести трек |
| POST | `/api/playlists/{id}/tracks/{tid}/select` | Установить фокус на трек |

---

## English

A plugin for remote control of AIMP via HTTP API. Compatible with AIMP version 5.40.2716 and above. Available for Windows (`.dll`). Designed for controlling the player in Bitfocus Companion via the corresponding module. Works with Companion 4.3.2 and above. The Companion module is available in the module catalog.

### Installation

#### Via `.aimppack` package (recommended)

Download `AimpHttpControl.aimppack` from releases and double-click it — AIMP will install the plugin automatically, selecting the correct architecture (x64 or x32).

#### Manual

Copy the appropriate `.dll` directly to the AIMP plugins folder and rename it to `AimpHttpControl.dll`:

```
%APPDATA%\AIMP\Plugins\AimpHttpControl.dll
```

Use `AimpHttpControl64.dll` for 64-bit AIMP, `AimpHttpControl32.dll` for 32-bit.

### Build

#### Quick build (x64 + x32 + package)

```bash
bash build_package.sh
```

Produces `AimpHttpControl64.dll`, `AimpHttpControl32.dll` and `AimpHttpControl.aimppack`.

#### Windows (x64)

```bash
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -Wno-missing-braces -Wno-delete-non-virtual-dtor \
    -D_WIN32_WINNT=0x0A00 \
    -Isrc -isystem sdk -isystem third_party \
    src/globals.cpp src/utils.cpp src/network.cpp src/focus_sync.cpp \
    src/settings.cpp src/player_api.cpp src/http_server.cpp \
    src/options_frame.cpp src/plugin.cpp \
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
    -Isrc -isystem sdk -isystem third_party \
    src/globals.cpp src/utils.cpp src/network.cpp src/focus_sync.cpp \
    src/settings.cpp src/player_api.cpp src/http_server.cpp \
    src/options_frame.cpp src/plugin.cpp \
    -shared -o AimpHttpControl32.dll \
    -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic \
    src/plugin.def
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

#### Player

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/player/status` | Full player status (state, track, volume, position, playlist, focus) |
| POST | `/api/player/play` | Resume playback |
| POST | `/api/player/pause` | Pause |
| POST | `/api/player/stop` | Stop |
| POST | `/api/player/next` | Next track |
| POST | `/api/player/prev` | Previous track |
| GET | `/api/player/volume` | Get volume |
| PUT | `/api/player/volume` | Set volume (body: `{"volume": 75}` or `?volume=75`) |
| POST | `/api/player/mute` | Toggle mute |
| PUT | `/api/player/position` | Set position in seconds (body: `{"position": 30}`) |
| GET | `/api/player/shuffle` | Get shuffle state |
| POST | `/api/player/shuffle` | Toggle shuffle |
| GET | `/api/player/repeat` | Get repeat state |
| POST | `/api/player/repeat` | Toggle repeat |
| GET | `/api/player/auto-jump` | Get auto-jump state |
| POST | `/api/player/auto-jump` | Toggle auto-jump |

#### Focus (playlist navigation without playback)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focus` | Current focus (playlist + track) |
| POST | `/api/focus/playlist/next` | Focus next playlist |
| POST | `/api/focus/playlist/prev` | Focus previous playlist |
| POST | `/api/focus/track/next` | Focus next track |
| POST | `/api/focus/track/prev` | Focus previous track |
| POST | `/api/focus/play` | Play focused track |

#### Playlists

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/playlists` | List all playlists |
| GET | `/api/playlists/{id}` | Playlist info |
| GET | `/api/playlists/{id}/tracks` | Playlist tracks (`?limit=50&offset=0`) |
| GET | `/api/playlists/{id}/tracks/{tid}` | Track info |
| POST | `/api/playlists/{id}/play` | Play playlist from the first track |
| POST | `/api/playlists/{id}/resume` | Play playlist from the last position (focused track) |
| POST | `/api/playlists/{id}/select` | Set playlist as active |
| POST | `/api/playlists/{id}/tracks/{tid}/play` | Play track |
| POST | `/api/playlists/{id}/tracks/{tid}/select` | Focus track |
