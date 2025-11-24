#include <stdio.h>
#include "new_players.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

New_Players g_New_Players;
PLUGIN_EXPOSE(New_Players, g_New_Players);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;

IMySQLClient *g_pMysqlClient;
IMySQLConnection* g_pConnection;

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

bool New_Players::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	return true;
}

bool New_Players::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	
	return true;
}

void LoadDatabase()
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
			META_CONPRINT("Failed to connect the mysql database\n");
		} else {
			char szQuery[1024];
			g_SMAPI->Format(szQuery, sizeof(szQuery), "CREATE TABLE IF NOT EXISTS new_players (\
															id INT PRIMARY KEY AUTO_INCREMENT,\
															steamid VARCHAR(32) UNIQUE,\
															connect INT\
														);");
			g_pConnection->Query(szQuery, [](ISQLQuery *pQuery) {});
		}
	});
}

void OnClientAuthorized(int iSlot, uint64 iSteamID64)
{
	char szQuery[1024];
	g_SMAPI->Format(szQuery, sizeof(szQuery), "INSERT IGNORE INTO new_players (steamid, connect) VALUES ('%llu', %i)", iSteamID64, std::time(0));
	g_pConnection->Query(szQuery, [](ISQLQuery *pQuery) {});
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
	
	LoadDatabase();
	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuthorized);
}

///////////////////////////////////////
const char* New_Players::GetLicense()
{
	return "GPL";
}

const char* New_Players::GetVersion()
{
	return "1.1";
}

const char* New_Players::GetDate()
{
	return __DATE__;
}

const char *New_Players::GetLogTag()
{
	return "New_Players";
}

const char* New_Players::GetAuthor()
{
	return "PattHs";
}

const char* New_Players::GetDescription()
{
	return "New_Players";
}

const char* New_Players::GetName()
{
	return "First Connect Players";
}

const char* New_Players::GetURL()
{
	return "https://nova-hosting.ru?ref=TNC36I97";
}
