// aimp_http_plugin.cpp
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <commctrl.h>

// EM_SETCUEBANNER может отсутствовать в старых MinGW-заголовках
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

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
// Bind mode: 0 = 127.0.0.1 (localhost only), 1 = 0.0.0.0 (all interfaces), 2 = конкретный IP (g_bindIp)
int        g_bindMode = 1;
std::wstring g_bindIp = L"";         // IP для bindMode=2 (конкретный интерфейс)
bool       g_running = false;
std::thread g_serverThread;
SOCKET     g_serverSocket = INVALID_SOCKET;  // для корректного перезапуска

// Политика доступа: allowlist по IP/CIDR
bool       g_allowListEnabled = false;
std::string g_allowList = "";        // Список IP/CIDR через запятую

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
// Утилиты: конвертация wstring <-> string (UTF-8)
// ==========================================
std::string WStrToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWStr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

// ==========================================
// Перечисление сетевых интерфейсов (IPv4)
// ==========================================
struct NetworkInterface {
    std::wstring displayName;  // "Ethernet (192.168.1.5)"
    std::wstring ip;           // "192.168.1.5"
};

static std::vector<NetworkInterface> EnumNetworkInterfaces() {
    std::vector<NetworkInterface> result;
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    // Пробуем с увеличением буфера если не хватило
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            buf.resize(bufLen);
            continue;
        }
        if (ret != ERROR_SUCCESS) break;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            // Пропускаем loopback и неактивные
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                auto* sin = (sockaddr_in*)ua->Address.lpSockaddr;
                wchar_t ipBuf[INET_ADDRSTRLEN] = {};
                InetNtopW(AF_INET, &sin->sin_addr, ipBuf, INET_ADDRSTRLEN);
                // Формируем отображаемое имя: "Friendly Name (IP)"
                std::wstring friendly = a->FriendlyName ? std::wstring(a->FriendlyName) : L"Unknown";
                NetworkInterface iface;
                iface.ip          = ipBuf;
                iface.displayName = friendly + L" (" + ipBuf + L")";
                result.push_back(std::move(iface));
            }
        }
        break;
    }
    return result;
}

// ==========================================
// IP/CIDR allowlist — проверка входящего соединения
// ==========================================

// Парсит строку вида "192.168.1.5" -> uint32 (host byte order). Возвращает false при ошибке.
static bool ParseIPv4(const std::string& s, uint32_t& out) {
    unsigned int a, b, c, d;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

// Возвращает true если ip (host byte order) попадает в CIDR или совпадает с точным адресом.
static bool MatchCIDR(uint32_t ip, const std::string& entry) {
    auto slash = entry.find('/');
    if (slash == std::string::npos) {
        // точный адрес
        uint32_t addr = 0;
        return ParseIPv4(entry, addr) && (ip == addr);
    }
    std::string addrPart = entry.substr(0, slash);
    std::string prefixPart = entry.substr(slash + 1);
    uint32_t addr = 0;
    if (!ParseIPv4(addrPart, addr)) return false;
    int prefix = 0;
    try { prefix = std::stoi(prefixPart); } catch (...) { return false; }
    if (prefix < 0 || prefix > 32) return false;
    if (prefix == 0) return true;  // 0.0.0.0/0 — все
    uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
    return (ip & mask) == (addr & mask);
}

// Разбивает строку по разделителю
static std::vector<std::string> SplitTrim(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim)) {
        // trim spaces
        auto start = token.find_first_not_of(" \t\r\n");
        auto end   = token.find_last_not_of(" \t\r\n");
        if (start != std::string::npos)
            result.push_back(token.substr(start, end - start + 1));
    }
    return result;
}

// Проверяет, разрешён ли клиентский IP (в network byte order из accept()).
// Если allowlist выключен — всегда true.
bool IsAllowedClient(uint32_t clientIpNetworkOrder) {
    if (!g_allowListEnabled) return true;
    if (g_allowList.empty()) return false;  // список включён, но пуст — никого не пускаем
    uint32_t ip = ntohl(clientIpNetworkOrder);
    for (const auto& entry : SplitTrim(g_allowList, ',')) {
        if (MatchCIDR(ip, entry)) return true;
    }
    return false;
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
    // Bind mode: 0 = localhost, 1 = 0.0.0.0 (все интерфейсы), 2 = конкретный IP из g_bindIp
    if (g_bindMode == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (g_bindMode == 2 && !g_bindIp.empty()) {
        if (InetPtonW(AF_INET, g_bindIp.c_str(), &addr.sin_addr) != 1)
            addr.sin_addr.s_addr = INADDR_ANY;  // fallback если адрес некорректен
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
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

        // Allowlist: фильтруем по IP/CIDR если включена политика доступа
        if (!IsAllowedClient(clientAddr.sin_addr.s_addr)) {
            closesocket(cl); continue;
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
                rsp["name"] = "AIMP HTTP Control API v0.8";
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
    wchar_t appData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\AIMP";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\AimpHttpControl.ini";
    }
    // Fallback: рядом с DLL
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.rfind(L'\\');
    if (pos != std::wstring::npos) s = s.substr(0, pos + 1);
    return s + L"AimpHttpControl.ini";
}

void LoadSettings() {
    std::wstring ini = GetSettingsPath();
    g_port     = GetPrivateProfileIntW(L"Server", L"Port", 19122, ini.c_str());
    g_bindMode = GetPrivateProfileIntW(L"Server", L"BindMode", 2, ini.c_str());
    if (g_port < 1 || g_port > 65535) g_port = 19122;
    if (g_bindMode < 0 || g_bindMode > 2) g_bindMode = 2;

    // BindIP для bindMode=1
    wchar_t bindIpBuf[64] = {};
    GetPrivateProfileStringW(L"Server", L"BindIP", L"", bindIpBuf, 64, ini.c_str());
    g_bindIp = std::wstring(bindIpBuf);

    // AllowList
    g_allowListEnabled = GetPrivateProfileIntW(L"Access", L"AllowListEnabled", 0, ini.c_str()) != 0;
    wchar_t alBuf[1024] = {};
    GetPrivateProfileStringW(L"Access", L"AllowList", L"", alBuf, 1024, ini.c_str());
    g_allowList = WStrToUtf8(std::wstring(alBuf));
}

void SaveSettings() {
    std::wstring ini = GetSettingsPath();
    wchar_t buf[16];
    wsprintfW(buf, L"%d", g_port);
    WritePrivateProfileStringW(L"Server", L"Port", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_bindMode);
    WritePrivateProfileStringW(L"Server", L"BindMode", buf, ini.c_str());
    WritePrivateProfileStringW(L"Server", L"BindIP",
        g_bindIp.empty() ? L"" : g_bindIp.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Access", L"AllowListEnabled",
        g_allowListEnabled ? L"1" : L"0", ini.c_str());
    std::wstring alW = Utf8ToWStr(g_allowList);
    WritePrivateProfileStringW(L"Access", L"AllowList",
        alW.empty() ? L"" : alW.c_str(), ini.c_str());
}

// ==========================================
// IAIMPOptionsDialogFrame — страница настроек в меню Options AIMP
// ==========================================

// Control IDs (используются внутри фрейма)
#define IDC_PORT_EDIT       101
#define IDC_BIND_COMBO      102
#define IDC_STATUS_LABEL    103
#define IDC_ALLOW_CHECK     104
#define IDC_ALLOW_EDIT      105
#define IDC_ALLOW_LABEL     106

void RestartHttpServer();  // forward decl

// Временные значения настроек, редактируемые во фрейме (применяются по Notification(SAVE))
static int   s_port            = 19122;
static int   s_bindMode        = 1;   // 0=localhost, 1=all, 2=конкретный интерфейс
static std::wstring s_bindIp   = L"";
static bool  s_allowEnabled    = false;
static std::string s_allowList = "";

// Список интерфейсов, загружается при CreateFrame
static std::vector<NetworkInterface> s_interfaces;

// Дескриптор окна фрейма (дочернего для родительского окна настроек AIMP)
static HWND  s_frameHwnd = nullptr;

// Возвращает индекс в комбобоксе для текущего s_bindIp среди s_interfaces (или -1)
static int FindInterfaceComboIndex(const std::wstring& ip) {
    for (int i = 0; i < (int)s_interfaces.size(); ++i)
        if (s_interfaces[i].ip == ip) return 2 + i;  // 0=localhost, 1=all, 2+i=интерфейсы
    return -1;
}

static void UpdateFrameControls() {
    if (!s_frameHwnd) return;
    // bindMode=2 -> конкретный интерфейс, но поле IP скрыто — выбор через комбо
    // allowList -> показываем поле только если галочка стоит
    ShowWindow(GetDlgItem(s_frameHwnd, IDC_ALLOW_LABEL), s_allowEnabled ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(s_frameHwnd, IDC_ALLOW_EDIT),  s_allowEnabled ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK FrameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD notif = HIWORD(wParam);
        if (id == IDC_BIND_COMBO && notif == CBN_SELCHANGE) {
            int sel = (int)SendDlgItemMessageW(hWnd, IDC_BIND_COMBO, CB_GETCURSEL, 0, 0);
            if (sel == 0) {
                s_bindMode = 0;
            } else if (sel == 1) {
                s_bindMode = 1;
            } else {
                // конкретный интерфейс
                int ifIdx = sel - 2;
                if (ifIdx >= 0 && ifIdx < (int)s_interfaces.size()) {
                    s_bindMode = 2;
                    s_bindIp   = s_interfaces[ifIdx].ip;
                }
            }
            UpdateFrameControls();
        }
        if (id == IDC_ALLOW_CHECK && notif == BN_CLICKED) {
            s_allowEnabled = (SendDlgItemMessageW(hWnd, IDC_ALLOW_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
            UpdateFrameControls();
        }
        break;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Регистрируем класс окна фрейма один раз
static bool RegisterFrameClass() {
    WNDCLASSEXW wc = {};
    if (GetClassInfoExW(g_hInstance, L"AIMPHttpCtrlFrame", &wc)) return true;
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = FrameWndProc;
    wc.hInstance     = g_hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AIMPHttpCtrlFrame";
    return RegisterClassExW(&wc) != 0;
}

// Вспомогательная: добавить static-контрол (label)
static HWND AddLabel(HWND parent, int x, int y, int w, int h, WORD id, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_hInstance, nullptr);
}

// Вспомогательная: добавить edit-контрол
static HWND AddEdit(HWND parent, int x, int y, int w, int h, WORD id, const wchar_t* text, bool numOnly = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (numOnly) style |= ES_NUMBER;
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        style, x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_hInstance, nullptr);
    return hw;
}

class HttpControlOptionsFrame : public IAIMPOptionsDialogFrame {
    LONG ref_ = 1;
    IAIMPServiceOptionsDialog* svcOpts_ = nullptr;  // для FrameModified
public:
    explicit HttpControlOptionsFrame(IAIMPServiceOptionsDialog* svc) : svcOpts_(svc) {
        if (svcOpts_) svcOpts_->AddRef();
    }
    virtual ~HttpControlOptionsFrame() {
        if (svcOpts_) svcOpts_->Release();
    }

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPOptionsDialogFrame) {
            *ppv = static_cast<IAIMPOptionsDialogFrame*>(this);
            AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // IAIMPOptionsDialogFrame
    HRESULT WINAPI GetName(IAIMPString** s) override {
        if (!s) return E_POINTER;
        IAIMPString* str = nullptr;
        if (g_core && g_core->CreateObject(IID_IAIMPString, (void**)&str) == S_OK && str) {
            str->SetData(const_cast<wchar_t*>(L"HTTP Remote Control"), 19);
            *s = str;
            return S_OK;
        }
        return E_FAIL;
    }

    HWND WINAPI CreateFrame(HWND parentWnd) override {
        RegisterFrameClass();

        // Создаём дочернее окно фрейма
        RECT rc; GetClientRect(parentWnd, &rc);
        int W = rc.right - rc.left;

        HWND hw = CreateWindowExW(0, L"AIMPHttpCtrlFrame", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, W, rc.bottom - rc.top,
            parentWnd, nullptr, g_hInstance, nullptr);
        s_frameHwnd = hw;
        if (!hw) return nullptr;

        // Загружаем текущие значения в локальные переменные фрейма
        s_port         = g_port;
        s_bindMode     = g_bindMode;
        s_bindIp       = g_bindIp;
        s_allowEnabled = g_allowListEnabled;
        s_allowList    = g_allowList;

        // Перечисляем сетевые интерфейсы
        s_interfaces = EnumNetworkInterfaces();

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // --- Секция: Server ---
        // "Port:" label
        HWND hPortLabel = AddLabel(hw, 12, 14, 60, 16, (WORD)-1, L"Port:");
        SendMessageW(hPortLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Port edit
        wchar_t portBuf[16]; wsprintfW(portBuf, L"%d", s_port);
        HWND hPortEdit = AddEdit(hw, 76, 12, 70, 20, IDC_PORT_EDIT, portBuf, true);
        SendMessageW(hPortEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hPortEdit, EM_SETLIMITTEXT, 5, 0);

        // "Listen interface:" label
        HWND hBindLabel = AddLabel(hw, 12, 44, 120, 16, (WORD)-1, L"Listen interface:");
        SendMessageW(hBindLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Bind combo: первые два — фиксированные, далее — реальные интерфейсы
        HWND hCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            136, 42, 260, 200, hw, (HMENU)(UINT_PTR)IDC_BIND_COMBO, g_hInstance, nullptr);
        SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Localhost (127.0.0.1)");  // idx 0
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"All interfaces (0.0.0.0)"); // idx 1
        for (const auto& iface : s_interfaces)
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)iface.displayName.c_str());

        // Выбираем нужный элемент
        if (s_bindMode == 0) {
            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        } else if (s_bindMode == 2 && !s_bindIp.empty()) {
            int idx = FindInterfaceComboIndex(s_bindIp);
            SendMessageW(hCombo, CB_SETCURSEL, idx >= 0 ? idx : 1, 0);
        } else {
            SendMessageW(hCombo, CB_SETCURSEL, 1, 0);  // All interfaces
        }

        // --- Секция: Access policy ---
        HWND hSep = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            12, 76, W - 24, 2, hw, nullptr, g_hInstance, nullptr);
        (void)hSep;

        // Checkbox "Allow access only from:"
        HWND hCheck = CreateWindowExW(0, L"BUTTON",
            L"Allow access only from:",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            12, 88, 280, 20, hw, (HMENU)(UINT_PTR)IDC_ALLOW_CHECK, g_hInstance, nullptr);
        SendMessageW(hCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCheck, BM_SETCHECK, s_allowEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

        // "IP/CIDR list:" label
        HWND hAlLabel = AddLabel(hw, 12, 120, 120, 16, IDC_ALLOW_LABEL, L"IP / CIDR list:");
        SendMessageW(hAlLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // AllowList edit
        HWND hAlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            Utf8ToWStr(s_allowList).c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
            136, 118, W - 148, 20, hw, (HMENU)(UINT_PTR)IDC_ALLOW_EDIT, g_hInstance, nullptr);
        SendMessageW(hAlEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hAlEdit, EM_SETCUEBANNER, FALSE,
            (LPARAM)L"192.168.1.0/24, 10.0.0.5, 172.16.0.0/12");
        SendMessageW(hAlEdit, EM_SETLIMITTEXT, 1023, 0);

        // Status label
        wchar_t statusBuf[128];
        const wchar_t* bindStr = (g_bindMode == 0) ? L"127.0.0.1"
            : (g_bindMode == 2 && !g_bindIp.empty()) ? g_bindIp.c_str() : L"0.0.0.0";
        wsprintfW(statusBuf, L"Server: %s:%d", bindStr, g_port);
        HWND hStatus = AddLabel(hw, 12, 148, W - 24, 16, IDC_STATUS_LABEL, statusBuf);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        UpdateFrameControls();
        return hw;
    }

    void WINAPI DestroyFrame() override {
        if (s_frameHwnd) {
            DestroyWindow(s_frameHwnd);
            s_frameHwnd = nullptr;
        }
    }

    void WINAPI Notification(int id) override {
        switch (id) {
        case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_LOAD:
            if (s_frameHwnd) {
                s_port         = g_port;
                s_bindMode     = g_bindMode;
                s_bindIp       = g_bindIp;
                s_allowEnabled = g_allowListEnabled;
                s_allowList    = g_allowList;

                wchar_t buf[16]; wsprintfW(buf, L"%d", s_port);
                SetDlgItemTextW(s_frameHwnd, IDC_PORT_EDIT, buf);

                // Выбираем нужный элемент комбобокса
                if (s_bindMode == 0) {
                    SendDlgItemMessageW(s_frameHwnd, IDC_BIND_COMBO, CB_SETCURSEL, 0, 0);
                } else if (s_bindMode == 2 && !s_bindIp.empty()) {
                    int idx = FindInterfaceComboIndex(s_bindIp);
                    SendDlgItemMessageW(s_frameHwnd, IDC_BIND_COMBO, CB_SETCURSEL,
                        idx >= 0 ? idx : 1, 0);
                } else {
                    SendDlgItemMessageW(s_frameHwnd, IDC_BIND_COMBO, CB_SETCURSEL, 1, 0);
                }

                SendDlgItemMessageW(s_frameHwnd, IDC_ALLOW_CHECK, BM_SETCHECK,
                    s_allowEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SetDlgItemTextW(s_frameHwnd, IDC_ALLOW_EDIT,
                    Utf8ToWStr(s_allowList).c_str());
                UpdateFrameControls();
            }
            break;

        case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_SAVE:
            if (s_frameHwnd) {
                BOOL ok = FALSE;
                int port = GetDlgItemInt(s_frameHwnd, IDC_PORT_EDIT, &ok, FALSE);
                if (!ok || port < 1 || port > 65535) port = g_port;

                // s_bindMode и s_bindIp уже обновлены в FrameWndProc при CBN_SELCHANGE
                bool allowEnabled = (SendDlgItemMessageW(s_frameHwnd, IDC_ALLOW_CHECK,
                    BM_GETCHECK, 0, 0) == BST_CHECKED);
                wchar_t alBuf[1024] = {};
                GetDlgItemTextW(s_frameHwnd, IDC_ALLOW_EDIT, alBuf, 1024);
                std::string allowList = WStrToUtf8(std::wstring(alBuf));

                bool needRestart = (port != g_port || s_bindMode != g_bindMode || s_bindIp != g_bindIp);

                g_port             = port;
                g_bindMode         = s_bindMode;
                g_bindIp           = s_bindIp;
                g_allowListEnabled = allowEnabled;
                g_allowList        = allowList;

                SaveSettings();
                if (needRestart) RestartHttpServer();

                // Обновляем строку статуса
                wchar_t statusBuf[128];
                const wchar_t* bstr = (g_bindMode == 0) ? L"127.0.0.1"
                    : (g_bindMode == 2 && !g_bindIp.empty()) ? g_bindIp.c_str() : L"0.0.0.0";
                wsprintfW(statusBuf, L"Server: %s:%d", bstr, g_port);
                SetDlgItemTextW(s_frameHwnd, IDC_STATUS_LABEL, statusBuf);
            }
            break;

        case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_RESET:
            if (s_frameHwnd) {
                SetDlgItemTextW(s_frameHwnd, IDC_PORT_EDIT, L"19122");
                SendDlgItemMessageW(s_frameHwnd, IDC_BIND_COMBO, CB_SETCURSEL, 1, 0);  // All interfaces
                SendDlgItemMessageW(s_frameHwnd, IDC_ALLOW_CHECK, BM_SETCHECK, BST_UNCHECKED, 0);
                SetDlgItemTextW(s_frameHwnd, IDC_ALLOW_EDIT, L"");
                s_bindMode     = 1;
                s_bindIp       = L"";
                s_allowEnabled = false;
                UpdateFrameControls();
            }
            break;
        }
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
// Плагин AIMP — реализует IAIMPPlugin + IAIMPOptionsDialogFrame на одном объекте
// ==========================================
class HttpControlPlugin : public IAIMPPlugin, public IAIMPOptionsDialogFrame {
    LONG ref_ = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IAIMPPlugin*>(this); AddRef(); return S_OK;
        }
        if (riid == IID_IAIMPOptionsDialogFrame) {
            *ppv = static_cast<IAIMPOptionsDialogFrame*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef()  override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref_); if (r==0) delete this; return r; }

    // IAIMPPlugin
    TChar* WINAPI InfoGet(int Index) override {
        static wchar_t n[] = L"AIMP HTTP Remote Control";
        static wchar_t a[] = L"SLV Tech";
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
        IAIMPServicePlayer* player = nullptr;
        if (core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) != S_OK || !player)
            return E_FAIL;
        player->Release();

        g_core = core;
        LoadSettings();

        // Регистрируем себя как страницу настроек.
        // Первый аргумент — IID_IAIMPServiceOptionsDialog (не IID_IAIMPOptionsDialogFrame).
        core->RegisterExtension(IID_IAIMPServiceOptionsDialog,
            static_cast<IAIMPOptionsDialogFrame*>(this));

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
        FinalizeFocusSync();
        g_core = nullptr;
        return S_OK;
    }

    // IAIMPOptionsDialogFrame
    HRESULT WINAPI GetName(IAIMPString** s) override {
        if (!s) return E_POINTER;
        IAIMPString* str = nullptr;
        if (g_core && g_core->CreateObject(IID_IAIMPString, (void**)&str) == S_OK && str) {
            str->SetData(const_cast<wchar_t*>(L"HTTP Remote Control"), 19);
            *s = str;
            return S_OK;
        }
        return E_FAIL;
    }

    HWND WINAPI CreateFrame(HWND parentWnd) override {
        return frame_.CreateFrame(parentWnd);
    }

    void WINAPI DestroyFrame() override {
        frame_.DestroyFrame();
    }

    void WINAPI Notification(int id) override {
        frame_.Notification(id);
    }

private:
    HttpControlOptionsFrame frame_{nullptr};
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
