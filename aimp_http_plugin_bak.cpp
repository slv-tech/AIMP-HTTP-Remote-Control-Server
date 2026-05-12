// aimp_http_plugin.cpp
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <thread>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>

#include "third_party/json.hpp"
using json = nlohmann::json;

#include "sdk/apiPlugin.h"
#include "sdk/apiPlayer.h"
#include "sdk/apiPlaylists.h"
#include "sdk/apiObjects.h"
#include "sdk/apiFileManager.h"
#include "sdk/apiThreading.h"

// ==========================================
// Глобальные переменные
// ==========================================
IAIMPCore* g_core    = nullptr;
std::mutex g_mutex;
int        g_port    = 3553;
bool       g_running = false;
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
// ExecuteInMainThread — запуск лямбды в главном потоке AIMP с ожиданием
// Все вызовы к IAIMPPlaylist* и IAIMPServicePlaylistManager требуют главного потока.
// ==========================================
class AIMPMainThreadTask : public IUnknown, public IAIMPTask {
    LONG ref_ = 1;
    std::function<void()> fn_;
public:
    explicit AIMPMainThreadTask(std::function<void()> fn) : fn_(std::move(fn)) {}

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPTask) {
            *ppv = static_cast<IAIMPTask*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // IAIMPTask
    void WINAPI Execute(IAIMPTaskOwner*) override { fn_(); }
};

// Выполняет fn() в главном потоке AIMP и блокируется до завершения.
// Возвращает false если IAIMPServiceThreads недоступен.
bool RunInMainThread(std::function<void()> fn) {
    if (!g_core) return false;
    IAIMPServiceThreads* threads = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServiceThreads, (void**)&threads) != S_OK || !threads)
        return false;
    auto* task = new AIMPMainThreadTask(std::move(fn));
    // AIMP_SERVICE_THREADS_FLAGS_WAITFOR = 1 — блокируемся до завершения
    HRESULT hr = threads->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);
    task->Release();
    threads->Release();
    return hr == S_OK;
}

// ==========================================
// Вспомогательные функции (вызываются ТОЛЬКО из главного потока)
// ==========================================

// IAIMPPlaylist реализует IAIMPPropertyList — получаем через QI
struct PlaylistProps {
    IAIMPPropertyList* ptr = nullptr;
    explicit PlaylistProps(IAIMPPlaylist* pl) {
        if (pl) pl->QueryInterface(IID_IAIMPPropertyList, (void**)&ptr);
    }
    ~PlaylistProps() { if (ptr) ptr->Release(); }
    operator bool()               const { return ptr != nullptr; }
    IAIMPPropertyList* operator->() const { return ptr; }
};

std::string GetPlaylistName(IAIMPPlaylist* pl) {
    PlaylistProps p(pl);
    if (!p) return "Unknown";
    IAIMPString* s = nullptr;
    if (p->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, (void**)&s) == S_OK && s) {
        std::string r = WStr(s->GetData());
        s->Release();
        return r;
    }
    return "Unknown";
}

std::string GetPlaylistId(IAIMPPlaylist* pl) {
    PlaylistProps p(pl);
    if (!p) return "";
    IAIMPString* s = nullptr;
    if (p->GetValueAsObject(AIMP_PLAYLIST_PROPID_ID, IID_IAIMPString, (void**)&s) == S_OK && s) {
        std::string r = WStr(s->GetData());
        s->Release();
        return r;
    }
    return "";
}

int GetPlayingIndex(IAIMPPlaylist* pl) {
    int idx = -1;
    PlaylistProps p(pl);
    if (p) p->GetValueAsInt32(AIMP_PLAYLIST_PROPID_PLAYINGINDEX, &idx);
    return idx;
}

int GetFocusedIndex(IAIMPPlaylist* pl) {
    int idx = -1;
    PlaylistProps p(pl);
    if (p) p->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &idx);
    return idx;
}

double GetPlaylistDuration(IAIMPPlaylist* pl) {
    double d = 0;
    PlaylistProps p(pl);
    if (p) p->GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION, &d);
    return d;
}

void GetFileInfo(IAIMPPlaylistItem* item, json& out) {
    if (!item) return;
    IAIMPString* s = nullptr;

    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, (void**)&s) == S_OK && s) {
        out["file_path"] = WStr(s->GetData()); s->Release(); s = nullptr;
    }

    IAIMPFileInfo* fi = nullptr;
    if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo, (void**)&fi) == S_OK && fi) {
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_TITLE,  IID_IAIMPString, (void**)&s) == S_OK && s) { out["title"]  = WStr(s->GetData()); s->Release(); s = nullptr; }
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_ARTIST, IID_IAIMPString, (void**)&s) == S_OK && s) { out["artist"] = WStr(s->GetData()); s->Release(); s = nullptr; }
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_ALBUM,  IID_IAIMPString, (void**)&s) == S_OK && s) { out["album"]  = WStr(s->GetData()); s->Release(); s = nullptr; }
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_DATE,   IID_IAIMPString, (void**)&s) == S_OK && s) { out["year"]   = WStr(s->GetData()); s->Release(); s = nullptr; }
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_GENRE,  IID_IAIMPString, (void**)&s) == S_OK && s) { out["genre"]  = WStr(s->GetData()); s->Release(); s = nullptr; }
        if (fi->GetValueAsObject(AIMP_FILEINFO_PROPID_TRACKNUMBER, IID_IAIMPString, (void**)&s) == S_OK && s) { out["track_number"] = WStr(s->GetData()); s->Release(); s = nullptr; }
        double dur = 0; if (fi->GetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, &dur) == S_OK) out["duration"] = dur;
        int    br  = 0; if (fi->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITRATE,  &br)  == S_OK) out["bitrate"]  = br;
        fi->Release();
    }

    if (!out.contains("title") || out["title"].get<std::string>().empty()) {
        if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_DISPLAYTEXT, IID_IAIMPString, (void**)&s) == S_OK && s) {
            out["title"] = WStr(s->GetData()); s->Release();
        }
    }
}

// ==========================================
// API-функции
// ==========================================

// Player status — IAIMPServicePlayer thread-safe, вызывается из HTTP-потока напрямую
json GetPlayerStatus() {
    json r;
    r["state"]    = "stopped";
    r["volume"]   = 0;
    r["muted"]    = false;
    r["position"] = 0.0;
    r["duration"] = 0.0;
    if (!g_core) return r;

    IAIMPServicePlayer* player = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) != S_OK || !player)
        return r;

    int st = player->GetState();
    r["state"] = (st == AIMP_PLAYER_STATE_PLAYING) ? "playing"
               : (st == AIMP_PLAYER_STATE_PAUSED)  ? "paused" : "stopped";

    double pos = 0; player->GetPosition(&pos); r["position"] = pos;
    double dur = 0; player->GetDuration(&dur);  r["duration"] = dur;
    float  vol = 0; player->GetVolume(&vol);    r["volume"]   = (int)(vol * 100.0f);
    BOOL muted = FALSE; player->GetMute(&muted); r["muted"] = (muted != FALSE);

    // Текущий трек — тоже через player, который thread-safe
    IAIMPPlaylistItem* pi = nullptr;
    if (player->GetPlaylistItem(&pi) == S_OK && pi) {
        json ti;
        // filename можно читать из HTTP-потока — это просто строка
        IAIMPString* fn = nullptr;
        if (pi->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, (void**)&fn) == S_OK && fn) {
            r["filenameplaying"] = WStr(fn->GetData()); fn->Release();
        }
        // FileInfo тоже читаем здесь — player->GetPlaylistItem вернул нам объект
        // в главном потоке (AIMP сам вызывает нас из главного потока для player API)
        GetFileInfo(pi, ti);
        if (ti.contains("title"))  r["track_title"]  = ti["title"];
        if (ti.contains("artist")) r["track_artist"]  = ti["artist"];
        if (ti.contains("album"))  r["track_album"]   = ti["album"];
        if (ti.contains("duration")) r["duration"]    = ti["duration"];
        pi->Release();
    }
    player->Release();
    return r;
}

// Плейлисты — ТОЛЬКО через RunInMainThread
json GetPlaylistsResponse() {
    json r;
    r["playlists"] = json::array();
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    bool ok = RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }

        IAIMPPlaylist* activePl  = nullptr; mgr->GetActivePlaylist(&activePl);
        IAIMPPlaylist* playingPl = nullptr; mgr->GetPlayingPlaylist(&playingPl);

        int count = mgr->GetLoadedPlaylistCount();
        for (int i = 0; i < count; i++) {
            IAIMPPlaylist* pl = nullptr;
            if (mgr->GetLoadedPlaylist(i, &pl) != S_OK || !pl) continue;

            json p;
            p["id"]          = i;
            p["aimp_id"]     = GetPlaylistId(pl);
            p["name"]        = GetPlaylistName(pl);
            p["track_count"] = pl->GetItemCount();
            p["duration"]    = GetPlaylistDuration(pl);

            bool isPlaying = (playingPl && playingPl == pl);
            bool isActive  = (activePl  && activePl  == pl);
            if (isPlaying) {
                p["state"] = (GetPlayingIndex(pl) >= 0) ? "playing" : "active";
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
    });

    if (!ok) r["error"] = "ExecuteInMainThread failed";
    return r;
}

json GetPlaylistResponse(int idx) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    bool ok = RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(idx, &pl) != S_OK || !pl) {
            mgr->Release(); r["error"]["code"] = "PLAYLIST_NOT_FOUND"; return;
        }
        IAIMPPlaylist* activePl  = nullptr; mgr->GetActivePlaylist(&activePl);
        IAIMPPlaylist* playingPl = nullptr; mgr->GetPlayingPlaylist(&playingPl);

        r["id"]          = idx;
        r["aimp_id"]     = GetPlaylistId(pl);
        r["name"]        = GetPlaylistName(pl);
        r["track_count"] = pl->GetItemCount();
        r["duration"]    = GetPlaylistDuration(pl);

        bool isPlaying = (playingPl && playingPl == pl);
        bool isActive  = (activePl  && activePl  == pl);
        if (isPlaying)      r["state"] = (GetPlayingIndex(pl) >= 0) ? "playing" : "active";
        else if (isActive)  r["state"] = "active";
        else                r["state"] = nullptr;

        pl->Release();
        if (activePl)  activePl->Release();
        if (playingPl) playingPl->Release();
        mgr->Release();
    });

    if (!ok) r["error"] = "ExecuteInMainThread failed";
    return r;
}

json GetTracksResponse(int plIdx, int limit, int offset) {
    json r;
    r["playlist_id"] = plIdx;
    r["tracks"]      = json::array();
    r["offset"]      = offset;
    r["limit"]       = limit;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    bool ok = RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(plIdx, &pl) != S_OK || !pl) {
            mgr->Release(); r["error"]["code"] = "PLAYLIST_NOT_FOUND"; return;
        }
        mgr->Release();

        int total      = pl->GetItemCount();
        int playingIdx = GetPlayingIndex(pl);
        int focusedIdx = GetFocusedIndex(pl);
        r["total"] = total;

        int end = std::min(offset + limit, total);
        for (int i = offset; i < end; i++) {
            IAIMPPlaylistItem* item = nullptr;
            if (pl->GetItem(i, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item) continue;

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
    });

    if (!ok) r["error"] = "ExecuteInMainThread failed";
    return r;
}

json GetTrackResponse(int plIdx, int trackIdx) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    bool ok = RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(plIdx, &pl) != S_OK || !pl) {
            mgr->Release(); r["error"]["code"] = "PLAYLIST_NOT_FOUND"; return;
        }
        mgr->Release();

        if (trackIdx < 0 || trackIdx >= pl->GetItemCount()) {
            pl->Release(); r["error"]["code"] = "TRACK_NOT_FOUND"; return;
        }
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(trackIdx, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item) {
            pl->Release(); r["error"]["code"] = "TRACK_NOT_FOUND"; return;
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
    });

    if (!ok) r["error"] = "ExecuteInMainThread failed";
    return r;
}

// ==========================================
// HTTP парсер
// ==========================================
struct HttpRequest {
    std::string method, path, body;
    std::map<std::string, std::string> params;
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
    // пропускаем заголовки до пустой строки
    while (std::getline(ss, line) && line != "\r" && !line.empty()) {}
    std::string rest;
    while (std::getline(ss, line)) rest += line + "\n";
    req.body = rest;
    return req;
}

// ==========================================
// HTTP Сервер
// ==========================================
void SendResponse(SOCKET client, int code, const json& body) {
    std::string js = body.dump();
    std::string status = (code==200)?"OK":(code==201)?"Created":(code==400)?"Bad Request":(code==404)?"Not Found":"Internal Server Error";
    std::string resp =
        "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: " + std::to_string(js.size()) + "\r\n"
        "Connection: close\r\n\r\n" + js;
    send(client, resp.c_str(), (int)resp.size(), 0);
}

struct ParsedPath {
    int playlistId = -1;
    int trackId    = -1;
    std::string action;
};

ParsedPath ParsePath(const std::string& path) {
    ParsedPath r;
    std::string p = path;
    if (p.find("/api/") == 0) p = p.substr(5);
    if (p.find("playlists/") != 0) return r;
    p = p.substr(10);
    size_t sl = p.find('/');
    if (sl == std::string::npos) {
        try { r.playlistId = std::stoi(p); } catch(...) {}
        r.action = "info";
        return r;
    }
    try { r.playlistId = std::stoi(p.substr(0, sl)); } catch(...) {}
    p = p.substr(sl + 1);
    if (p.find("tracks/") == 0) {
        p = p.substr(7);
        sl = p.find('/');
        if (sl == std::string::npos) {
            try { r.trackId = std::stoi(p); } catch(...) {}
            r.action = "info";
        } else {
            try { r.trackId = std::stoi(p.substr(0, sl)); } catch(...) {}
            r.action = p.substr(sl + 1);
        }
    } else {
        r.action = p.empty() ? "info" : p;
    }
    return r;
}

// Выполняет действие с плейлистом/треком в главном потоке
void DoPlaylistAction(const ParsedPath& pp, const std::string& method, const std::string& body, json& rsp, int& code) {
    RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (!g_core || g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            rsp["error"]["code"] = "NO_MANAGER"; code = 500; return;
        }
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(pp.playlistId, &pl) != S_OK || !pl) {
            mgr->Release(); rsp["error"]["code"] = "PLAYLIST_NOT_FOUND"; code = 404; return;
        }

        if (pp.trackId < 0) {
            // Действия с плейлистом
            if (pp.action == "select") {
                mgr->SetActivePlaylist(pl);
                rsp["id"] = pp.playlistId; rsp["state"] = "active";
            } else if (pp.action == "play") {
                mgr->SetActivePlaylist(pl);
                IAIMPServicePlayer* player = nullptr;
                if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->Play3(pl); player->Release();
                }
                rsp["id"] = pp.playlistId; rsp["state"] = "playing";
            }
        } else {
            // Действия с треком
            IAIMPPlaylistItem* item = nullptr;
            if (pl->GetItem(pp.trackId, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item) {
                pl->Release(); mgr->Release();
                rsp["error"]["code"] = "TRACK_NOT_FOUND"; code = 404; return;
            }
            if (pp.action == "play") {
                IAIMPServicePlayer* player = nullptr;
                if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
                    player->Play2(item); player->Release();
                }
                rsp["id"] = pp.trackId; rsp["state"] = "playing";
            } else if (pp.action == "select") {
                PlaylistProps props(pl);
                if (props) props->SetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, pp.trackId);
                rsp["id"] = pp.trackId; rsp["state"] = "focused";
            }
            item->Release();
        }

        pl->Release();
        mgr->Release();
    });
}

void RunHttpServer() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { WSACleanup(); return; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(g_port); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(srv); WSACleanup(); return; }
    listen(srv, SOMAXCONN);
    g_running = true;

    while (g_running) {
        fd_set rs; FD_ZERO(&rs); FD_SET(srv, &rs);
        timeval to{1, 0};
        if (select(0, &rs, nullptr, nullptr, &to) <= 0) continue;
        SOCKET cl = accept(srv, nullptr, nullptr);
        if (cl == INVALID_SOCKET) continue;

        char buf[16384];
        int n = recv(cl, buf, sizeof(buf)-1, 0);
        if (n > 0) {
            buf[n] = 0;
            HttpRequest req = ParseRequest(std::string(buf, n));
            json rsp; int code = 200;

            if (req.method == "OPTIONS") {
                std::string cors = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n";
                send(cl, cors.c_str(), (int)cors.size(), 0);
                closesocket(cl); continue;
            }
            else if (req.path == "/api/player/status" && req.method == "GET") {
                rsp = GetPlayerStatus();
            }
            else if (req.path == "/api/player/play" && req.method == "POST") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->Resume(); rsp["state"]="playing"; p->Release(); }
            }
            else if (req.path == "/api/player/pause" && req.method == "POST") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->Pause(); rsp["state"]="paused"; p->Release(); }
            }
            else if (req.path == "/api/player/stop" && req.method == "POST") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->Stop(); rsp["state"]="stopped"; p->Release(); }
            }
            else if (req.path == "/api/player/next" && req.method == "POST") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->GoToNext(); p->Release(); rsp["ok"]=true; }
            }
            else if (req.path == "/api/player/prev" && req.method == "POST") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->GoToPrev(); p->Release(); rsp["ok"]=true; }
            }
            else if (req.path == "/api/player/volume" && req.method == "GET") {
                IAIMPServicePlayer* p = nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) {
                    float v=0; BOOL m=FALSE; p->GetVolume(&v); p->GetMute(&m);
                    rsp["volume"]=(int)(v*100); rsp["muted"]=(m!=FALSE); p->Release();
                }
            }
            else if (req.path == "/api/player/volume" && (req.method=="PUT"||req.method=="POST")) {
                float v = -1;
                if (req.params.count("volume")) try { v=std::stof(req.params["volume"])/100.f; } catch(...) {}
                if (v<0 && !req.body.empty()) try { json b=json::parse(req.body); if(b.contains("volume")) v=b["volume"].get<float>()/100.f; } catch(...) {}
                if (v>=0 && v<=1) {
                    IAIMPServicePlayer* p=nullptr;
                    if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->SetVolume(v); rsp["volume"]=(int)(v*100); p->Release(); }
                } else { rsp["error"]["code"]="INVALID_VOLUME"; code=400; }
            }
            else if (req.path == "/api/player/mute" && req.method == "POST") {
                IAIMPServicePlayer* p=nullptr;
                if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) {
                    BOOL m=FALSE; p->GetMute(&m); p->SetMute(!m); rsp["muted"]=(m==FALSE); p->Release();
                }
            }
            else if (req.path == "/api/player/position" && (req.method=="PUT"||req.method=="POST")) {
                double pos=-1;
                if (req.params.count("position")) try { pos=std::stod(req.params["position"]); } catch(...) {}
                if (pos<0 && !req.body.empty()) try { json b=json::parse(req.body); if(b.contains("position")) pos=b["position"].get<double>(); } catch(...) {}
                if (pos>=0) {
                    IAIMPServicePlayer* p=nullptr;
                    if (g_core && g_core->QueryInterface(IID_IAIMPServicePlayer,(void**)&p)==S_OK && p) { p->SetPosition(pos); rsp["position"]=pos; p->Release(); }
                } else { rsp["error"]["code"]="INVALID_POSITION"; code=400; }
            }
            else if (req.path == "/api/playlists" && req.method == "GET") {
                rsp = GetPlaylistsResponse();
            }
            else if (req.path.find("/api/playlists/") == 0) {
                ParsedPath pp = ParsePath(req.path);
                if (pp.playlistId < 0) {
                    rsp["error"]["code"] = "INVALID_PATH"; code = 400;
                } else if (pp.action == "info" && req.method == "GET" && pp.trackId < 0) {
                    rsp = GetPlaylistResponse(pp.playlistId);
                } else if (pp.action == "tracks" && req.method == "GET" && pp.trackId < 0) {
                    int lim=50, off=0;
                    if (req.params.count("limit"))  try { lim=std::stoi(req.params["limit"]);  } catch(...) {}
                    if (req.params.count("offset")) try { off=std::stoi(req.params["offset"]); } catch(...) {}
                    rsp = GetTracksResponse(pp.playlistId, lim, off);
                } else if (pp.action == "info" && req.method == "GET" && pp.trackId >= 0) {
                    rsp = GetTrackResponse(pp.playlistId, pp.trackId);
                } else if ((pp.action=="play"||pp.action=="select") && req.method=="POST") {
                    DoPlaylistAction(pp, req.method, req.body, rsp, code);
                } else if (pp.action == "duration" && req.method == "GET" && pp.trackId >= 0) {
                    json ti = GetTrackResponse(pp.playlistId, pp.trackId);
                    rsp["id"] = pp.trackId; rsp["duration"] = ti.value("duration", 0.0);
                } else {
                    rsp["error"]["code"] = "NOT_FOUND"; code = 404;
                }
            }
            else if (req.path == "/api" || req.path == "/api/") {
                rsp["name"] = "AIMP HTTP Control API v2.0";
                rsp["endpoints"] = json::array({
                    "GET  /api/player/status", "POST /api/player/play", "POST /api/player/pause",
                    "POST /api/player/stop",   "POST /api/player/next", "POST /api/player/prev",
                    "GET  /api/player/volume", "PUT  /api/player/volume", "POST /api/player/mute",
                    "PUT  /api/player/position",
                    "GET  /api/playlists",
                    "GET  /api/playlists/:id",
                    "GET  /api/playlists/:id/tracks",
                    "GET  /api/playlists/:id/tracks/:tid",
                    "POST /api/playlists/:id/play",   "POST /api/playlists/:id/select",
                    "POST /api/playlists/:id/tracks/:tid/play",
                    "POST /api/playlists/:id/tracks/:tid/select"
                });
            }
            else {
                rsp["error"]["code"] = "NOT_FOUND"; code = 404;
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
    LONG ref_ = 1;
public:
    virtual ~HttpControlPlugin() = default;
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IAIMPPlugin*>(this); AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref_); if (r==0) delete this; return r; }

    TChar* WINAPI InfoGet(int Index) override {
        static wchar_t n[] = L"AIMP HTTP Control API v2";
        static wchar_t a[] = L"DebianDev";
        static wchar_t d[] = L"Full REST API on port 3553";
        switch (Index) {
            case AIMP_PLUGIN_INFO_NAME:              return n;
            case AIMP_PLUGIN_INFO_AUTHOR:            return a;
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return d;
            default: return nullptr;
        }
    }
    LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }
    void WINAPI SystemNotification(int, IUnknown*) override {}

    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        g_core = core;
        g_serverThread = std::thread(RunHttpServer);
        g_serverThread.detach();
        return S_OK;
    }
    HRESULT WINAPI Finalize() override {
        g_running = false;
        Sleep(300);
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
