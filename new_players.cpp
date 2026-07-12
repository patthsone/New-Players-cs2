#include <stdio.h>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "new_players.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "steam/isteamhttp.h"
#include "steam/steam_api.h"

New_Players g_New_Players;
PLUGIN_EXPOSE(New_Players, g_New_Players);

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IMySQLClient* g_pMysqlClient = nullptr;
IMySQLConnection* g_pConnection = nullptr;

std::string g_sWebhook;
std::string g_sBotToken;
std::string g_sChannelId;
std::string g_sSteamApiKey;
std::string g_sFooter = "LUXECS2.RU • NewPlayers";
int g_iEmbedColor = 0x5865F2;

bool g_bDBConnected = false;

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    gpGlobals = g_pUtils->GetCGlobalVars();
}

static void UnloadSelf(const char* reason)
{
    ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), reason);
    std::string command = "meta unload " + std::to_string(g_PLID);
    engine->ServerCommand(command.c_str());
}

static bool LoadDiscordConfig()
{
    KeyValues* config = new KeyValues("NewPlayersDiscord");
    if (!config->LoadFromFile(g_pFullFileSystem, "addons/configs/new_players/discord.ini"))
    {
        delete config;
        g_pUtils->ErrorLog("[%s] Failed to load addons/configs/new_players/discord.ini", g_PLAPI->GetLogTag());
        return false;
    }

    g_sWebhook = config->GetString("webhook", "");
    g_sBotToken = config->GetString("bot_token", "");
    g_sChannelId = config->GetString("channel_id", "");
    g_sSteamApiKey = config->GetString("steam_api_key", "");
    g_sFooter = config->GetString("footer", "LUXECS2.RU • NewPlayers");
    g_iEmbedColor = config->GetInt("embed_color", 5793266);
    delete config;

    if (g_sWebhook.empty() && (g_sBotToken.empty() || g_sChannelId.empty()))
    {
        g_pUtils->ErrorLog("[%s] Discord config is incomplete: set webhook or bot_token + channel_id", g_PLAPI->GetLogTag());
        return false;
    }

    return true;
}

static std::string GetCurrentServerName()
{
    if (!g_pCVar)
        return "CS2 Server";

    ConVarRefAbstract hostname = g_pCVar->FindConVar("hostname");
    if (!hostname.IsValidRef())
        return "CS2 Server";

    const char* value = hostname.GetString();
    if (!value || value[0] == '\0')
        return "CS2 Server";

    return value;
}

static void LoadDatabase()
{
    KeyValues* pKVConfig = new KeyValues("Databases");
    char error[64];
    if (!pKVConfig->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg")) {
        V_strncpy(error, "Failed to load databases config 'addons/config/databases.cfg'", sizeof(error));
        g_pUtils->ErrorLog("[%s] %s", g_PLAPI->GetLogTag(), error);
        return;
    }

    pKVConfig = pKVConfig->FindKey("new_players", false);
    if (!pKVConfig) {
        g_SMAPI->Format(error, sizeof(error), "No databases.cfg 'new_players'");
        g_pUtils->ErrorLog("[%s] %s", g_PLAPI->GetLogTag(), error);
        return;
    }

    MySQLConnectionInfo info;
    info.host = pKVConfig->GetString("host", nullptr);
    info.user = pKVConfig->GetString("user", nullptr);
    info.pass = pKVConfig->GetString("pass", nullptr);
    info.database = pKVConfig->GetString("database", nullptr);
    info.port = pKVConfig->GetInt("port");
    g_pConnection = g_pMysqlClient->CreateMySQLConnection(info);

    g_pConnection->Connect([](bool connect) {
        if (!connect) {
            META_CONPRINT("[New_Players] Failed to connect to MySQL\n");
            g_bDBConnected = false;
        } else {
            META_CONPRINT("[New_Players] MySQL connected successfully\n");
            char szQuery[1024];
            g_SMAPI->Format(szQuery, sizeof(szQuery), "CREATE TABLE IF NOT EXISTS new_players (\
                                                            id INT PRIMARY KEY AUTO_INCREMENT,\
                                                            steamid VARCHAR(32) UNIQUE,\
                                                            connect INT\
                                                        );");
            g_pConnection->Query(szQuery, [](ISQLQuery *pQuery) {
                META_CONPRINT("[New_Players] Table creation query executed\n");
                g_bDBConnected = true;
            });
        }
    });
}

static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string ExtractJsonValue(const std::string& json, const std::string& key)
{
    std::string sk = "\"" + key + "\":";
    size_t pos = json.find(sk);
    if (pos == std::string::npos) return "";
    pos += sk.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.length() && json[pos] == '"') {
        pos++;
        std::string out;
        while (pos < json.length()) {
            char c = json[pos++];
            if (c == '\\' && pos < json.length()) { out += json[pos]; ++pos; continue; }
            if (c == '"') break;
            out += c;
        }
        return out;
    }
    return "";
}

class HttpJob {
public:
    using DoneCb = std::function<void(bool ok, const std::string& body)>;

    HttpJob(EHTTPMethod method, std::string url, std::string body, DoneCb cb = {})
        : m_method(method), m_url(std::move(url)),
          m_body(std::move(body)), m_cb(std::move(cb)) {}

    void SetHeader(const std::string& k, const std::string& v) {
        m_headers.emplace_back(k, v);
    }

    bool Start()
    {
        ISteamHTTP* http = SteamGameServerHTTP();
        if (!http) return false;
        m_http = http;

        HTTPRequestHandle req = m_http->CreateHTTPRequest(m_method, m_url.c_str());
        if (!req) return false;
        m_req = req;

        m_http->SetHTTPRequestHeaderValue(req, "Accept", "application/json");
        m_http->SetHTTPRequestHeaderValue(req, "Content-Type", "application/json");
        for (auto& h : m_headers) {
            m_http->SetHTTPRequestHeaderValue(req, h.first.c_str(), h.second.c_str());
        }
        if (!m_body.empty()) {
            m_http->SetHTTPRequestRawPostBody(req, "application/json",
                (uint8*)m_body.data(), (uint32)m_body.size());
        }

        SteamAPICall_t hCall{};
        if (!m_http->SendHTTPRequest(req, &hCall)) {
            m_http->ReleaseHTTPRequest(req); m_req = 0;
            return false;
        }
        m_call.SetGameserverFlag();
        m_call.Set(hCall, this, &HttpJob::OnDone);
        return true;
    }

    void OnDone(HTTPRequestCompleted_t* p, bool bFailed)
    {
        std::string body;
        bool ok = !bFailed && p && p->m_eStatusCode >= 200 && p->m_eStatusCode < 300;
        if (m_http && p) {
            uint32 size = 0;
            m_http->GetHTTPResponseBodySize(p->m_hRequest, &size);
            if (size > 0) {
                std::vector<uint8> buf(size + 1, 0);
                m_http->GetHTTPResponseBodyData(p->m_hRequest, buf.data(), size);
                body.assign((char*)buf.data(), size);
            }
        }
        if (m_http && m_req) m_http->ReleaseHTTPRequest(m_req);
        if (m_cb) m_cb(ok, body);
        delete this;
    }

private:
    CCallResult<HttpJob, HTTPRequestCompleted_t> m_call;
    ISteamHTTP* m_http = nullptr;
    HTTPRequestHandle m_req = 0;
    EHTTPMethod m_method;
    std::string m_url, m_body;
    DoneCb m_cb;
    std::vector<std::pair<std::string, std::string>> m_headers;
};

static void FetchSteamAvatar(uint64_t sid, std::function<void(std::string avatarUrl)> cb)
{
    if (!cb) return;
    if (g_sSteamApiKey.empty()) { cb(""); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key=%s&steamids=%llu",
        g_sSteamApiKey.c_str(), (unsigned long long)sid);

    auto* job = new HttpJob(k_EHTTPMethodGET, url, "",
        [cb](bool ok, const std::string& body) {
            if (!ok || body.empty()) { cb(""); return; }
            cb(ExtractJsonValue(body, "avatarfull"));
        });
    if (!job->Start()) { delete job; cb(""); }
}

static void SendDiscordConnectWithAvatar(int slot, uint64 steamId64, const std::string& avatarUrl)
{
    if (steamId64 == 0) return;

    const char* ip = g_pPlayers ? g_pPlayers->GetIpAddress(slot) : nullptr;
    std::string steamId = std::to_string(steamId64);
    std::string profileUrl = "https://steamcommunity.com/profiles/" + steamId;
    std::string slotText = std::to_string(slot + 1);
    std::string ipText = (ip && ip[0] != '\0') ? ip : "Не определён";
    const std::string serverName = GetCurrentServerName();

    std::string json = "{";
    json += "\"embeds\": [{";
    json += "\"title\": \"Игрок подключился к серверу\",";
    json += "\"description\": \"" + EscapeJson(serverName) + "\",";
    json += "\"url\": \"" + profileUrl + "\",";
    json += "\"color\": " + std::to_string(g_iEmbedColor) + ",";

    if (!avatarUrl.empty()) {
        json += "\"thumbnail\": {\"url\": \"" + avatarUrl + "\"},";
    }

    json += "\"fields\": [";
    json += "{\"name\": \"SteamID64\", \"value\": \"" + steamId + "\", \"inline\": true},";
    json += "{\"name\": \"Слот\", \"value\": \"" + slotText + "\", \"inline\": true},";
    json += "{\"name\": \"IP-адрес\", \"value\": \"" + EscapeJson(ipText) + "\", \"inline\": false},";
    json += "{\"name\": \"Steam-профиль\", \"value\": \"[Открыть профиль](" + profileUrl + ")\", \"inline\": false}";
    json += "],";
    json += "\"footer\": {\"text\": \"" + EscapeJson(g_sFooter) + "\"}";
    json += "}]";
    json += "}";

    if (!g_sWebhook.empty()) {
        auto* job = new HttpJob(k_EHTTPMethodPOST, g_sWebhook, json);
        if (!job->Start()) delete job;
    } else if (!g_sBotToken.empty() && !g_sChannelId.empty()) {
        std::string url = "https://discord.com/api/channels/" + g_sChannelId + "/messages";
        auto* job = new HttpJob(k_EHTTPMethodPOST, url, json);
        job->SetHeader("Authorization", "Bot " + g_sBotToken);
        if (!job->Start()) delete job;
    }
}

static void SendDiscordConnect(int slot, uint64 steamId64)
{
    if (steamId64 == 0) return;

    FetchSteamAvatar(steamId64, [slot, steamId64](std::string avatarUrl) {
        SendDiscordConnectWithAvatar(slot, steamId64, avatarUrl);
    });
}

static void OnClientAuthorized(int iSlot, uint64 iSteamID64)
{
    if (g_pPlayers && g_pPlayers->IsFakeClient(iSlot)) return;

    if (g_pConnection && g_bDBConnected)
    {
        char szQuery[1024];
        g_SMAPI->Format(szQuery, sizeof(szQuery), 
            "INSERT INTO new_players (steamid, connect) VALUES ('%llu', %lld) ON DUPLICATE KEY UPDATE connect = VALUES(connect)", 
            static_cast<unsigned long long>(iSteamID64), 
            static_cast<long long>(std::time(0))
        );
        g_pConnection->Query(szQuery, [](ISQLQuery *pQuery) {});
    }

    SendDiscordConnect(iSlot, iSteamID64);
}

bool New_Players::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);

    g_SMAPI->AddListener(this, this);
    return true;
}

bool New_Players::Unload(char* error, size_t maxlen)
{
    if (g_pUtils)
        g_pUtils->ClearAllHooks(g_PLID);
    g_bDBConnected = false;
    ConVar_Unregister();
    return true;
}

void New_Players::AllPluginsLoaded()
{
    char error[64];
    int ret;
    g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string sBuffer = "meta unload "+std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
        return;
    }
    
    g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string sBuffer = "meta unload "+std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
        return;
    }
    
    ISQLInterface* g_SqlInterface = (ISQLInterface *)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        g_pUtils->ErrorLog("[%s] Missing MYSQL plugin", g_PLAPI->GetLogTag());
        std::string sBuffer = "meta unload "+std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
        return;
    }
    g_pMysqlClient = g_SqlInterface->GetMySQLClient();

    if (!LoadDiscordConfig())
    {
        UnloadSelf("Discord configuration is missing or incomplete");
        return;
    }
    
    LoadDatabase();
    g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuthorized);

    ConColorMsg(Color(87, 242, 135, 255), "[%s] Discord notifications enabled via HTTP\n", GetLogTag());
}

const char* New_Players::GetLicense() { return "GPL"; }
const char* New_Players::GetVersion() { return "1.0.1"; }
const char* New_Players::GetDate() { return __DATE__; }
const char* New_Players::GetLogTag() { return "New_Players"; }
const char* New_Players::GetAuthor() { return "Pisex && PattHs"; }
const char* New_Players::GetDescription() { return "Stores new players and sends every connection to Discord"; }
const char* New_Players::GetName() { return "First Connect Players"; }
const char* New_Players::GetURL() { return "https://luxecs2.ru/"; }