// plugin.cpp
#include <windows.h>
#include <thread>
#include <mutex>
#include <string>
#include <memory>
#include <algorithm>

// Header-only библиотеки
#include "third_party/httplib.h"
#include "third_party/json.hpp"

// AIMP SDK
#include "sdk/apiPlugin.h"
#include "sdk/apiObjects.h"
#include "sdk/apiCore.h"

using json = nlohmann::json;

// Глобальные переменные
IAIMPCore* g_core = nullptr;
std::unique_ptr<httplib::Server> g_httpServer;
std::thread g_serverThread;
std::mutex g_mutex;
int g_port = 3553;
bool g_running = false;

// Вспомогательные функции для работы с AIMP API
json GetCurrentTrackInfo() {
    json result;
    
    IAIMPServicePlayer* player = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) != S_OK)
        return {{"error", "Cannot access player"}};
    
    IAIMPPlaylist* playlist = nullptr;
    IAIMPPlaylistItem* item = nullptr;
    
    if (player->GetCurrentPlaylist(&playlist) == S_OK && playlist) {
        if (playlist->GetCurrentObject(&item) == S_OK && item) {
            IAIMPString* str = nullptr;
            double duration = 0;
            double position = 0;
            float volume = 0;
            int state = 0;
            
            if (item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPERTYID_FILENAME, 
                                       IID_IAIMPString, (void**)&str) == S_OK) {
                result["file"] = str->GetData();
                str->Release();
            }
            
            item->Release();
        }
        playlist->Release();
    }
    
    if (player->GetPosition(&position) == S_OK)
        result["position"] = position;
    if (player->GetVolume(&volume) == S_OK)
        result["volume"] = volume;
    
    state = player->GetState();
    result["state"] = (state == 1) ? "playing" : (state == 2) ? "paused" : "stopped";
    
    player->Release();
    return result;
}

json GetPlaylists() {
    json result = json::array();
    return result;  // Заглушка, можно дописать позже
}

json GetPlaylistTracks(int playlistId) {
    json result = json::array();
    return result;  // Заглушка
}

void RunHttpServer() {
    g_httpServer = std::make_unique<httplib::Server>();
    
    // GET /api/current
    g_httpServer->Get("/api/current", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        json j = GetCurrentTrackInfo();
        res.set_content(j.dump(), "application/json");
    });
    
    // GET /api/playlists
    g_httpServer->Get("/api/playlists", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        json j = GetPlaylists();
        res.set_content(j.dump(), "application/json");
    });
    
    // GET /api/playlist/{id}
    g_httpServer->Get(R"(/api/playlist/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        int id = std::stoi(req.matches[1]);
        json j = GetPlaylistTracks(id);
        res.set_content(j.dump(), "application/json");
    });
    
    // POST /api/player/playpause
    g_httpServer->Post("/api/player/playpause", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK) {
            int state = player->GetState();
            if (state == 1) { // playing
                player->Pause();
            } else {
                player->Resume();
            }
            player->Release();
        }
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    // POST /api/player/next
    g_httpServer->Post("/api/player/next", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK) {
            player->Next();
            player->Release();
        }
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    // POST /api/player/prev
    g_httpServer->Post("/api/player/prev", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK) {
            player->Prev();
            player->Release();
        }
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    // POST /api/player/volume
    g_httpServer->Post("/api/player/volume", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        
        json body = json::parse(req.body);
        float volume = std::max(0.0f, std::min(1.0f, body.value("volume", 0.5f)));
        
        IAIMPServicePlayer* player = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player) == S_OK) {
            player->SetVolume(volume);
            player->Release();
        }
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    g_running = true;
    g_httpServer->listen("0.0.0.0", g_port);
}

// Реализация интерфейса плагина
class HttpControlPlugin : public IAIMPPlugin {
private:
    int refCount = 1;
    
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IAIMPPlugin) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    
    ULONG WINAPI AddRef() override { return ++refCount; }
    
    ULONG WINAPI Release() override {
        int ref = --refCount;
        if (ref == 0) delete this;
        return ref;
    }
    
    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        g_core = core;
        g_serverThread = std::thread(RunHttpServer);
        g_serverThread.detach();
        return S_OK;
    }
    
    HRESULT WINAPI Finalize() override {
        if (g_httpServer) g_httpServer->stop();
        g_core = nullptr;
        return S_OK;
    }
    
    HRESULT WINAPI SystemNotification(int notify, void* data, HRESULT* result) override {
        return S_OK;
    }
    
    HRESULT WINAPI GetInfo(IAIMPPluginInfo** info) override {
        static IAIMPPluginInfo pluginInfo = {0};
        static wchar_t name[] = L"AIMP HTTP Control API";
        static wchar_t author[] = L"DebianDev";
        static wchar_t desc[] = L"REST API on port 3553";
        
        pluginInfo.StructSize = sizeof(IAIMPPluginInfo);
        pluginInfo.PluginId = {0x8F3E1A2B, 0x4C5D, 0x6E7F, {0x80, 0x91, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7}};
        pluginInfo.PluginName = name;
        pluginInfo.Author = author;
        pluginInfo.ShortDescription = desc;
        
        *info = &pluginInfo;
        return S_OK;
    }
};

// Точка входа
extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    static HttpControlPlugin* plugin = nullptr;
    static std::once_flag flag;
    
    std::call_once(flag, []() {
        plugin = new HttpControlPlugin();
    });
    
    *header = plugin;
    return S_OK;
}

// DllMain для thread attach/detach
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
