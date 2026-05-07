// aimp_http_server.cpp
// AIMP HTTP REST API Server Plugin
// Version: 1.1
// SDK: AIMP 5.40+

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
#include <functional>
#include <atomic>

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
std::atomic<bool> g_running(false);
std::thread g_serverThread;
int g_port = 3553;

// ==========================================
// Утилиты
// ==========================================

// Конвертация wchar_t* -> UTF-8 std::string
std::string WStr(const wchar_t* w) {
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// Получить свойства плейлиста
IAIMPPlaylistProperties* GetPlaylistProps(IAIMPPlaylist* pl) {
    if (!pl) return nullptr;
    IAIMPPlaylistProperties* props = nullptr;
    pl->QueryInterface(IID_IAIMPPlaylistProperties, (void**)&props);
    return props;
}

static int GetPlaylistID(IAIMPPlaylist* pl) {
    if (!pl) return -1;
    IAIMPPlaylistProperties* props = GetPlaylistProps(pl);
    if (!props) return -1;

    int id = -1;
    props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_ID, &id);
    props->Release();
    return id;
}

// Получить имя плейлиста
std::string GetPlaylistName(IAIMPPlaylist* pl) {
    std::string result = "Unknown";
    IAIMPPlaylistProperties* props = GetPlaylistProps(pl);
    if (props) {
        IAIMPString* name = nullptr;
        if (props->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, (void**)&name) == S_OK && name) {
            result = WStr(name->GetData());
            name->Release();
        }
        props->Release();
    }
    return result;
}

// Получить индекс играющего трека
int GetPlayingIndex(IAIMPPlaylist* pl) {
    int index = -1;
    IAIMPPlaylistProperties* props = GetPlaylistProps(pl);
    if (props) {
        props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_PLAYINGINDEX, &index);
        props->Release();
    }
    return index;
}

// Получить индекс сфокусированного трека (на котором курсор)
int GetFocusedIndex(IAIMPPlaylist* pl) {
    int index = -1;
    IAIMPPlaylistProperties* props = GetPlaylistProps(pl);
    if (props) {
        props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &index);
        props->Release();
    }
    return index;
}

static bool SetFocusedIndex(IAIMPPlaylist* pl, int index) {
    if (!pl || index < 0) return false;

    IAIMPPlaylistProperties* props = GetPlaylistProps(pl);
    if (!props) return false;

    // AIMP playlist focus cursor
    HRESULT hr = props->SetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, index);
    props->Release();
    return hr == S_OK;
}

// Извлечь информацию о треке из IAIMPPlaylistItem
json ExtractTrackInfo(IAIMPPlaylistItem* item) {
    json track;
    IAIMPString* s = nullptr;
    
    // Filename
    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, (void**)&s) == S_OK && s) {
        track["filename"] = WStr(s->GetData());
        s->Release();
    }
    
    // Индекс
    int index = -1;
    if (item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_INDEX, &index) == S_OK) {
        track["index"] = index;
    }
    
    // Выделен ли?
    int selected = 0;
    if (item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, &selected) == S_OK) {
        track["selected"] = (selected != 0);
    }
    
    // FileInfo (метаданные)
    IUnknown* unk = nullptr;
    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPPropertyList, (void**)&unk) == S_OK && unk) {
        IAIMPPropertyList* props = static_cast<IAIMPPropertyList*>(unk);
        
        if (props->GetValueAsObject(1, IID_IAIMPString, (void**)&s) == S_OK && s) {
            track["title"] = WStr(s->GetData()); s->Release();
        }
        if (props->GetValueAsObject(2, IID_IAIMPString, (void**)&s) == S_OK && s) {
            track["artist"] = WStr(s->GetData()); s->Release();
        }
        if (props->GetValueAsObject(3, IID_IAIMPString, (void**)&s) == S_OK && s) {
            track["album"] = WStr(s->GetData()); s->Release();
        }
        if (props->GetValueAsObject(4, IID_IAIMPString, (void**)&s) == S_OK && s) {
            track["genre"] = WStr(s->GetData()); s->Release();
        }
        if (props->GetValueAsObject(5, IID_IAIMPString, (void**)&s) == S_OK && s) {
            track["year"] = WStr(s->GetData()); s->Release();
        }
        
        double duration = 0;
        if (props->GetValueAsFloat(6, &duration) == S_OK) {
            track["duration"] = duration;
        }
        
        int bitrate = 0;
        if (props->GetValueAsInt32(7, &bitrate) == S_OK) {
            track["bitrate"] = bitrate;
        }
        
        int samplerate = 0;
        if (props->GetValueAsInt32(8, &samplerate) == S_OK) {
            track["samplerate"] = samplerate;
        }
        
        int channels = 0;
        if (props->GetValueAsInt32(9, &channels) == S_OK) {
            track["channels"] = channels;
        }
        
        props->Release();
    }
    
    return track;
}

// ==========================================
// AIMP API функции
// ==========================================

json GetActivePlaylistID() {
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        return json{{"success", false}, {"error", "No playlist manager"}};
    }
    
    IAIMPPlaylist* pl = nullptr;
    mgr->GetActivePlaylist(&pl);
    mgr->Release();
    
    if (!pl) {
        return json{{"success", false}, {"error", "No active playlist"}};
    }
    
    // Ищем ID активного плейлиста
    IAIMPServicePlaylistManager* mgr2 = nullptr;
    g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr2);
    int count = mgr2 ? mgr2->GetLoadedPlaylistCount() : 0;
    int activeId = -1;
    
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* p = nullptr;
        if (mgr2->GetLoadedPlaylist(i, &p) == S_OK && p) {
            if (p == pl) {
                activeId = i;
                p->Release();
                break;
            }
            p->Release();
        }
    }
    
    pl->Release();
    if (mgr2) mgr2->Release();
    
    return json{{"success", true}, {"playlist_id", activeId}};
}

// helper: текущий playlist для UI/selection
static bool GetCurrentPlaylist(IAIMPServicePlaylistManager* mgr, IAIMPPlaylist** out) {
    if (!mgr || !out) return false;
    *out = nullptr;

    // 1) Active
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) == S_OK && pl) {
        *out = pl;
        return true;
    }

    // 2) Playing (fallback)
    if (mgr->GetPlayingPlaylist(&pl) == S_OK && pl) {
        *out = pl;
        return true;
    }

    return false;
}

// GET /api/player — полное состояние плеера
json GetPlayerState() {
    json r;
    r["success"] = false;
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlayer* player = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) != S_OK || !player) {
        r["error"] = "No player"; return r;
    }
    
    // Основное состояние
    double pos = 0; player->GetPosition(&pos);
    float vol = 0; player->GetVolume(&vol);
    int st = player->GetState();
    
    r["position"] = pos;
    r["volume"] = vol;
    r["state"] = (st == 1) ? "playing" : (st == 2) ? "paused" : "stopped";
    r["state_code"] = st;
    
    // Дополнительно — текущий трек
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) == S_OK && mgr) {
        IAIMPPlaylist* pl = nullptr;
        if (GetCurrentPlaylist(mgr, &pl) && pl) {
            int playingIdx = GetPlayingIndex(pl);
            int focusedIdx = GetFocusedIndex(pl);
            
            r["playing_index"] = playingIdx;
            r["focused_index"] = focusedIdx;
            // new UI-selection concept (selected != playing)
            r["selected_index"] = focusedIdx;
            
            r["playlist_name"] = GetPlaylistName(pl);
            r["playlist_tracks"] = pl->GetItemCount();
            
            // Информация о текущем треке (playing)
            if (playingIdx >= 0) {
                IAIMPPlaylistItem* item = nullptr;
                if (pl->GetItem(playingIdx, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
                    json trackInfo = ExtractTrackInfo(item);
                    r["track"] = trackInfo;
                    item->Release();
                }
            }
            
            // Информация о выбранном треке (selected == focus)
            if (focusedIdx >= 0) {
                IAIMPPlaylistItem* item = nullptr;
                if (pl->GetItem(focusedIdx, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
                    r["selected_track"] = ExtractTrackInfo(item);
                    item->Release();
                }
            }
            
            pl->Release();
        }
        mgr->Release();
    }
    
    r["success"] = true;
    player->Release();
    return r;
}

// GET /api/player/track — текущий трек
json GetCurrentTrack() {
    json r;
    r["success"] = false;
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "No active playlist"; return r;
    }
    
    int playingIdx = GetPlayingIndex(pl);
    if (playingIdx >= 0) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(playingIdx, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            r["track"] = ExtractTrackInfo(item);
            r["playing_index"] = playingIdx;
            r["success"] = true;
            item->Release();
        }
    } else {
        r["error"] = "No track playing";
    }
    
    pl->Release();
    mgr->Release();
    return r;
}

// GET /api/player/track/focused — трек на курсоре
json GetFocusedTrack() {
    json r;
    r["success"] = false;
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "No active playlist"; return r;
    }
    
    int focusedIdx = GetFocusedIndex(pl);
    if (focusedIdx >= 0) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(focusedIdx, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            r["track"] = ExtractTrackInfo(item);
            r["focused_index"] = focusedIdx;
            r["success"] = true;
            item->Release();
        }
    } else {
        r["error"] = "No track focused";
    }
    
    pl->Release();
    mgr->Release();
    return r;
}

// GET /api/player/track/selected — selected == FOCUSINDEX (UI selection, not playing)
json GetSelectedTrack() {
    json r;
    r["success"] = false;
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "No active playlist"; return r;
    }
    
    int focusedIdx = GetFocusedIndex(pl);
    if (focusedIdx >= 0) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(focusedIdx, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            r["selected_index"] = focusedIdx;
            r["track"] = ExtractTrackInfo(item);
            r["success"] = true;
            item->Release();
        } else {
            r["error"] = "Cannot get focused track item";
        }
    } else {
        r["error"] = "No track focused";
    }
    
    pl->Release();
    mgr->Release();
    return r;
}

// POST /api/player/play — запустить трек из активного плейлиста
json PlayTrackInPlaylist(int trackIndex) {
    json r;
    r["success"] = false;
    
    if (trackIndex < 0) {
        r["error"] = "Invalid track index"; return r;
    }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (!g_core || g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "No active playlist"; return r;
    }
    
    if (trackIndex >= pl->GetItemCount()) {
        pl->Release(); mgr->Release();
        r["error"] = "Track index out of range"; return r;
    }
    
    IAIMPPlaylistItem* item = nullptr;
    if (pl->GetItem(trackIndex, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
            player->Play2(item);
            r["action"] = "play";
            r["track_index"] = trackIndex;
            r["success"] = true;
            player->Release();
        }
        item->Release();
    } else {
        r["error"] = "Cannot get track item";
    }
    
    pl->Release();
    mgr->Release();
    return r;
}

// GET /api/playlists — список плейлистов
json GetPlaylists() {
    json r;
    r["success"] = false;
    r["playlists"] = json::array();
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* activePl = nullptr;
    mgr->GetActivePlaylist(&activePl);
    
    int count = mgr->GetLoadedPlaylistCount();
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(i, &pl) == S_OK && pl) {
            json p;
            p["id"] = GetPlaylistID(pl);
            p["name"] = GetPlaylistName(pl);
            p["tracks_count"] = pl->GetItemCount();
            p["active"] = (pl == activePl);
            
            int playingIdx = GetPlayingIndex(pl);
            p["playing_index"] = playingIdx;
            
            int focusedIdx = GetFocusedIndex(pl);
            p["focused_index"] = focusedIdx;
            
            r["playlists"].push_back(p);
            pl->Release();
        }
    }
    
    if (activePl) activePl->Release();
    
    r["total"] = count;
    r["success"] = true;
    mgr->Release();
    return r;
}

// GET /api/playlist/{id} — информация о плейлисте
json GetPlaylistInfo(int playlistId) {
    json r;
    r["success"] = false;
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistId, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "Playlist not found"; return r;
    }
    
    r["id"] = playlistId;
    r["name"] = GetPlaylistName(pl);
    r["tracks_count"] = pl->GetItemCount();
    r["playing_index"] = GetPlayingIndex(pl);
    r["focused_index"] = GetFocusedIndex(pl);
    r["success"] = true;
    
    pl->Release();
    mgr->Release();
    return r;
}

// GET /api/playlist/{id}/tracks — треки в плейлисте
json GetPlaylistTracks(int playlistId) {
    json r;
    r["success"] = false;
    r["tracks"] = json::array();
    
    if (!g_core) { r["error"] = "No core"; return r; }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistId, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "Playlist not found"; return r;
    }
    
    int count = pl->GetItemCount();
    int playingIdx = GetPlayingIndex(pl);
    int focusedIdx = GetFocusedIndex(pl);
    
    r["name"] = GetPlaylistName(pl);
    r["playing_index"] = playingIdx;
    r["focused_index"] = focusedIdx;
    // new UI-selection concept
    r["selected_index"] = focusedIdx;
    
    for (int i = 0; i < count; i++) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(i, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            json track = ExtractTrackInfo(item);
            track["id"] = i;
            track["playing"] = (i == playingIdx);
            track["focused"] = (i == focusedIdx);
            track["selected"] = (i == focusedIdx); // selected == focus
            r["tracks"].push_back(track);
            item->Release();
        }
    }
    
    r["total"] = count;
    r["success"] = true;
    
    pl->Release();
    mgr->Release();
    return r;
}

// POST /api/playlist/{id}/play — запустить трек из плейлиста
json PlayPlaylistTrack(int playlistId, int trackIndex) {
    json r;
    r["success"] = false;
    
    if (playlistId < 0 || trackIndex < 0) {
        r["error"] = "Invalid playlist or track index"; return r;
    }
    
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (!g_core || g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager"; return r;
    }
    
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(playlistId, &pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "Playlist not found"; return r;
    }
    
    if (trackIndex >= pl->GetItemCount()) {
        pl->Release(); mgr->Release();
        r["error"] = "Track index out of range"; return r;
    }
    
    IAIMPPlaylistItem* item = nullptr;
    if (pl->GetItem(trackIndex, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
            player->Play2(item);
            r["action"] = "play";
            r["playlist_id"] = playlistId;
            r["track_id"] = trackIndex;
            r["success"] = true;
            player->Release();
        }
        item->Release();
    }
    
    pl->Release();
    mgr->Release();
    return r;
}

// POST /api/player/select — смена focus трека (selected != playing)
json SelectTrackInActivePlaylist(int trackIndex) {
    json r;
    r["success"] = false;

    if (trackIndex < 0) {
        r["error"] = "Invalid track index";
        return r;
    }

    if (!g_core) {
        r["error"] = "No core";
        return r;
    }

    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
        r["error"] = "No playlist manager";
        return r;
    }

    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetActivePlaylist(&pl) != S_OK || !pl) {
        mgr->Release();
        r["error"] = "No active playlist";
        return r;
    }

    if (trackIndex >= pl->GetItemCount()) {
        pl->Release();
        mgr->Release();
        r["error"] = "Track index out of range";
        return r;
    }

    bool ok = SetFocusedIndex(pl, trackIndex);
    pl->Release();
    mgr->Release();

    if (!ok) {
        r["error"] = "Cannot set focus index";
        return r;
    }

    r["action"] = "select";
    r["selected_index"] = trackIndex;
    r["success"] = true;
    return r;
}

// ==========================================
// HTTP парсер
// ==========================================
struct HttpRequest {
    std::string method, path, body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params;
};

HttpRequest ParseRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream ss(raw);
    std::string line;
    
    // Первая строка
    if (std::getline(ss, line)) {
        // Убираем \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        std::istringstream ls(line);
        ls >> req.method >> req.path;
        
        // Парсим query параметры
        size_t q = req.path.find('?');
        if (q != std::string::npos) {
            std::string query = req.path.substr(q + 1);
            req.path = req.path.substr(0, q);
            
            std::istringstream qs(query);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                size_t e = pair.find('=');
                if (e != std::string::npos) {
                    req.params[pair.substr(0, e)] = pair.substr(e + 1);
                }
            }
        }
    }
    
    // Заголовки
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // Убираем пробелы в начале значения
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            req.headers[key] = value;
        }
    }
    
    // Тело
    std::string rest;
    while (std::getline(ss, line)) {
        rest += line + "\n";
    }
    req.body = rest;
    
    return req;
}

// ==========================================
// HTTP Сервер
// ==========================================
void RunHttpServer() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(srv);
        WSACleanup();
        return;
    }
    
    if (listen(srv, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(srv);
        WSACleanup();
        return;
    }
    
    g_running = true;
    
    while (g_running) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(srv, &rs);
        timeval to{1, 0};
        
        if (select(0, &rs, nullptr, nullptr, &to) <= 0) continue;
        
        SOCKET cl = accept(srv, nullptr, nullptr);
        if (cl == INVALID_SOCKET) continue;
        
        char buf[32768];
        int n = recv(cl, buf, sizeof(buf) - 1, 0);
        
        if (n > 0) {
            buf[n] = 0;
            HttpRequest req = ParseRequest(std::string(buf));
            
            json rsp;
            int code = 200;
            std::string contentType = "application/json";
            
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                
                try {
                    // ========== API МАРШРУТЫ ==========
                    
                    // GET /api/ping
                    if (req.path == "/api/ping") {
                        rsp["status"] = "ok";
                        rsp["timestamp"] = time(nullptr);
                        rsp["version"] = "1.1";
                    }
                    
                    // GET /api/status — полный статус
                    else if (req.path == "/api/status") {
                        rsp = GetPlayerState();
                        rsp["playlists"] = GetPlaylists()["playlists"];
                    }
                    
                    // GET /api/player — состояние плеера
                    else if (req.path == "/api/player") {
                        rsp = GetPlayerState();
                    }
                    
                    // GET /api/player/state — только состояние (playing/paused/stopped)
                    else if (req.path == "/api/player/state") {
                        if (g_core) {
                            IAIMPServicePlayer* p = nullptr;
                            if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                int st = p->GetState();
                                rsp["state"] = (st == 1) ? "playing" : (st == 2) ? "paused" : "stopped";
                                rsp["state_code"] = st;
                                rsp["success"] = true;
                                p->Release();
                            }
                        }
                    }
                    
                    // GET /api/player/position
                    else if (req.path == "/api/player/position") {
                        if (req.method == "GET") {
                            IAIMPServicePlayer* p = nullptr;
                            if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                double pos = 0;
                                p->GetPosition(&pos);
                                rsp["position"] = pos;
                                rsp["success"] = true;
                                p->Release();
                            }
                        }
                    }
                    
                    // POST /api/player/position
                    else if (req.path == "/api/player/position") {
                        if (req.method == "POST" || req.method == "PUT") {
                            double np = -1;
                            auto it = req.params.find("position");
                            if (it != req.params.end()) np = std::stod(it->second);
                            if (np < 0 && !req.body.empty()) {
                                try {
                                    json b = json::parse(req.body);
                                    if (b.contains("position")) np = b["position"];
                                } catch (...) {}
                            }
                            if (np >= 0) {
                                IAIMPServicePlayer* p = nullptr;
                                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                    p->SetPosition(np);
                                    rsp["position"] = np;
                                    rsp["success"] = true;
                                    p->Release();
                                }
                            } else {
                                rsp["error"] = "Need position parameter";
                                code = 400;
                            }
                        }
                    }
                    
                    // GET /api/player/volume
                    else if (req.path == "/api/player/volume") {
                        if (req.method == "GET") {
                            IAIMPServicePlayer* p = nullptr;
                            if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                float vol = 0;
                                p->GetVolume(&vol);
                                rsp["volume"] = vol;
                                rsp["volume_percent"] = static_cast<int>(vol * 100);
                                rsp["success"] = true;
                                p->Release();
                            }
                        }
                    }
                    
                    // POST /api/player/volume
                    else if (req.path == "/api/player/volume") {
                        if (req.method == "POST" || req.method == "PUT") {
                            float v = -1;
                            auto it = req.params.find("volume");
                            if (it != req.params.end()) v = std::stof(it->second);
                            if (v < 0 && !req.body.empty()) {
                                try {
                                    json b = json::parse(req.body);
                                    if (b.contains("volume")) v = b["volume"];
                                } catch (...) {}
                            }
                            if (v >= 0 && v <= 1) {
                                IAIMPServicePlayer* p = nullptr;
                                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                    p->SetVolume(v);
                                    rsp["volume"] = v;
                                    rsp["volume_percent"] = static_cast<int>(v * 100);
                                    rsp["success"] = true;
                                    p->Release();
                                }
                            } else {
                                rsp["error"] = "Volume must be 0.0-1.0";
                                code = 400;
                            }
                        }
                    }
                    
                    // GET /api/player/track
                    else if (req.path == "/api/player/track") {
                        rsp = GetCurrentTrack();
                    }
                    
                    // GET /api/player/track/focused
                    else if (req.path == "/api/player/track/focused") {
                        rsp = GetFocusedTrack();
                    }
                    
                    // GET /api/player/track/selected
                    else if (req.path == "/api/player/track/selected") {
                        rsp = GetSelectedTrack();
                    }
                    
                    // POST /api/player/playpause
                    else if (req.path == "/api/player/playpause") {
                        IAIMPServicePlayer* p = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                            int st = p->GetState();
                            if (st == 1) p->Pause();
                            else p->Resume();
                            st = p->GetState();
                            rsp["state"] = (st == 1) ? "playing" : (st == 2) ? "paused" : "stopped";
                            rsp["success"] = true;
                            p->Release();
                        }
                    }
                    
                    // POST /api/player/play
                    else if (req.path == "/api/player/play") {
                        int trackIndex = -1;
                        auto it = req.params.find("track");
                        if (it != req.params.end()) trackIndex = std::stoi(it->second);
                        if (trackIndex < 0 && !req.body.empty()) {
                            try {
                                json b = json::parse(req.body);
                                if (b.contains("track")) trackIndex = b["track"];
                            } catch (...) {}
                        }
                        if (trackIndex >= 0) {
                            rsp = PlayTrackInPlaylist(trackIndex);
                        } else {
                            // Просто Play
                            IAIMPServicePlayer* p = nullptr;
                            if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                                p->Resume();
                                rsp["action"] = "play";
                                rsp["success"] = true;
                                p->Release();
                            }
                        }
                    }
                    
                    // POST /api/player/pause
                    else if (req.path == "/api/player/pause") {
                        IAIMPServicePlayer* p = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                            p->Pause();
                            rsp["action"] = "paused";
                            rsp["success"] = true;
                            p->Release();
                        }
                    }
                    
                    // POST /api/player/stop
                    else if (req.path == "/api/player/stop") {
                        IAIMPServicePlayer* p = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                            p->Pause();
                            p->SetPosition(0);
                            rsp["action"] = "stopped";
                            rsp["success"] = true;
                            p->Release();
                        }
                    }
                    
                    // POST /api/player/next
                    else if (req.path == "/api/player/next") {
                        IAIMPServicePlayer* p = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                            p->GoToNext();
                            rsp["action"] = "next";
                            rsp["success"] = true;
                            p->Release();
                        }
                    }
                    
                    // POST /api/player/prev
                    else if (req.path == "/api/player/prev") {
                        IAIMPServicePlayer* p = nullptr;
                        if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&p) == S_OK && p) {
                            p->GoToPrev();
                            rsp["action"] = "prev";
                            rsp["success"] = true;
                            p->Release();
                        }
                    }
                    
                    // GET /api/playlists
                    else if (req.path == "/api/playlists") {
                        rsp = GetPlaylists();
                    }
                    
                    // POST /api/player/select?track=IDX
                    else if (req.path == "/api/player/select") {
                        int trackIndex = -1;
                        auto it = req.params.find("track");
                        if (it != req.params.end()) trackIndex = std::stoi(it->second);
                        if (trackIndex < 0 && !req.body.empty()) {
                            try {
                                json b = json::parse(req.body);
                                if (b.contains("track")) trackIndex = b["track"];
                            } catch (...) {}
                        }
                        if (trackIndex >= 0) {
                            rsp = SelectTrackInActivePlaylist(trackIndex);
                        } else {
                            rsp["error"] = "Need track parameter";
                            code = 400;
                        }
                    }

                    // GET /api/playlist/{id}
                    // GET /api/playlist/{id}/tracks
                    // POST /api/playlist/{id}/play
// POST /api/playlist/{id}/focus
                    else if (req.path.find("/api/playlist/") == 0) {
                        std::string path = req.path;
                        size_t s = 14; // длина "/api/playlist/"
                        size_t e = path.find("/", s);
                        std::string idStr = (e != std::string::npos) ? path.substr(s, e - s) : path.substr(s);
                        
                        int playlistId = -1;
                        try { playlistId = std::stoi(idStr); }
                        catch (...) { rsp["error"] = "Invalid playlist ID"; code = 400; }
                        
                        if (playlistId >= 0) {
                            if (path.find("/focus") != std::string::npos) {
                                // POST /api/playlist/{id}/focus
                                IAIMPServicePlaylistManager* mgr = nullptr;
                                if (!g_core || g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
                                    rsp["error"] = "No playlist manager";
                                    code = 500;
                                } else {
                                    IAIMPPlaylist* pl = nullptr;

                                    // Find playlist by ID: SDK has GetLoadedPlaylistByID(IAIMPString* ID,...)
                                    // We'll use same numeric id converted to string (Win wide strings in SDK typically need IAIMPString).
                                    // Since we don't have helper to create IAIMPString here, we fallback to index-search by loaded playlists.
                                    int count = mgr->GetLoadedPlaylistCount();
                                    bool found = false;
                                    for (int i = 0; i < count; i++) {
                                        IAIMPPlaylist* tmp = nullptr;
                                        if (mgr->GetLoadedPlaylist(i, &tmp) == S_OK && tmp) {
                                            if (GetPlaylistID(tmp) == playlistId) { pl = tmp; found = true; break; }
                                            tmp->Release();
                                        }
                                    }

                                    if (!found || !pl) {
                                        rsp["error"] = "Playlist not found";
                                        code = 404;
                                    } else {
                                        HRESULT hr = mgr->SetActivePlaylist(pl);
                                        rsp["action"] = "focus_playlist";
                                        rsp["playlist_id"] = playlistId;
                                        rsp["success"] = (hr == S_OK);
                                        pl->Release();
                                    }
                                    mgr->Release();
                                }
                            } else if (path.find("/play") != std::string::npos) {
                                // POST /api/playlist/{id}/play
                                int trackIndex = -1;
                                auto it = req.params.find("track");
                                if (it != req.params.end()) trackIndex = std::stoi(it->second);
                                if (trackIndex < 0 && !req.body.empty()) {
                                    try {
                                        json b = json::parse(req.body);
                                        if (b.contains("track")) trackIndex = b["track"];
                                    } catch (...) {}
                                }
                                rsp = PlayPlaylistTrack(playlistId, trackIndex);
                            }
                            else if (path.find("/tracks") != std::string::npos || path.find("/tracks") == std::string::npos) {
                                // GET /api/playlist/{id}/tracks или GET /api/playlist/{id}
                                if (path.find("/tracks") != std::string::npos) {
                                    rsp = GetPlaylistTracks(playlistId);
                                } else {
                                    rsp = GetPlaylistInfo(playlistId);
                                }
                            }
                        }
                    }
                    
                    // OPTIONS — CORS preflight
                    else if (req.method == "OPTIONS") {
                        std::string cors =
                            "HTTP/1.1 200 OK\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type\r\n"
                            "Content-Length: 0\r\n\r\n";
                        send(cl, cors.c_str(), cors.length(), 0);
                        closesocket(cl);
                        continue;
                    }
                    
                    // GET /
                    else if (req.path == "/" || req.path == "/api") {
                        rsp["name"] = "AIMP HTTP Control API";
                        rsp["version"] = "1.1";
                        rsp["description"] = "REST API for remote control of AIMP player";
                        rsp["documentation"] = "https://github.com/your-repo/aimp-http-plugin";
                        rsp["endpoints"] = json::array({
                            {{"method", "GET"},    {"path", "/api/ping"},                    {"desc", "Health check"}},
                            {{"method", "GET"},    {"path", "/api/status"},                  {"desc", "Full player status"}},
                            {{"method", "GET"},    {"path", "/api/player"},                  {"desc", "Player state"}},
                            {{"method", "GET"},    {"path", "/api/player/state"},            {"desc", "Playback state only"}},
                            {{"method", "GET"},    {"path", "/api/player/track"},            {"desc", "Playing track info"}},
                            {{"method", "GET"},    {"path", "/api/player/track/focused"},    {"desc", "Focused track info (FOCUSINDEX)"}},
                            {{"method", "GET"},    {"path", "/api/player/track/selected"},   {"desc", "Selected track info (selected == focus)"}},
                            {{"method", "POST"},   {"path", "/api/player/select?track=IDX"}, {"desc", "Select track by focus cursor without playing"}},
                            {{"method", "GET"},    {"path", "/api/player/position"},         {"desc", "Get position"}},
                            {{"method", "POST"},   {"path", "/api/player/position"},         {"desc", "Set position"}},
                            {{"method", "GET"},    {"path", "/api/player/volume"},           {"desc", "Get volume"}},
                            {{"method", "POST"},   {"path", "/api/player/volume"},           {"desc", "Set volume"}},
                            {{"method", "POST"},   {"path", "/api/player/playpause"},        {"desc", "Toggle play/pause"}},
                            {{"method", "POST"},   {"path", "/api/player/play"},             {"desc", "Play / Play specific track"}},
                            {{"method", "POST"},   {"path", "/api/player/pause"},            {"desc", "Pause"}},
                            {{"method", "POST"},   {"path", "/api/player/stop"},             {"desc", "Stop"}},
                            {{"method", "POST"},   {"path", "/api/player/next"},             {"desc", "Next track"}},
                            {{"method", "POST"},   {"path", "/api/player/prev"},             {"desc", "Previous track"}},
                            {{"method", "GET"},    {"path", "/api/playlists"},               {"desc", "List playlists"}},
                            {{"method", "GET"},    {"path", "/api/playlist/{id}"},           {"desc", "Playlist info"}},
                            {{"method", "GET"},    {"path", "/api/playlist/{id}/tracks"},    {"desc", "Tracks in playlist"}},
                            {{"method", "POST"},   {"path", "/api/playlist/{id}/play"},      {"desc", "Play track from playlist"}},
                            {{"method", "POST"},   {"path", "/api/playlist/{id}/focus"},     {"desc", "Focus (activate) playlist for UI without playing"}}
                        });
                    }
                    
                    // 404
                    else {
                        rsp["error"] = "Not found";
                        rsp["code"] = 404;
                        rsp["tip"] = "Visit / for API documentation";
                        code = 404;
                    }
                }
                catch (const std::exception& e) {
                    rsp["error"] = std::string("Internal error: ") + e.what();
                    rsp["success"] = false;
                    code = 500;
                }
                catch (...) {
                    rsp["error"] = "Unknown internal error";
                    rsp["success"] = false;
                    code = 500;
                }
            } // lock
            
            std::string jsonStr = rsp.dump();
            std::string statusText = (code == 200) ? "OK" :
                                     (code == 400) ? "Bad Request" :
                                     (code == 404) ? "Not Found" :
                                     (code == 500) ? "Internal Server Error" : "OK";
            
            std::string httpResponse =
                "HTTP/1.1 " + std::to_string(code) + " " + statusText + "\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Content-Length: " + std::to_string(jsonStr.length()) + "\r\n"
                "Connection: close\r\n"
                "Server: AIMP-HTTP-Plugin/1.1\r\n"
                "\r\n" + jsonStr;
            
            send(cl, httpResponse.c_str(), httpResponse.length(), 0);
        }
        
        closesocket(cl);
    }
    
    closesocket(srv);
    WSACleanup();
}

// ==========================================
// Реализация плагина AIMP
// ==========================================
class HttpControlPlugin : public IAIMPPlugin {
private:
    LONG refCount = 1;
    
public:
    HttpControlPlugin() = default;
    virtual ~HttpControlPlugin() = default;
    
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IAIMPPlugin*>(this);
        AddRef();
        return S_OK;
    }
    
    ULONG WINAPI AddRef() override {
        return InterlockedIncrement(&refCount);
    }
    
    ULONG WINAPI Release() override {
        LONG ref = InterlockedDecrement(&refCount);
        if (ref == 0) {
            delete this;
        }
        return ref;
    }
    
    TChar* WINAPI InfoGet(int Index) override {
        static wchar_t name[] = L"AIMP HTTP Control API";
        static wchar_t author[] = L"DebianDev";
        static wchar_t shortDesc[] = L"HTTP REST API server on port 3553";
        static wchar_t fullDesc[] = L"Provides full HTTP REST API for remote control and monitoring of AIMP player. Supports all player controls, playlist management, and track information retrieval.";
        
        switch (Index) {
            case AIMP_PLUGIN_INFO_NAME:              return name;
            case AIMP_PLUGIN_INFO_AUTHOR:            return author;
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return shortDesc;
            case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:  return fullDesc;
            default: return nullptr;
        }
    }
    
    LongWord WINAPI InfoGetCategories() override {
        return AIMP_PLUGIN_CATEGORY_ADDONS;
    }
    
    void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override {
        // Можно добавить обработку событий
    }
    
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

// ==========================================
// Точка входа
// ==========================================
extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = new HttpControlPlugin();
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
