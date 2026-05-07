// plugin_minimal.cpp
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <thread>
#include <mutex>
#include <string>
#include <memory>

#include "third_party/json.hpp"

using json = nlohmann::json;

// AIMP SDK
#include "sdk/apiPlugin.h"

std::mutex g_mutex;
int g_port = 3553;
bool g_running = false;
std::thread g_serverThread;

// ==========================================
// Простейший HTTP-сервер на WinSock
// ==========================================
void SimpleHttpServer() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    
    // Разрешаем переиспользование порта
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    listen(serverSocket, SOMAXCONN);
    g_running = true;
    
    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        
        timeval timeout{1, 0};
        int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
        
        if (selectResult <= 0) continue;
        
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) continue;
        
        char buffer[4096];
        int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::string request(buffer);
            
            json response;
            
            // Маршрутизация запросов
            if (request.find("GET /api/current") != std::string::npos) {
                response["status"] = "ok";
                response["track"] = "AIMP HTTP Plugin Active";
                response["version"] = "0.2-SDKv5.40";
                response["timestamp"] = time(nullptr);
            } else if (request.find("GET /api/playlists") != std::string::npos) {
                response["playlists"] = json::array();
                response["count"] = 0;
            } else if (request.find("POST /api/player/playpause") != std::string::npos) {
                response["status"] = "ok";
                response["action"] = "playpause";
            } else if (request.find("POST /api/player/next") != std::string::npos) {
                response["status"] = "ok";
                response["action"] = "next";
            } else if (request.find("POST /api/player/prev") != std::string::npos) {
                response["status"] = "ok";
                response["action"] = "prev";
            } else if (request.find("POST /api/player/volume") != std::string::npos) {
                response["status"] = "ok";
                response["action"] = "volume";
            } else if (request.find("OPTIONS") != std::string::npos) {
                // CORS preflight
                std::string corsResponse = 
                    "HTTP/1.1 200 OK\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send(clientSocket, corsResponse.c_str(), corsResponse.length(), 0);
                closesocket(clientSocket);
                continue;
            } else if (request.find("GET /") != std::string::npos) {
                // Корневой эндпоинт — список API
                response["name"] = "AIMP HTTP Control API";
                response["version"] = "0.2";
                response["endpoints"] = {
                    {{"method", "GET"}, {"path", "/api/current"}, {"desc", "Current track info"}},
                    {{"method", "GET"}, {"path", "/api/playlists"}, {"desc", "List of playlists"}},
                    {{"method", "POST"}, {"path", "/api/player/playpause"}, {"desc", "Play/Pause"}},
                    {{"method", "POST"}, {"path", "/api/player/next"}, {"desc", "Next track"}},
                    {{"method", "POST"}, {"path", "/api/player/prev"}, {"desc", "Previous track"}},
                    {{"method", "POST"}, {"path", "/api/player/volume"}, {"desc", "Set volume (body: {\"volume\": 0.5})"}}
                };
            } else {
                response["error"] = "Not found";
                response["code"] = 404;
            }
            
            std::string jsonStr = response.dump();
            std::string httpResponse = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: " + std::to_string(jsonStr.length()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + jsonStr;
            
            send(clientSocket, httpResponse.c_str(), httpResponse.length(), 0);
        }
        
        closesocket(clientSocket);
    }
    
    closesocket(serverSocket);
    WSACleanup();
}

// ==========================================
// Реализация плагина AIMP
// ==========================================
class HttpControlPlugin : public IAIMPPlugin {
private:
    LONG refCount;
    
public:
    HttpControlPlugin() : refCount(1) {}
    virtual ~HttpControlPlugin() = default;
    
    // --- IUnknown ---
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        
        // В SDK 5.40 IAIMPPlugin = IUnknown, других интерфейсов нет
        if (IsEqualGUID(riid, IID_IUnknown)) {
            *ppv = static_cast<IAIMPPlugin*>(this);
            AddRef();
            return S_OK;
        }
        
        // Поддержка внешних настроек (опционально)
        if (IsEqualGUID(riid, IID_IAIMPExternalSettingsDialog)) {
            // Пока не реализовано
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        
        *ppv = nullptr;
        return E_NOINTERFACE;
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
    
    // --- IAIMPPlugin ---
    virtual TChar* WINAPI InfoGet(int Index) override {
        static wchar_t name[] = L"AIMP HTTP Control API";
        static wchar_t author[] = L"DebianDev";
        static wchar_t shortDesc[] = L"REST API server on port 3553";
        static wchar_t fullDesc[] = L"Provides HTTP API for remote control of AIMP player";
        
        switch (Index) {
            case AIMP_PLUGIN_INFO_NAME:              return name;
            case AIMP_PLUGIN_INFO_AUTHOR:            return author;
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return shortDesc;
            case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:  return fullDesc;
            default: return nullptr;
        }
    }
    
    virtual LongWord WINAPI InfoGetCategories() override {
        return AIMP_PLUGIN_CATEGORY_ADDONS;
    }
    
    virtual void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override {
        // Можно обрабатывать: AIMP_SYSTEM_NOTIFICATION_SERVICE_ADDED и т.д.
    }
    
    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        g_serverThread = std::thread(SimpleHttpServer);
        g_serverThread.detach();
        return S_OK;
    }
    
    HRESULT WINAPI Finalize() override {
        g_running = false;
        Sleep(100);
        return S_OK;
    }
};

// ==========================================
// Экспорт точки входа для AIMP
// ==========================================
extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = new HttpControlPlugin();
    return S_OK;
}

// ==========================================
// DllMain
// ==========================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
