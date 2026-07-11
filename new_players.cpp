#include <stdio.h>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "new_players.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

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
IDiscordWebhookApi* g_pDiscordWebhook = nullptr;
IDiscordBotApi* g_pDiscordBotApi = nullptr;

std::string g_sWebhook;
std::string g_sBotToken;
std::string g_sChannelId;
std::string g_sFooter = "LUXECS2.RU • NewPlayers";
int g_iEmbedColor = 0x5865F2;
std::unique_ptr<DiscordBot> g_pDiscordBot;

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
    g_sFooter = config->GetString("footer", "LUXECS2.RU • NewPlayers");
    g_iEmbedColor = config->GetInt("embed_color", 5793266);
    delete config;

    if (g_sWebhook.empty() && (g_sBotToken.empty() || g_sChannelId.empty()))
    {
        g_pUtils->ErrorLog("[%s] Discord config is incomplete: set webhook or bot_token + channel_id", g_PLAPI->GetLogTag());
        return false;
    }

    if (!g_sBotToken.empty() && !g_sChannelId.empty())
        g_pDiscordBot = std::make_unique<DiscordBot>(g_sBotToken);

    return true;
}

static void LoadDatabase()
{
    KeyValues* config = new KeyValues("Databases");
    if (!config->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg"))
    {
        delete config;
        g_pUtils->ErrorLog("[%s] Failed to load addons/configs/databases.cfg", g_PLAPI->GetLogTag());
        return;
    }

    KeyValues* database = config->FindKey("new_players", false);
    if (!database)
    {
        delete config;
        g_pUtils->ErrorLog("[%s] Section new_players was not found in databases.cfg", g_PLAPI->GetLogTag());
        return;
    }

    MySQLConnectionInfo info;
    info.host = database->GetString("host", "127.0.0.1");
    info.user = database->GetString("user", "root");
    info.pass = database->GetString("pass", "");
    info.database = database->GetString("database", "");
    info.port = database->GetInt("port", 3306);
    g_pConnection = g_pMysqlClient->CreateMySQLConnection(info);
    delete config;

    g_pConnection->Connect([](bool connected)
    {
        if (!connected)
        {
            META_CONPRINT("[New_Players] Failed to connect to MySQL\n");
            return;
        }

        const char* query =
            "CREATE TABLE IF NOT EXISTS new_players ("
            "id INT PRIMARY KEY AUTO_INCREMENT,"
            "steamid VARCHAR(32) UNIQUE,"
            "connect INT"
            ");";
        g_pConnection->Query(query, [](ISQLQuery* result) {});
    });
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

static void SendDiscordConnect(int slot, uint64 steamId64)
{
    if ((!g_pDiscordWebhook && !g_pDiscordBotApi) || steamId64 == 0)
        return;

    const char* ip = g_pPlayers ? g_pPlayers->GetIpAddress(slot) : nullptr;
    std::string steamId = std::to_string(steamId64);
    std::string profileUrl = "https://steamcommunity.com/profiles/" + steamId;
    std::string slotText = std::to_string(slot + 1);
    std::string ipText = (ip && ip[0] != '\0') ? ip : "Не определён";

    Embed embed;
    embed.SetTitle("Игрок подключился к серверу");
    const std::string serverName = GetCurrentServerName();
    embed.SetDescription(serverName.c_str());
    embed.SetURL(profileUrl.c_str());
    embed.SetColor(g_iEmbedColor);
    embed.AddField("SteamID64", steamId, true);
    embed.AddField("Слот", slotText, true);
    embed.AddField("IP-адрес", ipText, false);
    embed.AddField("Steam-профиль", "[Открыть профиль](" + profileUrl + ")", false);
    embed.SetFooter(g_sFooter.c_str());

    std::vector<Embed*> embeds = { &embed };

    if (!g_sWebhook.empty() && g_pDiscordWebhook)
    {
        g_pDiscordWebhook->SendWebHook(g_sWebhook.c_str(), "", embeds);
        return;
    }

    if (g_pDiscordBotApi && g_pDiscordBot && !g_sChannelId.empty())
    {
        g_pDiscordBotApi->SendMessage(
            g_pDiscordBot.get(),
            g_sChannelId.c_str(),
            "",
            embeds,
            [](int statusCode, const char* response)
            {
                if (statusCode < 200 || statusCode >= 300)
                {
                    g_pUtils->ErrorLog(
                        "[%s] Discord send failed. HTTP %d: %s",
                        g_PLAPI->GetLogTag(),
                        statusCode,
                        response ? response : "empty response"
                    );
                }
            }
        );
    }
}

static void OnClientAuthorized(int slot, uint64 steamId64)
{
    if (g_pPlayers && g_pPlayers->IsFakeClient(slot))
        return;

    if (g_pConnection)
    {
        char query[512];
        g_SMAPI->Format(
            query,
            sizeof(query),
            "INSERT IGNORE INTO new_players (steamid, connect) VALUES ('%llu', %lld)",
            static_cast<unsigned long long>(steamId64),
            static_cast<long long>(std::time(nullptr))
        );
        g_pConnection->Query(query, [](ISQLQuery* result) {});
    }

    SendDiscordConnect(slot, steamId64);
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

    g_pDiscordBot.reset();
    ConVar_Unregister();
    return true;
}

void New_Players::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = static_cast<IUtilsApi*>(g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        UnloadSelf("Missing Utils system plugin");
        return;
    }

    g_pPlayers = static_cast<IPlayersApi*>(g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pPlayers)
    {
        UnloadSelf("Missing Players system plugin");
        return;
    }

    ISQLInterface* sqlInterface = static_cast<ISQLInterface*>(g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !sqlInterface)
    {
        UnloadSelf("Missing SQLMM/MySQL plugin");
        return;
    }
    g_pMysqlClient = sqlInterface->GetMySQLClient();

    g_pDiscordWebhook = static_cast<IDiscordWebhookApi*>(g_SMAPI->MetaFactory(DISCORD_WEBHOOK_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED)
        g_pDiscordWebhook = nullptr;

    g_pDiscordBotApi = static_cast<IDiscordBotApi*>(g_SMAPI->MetaFactory(DISCORD_BOT_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED)
        g_pDiscordBotApi = nullptr;

    if (!g_pDiscordWebhook && !g_pDiscordBotApi)
    {
        UnloadSelf("Missing Discord Core plugin");
        return;
    }

    if (!LoadDiscordConfig())
    {
        UnloadSelf("Discord configuration is missing or incomplete");
        return;
    }

    LoadDatabase();
    g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuthorized);

    ConColorMsg(Color(87, 242, 135, 255), "[%s] Discord notifications enabled\n", GetLogTag());
}

const char* New_Players::GetLicense() { return "GPL"; }
const char* New_Players::GetVersion() { return "1.0.0"; }
const char* New_Players::GetDate() { return __DATE__; }
const char* New_Players::GetLogTag() { return "New_Players"; }
const char* New_Players::GetAuthor() { return "Pisex && PattHs"; }
const char* New_Players::GetDescription() { return "Stores new players and sends every connection to Discord"; }
const char* New_Players::GetName() { return "First Connect Players"; }
const char* New_Players::GetURL() { return "https://luxecs2.ru/"; }
