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
#include <set>

#include "third_party/json.hpp"
using json = nlohmann::json;

#include "sdk/apiPlugin.h"
#include "sdk/apiPlayer.h"
#include "sdk/apiPlaylists.h"
#include "sdk/apiObjects.h"
#include "sdk/apiFileManager.h"
#include "sdk/apiThreading.h"
#include "sdk/apiMessages.h"
#include "sdk/apiOptions.h"

// ==========================================
// Глобальные переменные
// ==========================================
HINSTANCE  g_hInstance = nullptr;  // DLL HINSTANCE для диалогов
IAIMPCore* g_core    = nullptr;
std::mutex g_mutex;
int        g_port    = 19122;
// Bind mode: 0 = 127.0.0.1 (localhost only), 1 = LAN (private ranges), 2 = 0.0.0.0 (all interfaces)
int        g_bindMode = 2;
bool       g_running = false;
std::thread g_serverThread;
SOCKET     g_serverSocket = INVALID_SOCKET;  // для корректного перезапуска

// Фокус навигации (для Bitfocus: < плейлист >, < трек >)
// Хранит индекс плейлиста и трека которые пользователь выбрал кнопками.
// Не зависит от того, что сейчас играет.
std::mutex g_focusMutex;
int g_focusPlaylistIdx = 0;  // индекс плейлиста в фокусе
int g_focusTrackIdx    = 0;  // индекс трека в фокусе (в рамках g_focusPlaylistIdx)

// Синхронизация фокуса с UI AIMP
// Forward declarations
class AIMPPlaylistListener;
class AIMPPlaylistManagerListener;
AIMPPlaylistManagerListener* g_managerListener = nullptr;  // глобальный слушатель менеджера
std::mutex g_listenersMutex;
// Набор {playlist_ptr -> listener_ptr} — для снятия подписки при Removed/Finalize
struct PlaylistListenerEntry {
    IAIMPPlaylist* playlist;
    AIMPPlaylistListener* listener;
};
std::vector<PlaylistListenerEntry> g_playlistListeners;

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
// PlaylistProps + GetFocusedIndex — вынесены выше для использования слушателями
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

int GetFocusedIndex(IAIMPPlaylist* pl) {
    int idx = -1;
    PlaylistProps p(pl);
    if (p) p->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &idx);
    return idx;
}

// Установить фокус + selection на трек trackIdx в плейлисте pl.
// Снимает selection со всех остальных треков, ставит на нужный.
// Вызывать из главного потока.
void SetPlaylistFocusAndSelection(IAIMPPlaylist* pl, int trackIdx) {
    if (!pl) return;
    // Устанавливаем FOCUSINDEX
    PlaylistProps props(pl);
    if (props) props->SetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, trackIdx);

    // Снимаем selection со всех, ставим на нужный
    int count = pl->GetItemCount();
    for (int i = 0; i < count; i++) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(i, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, (i == trackIdx) ? 1 : 0);
            item->Release();
        }
    }
}

// ==========================================
// Хелпер: найти индекс плейлиста по указателю IAIMPPlaylist*
// Вызывать ТОЛЬКО из главного потока AIMP.
// ==========================================
int FindPlaylistIndex(IAIMPPlaylist* targetPl) {
    if (!g_core || !targetPl) return -1;
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr)
        return -1;
    int count = mgr->GetLoadedPlaylistCount();
    int found = -1;
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(i, &pl) == S_OK && pl) {
            if (pl == targetPl) { found = i; pl->Release(); break; }
            pl->Release();
        }
    }
    mgr->Release();
    return found;
}

// ==========================================
// AIMPPlaylistListener — подписывается на конкретный плейлист.
// Обновляет g_focusTrackIdx при изменении FOCUSINDEX в AIMP UI.
// Вызывается AIMP из главного потока.
// ==========================================
class AIMPPlaylistListener : public IAIMPPlaylistListener {
    LONG ref_ = 1;
    IAIMPPlaylist* playlist_;  // weak ref — не AddRef, т.к. AIMP владеет плейлистом
public:
    explicit AIMPPlaylistListener(IAIMPPlaylist* pl) : playlist_(pl) {}

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPPlaylistListener) {
            *ppv = static_cast<IAIMPPlaylistListener*>(this);
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

    // IAIMPPlaylistListener
    void WINAPI Activated() override {
        // Вкладка плейлиста стала активной в UI — обновляем g_focusPlaylistIdx
        int idx = FindPlaylistIndex(playlist_);
        if (idx >= 0) {
            // Читаем AIMP-ный фокус трека из этого плейлиста
            int focusIdx = GetFocusedIndex(playlist_);
            std::lock_guard<std::mutex> lk(g_focusMutex);
            g_focusPlaylistIdx = idx;
            if (focusIdx >= 0) g_focusTrackIdx = focusIdx;
            else               g_focusTrackIdx = 0;
        }
    }

    void WINAPI Changed(LongWord Flags) override {
        if (Flags & AIMP_PLAYLIST_NOTIFY_FOCUSINDEX) {
            // Фокус трека изменился в UI — синхронизируем только если это наш фокус-плейлист
            int plIdx = FindPlaylistIndex(playlist_);
            if (plIdx < 0) return;
            int focusIdx = GetFocusedIndex(playlist_);
            if (focusIdx < 0) return;
            std::lock_guard<std::mutex> lk(g_focusMutex);
            if (g_focusPlaylistIdx == plIdx) {
                g_focusTrackIdx = focusIdx;
            }
        }
    }

    void WINAPI Removed() override {
        // Плейлист удалён — удаляем себя из g_playlistListeners
        // (AIMP автоматически снимает listener, но мы чистим нашу структуру)
        std::lock_guard<std::mutex> lk(g_listenersMutex);
        for (auto it = g_playlistListeners.begin(); it != g_playlistListeners.end(); ++it) {
            if (it->playlist == playlist_) {
                g_playlistListeners.erase(it);
                break;
            }
        }
    }
};

// Навесить IAIMPPlaylistListener на один плейлист (если ещё не подписаны).
// Вызывать из главного потока.
void AttachListenerToPlaylist(IAIMPPlaylist* pl) {
    if (!pl) return;
    {
        std::lock_guard<std::mutex> lk(g_listenersMutex);
        for (auto& e : g_playlistListeners) {
            if (e.playlist == pl) return;  // уже подписаны
        }
    }
    auto* listener = new AIMPPlaylistListener(pl);
    if (pl->ListenerAdd(listener) == S_OK) {
        std::lock_guard<std::mutex> lk(g_listenersMutex);
        g_playlistListeners.push_back({pl, listener});
    } else {
        listener->Release();  // не удалось подписаться
    }
}

// Навесить слушателей на ВСЕ загруженные плейлисты.
// Вызывать из главного потока.
void AttachListenerToAllPlaylists() {
    if (!g_core) return;
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr)
        return;
    int count = mgr->GetLoadedPlaylistCount();
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(i, &pl) == S_OK && pl) {
            AttachListenerToPlaylist(pl);
            pl->Release();
        }
    }
    mgr->Release();
}

// Снять всех слушателей со всех плейлистов.
// Вызывать из главного потока.
void DetachAllPlaylistListeners() {
    std::lock_guard<std::mutex> lk(g_listenersMutex);
    for (auto& e : g_playlistListeners) {
        if (e.playlist && e.listener) {
            e.playlist->ListenerRemove(e.listener);
            e.listener->Release();
        }
    }
    g_playlistListeners.clear();
}

// ==========================================
// AIMPPlaylistManagerListener — глобальный слушатель менеджера плейлистов.
// Регистрируется через g_core->RegisterExtension(IID_IAIMPServicePlaylistManager, ...).
// Вызывается AIMP из главного потока.
// ==========================================
class AIMPPlaylistManagerListener : public IAIMPExtensionPlaylistManagerListener {
    LONG ref_ = 1;
public:
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPExtensionPlaylistManagerListener) {
            *ppv = static_cast<IAIMPExtensionPlaylistManagerListener*>(this);
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

    // IAIMPExtensionPlaylistManagerListener
    void WINAPI PlaylistActivated(IAIMPPlaylist* Playlist) override {
        // Пользователь переключил вкладку в AIMP UI
        int idx = FindPlaylistIndex(Playlist);
        if (idx >= 0) {
            int focusIdx = GetFocusedIndex(Playlist);
            std::lock_guard<std::mutex> lk(g_focusMutex);
            g_focusPlaylistIdx = idx;
            if (focusIdx >= 0) g_focusTrackIdx = focusIdx;
            else               g_focusTrackIdx = 0;
        }
    }

    void WINAPI PlaylistAdded(IAIMPPlaylist* Playlist) override {
        // Новый плейлист — подписываемся на него
        AttachListenerToPlaylist(Playlist);
    }

    void WINAPI PlaylistRemoved(IAIMPPlaylist* Playlist) override {
        // Плейлист удалён — слушатель снимается автоматически в AIMPPlaylistListener::Removed()
        // Корректируем фокус если удалённый плейлист был в фокусе
        std::lock_guard<std::mutex> lk(g_focusMutex);
        // После удаления индексы могут сдвинуться, просто зажимаем
        // (более точная коррекция произойдёт при следующем обращении в GetPlayerStatus)
    }
};

// Инициализация системы синхронизации фокуса (вызывать из Initialize)
void InitFocusSync() {
    if (!g_core) return;
    // 1. Регистрируем глобальный слушатель менеджера
    g_managerListener = new AIMPPlaylistManagerListener();
    g_managerListener->AddRef();  // удерживаем ссылку
    g_core->RegisterExtension(IID_IAIMPServicePlaylistManager, g_managerListener);

    // 2. Навешиваем слушателей на все уже загруженные плейлисты
    //    + синхронизируем начальный фокус с текущим активным плейлистом AIMP
    RunInMainThread([&]() {
        AttachListenerToAllPlaylists();

        // Начальная синхронизация: берём активный плейлист AIMP как начальный фокус
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr)
            return;
        IAIMPPlaylist* activePl = nullptr;
        if (mgr->GetActivePlaylist(&activePl) == S_OK && activePl) {
            int idx = FindPlaylistIndex(activePl);
            if (idx >= 0) {
                int focusIdx = GetFocusedIndex(activePl);
                std::lock_guard<std::mutex> lk(g_focusMutex);
                g_focusPlaylistIdx = idx;
                g_focusTrackIdx = (focusIdx >= 0) ? focusIdx : 0;
            }
            activePl->Release();
        }
        mgr->Release();
    });
}

// Очистка системы синхронизации фокуса (вызывать из Finalize)
void FinalizeFocusSync() {
    // Снимаем слушателей с плейлистов (должно быть в главном потоке, но Finalize вызывается оттуда)
    DetachAllPlaylistListeners();

    // Дерегистрируем глобальный слушатель менеджера
    if (g_core && g_managerListener) {
        g_core->UnregisterExtension(g_managerListener);
    }
    if (g_managerListener) {
        g_managerListener->Release();
        g_managerListener = nullptr;
    }
}

// ==========================================
// Вспомогательные функции (вызываются ТОЛЬКО из главного потока)
// (PlaylistProps и GetFocusedIndex определены выше)
// ==========================================

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

// Вспомогательная: заполнить json информацией о плейлисте по индексу (вызывать из главного потока)
// ==========================================
// Message API helpers (shuffle, repeat, auto-jump)
// IAIMPServiceMessageDispatcher::Send() — thread-safe, можно из HTTP-потока
// ==========================================

// Получить bool-свойство через Message API
bool MsgGetBool(int propertyId) {
    if (!g_core) return false;
    IAIMPServiceMessageDispatcher* msgd = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&msgd) != S_OK || !msgd)
        return false;
    BOOL val = FALSE;
    msgd->Send(propertyId, AIMP_MSG_PROPVALUE_GET, &val);
    msgd->Release();
    return val != FALSE;
}

// Установить bool-свойство через Message API. Возвращает новое значение.
bool MsgSetBool(int propertyId, bool value) {
    if (!g_core) return false;
    IAIMPServiceMessageDispatcher* msgd = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&msgd) != S_OK || !msgd)
        return false;
    BOOL val = value ? TRUE : FALSE;
    msgd->Send(propertyId, AIMP_MSG_PROPVALUE_SET, &val);
    msgd->Release();
    return value;
}

// Toggle bool-свойство. Возвращает новое значение.
bool MsgToggleBool(int propertyId) {
    bool current = MsgGetBool(propertyId);
    return MsgSetBool(propertyId, !current);
}

// Возвращает false если плейлист не найден
bool FillPlaylistJson(IAIMPServicePlaylistManager* mgr, int idx, json& out) {
    if (idx < 0) return false;
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(idx, &pl) != S_OK || !pl) return false;
    out["id"]          = idx;
    out["aimp_id"]     = GetPlaylistId(pl);
    out["name"]        = GetPlaylistName(pl);
    out["track_count"] = pl->GetItemCount();
    pl->Release();
    return true;
}

// Вспомогательная: заполнить json информацией о треке (вызывать из главного потока)
bool FillTrackJson(IAIMPServicePlaylistManager* mgr, int plIdx, int trackIdx, json& out) {
    if (plIdx < 0 || trackIdx < 0) return false;
    IAIMPPlaylist* pl = nullptr;
    if (mgr->GetLoadedPlaylist(plIdx, &pl) != S_OK || !pl) return false;
    if (trackIdx >= pl->GetItemCount()) { pl->Release(); return false; }
    IAIMPPlaylistItem* item = nullptr;
    if (pl->GetItem(trackIdx, IID_IAIMPPlaylistItem, (void**)&item) != S_OK || !item) {
        pl->Release(); return false;
    }
    out["id"]    = trackIdx;
    out["playlist_id"] = plIdx;
    GetFileInfo(item, out);
    item->Release();
    pl->Release();
    return true;
}

json GetPlayerStatus() {
    json r;
    r["state"]    = "stopped";
    r["volume"]   = 0;
    r["muted"]    = false;
    r["position"] = 0.0;
    r["duration"] = 0.0;
    if (!g_core) return r;

    // --- Часть 1: IAIMPServicePlayer (thread-safe, вызываем напрямую) ---
    IAIMPServicePlayer* player = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK && player) {
        int st = player->GetState();
        r["state"] = (st == AIMP_PLAYER_STATE_PLAYING) ? "playing"
                   : (st == AIMP_PLAYER_STATE_PAUSED)  ? "paused" : "stopped";
        double pos = 0; player->GetPosition(&pos); r["position"]  = pos;
        double dur = 0; player->GetDuration(&dur);  r["duration"]  = dur;
        r["remaining"] = (dur > pos) ? (dur - pos) : 0.0;
        float  vol = 0; player->GetVolume(&vol);    r["volume"]   = (int)(vol * 100.0f);
        BOOL muted = FALSE; player->GetMute(&muted); r["muted"] = (muted != FALSE);
        player->Release();
    }

    // --- Часть 1.5: Свойства через Message API (thread-safe) ---
    r["shuffle"]         = MsgGetBool(AIMP_MSG_PROPERTY_SHUFFLE);
    r["repeat"]          = MsgGetBool(AIMP_MSG_PROPERTY_REPEAT);
    r["auto_jump"]       = MsgGetBool(AIMP_MSG_PROPERTY_AUTOJUMP_TO_NEXT_TRACK);

    r["next_track"] = nullptr;

    // --- Часть 2: Данные плейлистов/треков — требуют главного потока ---
    // playing_playlist, playing_track, focus_playlist, focus_track
    int focusPlIdx, focusTrIdx;
    {
        std::lock_guard<std::mutex> lk(g_focusMutex);
        focusPlIdx = g_focusPlaylistIdx;
        focusTrIdx = g_focusTrackIdx;
    }

    RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr)
            return;

        int totalPlaylists = mgr->GetLoadedPlaylistCount();
        r["playlist_count"] = totalPlaylists;

        // --- playing_playlist: тот, из которого играет трек ---
        IAIMPPlaylist* playingPl = nullptr;
        if (mgr->GetPlayingPlaylist(&playingPl) == S_OK && playingPl) {
            // Найдём его индекс
            for (int i = 0; i < totalPlaylists; i++) {
                IAIMPPlaylist* tmp = nullptr;
                if (mgr->GetLoadedPlaylist(i, &tmp) == S_OK && tmp) {
                    bool match = (tmp == playingPl);
                    tmp->Release();
                    if (match) {
                        json pp;
                        FillPlaylistJson(mgr, i, pp);
                        r["playing_playlist"] = pp;

                        // --- playing_track: трек который сейчас играет ---
                        int playingIdx = GetPlayingIndex(playingPl);
                        if (playingIdx >= 0) {
                            json pt;
                            FillTrackJson(mgr, i, playingIdx, pt);
                            r["playing_track"] = pt;
                        } else {
                            r["playing_track"] = nullptr;
                        }
                        break;
                    }
                }
            }
            playingPl->Release();
        } else {
            r["playing_playlist"] = nullptr;
            r["playing_track"]    = nullptr;
        }

        // --- focus_playlist: плейлист в фокусе (наш g_focusPlaylistIdx) ---
        // Зажимаем индекс в допустимых границах
        if (totalPlaylists > 0) {
            if (focusPlIdx >= totalPlaylists) focusPlIdx = totalPlaylists - 1;
            {
                std::lock_guard<std::mutex> lk(g_focusMutex);
                g_focusPlaylistIdx = focusPlIdx;
            }
            json fp;
            if (FillPlaylistJson(mgr, focusPlIdx, fp)) {
                r["focus_playlist"] = fp;
            } else {
                r["focus_playlist"] = nullptr;
            }

            // --- focus_track: трек в фокусе ---
            IAIMPPlaylist* focusPl = nullptr;
            if (mgr->GetLoadedPlaylist(focusPlIdx, &focusPl) == S_OK && focusPl) {
                int trackCount = focusPl->GetItemCount();
                if (focusTrIdx >= trackCount) focusTrIdx = std::max(0, trackCount - 1);
                {
                    std::lock_guard<std::mutex> lk(g_focusMutex);
                    g_focusTrackIdx = focusTrIdx;
                }
                json ft;
                if (trackCount > 0 && FillTrackJson(mgr, focusPlIdx, focusTrIdx, ft)) {
                    r["focus_track"] = ft;
                } else {
                    r["focus_track"] = nullptr;
                }
                focusPl->Release();
            } else {
                r["focus_track"] = nullptr;
            }
        } else {
            r["focus_playlist"] = nullptr;
            r["focus_track"]    = nullptr;
        }

        // --- next_track: следующий трек в очереди воспроизведения ---
        IAIMPServicePlaybackQueue* queue = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaybackQueue, (void**)&queue) == S_OK && queue) {
            IAIMPPlaybackQueueItem* nextItem = nullptr;
            if (queue->GetNextTrack(&nextItem) == S_OK && nextItem) {
                IAIMPPlaylistItem* plItem = nullptr;
                if (nextItem->GetValueAsObject(AIMP_PLAYBACKQUEUEITEM_PROPID_PLAYLISTITEM,
                        IID_IAIMPPlaylistItem, (void**)&plItem) == S_OK && plItem) {
                    json nt;
                    GetFileInfo(plItem, nt);
                    r["next_track"] = nt;
                    plItem->Release();
                }
                nextItem->Release();
            }
            queue->Release();
        }

        mgr->Release();
    });

    return r;
}

// --- Focus API: навигация кнопками < > ---

// Вспомогательная: сдвинуть фокус плейлиста и сбросить трек на 0
json FocusPlaylistShift(int delta) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }
        int total = mgr->GetLoadedPlaylistCount();
        if (total == 0) { mgr->Release(); r["error"] = "no playlists"; return; }

        int newIdx;
        {
            std::lock_guard<std::mutex> lk(g_focusMutex);
            newIdx = ((g_focusPlaylistIdx + delta) % total + total) % total;
            g_focusPlaylistIdx = newIdx;
            g_focusTrackIdx    = 0;  // при смене плейлиста — трек сбрасываем на первый
        }

        // Переключаем вкладку в UI AIMP + выделяем первый трек
        IAIMPPlaylist* newPl = nullptr;
        if (mgr->GetLoadedPlaylist(newIdx, &newPl) == S_OK && newPl) {
            mgr->SetActivePlaylist(newPl);
            SetPlaylistFocusAndSelection(newPl, 0);
            newPl->Release();
        }

        json fp;
        FillPlaylistJson(mgr, newIdx, fp);
        r["focus_playlist"] = fp;

        // Возвращаем и первый трек нового плейлиста
        json ft;
        if (FillTrackJson(mgr, newIdx, 0, ft))
            r["focus_track"] = ft;
        else
            r["focus_track"] = nullptr;

        mgr->Release();
    });
    return r;
}

// Вспомогательная: сдвинуть фокус трека внутри текущего плейлиста
json FocusTrackShift(int delta) {
    json r;
    if (!g_core) { r["error"] = "core not initialized"; return r; }

    RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            r["error"] = "playlist manager unavailable"; return;
        }

        int plIdx;
        {
            std::lock_guard<std::mutex> lk(g_focusMutex);
            plIdx = g_focusPlaylistIdx;
        }

        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(plIdx, &pl) != S_OK || !pl) {
            mgr->Release(); r["error"] = "playlist not found"; return;
        }

        int total = pl->GetItemCount();
        if (total == 0) { pl->Release(); mgr->Release(); r["error"] = "playlist is empty"; return; }

        int newTrackIdx;
        {
            std::lock_guard<std::mutex> lk(g_focusMutex);
            newTrackIdx = ((g_focusTrackIdx + delta) % total + total) % total;
            g_focusTrackIdx = newTrackIdx;
        }

        // Устанавливаем фокус + selection трека в UI AIMP
        SetPlaylistFocusAndSelection(pl, newTrackIdx);
        pl->Release();

        json ft;
        FillTrackJson(mgr, plIdx, newTrackIdx, ft);
        r["focus_track"] = ft;

        // Возвращаем и текущий плейлист для контекста
        json fp;
        FillPlaylistJson(mgr, plIdx, fp);
        r["focus_playlist"] = fp;

        mgr->Release();
    });
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
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(g_port);
    // Bind mode: 0 = localhost, 1 = LAN (0.0.0.0 but we could filter — for now same as 2), 2 = all
    if (g_bindMode == 0)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(srv); WSACleanup(); return; }
    listen(srv, SOMAXCONN);
    g_serverSocket = srv;
    g_running = true;

    while (g_running) {
        fd_set rs; FD_ZERO(&rs); FD_SET(srv, &rs);
        timeval to{1, 0};
        if (select(0, &rs, nullptr, nullptr, &to) <= 0) continue;
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET cl = accept(srv, (sockaddr*)&clientAddr, &clientLen);
        if (cl == INVALID_SOCKET) continue;

        // LAN mode: фильтруем — допускаем только private ranges + localhost
        if (g_bindMode == 1) {
            unsigned long ip = ntohl(clientAddr.sin_addr.s_addr);
            bool isPrivate = (ip >> 24 == 127)                       // 127.x.x.x
                          || (ip >> 24 == 10)                        // 10.x.x.x
                          || ((ip >> 20) == (172 << 4 | 1))          // 172.16-31.x.x
                          || ((ip >> 16) == (192 << 8 | 168));       // 192.168.x.x
            if (!isPrivate) { closesocket(cl); continue; }
        }

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
            // ---- Player toggles ----
            // POST /api/player/shuffle — toggle shuffle
            else if (req.path == "/api/player/shuffle" && req.method == "POST") {
                bool val = MsgToggleBool(AIMP_MSG_PROPERTY_SHUFFLE);
                rsp["shuffle"] = val;
            }
            // GET /api/player/shuffle — get shuffle state
            else if (req.path == "/api/player/shuffle" && req.method == "GET") {
                rsp["shuffle"] = MsgGetBool(AIMP_MSG_PROPERTY_SHUFFLE);
            }
            // POST /api/player/repeat — toggle repeat
            else if (req.path == "/api/player/repeat" && req.method == "POST") {
                bool val = MsgToggleBool(AIMP_MSG_PROPERTY_REPEAT);
                rsp["repeat"] = val;
            }
            // GET /api/player/repeat — get repeat state
            else if (req.path == "/api/player/repeat" && req.method == "GET") {
                rsp["repeat"] = MsgGetBool(AIMP_MSG_PROPERTY_REPEAT);
            }
            // POST /api/player/auto-jump — toggle "automatically jump to next track"
            else if (req.path == "/api/player/auto-jump" && req.method == "POST") {
                bool val = MsgToggleBool(AIMP_MSG_PROPERTY_AUTOJUMP_TO_NEXT_TRACK);
                rsp["auto_jump"] = val;
            }
            // GET /api/player/auto-jump — get auto-jump state
            else if (req.path == "/api/player/auto-jump" && req.method == "GET") {
                rsp["auto_jump"] = MsgGetBool(AIMP_MSG_PROPERTY_AUTOJUMP_TO_NEXT_TRACK);
            }
            // ---- Focus API ----
            // POST /api/focus/playlist/next  — следующий плейлист в фокусе
            else if (req.path == "/api/focus/playlist/next" && req.method == "POST") {
                rsp = FocusPlaylistShift(+1);
            }
            // POST /api/focus/playlist/prev  — предыдущий плейлист в фокусе
            else if (req.path == "/api/focus/playlist/prev" && req.method == "POST") {
                rsp = FocusPlaylistShift(-1);
            }
            // POST /api/focus/track/next  — следующий трек в фокусе
            else if (req.path == "/api/focus/track/next" && req.method == "POST") {
                rsp = FocusTrackShift(+1);
            }
            // POST /api/focus/track/prev  — предыдущий трек в фокусе
            else if (req.path == "/api/focus/track/prev" && req.method == "POST") {
                rsp = FocusTrackShift(-1);
            }
            // POST /api/focus/play  — воспроизвести трек в фокусе
            else if (req.path == "/api/focus/play" && req.method == "POST") {
                int plIdx, trIdx;
                {
                    std::lock_guard<std::mutex> lk(g_focusMutex);
                    plIdx = g_focusPlaylistIdx;
                    trIdx = g_focusTrackIdx;
                }
                ParsedPath pp;
                pp.playlistId = plIdx;
                pp.trackId    = trIdx;
                pp.action     = "play";
                DoPlaylistAction(pp, "POST", "", rsp, code);
            }
            // GET /api/focus  — текущее состояние фокуса без полного статуса
            else if (req.path == "/api/focus" && req.method == "GET") {
                int plIdx, trIdx;
                {
                    std::lock_guard<std::mutex> lk(g_focusMutex);
                    plIdx = g_focusPlaylistIdx;
                    trIdx = g_focusTrackIdx;
                }
                RunInMainThread([&]() {
                    IAIMPServicePlaylistManager* mgr = nullptr;
                    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
                        rsp["error"] = "playlist manager unavailable"; return;
                    }
                    json fp; FillPlaylistJson(mgr, plIdx, fp); rsp["focus_playlist"] = fp;
                    json ft; FillTrackJson(mgr, plIdx, trIdx, ft); rsp["focus_track"] = ft;
                    mgr->Release();
                });
            }
            // ---- Playlists API ----
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
                    "GET  /api/player/status  — полный статус + shuffle/repeat/auto_jump/next_track",
                    "POST /api/player/play",
                    "POST /api/player/pause",
                    "POST /api/player/stop",
                    "POST /api/player/next",
                    "POST /api/player/prev",
                    "GET  /api/player/volume",
                    "PUT  /api/player/volume",
                    "POST /api/player/mute",
                    "PUT  /api/player/position",
                    "--- Toggles ---",
                    "GET  /api/player/shuffle    — состояние shuffle",
                    "POST /api/player/shuffle    — toggle shuffle",
                    "GET  /api/player/repeat     — состояние repeat",
                    "POST /api/player/repeat     — toggle repeat",
                    "GET  /api/player/auto-jump  — auto jump to next track",
                    "POST /api/player/auto-jump  — toggle auto jump",
                    "--- Focus (Bitfocus navigation) ---",
                    "GET  /api/focus              — текущий плейлист и трек в фокусе",
                    "POST /api/focus/playlist/next — следующий плейлист в фокусе",
                    "POST /api/focus/playlist/prev — предыдущий плейлист в фокусе",
                    "POST /api/focus/track/next    — следующий трек в фокусе",
                    "POST /api/focus/track/prev    — предыдущий трек в фокусе",
                    "POST /api/focus/play          — воспроизвести трек в фокусе",
                    "--- Playlists ---",
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
                rsp["error"]["code"] = "NOT_FOUND"; code = 404;
            }

            SendResponse(cl, code, rsp);
        }
        closesocket(cl);
    }
    g_serverSocket = INVALID_SOCKET;
    closesocket(srv);
    WSACleanup();
}

// ==========================================
// Настройки (INI)
// ==========================================
std::wstring GetSettingsPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.rfind(L'\\');
    if (pos != std::wstring::npos) s = s.substr(0, pos + 1);
    s += L"AimpHttpControl.ini";
    return s;
}

void LoadSettings() {
    std::wstring ini = GetSettingsPath();
    g_port     = GetPrivateProfileIntW(L"Server", L"Port", 3553, ini.c_str());
    g_bindMode = GetPrivateProfileIntW(L"Server", L"BindMode", 2, ini.c_str());
    if (g_port < 1 || g_port > 65535) g_port = 3553;
    if (g_bindMode < 0 || g_bindMode > 2) g_bindMode = 2;
}

void SaveSettings() {
    std::wstring ini = GetSettingsPath();
    wchar_t buf[16];
    wsprintfW(buf, L"%d", g_port);
    WritePrivateProfileStringW(L"Server", L"Port", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_bindMode);
    WritePrivateProfileStringW(L"Server", L"BindMode", buf, ini.c_str());
}

// ==========================================
// Win32 Диалог настроек
// ==========================================
// Control IDs
#define IDC_PORT_EDIT    101
#define IDC_BIND_COMBO   102
#define IDC_BTN_OK       103
#define IDC_BTN_CANCEL   104
#define IDC_STATUS_LABEL 105

void RestartHttpServer();  // forward decl

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        // Заполняем порт
        SetDlgItemInt(hDlg, IDC_PORT_EDIT, g_port, FALSE);

        // Заполняем ComboBox
        HWND hCombo = GetDlgItem(hDlg, IDC_BIND_COMBO);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"127.0.0.1 (localhost only)");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"LAN (private networks)");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"0.0.0.0 (all interfaces)");
        SendMessageW(hCombo, CB_SETCURSEL, g_bindMode, 0);

        // Статус
        wchar_t status[128];
        const wchar_t* bindStr = (g_bindMode == 0) ? L"127.0.0.1" : (g_bindMode == 1) ? L"LAN" : L"0.0.0.0";
        wsprintfW(status, L"Server running on %s:%d", bindStr, g_port);
        SetDlgItemTextW(hDlg, IDC_STATUS_LABEL, status);

        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OK: {
            BOOL ok = FALSE;
            int port = GetDlgItemInt(hDlg, IDC_PORT_EDIT, &ok, FALSE);
            if (!ok || port < 1 || port > 65535) {
                MessageBoxW(hDlg, L"Port must be between 1 and 65535", L"Invalid Port", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            int bind = (int)SendDlgItemMessageW(hDlg, IDC_BIND_COMBO, CB_GETCURSEL, 0, 0);
            if (bind < 0 || bind > 2) bind = 2;

            bool needRestart = (port != g_port || bind != g_bindMode);
            g_port = port;
            g_bindMode = bind;
            SaveSettings();

            if (needRestart) {
                RestartHttpServer();
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDC_BTN_CANCEL:
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

HWND CreateSettingsDialog(HWND parent) {
    // Создаём диалог в памяти (DLGTEMPLATE) — не нужен .rc файл
    // Размер 320x180 dialog units
    const int DLG_W = 280, DLG_H = 130;

    // Выделяем буфер для template
    WORD dlgBuf[2048] = {};
    WORD* p = dlgBuf;

    // DLGTEMPLATE
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)p;
    dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->cdit  = 7;  // количество контролов
    dlg->x = 0; dlg->y = 0; dlg->cx = DLG_W; dlg->cy = DLG_H;
    p = (WORD*)(dlg + 1);
    *p++ = 0; // menu
    *p++ = 0; // class
    // title "AIMP HTTP Control — Settings"
    const wchar_t* title = L"AIMP HTTP Control \x2014 Settings";
    int titleLen = (int)wcslen(title) + 1;
    memcpy(p, title, titleLen * 2); p += titleLen;

    // Helper lambda: add DLGITEMTEMPLATE
    auto addItem = [&](DWORD style, int x, int y, int cx, int cy, WORD id, const wchar_t* cls, const wchar_t* text) {
        // Align to DWORD
        if ((ULONG_PTR)p & 2) p++;
        DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
        item->style = style | WS_CHILD | WS_VISIBLE;
        item->x = (short)x; item->y = (short)y; item->cx = (short)cx; item->cy = (short)cy;
        item->id = id;
        p = (WORD*)(item + 1);
        // class
        int clsLen = (int)wcslen(cls) + 1;
        memcpy(p, cls, clsLen * 2); p += clsLen;
        // text
        int txtLen = (int)wcslen(text) + 1;
        memcpy(p, text, txtLen * 2); p += txtLen;
        *p++ = 0; // extra
    };

    // Label "Port:"
    addItem(SS_LEFT, 14, 16, 30, 10, (WORD)-1, L"STATIC", L"Port:");
    // Edit (port)
    addItem(ES_NUMBER | WS_BORDER | WS_TABSTOP, 50, 14, 50, 14, IDC_PORT_EDIT, L"EDIT", L"3553");
    // Label "Bind:"
    addItem(SS_LEFT, 14, 38, 30, 10, (WORD)-1, L"STATIC", L"Bind:");
    // ComboBox (bind mode)
    addItem(CBS_DROPDOWNLIST | WS_TABSTOP, 50, 36, 160, 80, IDC_BIND_COMBO, L"COMBOBOX", L"");
    // Status label
    addItem(SS_LEFT, 14, 62, 250, 10, IDC_STATUS_LABEL, L"STATIC", L"");
    // OK button
    addItem(BS_DEFPUSHBUTTON | WS_TABSTOP, 100, 90, 60, 18, IDC_BTN_OK, L"BUTTON", L"Save");
    // Cancel button
    addItem(WS_TABSTOP, 170, 90, 60, 18, IDC_BTN_CANCEL, L"BUTTON", L"Cancel");

    return (HWND)(INT_PTR)DialogBoxIndirectW(g_hInstance, (DLGTEMPLATE*)dlgBuf, parent, SettingsDlgProc);
}

// ==========================================
// IAIMPExternalSettingsDialog — кнопка Settings в менеджере плагинов
// ==========================================
class PluginSettingsDialog : public IAIMPExternalSettingsDialog {
    LONG ref_ = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPExternalSettingsDialog) {
            *ppv = static_cast<IAIMPExternalSettingsDialog*>(this);
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

    void WINAPI Show(HWND ParentWindow) override {
        CreateSettingsDialog(ParentWindow);
    }
};

// ==========================================
// Перезапуск HTTP сервера
// ==========================================
void RestartHttpServer() {
    // Останавливаем текущий сервер
    g_running = false;
    // Закрываем серверный сокет чтобы прервать select/accept
    if (g_serverSocket != INVALID_SOCKET) {
        closesocket(g_serverSocket);
        g_serverSocket = INVALID_SOCKET;
    }
    Sleep(300);
    // Запускаем заново
    g_serverThread = std::thread(RunHttpServer);
    g_serverThread.detach();
}

// ==========================================
// Плагин AIMP
// ==========================================
class HttpControlPlugin : public IAIMPPlugin {
    LONG ref_ = 1;
    PluginSettingsDialog* settingsDlg_ = nullptr;
public:
    virtual ~HttpControlPlugin() {
        if (settingsDlg_) settingsDlg_->Release();
    }
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IAIMPExternalSettingsDialog) {
            if (!settingsDlg_) settingsDlg_ = new PluginSettingsDialog();
            *ppv = static_cast<IAIMPExternalSettingsDialog*>(settingsDlg_);
            settingsDlg_->AddRef();
            return S_OK;
        }
        *ppv = static_cast<IAIMPPlugin*>(this); AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref_); if (r==0) delete this; return r; }

    TChar* WINAPI InfoGet(int Index) override {
        static wchar_t n[] = L"AIMP HTTP Control API v2";
        static wchar_t a[] = L"SLV Dev Team";
        static wchar_t d[] = L"Full REST API for AIMP";
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
        // Загружаем настройки из INI
        LoadSettings();
        // Запускаем синхронизацию фокуса с UI AIMP
        InitFocusSync();
        g_serverThread = std::thread(RunHttpServer);
        g_serverThread.detach();
        return S_OK;
    }
    HRESULT WINAPI Finalize() override {
        g_running = false;
        if (g_serverSocket != INVALID_SOCKET) {
            closesocket(g_serverSocket);
            g_serverSocket = INVALID_SOCKET;
        }
        Sleep(300);
        // Останавливаем синхронизацию фокуса
        FinalizeFocusSync();
        g_core = nullptr;
        return S_OK;
    }
};

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = new HttpControlPlugin();
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hInstance = hInstDLL;
    return TRUE;
}
