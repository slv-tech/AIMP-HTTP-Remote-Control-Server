// aimp_http_plugin.cpp
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <thread>
#include <mutex>
#include <string>
#include <ctime>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>

#include "third_party/json.hpp"
using json = nlohmann::json;

#include "sdk/apiPlugin.h"
#include "sdk/apiPlayer.h"
#include "sdk/apiPlaylists.h"
#include "sdk/apiObjects.h"

// ==========================================
// Глобальные переменные
// ==========================================
IAIMPCore* g_core = nullptr;
std::mutex g_mutex;
int g_port = 3553;
bool g_running = false;
std::thread g_serverThread;

// ==========================================
// Утилиты
// ==========================================
std::string WStr(const wchar_t* w) {
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}


// ==========================================
// Вспомогательные функции AIMP API
// ==========================================

// Согласно официальному демо SDK (TestPreimageAPIUnit.pas, строка 347):
//   PropListGetStr(Playlist as IAIMPPropertyList, AIMP_PLAYLIST_PROPID_ID)
// В Delphi "as" для COM-интерфейсов = QueryInterface.
// QueryInterface возвращает новый указатель с AddRef — его нужно Release'ить.
// Оборачиваем в helper чтобы не забывать про Release.
struct PlaylistProps {
    IAIMPPropertyList* ptr = nullptr;
    explicit PlaylistProps(IAIMPPlaylist* pl) {
        if (pl) pl->QueryInterface(IID_IAIMPPropertyList, (void**)&ptr);
    }
    ~PlaylistProps() { if (ptr) ptr->Release(); }
    operator bool() const { return ptr != nullptr; }
    IAIMPPropertyList* operator->() const { return ptr; }
};

std::string GetPlaylistName(IAIMPPlaylist* pl) {
    std::string result = "Unknown";
    PlaylistProps props(pl);
    if (props) {
        IAIMPString* name = nullptr;
        if (props->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, (void**)&name) == S_OK && name) {
            result = WStr(name->GetData());
            name->Release();
        }
    }
    return result;
}

std::string GetPlaylistId(IAIMPPlaylist* pl) {
    std::string result = "";
    PlaylistProps props(pl);
    if (props) {
        IAIMPString* idStr = nullptr;
        if (props->GetValueAsObject(AIMP_PLAYLIST_PROPID_ID, IID_IAIMPString, (void**)&idStr) == S_OK && idStr) {
            result = WStr(idStr->GetData());
            idStr->Release();
        }
    }
    return result;
}

// AIMP_PLAYLIST_PROPID_PLAYINGINDEX = 52
int GetPlayingIndex(IAIMPPlaylist* pl) {
    int index = -1;
    PlaylistProps props(pl);
    if (props) props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_PLAYINGINDEX, &index);
    return index;
}

// AIMP_PLAYLIST_PROPID_FOCUSINDEX = 50
int GetFocusedIndex(IAIMPPlaylist* pl) {
    int index = -1;
    PlaylistProps props(pl);
    if (props) props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &index);
    return index;
}

// AIMP_PLAYLIST_PROPID_DURATION = 54
double GetPlaylistDuration(IAIMPPlaylist* pl) {
    double duration = 0;
    PlaylistProps props(pl);
    if (props) props->GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION, &duration);
    return duration;
}


// Получить IAIMPFileInfo из IAIMPPlaylistItem через GetValueAsObject(PROPID_FILEINFO)
// Согласно SDK, AIMP_PLAYLISTITEM_PROPID_FILEINFO = 2, тип IAIMPFileInfo
// IAIMPFileInfo — это IAIMPPropertyList, PropertyID для полей:
//   AIMP_FILEINFO_PROPID_TITLE    = 1
//   AIMP_FILEINFO_PROPID_ARTIST   = 2
//   AIMP_FILEINFO_PROPID_ALBUM    = 3
//   AIMP_FILEINFO_PROPID_YEAR     = 4 (строка)
//   AIMP_FILEINFO_PROPID_GENRE    = 6
//   AIMP_FILEINFO_PROPID_DURATION = 11 (double)
//   AIMP_FILEINFO_PROPID_BITRATE  = 12 (int32, кбит/с)
// Правильный IID для IAIMPFileInfo нужно взять из apiFileManager.h
#include "sdk/apiFileManager.h"

void GetFileInfo(IAIMPPlaylistItem* item, json& result) {
    if (!item) return;

    // Имя файла
    IAIMPString* s = nullptr;
    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, (void**)&s) == S_OK && s) {
        result["file_path"] = WStr(s->GetData());
        s->Release();
        s = nullptr;
    }

    // Метаданные через IAIMPFileInfo
    // AIMP_PLAYLISTITEM_PROPID_FILEINFO = 2, возвращает IAIMPFileInfo
    IAIMPFileInfo* fileInfo = nullptr;
    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo, (void**)&fileInfo) == S_OK && fileInfo) {
        // Правильные ID из apiFileManager.h:
        // AIMP_FILEINFO_PROPID_TITLE  = 25
        // AIMP_FILEINFO_PROPID_ARTIST = 6
        // AIMP_FILEINFO_PROPID_ALBUM  = 1
        // AIMP_FILEINFO_PROPID_DATE   = 14 (год как строка)
        // AIMP_FILEINFO_PROPID_GENRE  = 20
        // AIMP_FILEINFO_PROPID_DURATION = 17 (double, секунды)
        // AIMP_FILEINFO_PROPID_BITRATE  = 7  (int32, кбит/с)

        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["title"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }
        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_ARTIST, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["artist"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }
        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_ALBUM, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["album"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }
        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_DATE, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["year"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }
        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_GENRE, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["genre"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }

        double duration = 0;
        if (fileInfo->GetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, &duration) == S_OK)
            result["duration"] = duration;

        int bitrate = 0;
        if (fileInfo->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITRATE, &bitrate) == S_OK)
            result["bitrate"] = bitrate;

        if (fileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_TRACKNUMBER, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["track_number"] = WStr(s->GetData()); s->Release(); s = nullptr;
        }

        fileInfo->Release();
    }

    // Fallback: display text как title
    if (!result.contains("title") || result["title"].get<std::string>().empty()) {
        if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_DISPLAYTEXT, IID_IAIMPString, (void**)&s) == S_OK && s) {
            result["title"] = WStr(s->GetData());
            s->Release();
        }
    }
}

// ==========================================
// API-функции
// ==========================================

json GetPlayerStatus() {
    json r;
    // Константы состояния: AIMP_PLAYER_STATE_STOPPED=0, PAUSED=1, PLAYING=2
    r["state"] = "stopped";
    r["volume"] = 0;
    r["muted"] = false;
    r["position"] = 0.0;
    r["duration"] = 0.0;

    if (!g_core) return r;

    IAIMPServicePlayer* player = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
        int st = player->GetState();
        if (st == AIMP_PLAYER_STATE_PLAYING)      r["state"] = "playing";
        else if (st == AIMP_PLAYER_STATE_PAUSED)  r["state"] = "paused";
        else                                       r["state"] = "stopped";

        double pos = 0;
        player->GetPosition(&pos);
        r["position"] = pos;

        double dur = 0;
        player->GetDuration(&dur);
        r["duration"] = dur;

        float vol = 0;
        player->GetVolume(&vol);
        r["volume"] = (int)(vol * 100.0f);

        BOOL muted = FALSE;
        player->GetMute(&muted);
        r["muted"] = (muted != FALSE);

        // Текущий играющий трек
        IAIMPPlaylistItem* playingItem = nullptr;
        if (player->GetPlaylistItem(&playingItem) == S_OK && playingItem) {
            IAIMPString* fn = nullptr;
            if (playingItem->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, (void**)&fn) == S_OK && fn) {
                r["filenameplaying"] = WStr(fn->GetData());
                fn->Release();
            }
            json trackInfo;
            GetFileInfo(playingItem, trackInfo);
            if (trackInfo.contains("title"))  r["track_title"]  = trackInfo["title"];
            if (trackInfo.contains("artist")) r["track_artist"] = trackInfo["artist"];
            if (trackInfo.contains("album"))  r["track_album"]  = trackInfo["album"];
            playingItem->Release();
        }

        player->Release();
    }

    return r;
}

// Возвращает список всех загруженных плейлистов
json GetPlaylistsResponse() {
    json r;
    r["playlists"] = json::array();

    if (!g_core) {
        r["error"] = "core not initialized";
        return r;
    }

    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "playlist manager unavailable";
        return r;
    }

    // Активный плейлист (выбранная вкладка)
    IAIMPPlaylist* activePl = nullptr;
    mgr->GetActivePlaylist(&activePl);

    // Воспроизводимый плейлист (тот, из которого играет трек)
    IAIMPPlaylist* playingPl = nullptr;
    mgr->GetPlayingPlaylist(&playingPl);

    int count = mgr->GetLoadedPlaylistCount();
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(i, &pl) != S_OK || !pl)
            continue;

        json p;
        p["id"]          = i;                    // числовой индекс для маршрутизации (/api/playlists/0/tracks и т.д.)
        p["aimp_id"]     = GetPlaylistId(pl);    // реальный строковый ID из AIMP (AIMP_PLAYLIST_PROPID_ID)
        p["name"]        = GetPlaylistName(pl);
        p["track_count"] = pl->GetItemCount();
        p["duration"]    = GetPlaylistDuration(pl);

        bool isActive  = (activePl  && activePl  == pl);
        bool isPlaying = (playingPl && playingPl == pl);

        if (isPlaying) {
            int playingIdx = GetPlayingIndex(pl);
            p["state"] = (playingIdx >= 0) ? "playing" : "active";
        } else if (isActive) {
            p["state"] = "active";
        } else {
            p["state"] = nullptr;
        }

        r["playlists"].push_back(p);
        pl->Release();
    }

    if (activePl)  activePl->Release();
    if (playingPl) playingPl->Release();
    mgr->Release();

    return r;
}

json GetPlaylistResponse(int playlistIdx) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "playlist manager unavailable"; return r;
    }

    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistIdx, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"]["code"]    = "PLAYLIST_NOT_FOUND";
        r["error"]["message"] = "Playlist index out of range";
        return r;
    }

    IAIMPPlaylist* activePl  = nullptr; mgr->GetActivePlaylist(&activePl);
    IAIMPPlaylist* playingPl = nullptr; mgr->GetPlayingPlaylist(&playingPl);

    r["id"]          = playlistIdx;
    r["name"]        = GetPlaylistName(pl);
    r["track_count"] = pl->GetItemCount();
    r["duration"]    = GetPlaylistDuration(pl);

    bool isPlaying = (playingPl && playingPl == pl);
    bool isActive  = (activePl  && activePl  == pl);
    if (isPlaying) {
        int idx = GetPlayingIndex(pl);
        r["state"] = (idx >= 0) ? "playing" : "active";
    } else if (isActive) {
        r["state"] = "active";
    } else {
        r["state"] = nullptr;
    }

    pl->Release();
    if (activePl)  activePl->Release();
    if (playingPl) playingPl->Release();
    mgr->Release();
    return r;
}

json GetTracksResponse(int playlistIdx, int limit, int offset) {
    json r;
    r["playlist_id"] = playlistIdx;
    r["tracks"]      = json::array();
    r["offset"]      = offset;
    r["limit"]       = limit;

    if (!g_core) { r["error"] = "core not initialized"; return r; }

    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "playlist manager unavailable"; return r;
    }

    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistIdx, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"]["code"]    = "PLAYLIST_NOT_FOUND";
        r["error"]["message"] = "Playlist not found";
        return r;
    }
    mgr->Release();

    int total = pl->GetItemCount();
    r["total"] = total;

    int playingIdx = GetPlayingIndex(pl);
    int focusedIdx = GetFocusedIndex(pl);

    int end = std::min(offset + limit, total);
    for (int i = offset; i < end; i++) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(i, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item)
            continue;

        json t;
        t["id"]                   = i;
        t["position_in_playlist"] = i + 1;
        GetFileInfo(item, t);

        if (i == playingIdx)      t["state"] = "playing";
        else if (i == focusedIdx) t["state"] = "focused";
        else                      t["state"] = nullptr;

        r["tracks"].push_back(t);
        item->Release();
    }

    pl->Release();
    return r;
}

json GetTrackResponse(int playlistIdx, int trackIdx) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "playlist manager unavailable"; return r;
    }

    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistIdx, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"]["code"]    = "PLAYLIST_NOT_FOUND";
        r["error"]["message"] = "Playlist not found";
        return r;
    }
    mgr->Release();

    int total = pl->GetItemCount();
    if (trackIdx < 0 || trackIdx >= total) {
        pl->Release();
        r["error"]["code"]    = "TRACK_NOT_FOUND";
        r["error"]["message"] = "Track index out of range";
        return r;
    }

    IAIMPPlaylistItem* item = nullptr;
    if (pl->GetItem(trackIdx, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item) {
        pl->Release();
        r["error"]["code"]    = "TRACK_NOT_FOUND";
        r["error"]["message"] = "Cannot get track item";
        return r;
    }

    r["id"]                   = trackIdx;
    r["position_in_playlist"] = trackIdx + 1;
    GetFileInfo(item, r);

    int playingIdx = GetPlayingIndex(pl);
    int focusedIdx = GetFocusedIndex(pl);
    if (trackIdx == playingIdx)      r["state"] = "playing";
    else if (trackIdx == focusedIdx) r["state"] = "focused";
    else                             r["state"] = nullptr;

    item->Release();
    pl->Release();
    return r;
}

// ==========================================
// HTTP парсер
// ==========================================
struct HttpRequest {
    std::string method, path, body;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> headers;
};

HttpRequest ParseRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream ss(raw);
    std::string line;

    if (std::getline(ss, line)) {
        std::istringstream ls(line);
        ls >> req.method >> req.path;

        size_t q = req.path.find('?');
        if (q != std::string::npos) {
            std::string query = req.path.substr(q + 1);
            req.path = req.path.substr(0, q);
            std::istringstream qs(query);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                size_t e = pair.find('=');
                if (e != std::string::npos)
                    req.params[pair.substr(0, e)] = pair.substr(e + 1);
            }
        }
    }

    while (std::getline(ss, line) && line != "\r" && !line.empty()) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(' '));
            req.headers[key] = val;
        }
    }

    std::string rest;
    while (std::getline(ss, line)) rest += line + "\n";
    req.body = rest;

    return req;
}

// ==========================================
// HTTP Сервер
// ==========================================
void SendResponse(SOCKET client, int code, const json& body) {
    std::string jsonStr = body.dump();
    std::string status =
        (code == 200) ? "OK" :
        (code == 201) ? "Created" :
        (code == 400) ? "Bad Request" :
        (code == 404) ? "Not Found" :
        (code == 409) ? "Conflict" : "Internal Server Error";

    std::string response =
        "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: " + std::to_string(jsonStr.size()) + "\r\n"
        "Connection: close\r\n\r\n" + jsonStr;

    send(client, response.c_str(), (int)response.size(), 0);
}

// Парсинг пути /api/playlists/:id/tracks/:track_id/action
struct ParsedPath {
    int playlistId = -1;
    int trackId    = -1;
    std::string action;
};

ParsedPath ParsePath(const std::string& path) {
    ParsedPath result;
    std::string p = path;

    if (p.find("/api/") == 0) p = p.substr(5);

    if (p.find("playlists/") == 0) {
        p = p.substr(10);
        size_t slash = p.find('/');
        if (slash != std::string::npos) {
            try { result.playlistId = std::stoi(p.substr(0, slash)); } catch(...) {}
            p = p.substr(slash + 1);

            if (p.find("tracks/") == 0) {
                p = p.substr(7);
                slash = p.find('/');
                if (slash != std::string::npos) {
                    try { result.trackId = std::stoi(p.substr(0, slash)); } catch(...) {}
                    result.action = p.substr(slash + 1);
                } else {
                    try { result.trackId = std::stoi(p); } catch(...) {}
                    result.action = "info";
                }
            } else {
                result.action = p.empty() ? "info" : p;
            }
        } else {
            // /api/playlists/N  без слеша после
            try { result.playlistId = std::stoi(p); } catch(...) {}
            result.action = "info";
        }
    }

    return result;
}

void RunHttpServer() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { WSACleanup(); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(srv); WSACleanup(); return;
    }

    listen(srv, SOMAXCONN);
    g_running = true;

    while (g_running) {
        fd_set rs; FD_ZERO(&rs); FD_SET(srv, &rs);
        timeval to{1, 0};
        if (select(0, &rs, nullptr, nullptr, &to) <= 0) continue;

        SOCKET cl = accept(srv, nullptr, nullptr);
        if (cl == INVALID_SOCKET) continue;

        char buf[16384];
        int n = recv(cl, buf, sizeof(buf) - 1, 0);

        if (n > 0) {
            buf[n] = 0;
            HttpRequest req = ParseRequest(std::string(buf, n));
            json rsp;
            int code = 200;

            std::lock_guard<std::mutex> lock(g_mutex);

            // OPTIONS — preflight CORS
            if (req.method == "OPTIONS") {
                std::string cors =
                    "HTTP/1.1 200 OK\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Content-Length: 0\r\n\r\n";
                send(cl, cors.c_str(), (int)cors.size(), 0);
                closesocket(cl);
                continue;
            }

            // GET /api/player/status
            else if (req.path == "/api/player/status" && req.method == "GET") {
                rsp = GetPlayerStatus();
            }

            // POST /api/player/play
            else if (req.path == "/api/player/play" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->Resume();
                    int st = player->GetState();
                    rsp["state"] = (st == AIMP_PLAYER_STATE_PLAYING) ? "playing" :
                                   (st == AIMP_PLAYER_STATE_PAUSED)  ? "paused"  : "stopped";
                    player->Release();
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // POST /api/player/pause
            else if (req.path == "/api/player/pause" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->Pause();
                    int st = player->GetState();
                    rsp["state"] = (st == AIMP_PLAYER_STATE_PAUSED) ? "paused" : "stopped";
                    player->Release();
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // POST /api/player/stop
            else if (req.path == "/api/player/stop" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->Stop();
                    rsp["state"] = "stopped";
                    player->Release();
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // POST /api/player/next
            else if (req.path == "/api/player/next" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->GoToNext();
                    player->Release();
                    rsp["ok"] = true;
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // POST /api/player/prev
            else if (req.path == "/api/player/prev" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->GoToPrev();
                    player->Release();
                    rsp["ok"] = true;
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // GET /api/player/volume
            else if (req.path == "/api/player/volume" && req.method == "GET") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    float vol = 0;
                    player->GetVolume(&vol);
                    BOOL muted = FALSE;
                    player->GetMute(&muted);
                    rsp["volume"] = (int)(vol * 100.0f);
                    rsp["muted"]  = (muted != FALSE);
                    player->Release();
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // PUT /api/player/volume
            else if (req.path == "/api/player/volume" && (req.method == "PUT" || req.method == "POST")) {
                float vol = -1;
                if (req.params.count("volume")) {
                    try { vol = std::stof(req.params["volume"]) / 100.0f; } catch(...) {}
                }
                if (vol < 0 && !req.body.empty()) {
                    try {
                        json b = json::parse(req.body);
                        if (b.contains("volume")) vol = b["volume"].get<float>() / 100.0f;
                    } catch(...) {}
                }
                if (vol >= 0.0f && vol <= 1.0f) {
                    IAIMPServicePlayer* player = nullptr;
                    if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                        player->SetVolume(vol);
                        rsp["volume"] = (int)(vol * 100.0f);
                        player->Release();
                    }
                } else { rsp["error"]["code"] = "INVALID_VOLUME"; rsp["error"]["message"] = "Volume must be 0-100"; code = 400; }
            }

            // POST /api/player/mute
            else if (req.path == "/api/player/mute" && req.method == "POST") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    BOOL muted = FALSE;
                    player->GetMute(&muted);
                    player->SetMute(!muted);
                    rsp["muted"] = (muted == FALSE); // новое состояние — инверсия
                    player->Release();
                } else { rsp["error"]["code"] = "NO_PLAYER"; code = 500; }
            }

            // PUT /api/player/position
            else if (req.path == "/api/player/position" && (req.method == "PUT" || req.method == "POST")) {
                double pos = -1;
                if (req.params.count("position")) {
                    try { pos = std::stod(req.params["position"]); } catch(...) {}
                }
                if (pos < 0 && !req.body.empty()) {
                    try {
                        json b = json::parse(req.body);
                        if (b.contains("position")) pos = b["position"].get<double>();
                    } catch(...) {}
                }
                if (pos >= 0) {
                    IAIMPServicePlayer* player = nullptr;
                    if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                        player->SetPosition(pos);
                        rsp["position"] = pos;
                        player->Release();
                    }
                } else { rsp["error"]["code"] = "INVALID_POSITION"; code = 400; }
            }

            // GET /api/playlists
            else if (req.path == "/api/playlists" && req.method == "GET") {
                rsp = GetPlaylistsResponse();
            }

            // /api/playlists/:id/...
            else if (req.path.find("/api/playlists/") == 0) {
                ParsedPath pp = ParsePath(req.path);

                if (pp.playlistId >= 0 && pp.trackId < 0) {
                    // Действия с плейлистом
                    if (pp.action == "info" && req.method == "GET") {
                        rsp = GetPlaylistResponse(pp.playlistId);
                    }
                    else if (pp.action == "tracks" && req.method == "GET") {
                        int limit = 50, offset = 0;
                        if (req.params.count("limit"))  try { limit  = std::stoi(req.params["limit"]);  } catch(...) {}
                        if (req.params.count("offset")) try { offset = std::stoi(req.params["offset"]); } catch(...) {}
                        rsp = GetTracksResponse(pp.playlistId, limit, offset);
                    }
                    else if (pp.action == "select" && req.method == "POST") {
                        IAIMPServicePlaylistManager* mgr = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) == S_OK && mgr) {
                            IAIMPPlaylist* pl = nullptr;
                            if (mgr->GetLoadedPlaylist(pp.playlistId, &pl) == S_OK && pl) {
                                mgr->SetActivePlaylist(pl);
                                rsp["id"]    = pp.playlistId;
                                rsp["state"] = "active";
                                pl->Release();
                            } else { rsp["error"]["code"] = "PLAYLIST_NOT_FOUND"; code = 404; }
                            mgr->Release();
                        }
                    }
                    else if (pp.action == "play" && req.method == "POST") {
                        IAIMPServicePlaylistManager* mgr = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) == S_OK && mgr) {
                            IAIMPPlaylist* pl = nullptr;
                            if (mgr->GetLoadedPlaylist(pp.playlistId, &pl) == S_OK && pl) {
                                mgr->SetActivePlaylist(pl);
                                IAIMPServicePlayer* player = nullptr;
                                if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                                    player->Play3(pl); // играть весь плейлист с начала
                                    player->Release();
                                }
                                rsp["id"]    = pp.playlistId;
                                rsp["state"] = "playing";
                                pl->Release();
                            } else { rsp["error"]["code"] = "PLAYLIST_NOT_FOUND"; code = 404; }
                            mgr->Release();
                        }
                    }
                    else {
                        rsp["error"]["code"] = "NOT_FOUND"; code = 404;
                    }
                }
                else if (pp.playlistId >= 0 && pp.trackId >= 0) {
                    // Действия с треком
                    if (pp.action == "info" && req.method == "GET") {
                        rsp = GetTrackResponse(pp.playlistId, pp.trackId);
                    }
                    else if (pp.action == "play" && req.method == "POST") {
                        IAIMPServicePlaylistManager* mgr = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) == S_OK && mgr) {
                            IAIMPPlaylist* pl = nullptr;
                            if (mgr->GetLoadedPlaylist(pp.playlistId, &pl) == S_OK && pl) {
                                IAIMPPlaylistItem* item = nullptr;
                                if (pl->GetItem(pp.trackId, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
                                    IAIMPServicePlayer* player = nullptr;
                                    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                                        player->Play2(item);
                                        player->Release();
                                    }
                                    rsp["id"]    = pp.trackId;
                                    rsp["state"] = "playing";
                                    item->Release();
                                } else { rsp["error"]["code"] = "TRACK_NOT_FOUND"; code = 404; }
                                pl->Release();
                            } else { rsp["error"]["code"] = "PLAYLIST_NOT_FOUND"; code = 404; }
                            mgr->Release();
                        }
                    }
                    else if (pp.action == "select" && req.method == "POST") {
                        IAIMPServicePlaylistManager* mgr = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) == S_OK && mgr) {
                            IAIMPPlaylist* pl = nullptr;
                            if (mgr->GetLoadedPlaylist(pp.playlistId, &pl) == S_OK && pl) {
                                PlaylistProps props(pl);
                                if (props) props->SetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, pp.trackId);
                                rsp["id"]    = pp.trackId;
                                rsp["state"] = "focused";
                                pl->Release();
                            } else { rsp["error"]["code"] = "PLAYLIST_NOT_FOUND"; code = 404; }
                            mgr->Release();
                        }
                    }
                    else if (pp.action == "duration" && req.method == "GET") {
                        json ti = GetTrackResponse(pp.playlistId, pp.trackId);
                        rsp["id"]       = pp.trackId;
                        rsp["duration"] = ti.value("duration", 0.0);
                    }
                    else {
                        rsp["error"]["code"] = "NOT_FOUND"; code = 404;
                    }
                }
                else {
                    rsp["error"]["code"] = "INVALID_PATH"; code = 400;
                }
            }

            // GET /api/
            else if (req.path == "/api/" || req.path == "/api") {
                rsp["name"]    = "AIMP HTTP Control API v2.0";
                rsp["version"] = "2.0";
                rsp["endpoints"] = json::array({
                    "GET  /api/player/status",
                    "POST /api/player/play",
                    "POST /api/player/pause",
                    "POST /api/player/stop",
                    "POST /api/player/next",
                    "POST /api/player/prev",
                    "GET  /api/player/volume",
                    "PUT  /api/player/volume",
                    "POST /api/player/mute",
                    "PUT  /api/player/position",
                    "GET  /api/playlists",
                    "GET  /api/playlists/:id",
                    "GET  /api/playlists/:id/tracks",
                    "GET  /api/playlists/:id/tracks/:tid",
                    "POST /api/playlists/:id/play",
                    "POST /api/playlists/:id/select",
                    "POST /api/playlists/:id/tracks/:tid/play",
                    "POST /api/playlists/:id/tracks/:tid/select"
                });
            }

            else {
                rsp["error"]["code"]    = "NOT_FOUND";
                rsp["error"]["message"] = "Endpoint not found";
                code = 404;
            }

            SendResponse(cl, code, rsp);
        }
        closesocket(cl);
    }

    closesocket(srv);
    WSACleanup();
}

// ==========================================
// Плагин AIMP
// ==========================================
class HttpControlPlugin : public IAIMPPlugin {
    LONG ref = 1;
public:
    virtual ~HttpControlPlugin() = default;

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IAIMPPlugin*>(this);
        AddRef();
        return S_OK;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref); }
    ULONG WINAPI Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return r;
    }

    TChar* WINAPI InfoGet(int Index) override {
        static wchar_t n[] = L"AIMP HTTP Control API v2";
        static wchar_t a[] = L"DebianDev";
        static wchar_t d[] = L"Full REST API on port 3553";
        switch (Index) {
            case AIMP_PLUGIN_INFO_NAME:        return n;
            case AIMP_PLUGIN_INFO_AUTHOR:      return a;
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:  return d;
            default:                           return nullptr;
        }
    }
    LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override {}

    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        g_core = core;
        g_serverThread = std::thread(RunHttpServer);
        g_serverThread.detach();
        return S_OK;
    }

    HRESULT WINAPI Finalize() override {
        g_running = false;
        Sleep(200);
        g_core = nullptr;
        return S_OK;
    }
};

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = new HttpControlPlugin();
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
