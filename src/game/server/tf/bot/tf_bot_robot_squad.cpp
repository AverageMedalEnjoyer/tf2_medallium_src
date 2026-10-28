//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_robot_squad.cpp
// Engineer robot squads: teleporter exits that summon a squad of TFBots
// for the engineer's team outside of Mann vs. Machine.
//
// Squads come from scripts/robot_squads.txt, plus scripts/robot_squads/<map>.txt
// for squads that suit a particular map. Members are written in the same syntax
// as popfile TFBot blocks. Engineers pick their squad from the robot factory menu
// (right-click with the build PDA). Like an MvM engineer's teleporter, each exit
// sends robots through one at a time until the squad is full, and refills a dead
// member's place after a delay, as long as the exit is standing and usable.

#include "cbase.h"
#include "tf_bot_robot_squad.h"
#include "tf_obj_teleporter.h"
#include "tf_player.h"
#include "tf_gamerules.h"
#include "tf_weapon_medigun.h"
#include "filesystem.h"
#include "player_vs_environment/tf_populator_spawners.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar tf_robot_squad_file( "tf_robot_squad_file", "scripts/robot_squads.txt", FCVAR_NONE, "Squad templates for Engineer robot squads. Maps add their own in scripts/robot_squads/<map>.txt." );
ConVar tf_robot_squad_respawn_time( "tf_robot_squad_respawn_time", "10", FCVAR_NOTIFY, "Seconds after a summoned robot dies before its place in the squad is refilled, when its squad doesn't say." );
ConVar tf_robot_squad_spawn_interval( "tf_robot_squad_spawn_interval", "2", FCVAR_NOTIFY, "Seconds a teleporter exit takes to send each robot through, when its squad doesn't say." );
ConVar tf_robot_squad_giant_spawn_time( "tf_robot_squad_giant_spawn_time", "8", FCVAR_NOTIFY, "Seconds a teleporter exit takes to send a giant through, when its squad doesn't say." );
ConVar tf_robot_squad_first_spawn_delay( "tf_robot_squad_first_spawn_delay", "5", FCVAR_NOTIFY, "Seconds after a teleporter exit is built before it starts sending robots through." );
ConVar tf_robot_squad_max_per_team( "tf_robot_squad_max_per_team", "10", FCVAR_NOTIFY, "Maximum number of summoned robots per team." );
ConVar tf_robot_squad_reserve_slots( "tf_robot_squad_reserve_slots", "2", FCVAR_NOTIFY, "Player slots kept free for humans. Robots won't be summoned into them." );
ConVar tf_robot_squad_spawn_uber( "tf_robot_squad_spawn_uber", "2", FCVAR_NOTIFY, "Seconds of spawn invulnerability for summoned robots." );
ConVar tf_robot_squad_stuck_time( "tf_robot_squad_stuck_time", "5", FCVAR_NOTIFY, "Seconds a summoned robot can be stuck on map geometry before it's sent back to its exit (0 disables)." );

extern ConVar tf_mvm_miniboss_scale;

// spawn time and respawn delay multiplier by teleporter level
static const float s_flLevelSpawnMult[] = { 1.0f, 0.8f, 0.6f };

static const char *s_pszRobotSquadOrderNames[ TF_ROBOT_SQUAD_ORDER_COUNT ] =
{
	"attack",
	"defend",
	"follow",
};

static const char *s_pszRobotSquadOrderMessages[ TF_ROBOT_SQUAD_ORDER_COUNT ] =
{
	"Robot squad: ATTACK",
	"Robot squad: DEFEND",
	"Robot squad: FOLLOW",
};

#define ROBOT_SQUAD_MAX_TEMPLATE_DEPTH	8
#define ROBOT_SQUAD_RETRY_INTERVAL		2.0f
#define ROBOT_SQUAD_MAP_FOLDER			"scripts/robot_squads"

// Snipers come back as skeletons during Halloween, if the model's installed
#define ROBOT_SQUAD_UNDEAD_SNIPER_MODEL	"models/bots/skeleton_sniper/skeleton_sniper_fixed.mdl"

#define ROBOT_SQUAD_MENU_PAGE_SIZE		7		// 8 and 9 page, 0 closes
#define ROBOT_SQUAD_MENU_TIME			20		// seconds the menu stays up
#define ROBOT_SQUAD_MENU_CHUNK			200		// characters per ShowMenu message


//----------------------------------------------------------------------------
static int CountConnectedPlayers( void )
{
	int count = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		if ( UTIL_PlayerByIndex( i ) )
		{
			++count;
		}
	}
	return count;
}


//----------------------------------------------------------------------------
static int CountSummonedRobots( int team )
{
	int count = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *player = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( player && player->GetTeamNumber() == team && player->GetSummonedType() != TF_SUMMONED_NONE )
		{
			++count;
		}
	}
	return count;
}


//----------------------------------------------------------------------------
// "<Engineer>'s Giant Heavy"
static void FormatMemberName( char *pszName, int nSize, CTFPlayer *pEngineer, const RobotSquadMemberDef_t &def )
{
	const char *pszMemberName = def.m_name.IsEmpty() ? g_aPlayerClassNames_NonLocalized[ def.m_class ] : def.m_name.Get();
	V_snprintf( pszName, nSize, "%s's %s", pEngineer->GetPlayerName(), pszMemberName );
}


//----------------------------------------------------------------------------
// What each engineer picked from the robot factory menu, by user id so it
// doesn't pass to whoever gets their player slot next
struct RobotSquadPlayerState_t
{
	RobotSquadPlayerState_t()
	{
		m_nMenuPage = 0;
		m_flMenuExpireTime = -1.0f;
	}

	CUtlString m_squadChoice;
	int m_nMenuPage;
	float m_flMenuExpireTime;		// < curtime when our menu isn't up
};

static CUtlMap< int, RobotSquadPlayerState_t > s_robotSquadPlayerStates( DefLessFunc( int ) );

static RobotSquadPlayerState_t &GetPlayerState( CTFPlayer *pPlayer )
{
	int userID = pPlayer->GetUserID();

	unsigned short index = s_robotSquadPlayerStates.Find( userID );
	if ( index == s_robotSquadPlayerStates.InvalidIndex() )
	{
		index = s_robotSquadPlayerStates.Insert( userID );
	}

	return s_robotSquadPlayerStates[ index ];
}


//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
static CTFRobotSquadTemplates s_robotSquadTemplates;

CTFRobotSquadTemplates &TheRobotSquadTemplates( void )
{
	return s_robotSquadTemplates;
}


//----------------------------------------------------------------------------
CTFRobotSquadTemplates::CTFRobotSquadTemplates() : CAutoGameSystem( "CTFRobotSquadTemplates" )
{
	m_bLoaded = false;
	m_nGeneration = 0;
}


//----------------------------------------------------------------------------
CTFRobotSquadTemplates::~CTFRobotSquadTemplates()
{
	Clear();
}


//----------------------------------------------------------------------------
void CTFRobotSquadTemplates::LevelInitPreEntity( void )
{
	// reloaded on first use, so edits to the files apply on the next map
	Clear();
}


//----------------------------------------------------------------------------
void CTFRobotSquadTemplates::LevelShutdownPostEntity( void )
{
	Clear();
}


//----------------------------------------------------------------------------
void CTFRobotSquadTemplates::Clear( void )
{
	m_squads.PurgeAndDeleteElements();
	m_loadouts.PurgeAndDeleteElements();
	m_bLoaded = false;
	++m_nGeneration;
}


//----------------------------------------------------------------------------
void CTFRobotSquadTemplates::EnsureLoaded( void )
{
	if ( !m_bLoaded )
	{
		Load();
	}
}


//----------------------------------------------------------------------------
// Load the global squads, then the map's own. Map templates and squads replace
// global ones with the same name, and a map can drop the global squads entirely.
bool CTFRobotSquadTemplates::Load( void )
{
	Clear();

	// don't retry every think if the files are bad
	m_bLoaded = true;

	const char *pszFile = tf_robot_squad_file.GetString();

	KeyValues *pGlobalKV = new KeyValues( "RobotSquads" );
	bool bHaveGlobal = pGlobalKV->LoadFromFile( g_pFullFileSystem, pszFile, "GAME" );
	if ( !bHaveGlobal )
	{
		Warning( "Robot squads: Unable to load '%s'\n", pszFile );
	}

	// workshop maps look like "workshop/cp_foo.ugc1234"
	char szMapName[ MAX_PATH ];
	V_FileBase( STRING( gpGlobals->mapname ), szMapName, sizeof( szMapName ) );

	char szMapFile[ MAX_PATH ];
	V_sprintf_safe( szMapFile, "%s/%s.txt", ROBOT_SQUAD_MAP_FOLDER, szMapName );

	KeyValues *pMapKV = new KeyValues( "RobotSquads" );
	bool bHaveMap = g_pFullFileSystem->FileExists( szMapFile, "GAME" );
	if ( bHaveMap && !pMapKV->LoadFromFile( g_pFullFileSystem, szMapFile, "GAME" ) )
	{
		Warning( "Robot squads: Unable to load '%s'\n", szMapFile );
		bHaveMap = false;
	}

	// one set of templates for both files - the map's win
	KeyValues *pTemplatesKV = new KeyValues( "Templates" );
	KeyValues *pSources[] = { bHaveGlobal ? pGlobalKV : NULL, bHaveMap ? pMapKV : NULL };
	for ( int s = 0; s < ARRAYSIZE( pSources ); ++s )
	{
		KeyValues *pSourceTemplatesKV = pSources[s] ? pSources[s]->FindKey( "Templates" ) : NULL;
		if ( !pSourceTemplatesKV )
			continue;

		FOR_EACH_TRUE_SUBKEY( pSourceTemplatesKV, pTemplateKV )
		{
			KeyValues *pOldKV = pTemplatesKV->FindKey( pTemplateKV->GetName() );
			if ( pOldKV )
			{
				pTemplatesKV->RemoveSubKey( pOldKV );
				pOldKV->deleteThis();
			}

			pTemplatesKV->AddSubKey( pTemplateKV->MakeCopy() );
		}
	}

	bool bReplaceGlobal = bHaveMap && pMapKV->GetBool( "ReplaceGlobalSquads" );

	if ( bHaveGlobal && !bReplaceGlobal )
	{
		ParseFile( pGlobalKV, pTemplatesKV, pszFile );
	}

	if ( bHaveMap )
	{
		ParseFile( pMapKV, pTemplatesKV, szMapFile );
	}

	pTemplatesKV->deleteThis();
	pMapKV->deleteThis();
	pGlobalKV->deleteThis();

	if ( bHaveMap )
	{
		DevMsg( "Robot squads: Loaded %d squads from '%s' and '%s'\n", m_squads.Count(), bReplaceGlobal ? "(none)" : pszFile, szMapFile );
	}
	else
	{
		DevMsg( "Robot squads: Loaded %d squads from '%s'\n", m_squads.Count(), pszFile );
	}

	return m_squads.Count() > 0;
}


//----------------------------------------------------------------------------
bool CTFRobotSquadTemplates::ParseFile( KeyValues *pFileKV, KeyValues *pTemplatesKV, const char *pszFile )
{
	bool bOk = true;

	FOR_EACH_TRUE_SUBKEY( pFileKV, pSquadKV )
	{
		if ( !V_stricmp( pSquadKV->GetName(), "Squad" ) )
		{
			RobotSquadDef_t *pSquad = new RobotSquadDef_t;
			if ( !ParseSquad( pSquadKV, pTemplatesKV, pSquad ) )
			{
				Warning( "Robot squads: Skipping a bad squad in '%s'\n", pszFile );
				delete pSquad;
				bOk = false;
				continue;
			}

			// a squad with the same name as an earlier one replaces it, in its place in the menu
			int iExisting = m_squads.InvalidIndex();
			FOR_EACH_VEC( m_squads, i )
			{
				if ( !V_stricmp( m_squads[i]->m_name.Get(), pSquad->m_name.Get() ) )
				{
					iExisting = i;
					break;
				}
			}

			if ( m_squads.IsValidIndex( iExisting ) )
			{
				delete m_squads[ iExisting ];
				m_squads[ iExisting ] = pSquad;
			}
			else
			{
				m_squads.AddToTail( pSquad );
			}
		}
		else if ( V_stricmp( pSquadKV->GetName(), "Templates" ) )
		{
			Warning( "Robot squads: Unknown block '%s' in '%s'\n", pSquadKV->GetName(), pszFile );
		}
	}

	return bOk;
}


//----------------------------------------------------------------------------
bool CTFRobotSquadTemplates::ParseSquad( KeyValues *pSquadKV, KeyValues *pTemplatesKV, RobotSquadDef_t *pSquad )
{
	FOR_EACH_SUBKEY( pSquadKV, pData )
	{
		const char *name = pData->GetName();
		const char *value = pData->GetString();

		if ( !V_stricmp( name, "Name" ) )
		{
			pSquad->m_name = value;
		}
		else if ( !V_stricmp( name, "DisplayName" ) )
		{
			pSquad->m_displayName = value;
		}
		else if ( !V_stricmp( name, "RespawnDelay" ) )
		{
			pSquad->m_respawnDelay = pData->GetFloat();
		}
		else if ( !V_stricmp( name, "SpawnInterval" ) )
		{
			pSquad->m_spawnInterval = pData->GetFloat();
		}
		else if ( !V_stricmp( name, "Wrench" ) )
		{
			// squads used to be picked by wrench - they come from the robot factory menu now
			DevMsg( "Robot squads: Ignoring Wrench in squad '%s'\n", pSquad->m_name.Get() );
		}
		else if ( !V_stricmp( name, "TFBot" ) )
		{
			RobotSquadMemberDef_t member;
			if ( !ParseMember( pData, pTemplatesKV, member, 0 ) )
			{
				Warning( "Robot squads: Bad TFBot in squad '%s'\n", pSquad->m_name.Get() );
				return false;
			}

			pSquad->m_members.AddToTail( member );
		}
		else
		{
			Warning( "Robot squads: Ignoring unknown field '%s' in squad '%s'\n", name, pSquad->m_name.Get() );
		}
	}

	if ( pSquad->m_name.IsEmpty() )
	{
		Warning( "Robot squads: Squad is missing a Name\n" );
		return false;
	}

	if ( pSquad->m_members.Count() == 0 )
	{
		Warning( "Robot squads: Squad '%s' has no TFBot members\n", pSquad->m_name.Get() );
		return false;
	}

	return true;
}


//----------------------------------------------------------------------------
// Popfile TFBot fields that only mean something in MvM, so templates pasted in
// from popfiles load without complaint
static bool IsIgnoredPopfileField( const char *name )
{
	static const char *s_pszIgnored[] =
	{
		"ClassIcon",
		"TeleportWhere",
		"AutoJumpMin",
		"AutoJumpMax",
		"EventChangeAttributes",
	};

	for ( int i = 0; i < ARRAYSIZE( s_pszIgnored ); ++i )
	{
		if ( !V_stricmp( name, s_pszIgnored[i] ) )
			return true;
	}

	return false;
}


//----------------------------------------------------------------------------
bool CTFRobotSquadTemplates::ParseMember( KeyValues *pMemberKV, KeyValues *pTemplatesKV, RobotSquadMemberDef_t &member, int nDepth )
{
	if ( nDepth > ROBOT_SQUAD_MAX_TEMPLATE_DEPTH )
	{
		Warning( "Robot squads: Templates nested too deeply (recursive Template?)\n" );
		return false;
	}

	// pull in the template first so our own keys override it
	KeyValues *pTemplate = pMemberKV->FindKey( "Template" );
	if ( pTemplate )
	{
		KeyValues *pTemplateKV = pTemplatesKV ? pTemplatesKV->FindKey( pTemplate->GetString() ) : NULL;
		if ( !pTemplateKV )
		{
			Warning( "Robot squads: Unknown Template '%s'\n", pTemplate->GetString() );
			return false;
		}

		if ( !ParseMember( pTemplateKV, pTemplatesKV, member, nDepth + 1 ) )
		{
			return false;
		}
	}

	FOR_EACH_SUBKEY( pMemberKV, pData )
	{
		const char *name = pData->GetName();
		const char *value = pData->GetString();

		if ( !V_stricmp( name, "Template" ) )
		{
			continue;
		}
		else if ( !V_stricmp( name, "Class" ) )
		{
			member.m_class = GetClassIndexFromString( value );
			if ( member.m_class == TF_CLASS_UNDEFINED )
			{
				Warning( "Robot squads: Invalid class '%s'\n", value );
				return false;
			}
		}
		else if ( !V_stricmp( name, "Name" ) )
		{
			member.m_name = value;
		}
		else if ( !V_stricmp( name, "Health" ) )
		{
			member.m_health = pData->GetInt();
		}
		else if ( !V_stricmp( name, "Scale" ) )
		{
			member.m_scale = pData->GetFloat();
		}
		else if ( !V_stricmp( name, "Count" ) )
		{
			member.m_count = MAX( 1, pData->GetInt() );
		}
		else if ( !V_stricmp( name, "MinLevel" ) )
		{
			member.m_minLevel = pData->GetInt();
		}
		else if ( !V_stricmp( name, "MaxLevel" ) )
		{
			member.m_maxLevel = pData->GetInt();
		}
		else if ( !V_stricmp( name, "SpawnTime" ) )
		{
			member.m_spawnTime = pData->GetFloat();
		}
		else if ( !V_stricmp( name, "Loadout" ) )
		{
			const RobotSquadLoadoutDef_t *pLoadout = ParseLoadout( pData );
			if ( !pLoadout )
				return false;

			member.m_loadouts.AddToTail( pLoadout );
		}
		else if ( !V_stricmp( name, "Model" ) || !V_stricmp( name, "UseCustomModel" ) )
		{
			member.m_model = value;
		}
		else if ( !V_stricmp( name, "UndeadModel" ) )
		{
			member.m_undeadModel = value;
		}
		else if ( ParseDynamicAttributes( member.m_attributes, pData ) )
		{
			// Skill, Attributes, Item, ItemAttributes, CharacterAttributes, etc
		}
		else if ( IsIgnoredPopfileField( name ) )
		{
			// MvM-only - fine to paste in from a popfile
		}
		else
		{
			// probably a server plugin's popfile extension - skip it rather than lose the whole robot
			Warning( "Robot squads: Ignoring unsupported TFBot field '%s'\n", name );
		}
	}

	// templates don't need a class, but a finished member does
	if ( nDepth == 0 && member.m_class == TF_CLASS_UNDEFINED )
	{
		Warning( "Robot squads: TFBot is missing a Class\n" );
		return false;
	}

	return true;
}


//----------------------------------------------------------------------------
// Loadout { Name, Weight, Counters "Soldier Demoman Sentry", Orders "defend", plus Item/ItemAttributes/etc }
const RobotSquadLoadoutDef_t *CTFRobotSquadTemplates::ParseLoadout( KeyValues *pLoadoutKV )
{
	RobotSquadLoadoutDef_t *pLoadout = new RobotSquadLoadoutDef_t;

	FOR_EACH_SUBKEY( pLoadoutKV, pData )
	{
		const char *name = pData->GetName();
		const char *value = pData->GetString();

		if ( !V_stricmp( name, "Name" ) )
		{
			pLoadout->m_name = value;
		}
		else if ( !V_stricmp( name, "Weight" ) )
		{
			pLoadout->m_weight = MAX( 0.0f, pData->GetFloat() );
		}
		else if ( !V_stricmp( name, "Counters" ) || !V_stricmp( name, "Orders" ) )
		{
			bool bCounters = !V_stricmp( name, "Counters" );

			// space separated, and a name can be repeated to count it more
			CUtlStringList tokens;
			V_SplitString( value, " ", tokens );

			FOR_EACH_VEC( tokens, i )
			{
				const char *token = tokens[i];
				if ( !token[0] )
					continue;

				if ( !bCounters )
				{
					bool bFound = false;
					for ( int iOrder = 0; iOrder < TF_ROBOT_SQUAD_ORDER_COUNT; ++iOrder )
					{
						if ( !V_stricmp( token, s_pszRobotSquadOrderNames[ iOrder ] ) )
						{
							pLoadout->m_nOrderMask |= 1 << iOrder;
							bFound = true;
						}
					}

					if ( !bFound )
					{
						Warning( "Robot squads: Unknown order '%s' in Loadout (attack, defend, follow)\n", token );
					}
				}
				else if ( !V_stricmp( token, "Sentry" ) )
				{
					++pLoadout->m_counterSentryWeight;
				}
				else if ( !V_stricmp( token, "Giant" ) )
				{
					++pLoadout->m_counterGiantWeight;
				}
				else
				{
					int iClass = !V_stricmp( token, "Heavy" ) ? TF_CLASS_HEAVYWEAPONS : GetClassIndexFromString( token );
					if ( iClass == TF_CLASS_UNDEFINED )
					{
						Warning( "Robot squads: Unknown counter '%s' in Loadout (a class, Sentry, or Giant)\n", token );
						continue;
					}

					++pLoadout->m_counterClassWeight[ iClass ];
				}
			}
		}
		else if ( !V_stricmp( name, "Model" ) || !V_stricmp( name, "UseCustomModel" ) )
		{
			pLoadout->m_model = value;
		}
		else if ( ParseDynamicAttributes( pLoadout->m_attributes, pData ) )
		{
			// Item, ItemAttributes, CharacterAttributes, Attributes, etc
		}
		else
		{
			Warning( "Robot squads: Ignoring unsupported Loadout field '%s'\n", name );
		}
	}

	m_loadouts.AddToTail( pLoadout );
	return pLoadout;
}


//----------------------------------------------------------------------------
int CTFRobotSquadTemplates::GetSquadCount( void )
{
	EnsureLoaded();
	return m_squads.Count();
}


//----------------------------------------------------------------------------
const RobotSquadDef_t *CTFRobotSquadTemplates::GetSquad( int i )
{
	EnsureLoaded();
	return m_squads.IsValidIndex( i ) ? m_squads[i] : NULL;
}


//----------------------------------------------------------------------------
const RobotSquadDef_t *CTFRobotSquadTemplates::FindSquad( const char *pszName )
{
	EnsureLoaded();

	if ( !pszName || !pszName[0] )
		return NULL;

	FOR_EACH_VEC( m_squads, i )
	{
		if ( !V_stricmp( m_squads[i]->m_name.Get(), pszName ) )
		{
			return m_squads[i];
		}
	}

	return NULL;
}


//----------------------------------------------------------------------------
// The squad the engineer picked, or "default", or the first one
const RobotSquadDef_t *CTFRobotSquadTemplates::FindForEngineer( CTFPlayer *pEngineer )
{
	EnsureLoaded();

	const RobotSquadDef_t *pSquad = NULL;
	if ( pEngineer )
	{
		pSquad = FindSquad( GetPlayerState( pEngineer ).m_squadChoice.Get() );
	}

	if ( !pSquad )
	{
		pSquad = FindSquad( "default" );
	}

	if ( !pSquad && m_squads.Count() > 0 )
	{
		pSquad = m_squads[0];
	}

	return pSquad;
}


//----------------------------------------------------------------------------
static void PrecacheRobotSquadModel( const char *pszModel )
{
	if ( !pszModel || !pszModel[0] || engine->IsModelPrecached( pszModel ) )
		return;

	if ( !g_pFullFileSystem->FileExists( pszModel, "GAME" ) )
	{
		Warning( "Robot squads: Model '%s' is missing - robots that use it get the stock look\n", pszModel );
		return;
	}

	CBaseEntity::PrecacheModel( pszModel );
}


//----------------------------------------------------------------------------
void CTFRobotSquadTemplates::PrecacheModels( void )
{
	// pick up this map's squads, and any edits to the files
	Load();

	FOR_EACH_VEC( m_squads, i )
	{
		FOR_EACH_VEC( m_squads[i]->m_members, j )
		{
			const RobotSquadMemberDef_t &member = m_squads[i]->m_members[j];
			PrecacheRobotSquadModel( member.m_model.Get() );
			PrecacheRobotSquadModel( member.m_undeadModel.Get() );

			FOR_EACH_VEC( member.m_loadouts, k )
			{
				PrecacheRobotSquadModel( member.m_loadouts[k]->m_model.Get() );
			}
		}
	}

	PrecacheRobotSquadModel( ROBOT_SQUAD_UNDEAD_SNIPER_MODEL );
}


//----------------------------------------------------------------------------
CON_COMMAND_F( tf_robot_squad_reload, "Reload the Engineer robot squad templates.", FCVAR_GAMEDLL | FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	TheRobotSquadTemplates().Load();
}


//----------------------------------------------------------------------------
// Engineers order their squad around with voice commands or this command
CON_COMMAND_F( tf_robot_squad_order, "Order your robot squad: attack, defend, follow, or cycle.", FCVAR_GAMEDLL )
{
	CTFPlayer *pEngineer = ToTFPlayer( UTIL_GetCommandClient() );
	if ( !pEngineer )
		return;

	if ( args.ArgC() < 2 )
	{
		ClientPrint( pEngineer, HUD_PRINTCONSOLE, "Usage: tf_robot_squad_order <attack|defend|follow|cycle>\n" );
		return;
	}

	int nOrder = -1;
	for ( int i = 0; i < TF_ROBOT_SQUAD_ORDER_COUNT; ++i )
	{
		if ( !V_stricmp( args[1], s_pszRobotSquadOrderNames[i] ) )
		{
			nOrder = i;
			break;
		}
	}

	if ( nOrder < 0 && V_stricmp( args[1], "cycle" ) )
	{
		ClientPrint( pEngineer, HUD_PRINTCONSOLE, "Usage: tf_robot_squad_order <attack|defend|follow|cycle>\n" );
		return;
	}

	TFRobotSquad_OrderSquad( pEngineer, nOrder );
}


//----------------------------------------------------------------------------
// Open the robot factory menu, or pick a squad by name: tf_robot_squad_select [name]
CON_COMMAND_F( tf_robot_squad_select, "Pick your robot squad. With no name, opens the robot factory menu.", FCVAR_GAMEDLL )
{
	CTFPlayer *pEngineer = ToTFPlayer( UTIL_GetCommandClient() );
	if ( !pEngineer )
		return;

	if ( args.ArgC() < 2 )
	{
		TFRobotSquad_OpenMenu( pEngineer );
		return;
	}

	const RobotSquadDef_t *pSquad = TheRobotSquadTemplates().FindSquad( args[1] );
	if ( !pSquad )
	{
		ClientPrint( pEngineer, HUD_PRINTCONSOLE, CFmtStr( "No robot squad named '%s' on this map. Squads:\n", args[1] ) );

		for ( int i = 0; i < TheRobotSquadTemplates().GetSquadCount(); ++i )
		{
			const RobotSquadDef_t *pListed = TheRobotSquadTemplates().GetSquad( i );
			ClientPrint( pEngineer, HUD_PRINTCONSOLE, CFmtStr( "  %s - %s\n", pListed->m_name.Get(), pListed->GetDisplayName() ) );
		}
		return;
	}

	GetPlayerState( pEngineer ).m_squadChoice = pSquad->m_name;
	ClientPrint( pEngineer, HUD_PRINTCENTER, CFmtStr( "Robot factory: %s", pSquad->GetDisplayName() ) );
}


//----------------------------------------------------------------------------
bool TFRobotSquad_OrderSquad( CTFPlayer *pEngineer, int nOrder )
{
	if ( !pEngineer || !pEngineer->IsPlayerClass( TF_CLASS_ENGINEER ) )
		return false;

	bool bOrdered = false;
	for ( int i = 0; i < pEngineer->GetObjectCount(); ++i )
	{
		CObjectTeleporter *pTeleporter = dynamic_cast< CObjectTeleporter * >( pEngineer->GetObject( i ) );
		if ( !pTeleporter || !pTeleporter->GetRobotSquad() )
			continue;

		CTFRobotSquad *pSquad = pTeleporter->GetRobotSquad();

		int nNewOrder = nOrder;
		if ( nNewOrder < 0 )
		{
			nNewOrder = ( pSquad->GetOrder() + 1 ) % TF_ROBOT_SQUAD_ORDER_COUNT;
		}

		pTeleporter->SetRobotSquadOrder( nNewOrder );

		ClientPrint( pEngineer, HUD_PRINTCENTER, s_pszRobotSquadOrderMessages[ nNewOrder ] );

		bOrdered = true;
	}

	return bOrdered;
}


//----------------------------------------------------------------------------
// Send a numbered HUD menu, split over as many messages as it takes
static void SendMenu( CTFPlayer *pPlayer, int nValidSlots, int nDisplayTime, const char *pszText )
{
	CSingleUserRecipientFilter filter( pPlayer );
	filter.MakeReliable();

	const char *pszRemaining = pszText;
	int nRemaining = V_strlen( pszText );

	do
	{
		char szChunk[ ROBOT_SQUAD_MENU_CHUNK + 1 ];
		int nChunk = MIN( nRemaining, ROBOT_SQUAD_MENU_CHUNK );
		V_strncpy( szChunk, pszRemaining, nChunk + 1 );

		pszRemaining += nChunk;
		nRemaining -= nChunk;

		UserMessageBegin( filter, "ShowMenu" );
			WRITE_SHORT( nValidSlots );
			WRITE_CHAR( nDisplayTime );
			WRITE_BYTE( nRemaining > 0 ? 1 : 0 );
			WRITE_STRING( szChunk );
		MessageEnd();
	}
	while ( nRemaining > 0 );
}


//----------------------------------------------------------------------------
// The robot factory menu: every squad on this map, a page at a time
void TFRobotSquad_OpenMenu( CTFPlayer *pEngineer, int nPage )
{
	if ( !pEngineer || !pEngineer->IsPlayerClass( TF_CLASS_ENGINEER ) )
		return;

	if ( !tf_robot_squad_enabled.GetBool() || !TFGameRules() || TFGameRules()->IsMannVsMachineMode() )
		return;

	CTFRobotSquadTemplates &templates = TheRobotSquadTemplates();

	int nCount = templates.GetSquadCount();
	if ( nCount == 0 )
	{
		ClientPrint( pEngineer, HUD_PRINTCENTER, "No robot squads are set up for this map" );
		return;
	}

	const RobotSquadDef_t *pCurrent = templates.FindForEngineer( pEngineer );

	int nPages = ( nCount + ROBOT_SQUAD_MENU_PAGE_SIZE - 1 ) / ROBOT_SQUAD_MENU_PAGE_SIZE;
	if ( nPage < 0 )
	{
		// open on the page with the squad they have now
		nPage = 0;
		for ( int i = 0; i < nCount; ++i )
		{
			if ( templates.GetSquad( i ) == pCurrent )
			{
				nPage = i / ROBOT_SQUAD_MENU_PAGE_SIZE;
				break;
			}
		}
	}
	nPage = clamp( nPage, 0, nPages - 1 );

	RobotSquadPlayerState_t &state = GetPlayerState( pEngineer );
	state.m_nMenuPage = nPage;
	state.m_flMenuExpireTime = gpGlobals->curtime + ROBOT_SQUAD_MENU_TIME;

	CUtlString text;
	if ( nPages > 1 )
	{
		text += CFmtStr( "Robot Factory (%d/%d)\n \n", nPage + 1, nPages );
	}
	else
	{
		text += "Robot Factory\n \n";
	}

	int nValidSlots = 0;
	for ( int i = 0; i < ROBOT_SQUAD_MENU_PAGE_SIZE; ++i )
	{
		const RobotSquadDef_t *pSquad = templates.GetSquad( nPage * ROBOT_SQUAD_MENU_PAGE_SIZE + i );
		if ( !pSquad )
			break;

		text += CFmtStr( "%d. %s%s\n", i + 1, pSquad->GetDisplayName(), pSquad == pCurrent ? "  (current)" : "" );
		nValidSlots |= 1 << i;
	}

	text += " \n";

	if ( nPage > 0 )
	{
		text += "8. Back\n";
		nValidSlots |= 1 << 7;
	}

	if ( nPage < nPages - 1 )
	{
		text += "9. More\n";
		nValidSlots |= 1 << 8;
	}

	text += "0. Close";
	nValidSlots |= 1 << 9;

	SendMenu( pEngineer, nValidSlots, ROBOT_SQUAD_MENU_TIME, text.Get() );
}


//----------------------------------------------------------------------------
bool TFRobotSquad_HandleMenuSelect( CTFPlayer *pEngineer, int nSlot )
{
	if ( !pEngineer )
		return false;

	unsigned short index = s_robotSquadPlayerStates.Find( pEngineer->GetUserID() );
	if ( index == s_robotSquadPlayerStates.InvalidIndex() )
		return false;

	RobotSquadPlayerState_t &state = s_robotSquadPlayerStates[ index ];
	if ( state.m_flMenuExpireTime < gpGlobals->curtime )
		return false;

	// whatever they picked, our menu is closed now
	state.m_flMenuExpireTime = -1.0f;

	if ( nSlot == 8 )
	{
		TFRobotSquad_OpenMenu( pEngineer, state.m_nMenuPage - 1 );
		return true;
	}

	if ( nSlot == 9 )
	{
		TFRobotSquad_OpenMenu( pEngineer, state.m_nMenuPage + 1 );
		return true;
	}

	if ( nSlot < 1 || nSlot > ROBOT_SQUAD_MENU_PAGE_SIZE || !pEngineer->IsPlayerClass( TF_CLASS_ENGINEER ) )
		return true;

	const RobotSquadDef_t *pSquad = TheRobotSquadTemplates().GetSquad( state.m_nMenuPage * ROBOT_SQUAD_MENU_PAGE_SIZE + nSlot - 1 );
	if ( !pSquad )
		return true;

	state.m_squadChoice = pSquad->m_name;

	ClientPrint( pEngineer, HUD_PRINTCENTER, CFmtStr( "Robot factory: %s", pSquad->GetDisplayName() ) );

	return true;
}


//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
CTFRobotSquad::CTFRobotSquad( CObjectTeleporter *pFactory )
{
	m_hFactory = pFactory;
	m_hSpawnPoint = NULL;
	m_pRosterDef = NULL;
	m_nRosterLevel = 0;
	m_nRosterGeneration = -1;
	m_pChargingSlot = NULL;
	m_flChargeDoneTime = -1.0f;
	m_flFirstSpawnTime = gpGlobals->curtime + tf_robot_squad_first_spawn_delay.GetFloat();
	m_nOrder = TF_ROBOT_SQUAD_ORDER_ATTACK;
	m_bEngineerWasAlive = true;
}


//----------------------------------------------------------------------------
CTFRobotSquad::~CTFRobotSquad()
{
	OnFactoryRemoved();
}


//----------------------------------------------------------------------------
/*static*/ bool CTFRobotSquad::IsEligibleFactory( CObjectTeleporter *pFactory )
{
	if ( !tf_robot_squad_enabled.GetBool() )
		return false;

	if ( !TFGameRules() || TFGameRules()->IsMannVsMachineMode() )
		return false;

	if ( !pFactory || !pFactory->IsExit() )
		return false;

	CTFPlayer *pEngineer = pFactory->GetBuilder();
	if ( !pEngineer || !pEngineer->IsPlayerClass( TF_CLASS_ENGINEER ) )
		return false;

	// summoned engineers don't get squads of their own
	if ( pEngineer->GetSummonedType() != TF_SUMMONED_NONE )
		return false;

	return true;
}


//----------------------------------------------------------------------------
/*static*/ int CTFRobotSquad::GetSummonedTypeNow( void )
{
	return TF_IsHolidayActive( kHoliday_HalloweenOrFullMoon ) ? TF_SUMMONED_UNDEAD : TF_SUMMONED_ROBOT;
}


//----------------------------------------------------------------------------
int CTFRobotSquad::GetAliveCount( void ) const
{
	int count = 0;
	FOR_EACH_VEC( m_slots, i )
	{
		CTFBot *pBot = m_slots[i]->m_hBot;
		if ( pBot && pBot->IsAlive() )
		{
			++count;
		}
	}
	return count;
}


//----------------------------------------------------------------------------
int CTFRobotSquad::GetSize( void ) const
{
	int count = 0;
	FOR_EACH_VEC( m_slots, i )
	{
		if ( !m_slots[i]->m_bRetired )
		{
			++count;
		}
	}

	// survivors of the last roster still count while they're fighting
	return MAX( count, GetAliveCount() );
}


//----------------------------------------------------------------------------
void CTFRobotSquad::SetOrder( int nOrder )
{
	if ( nOrder < 0 || nOrder >= TF_ROBOT_SQUAD_ORDER_COUNT )
		return;

	m_nOrder = nOrder;

	FOR_EACH_VEC( m_slots, i )
	{
		CTFBot *pBot = m_slots[i]->m_hBot;
		if ( !pBot )
			continue;

		pBot->SetRobotSquadOrder( nOrder );

		if ( pBot->IsAlive() )
		{
			// restart the behavior system so the scenario monitor picks up the new order
			pBot->GetIntentionInterface()->Reset();
		}
	}
}


//----------------------------------------------------------------------------
void CTFRobotSquad::Update( void )
{
	CObjectTeleporter *pFactory = m_hFactory;
	if ( !pFactory )
		return;

	CTFPlayer *pEngineer = pFactory->GetBuilder();

	// followers fall back to the objective while their engineer is dead, and come back when they respawn
	bool bEngineerAlive = pEngineer && pEngineer->IsAlive();
	if ( bEngineerAlive != m_bEngineerWasAlive )
	{
		m_bEngineerWasAlive = bEngineerAlive;

		if ( m_nOrder == TF_ROBOT_SQUAD_ORDER_FOLLOW )
		{
			SetOrder( m_nOrder );
		}
	}

	TrackDeaths( pFactory );

	if ( pEngineer && IsEligibleFactory( pFactory ) )
	{
		RefreshRoster( pFactory, pEngineer );
	}

	ReleaseRetiredSlots();

	if ( GetAliveCount() > 0 )
	{
		RecoverStuckMembers( pFactory );
	}

	if ( !CanSpawn( pFactory, pEngineer ) || gpGlobals->curtime < m_flFirstSpawnTime )
	{
		// a sapped, carried, or disabled exit loses whoever was on the way
		m_pChargingSlot = NULL;
		m_flChargeDoneTime = -1.0f;
		return;
	}

	// the member on the way may have been retired or released since
	if ( m_pChargingSlot && ( !m_slots.HasElement( m_pChargingSlot ) || m_pChargingSlot->m_bRetired ) )
	{
		m_pChargingSlot = NULL;
		m_flChargeDoneTime = -1.0f;
	}

	if ( !m_pChargingSlot )
	{
		// start sending the next member through
		m_pChargingSlot = FindSlotToFill();
		if ( !m_pChargingSlot )
			return;

		m_flChargeDoneTime = gpGlobals->curtime + GetSpawnTime( m_pChargingSlot );
	}

	if ( gpGlobals->curtime < m_flChargeDoneTime )
		return;

	Slot_t *pSlot = m_pChargingSlot;
	m_pChargingSlot = NULL;
	m_flChargeDoneTime = -1.0f;

	if ( !SpawnSlot( pSlot, pFactory, pEngineer ) )
	{
		// out of player slots - try again in a bit
		pSlot->m_flReadyTime = gpGlobals->curtime + ROBOT_SQUAD_RETRY_INTERVAL;
	}
}


//----------------------------------------------------------------------------
// Notice members dying, and when their place can be refilled
void CTFRobotSquad::TrackDeaths( CObjectTeleporter *pFactory )
{
	float flRespawnDelay = tf_robot_squad_respawn_time.GetFloat();
	if ( m_pRosterDef && m_nRosterGeneration == TheRobotSquadTemplates().GetGeneration() && m_pRosterDef->m_respawnDelay >= 0.0f )
	{
		flRespawnDelay = m_pRosterDef->m_respawnDelay;
	}
	flRespawnDelay *= GetLevelMult( pFactory );

	FOR_EACH_VEC( m_slots, i )
	{
		Slot_t *pSlot = m_slots[i];
		CTFBot *pBot = pSlot->m_hBot;

		bool bAlive = pBot && pBot->IsAlive();
		if ( pSlot->m_bWasAlive && !bAlive )
		{
			pSlot->m_flReadyTime = gpGlobals->curtime + flRespawnDelay;
		}

		pSlot->m_bWasAlive = bAlive;
	}
}


//----------------------------------------------------------------------------
// Build the roster from the engineer's squad at the exit's level. Places that
// are still in the new roster keep their member; the rest are retired, and their
// members fight on until they die.
void CTFRobotSquad::RefreshRoster( CObjectTeleporter *pFactory, CTFPlayer *pEngineer )
{
	CTFRobotSquadTemplates &templates = TheRobotSquadTemplates();

	const RobotSquadDef_t *pDef = templates.FindForEngineer( pEngineer );
	int nLevel = pFactory->GetUpgradeLevel();
	int nGeneration = templates.GetGeneration();

	if ( pDef == m_pRosterDef && nLevel == m_nRosterLevel && nGeneration == m_nRosterGeneration )
		return;

	// the same squad at a new level keeps the members it still has
	bool bSameSquad = pDef && pDef == m_pRosterDef && nGeneration == m_nRosterGeneration;

	m_pRosterDef = pDef;
	m_nRosterLevel = nLevel;
	m_nRosterGeneration = nGeneration;

	CUtlVector< Slot_t * > oldSlots;
	oldSlots.Swap( m_slots );

	if ( pDef )
	{
		float flFirstReady = gpGlobals->curtime;

		FOR_EACH_VEC( pDef->m_members, iMember )
		{
			const RobotSquadMemberDef_t &def = pDef->m_members[ iMember ];
			if ( nLevel < def.m_minLevel || nLevel > def.m_maxLevel )
				continue;

			for ( int n = 0; n < def.m_count; ++n )
			{
				Slot_t *pSlot = NULL;

				if ( bSameSquad )
				{
					FOR_EACH_VEC( oldSlots, iOld )
					{
						if ( !oldSlots[ iOld ]->m_bRetired && oldSlots[ iOld ]->m_nMemberIndex == iMember && oldSlots[ iOld ]->m_nCopy == n )
						{
							pSlot = oldSlots[ iOld ];
							oldSlots.Remove( iOld );
							break;
						}
					}
				}

				if ( !pSlot )
				{
					pSlot = new Slot_t( def, iMember, n );
					pSlot->m_flReadyTime = flFirstReady;
				}

				m_slots.AddToTail( pSlot );
			}
		}
	}

	// whatever's left over is retired
	FOR_EACH_VEC( oldSlots, i )
	{
		oldSlots[i]->m_bRetired = true;
		m_slots.AddToTail( oldSlots[i] );
	}

	m_pChargingSlot = NULL;
	m_flChargeDoneTime = -1.0f;
}


//----------------------------------------------------------------------------
// Retired places go away once their member is dead. Their bots are reused for
// the new roster where possible, otherwise kicked.
void CTFRobotSquad::ReleaseRetiredSlots( void )
{
	int nNeeded = 0;
	FOR_EACH_VEC( m_slots, i )
	{
		if ( !m_slots[i]->m_bRetired && m_slots[i]->m_hBot == NULL )
		{
			++nNeeded;
		}
	}

	for ( int i = m_slots.Count() - 1; i >= 0; --i )
	{
		Slot_t *pSlot = m_slots[i];
		if ( !pSlot->m_bRetired )
			continue;

		CTFBot *pBot = pSlot->m_hBot;
		if ( pBot && pBot->IsAlive() )
			continue;

		if ( pBot )
		{
			// their attributes point into the slot we're about to delete
			pBot->ClearEventChangeAttributes();

			// hand the bot over to a place in the new roster that's waiting for one
			Slot_t *pTaker = NULL;
			if ( nNeeded > 0 )
			{
				FOR_EACH_VEC( m_slots, j )
				{
					if ( !m_slots[j]->m_bRetired && m_slots[j]->m_hBot == NULL )
					{
						pTaker = m_slots[j];
						break;
					}
				}
			}

			if ( pTaker )
			{
				pTaker->m_hBot = pBot;
				--nNeeded;
			}
			else
			{
				KickMember( pBot );
			}
		}

		m_slots.Remove( i );
		delete pSlot;
	}
}


//----------------------------------------------------------------------------
// The next place waiting to be filled, in roster order
CTFRobotSquad::Slot_t *CTFRobotSquad::FindSlotToFill( void ) const
{
	FOR_EACH_VEC( m_slots, i )
	{
		Slot_t *pSlot = m_slots[i];
		if ( pSlot->m_bRetired )
			continue;

		CTFBot *pBot = pSlot->m_hBot;
		if ( pBot && pBot->IsAlive() )
			continue;

		if ( gpGlobals->curtime < pSlot->m_flReadyTime )
			continue;

		// a new bot needs a free player slot - wait until one opens up rather than counting down for nothing
		if ( !pBot && !HasRoomForAnotherBot( m_hFactory->GetTeamNumber() ) )
			continue;

		return pSlot;
	}

	return NULL;
}


//----------------------------------------------------------------------------
bool CTFRobotSquad::HasRoomForAnotherBot( int nTeam ) const
{
	// leave room for humans to join
	if ( CountConnectedPlayers() + tf_robot_squad_reserve_slots.GetInt() >= gpGlobals->maxClients )
		return false;

	if ( CountSummonedRobots( nTeam ) >= tf_robot_squad_max_per_team.GetInt() )
		return false;

	return true;
}


//----------------------------------------------------------------------------
float CTFRobotSquad::GetLevelMult( CObjectTeleporter *pFactory ) const
{
	int iLevel = clamp( pFactory->GetUpgradeLevel(), 1, (int)ARRAYSIZE( s_flLevelSpawnMult ) );
	return s_flLevelSpawnMult[ iLevel - 1 ];
}


//----------------------------------------------------------------------------
// How long the exit takes to send this member through
float CTFRobotSquad::GetSpawnTime( const Slot_t *pSlot ) const
{
	float flTime = pSlot->m_def.m_spawnTime;

	if ( flTime < 0.0f )
	{
		if ( pSlot->m_def.IsGiant() )
		{
			flTime = tf_robot_squad_giant_spawn_time.GetFloat();
		}
		else if ( m_pRosterDef && m_nRosterGeneration == TheRobotSquadTemplates().GetGeneration() && m_pRosterDef->m_spawnInterval >= 0.0f )
		{
			flTime = m_pRosterDef->m_spawnInterval;
		}
		else
		{
			flTime = tf_robot_squad_spawn_interval.GetFloat();
		}
	}

	return flTime * GetLevelMult( m_hFactory );
}


//----------------------------------------------------------------------------
// Maps aren't built for giants, so they can wedge themselves on geometry they
// can't fit past. Bring members that have been stuck too long back to the exit.
void CTFRobotSquad::RecoverStuckMembers( CObjectTeleporter *pFactory )
{
	float flStuckTime = tf_robot_squad_stuck_time.GetFloat();
	if ( flStuckTime <= 0.0f )
		return;

	// can't send anyone through an exit that isn't working
	if ( pFactory->IsBuilding() || pFactory->IsPlacing() || pFactory->IsDisabled() || pFactory->HasSapper() || pFactory->IsPlasmaDisabled() )
		return;

	FOR_EACH_VEC( m_slots, i )
	{
		CTFBot *pBot = m_slots[i]->m_hBot;
		if ( !pBot || !pBot->IsAlive() )
			continue;

		ILocomotion *pMover = pBot->GetLocomotionInterface();
		if ( !pMover->IsStuck() || pMover->GetStuckDuration() < flStuckTime )
			continue;

		CBaseEntity *pSpawnPoint = GetSpawnPoint( pFactory );

		// stuck right at the exit - sending them back won't help
		if ( ( pBot->GetAbsOrigin() - pSpawnPoint->GetAbsOrigin() ).IsLengthLessThan( 100.0f ) )
			continue;

		Vector vecDest = pSpawnPoint->GetAbsOrigin();
		QAngle angDest = pSpawnPoint->GetAbsAngles();
		pBot->Teleport( &vecDest, &angDest, &vec3_origin );
		pBot->SnapEyeAngles( angDest );

		if ( !pBot->IsPlayerClass( TF_CLASS_SPY ) )
		{
			pBot->TeleportEffect();
		}

		pMover->ClearStuckStatus( "Robot squad returned stuck member to its exit" );

		// start over from the exit with a fresh path
		pBot->GetIntentionInterface()->Reset();
	}
}


//----------------------------------------------------------------------------
bool CTFRobotSquad::CanSpawn( CObjectTeleporter *pFactory, CTFPlayer *pEngineer ) const
{
	if ( !IsEligibleFactory( pFactory ) )
		return false;

	if ( pEngineer->GetTeamNumber() != pFactory->GetTeamNumber() )
		return false;

	// the exit has to be up and running
	if ( pFactory->IsBuilding() || pFactory->IsPlacing() || pFactory->IsDisabled() || pFactory->HasSapper() || pFactory->IsPlasmaDisabled() )
		return false;

	if ( TFGameRules()->IsInWaitingForPlayers() )
		return false;

	gamerules_roundstate_t roundState = TFGameRules()->State_Get();
	if ( roundState != GR_STATE_RND_RUNNING && roundState != GR_STATE_STALEMATE )
		return false;

	return true;
}


//----------------------------------------------------------------------------
CBaseEntity *CTFRobotSquad::GetSpawnPoint( CObjectTeleporter *pFactory )
{
	// the map cleanup on round restart removes this, so recreate it as needed
	if ( m_hSpawnPoint == NULL )
	{
		m_hSpawnPoint = CreateEntityByName( "info_target" );
		if ( m_hSpawnPoint == NULL )
			return pFactory;

		DispatchSpawn( m_hSpawnPoint );
	}

	// stand on top of the exit, facing the way it faces - it may have been moved since last time
	Vector vecSpawn = pFactory->GetAbsOrigin();
	vecSpawn.z += pFactory->CollisionProp()->OBBMaxs().z + 1.0f;

	m_hSpawnPoint->SetAbsOrigin( vecSpawn );
	m_hSpawnPoint->SetAbsAngles( pFactory->GetAbsAngles() );

	return m_hSpawnPoint;
}


//----------------------------------------------------------------------------
// What the other team is fielding right now
struct RobotSquadEnemyCensus_t
{
	RobotSquadEnemyCensus_t()
	{
		V_memset( m_nClassCount, 0, sizeof( m_nClassCount ) );
		m_nSentries = 0;
		m_nGiants = 0;
		m_nPlayers = 0;
	}

	int m_nClassCount[ TF_CLASS_COUNT_ALL ];
	int m_nSentries;
	int m_nGiants;
	int m_nPlayers;
};

static void TakeEnemyCensus( int nEnemyTeam, RobotSquadEnemyCensus_t &census )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || pPlayer->GetTeamNumber() != nEnemyTeam )
			continue;

		int iClass = pPlayer->GetPlayerClass()->GetClassIndex();
		if ( iClass > TF_CLASS_UNDEFINED && iClass < TF_CLASS_COUNT_ALL )
		{
			++census.m_nClassCount[ iClass ];
		}

		if ( pPlayer->IsMiniBoss() )
		{
			++census.m_nGiants;
		}

		for ( int iObject = 0; iObject < pPlayer->GetObjectCount(); ++iObject )
		{
			CBaseObject *pObject = pPlayer->GetObject( iObject );
			if ( pObject && pObject->GetType() == OBJ_SENTRYGUN && !pObject->IsPlacing() )
			{
				++census.m_nSentries;
			}
		}

		++census.m_nPlayers;
	}
}


//----------------------------------------------------------------------------
// Pick what the member carries this time: loadouts that counter more of the enemy
// team (and suit the squad's orders) are more likely, but anything can come up
const RobotSquadLoadoutDef_t *CTFRobotSquad::PickLoadout( const RobotSquadMemberDef_t &def, CObjectTeleporter *pFactory ) const
{
	if ( def.m_loadouts.Count() == 0 )
		return NULL;

	if ( def.m_loadouts.Count() == 1 )
		return def.m_loadouts[0];

	RobotSquadEnemyCensus_t census;
	TakeEnemyCensus( GetEnemyTeam( pFactory->GetTeamNumber() ), census );

	CUtlVector< float > chances;
	float flTotal = 0.0f;

	FOR_EACH_VEC( def.m_loadouts, i )
	{
		const RobotSquadLoadoutDef_t *pLoadout = def.m_loadouts[i];

		float flMatches = 2.0f * pLoadout->m_counterSentryWeight * census.m_nSentries + 2.0f * pLoadout->m_counterGiantWeight * census.m_nGiants;
		for ( int iClass = 0; iClass < TF_CLASS_COUNT_ALL; ++iClass )
		{
			flMatches += pLoadout->m_counterClassWeight[ iClass ] * census.m_nClassCount[ iClass ];
		}

		// the share of the enemy team it counters, so big teams don't drown out everything else
		float flScore = 1.0f + 3.0f * flMatches / MAX( 1, census.m_nPlayers );

		if ( pLoadout->m_nOrderMask & ( 1 << m_nOrder ) )
		{
			flScore += 1.0f;
		}

		// squared, so the best counter usually wins
		float flChance = pLoadout->m_weight * flScore * flScore;
		chances.AddToTail( flChance );
		flTotal += flChance;
	}

	if ( flTotal <= 0.0f )
		return def.m_loadouts[ RandomInt( 0, def.m_loadouts.Count() - 1 ) ];

	float flRoll = RandomFloat( 0.0f, flTotal );
	FOR_EACH_VEC( chances, i )
	{
		flRoll -= chances[i];
		if ( flRoll <= 0.0f )
			return def.m_loadouts[i];
	}

	return def.m_loadouts.Tail();
}


//----------------------------------------------------------------------------
// The member's own items and attributes, plus its loadout's on top
static CTFBot::EventChangeAttributes_t *BuildSpawnAttributes( const RobotSquadMemberDef_t &def, const RobotSquadLoadoutDef_t *pLoadout )
{
	CTFBot::EventChangeAttributes_t *pAttributes = new CTFBot::EventChangeAttributes_t( def.m_attributes );
	if ( !pLoadout )
		return pAttributes;

	const CTFBot::EventChangeAttributes_t &loadout = pLoadout->m_attributes;

	pAttributes->m_attributeFlags |= loadout.m_attributeFlags;

	if ( loadout.m_weaponRestriction != CTFBot::ANY_WEAPON )
	{
		pAttributes->m_weaponRestriction = loadout.m_weaponRestriction;
	}

	if ( loadout.m_maxVisionRange > 0.0f )
	{
		pAttributes->m_maxVisionRange = loadout.m_maxVisionRange;
	}

	// items given later replace earlier ones in the same slot
	FOR_EACH_VEC( loadout.m_items, i )
	{
		pAttributes->m_items.CopyAndAddToTail( loadout.m_items[i] );
	}

	FOR_EACH_VEC( loadout.m_itemsAttributes, i )
	{
		pAttributes->m_itemsAttributes.AddToTail( loadout.m_itemsAttributes[i] );
	}

	FOR_EACH_VEC( loadout.m_characterAttributes, i )
	{
		pAttributes->m_characterAttributes.AddToTail( loadout.m_characterAttributes[i] );
	}

	FOR_EACH_VEC( loadout.m_tags, i )
	{
		pAttributes->m_tags.CopyAndAddToTail( loadout.m_tags[i] );
	}

	return pAttributes;
}


//----------------------------------------------------------------------------
// Send one member through the exit, reusing its old bot if it has one
bool CTFRobotSquad::SpawnSlot( Slot_t *pSlot, CObjectTeleporter *pFactory, CTFPlayer *pEngineer )
{
	const RobotSquadMemberDef_t &def = pSlot->m_def;

	CBaseEntity *pSpawnPoint = GetSpawnPoint( pFactory );

	const RobotSquadLoadoutDef_t *pLoadout = PickLoadout( def, pFactory );

	CTFBot *pBot = pSlot->m_hBot;
	if ( pBot )
	{
		// back again, as whatever this place is now - it may have been handed over from another squad
		char szName[ MAX_PLAYER_NAME_LENGTH ];
		FormatMemberName( szName, sizeof( szName ), pEngineer, def );
		engine->SetFakeClientConVarValue( pBot->edict(), "name", szName );

		pBot->ClearEventChangeAttributes();
		pBot->SetSpawnPoint( pSpawnPoint );
		pBot->SetDesiredPlayerClassIndex( def.m_class );
		pBot->ForceRespawn();
	}
	else
	{
		pBot = CreateMember( pFactory, pEngineer, def, pSpawnPoint );
		if ( !pBot )
			return false;

		pSlot->m_hBot = pBot;
	}

	// the bot has let go of last time's attributes by now
	delete pSlot->m_pSpawnAttributes;
	pSlot->m_pSpawnAttributes = BuildSpawnAttributes( def, pLoadout );
	pSlot->m_pLoadout = pLoadout;

	if ( pLoadout )
	{
		DevMsg( "Robot squad: %s comes through carrying '%s'\n", pBot->GetPlayerName(), pLoadout->m_name.IsEmpty() ? "(unnamed loadout)" : pLoadout->m_name.Get() );
	}

	ApplyMember( pBot, def, pLoadout, *pSlot->m_pSpawnAttributes, pFactory, pEngineer );

	pSlot->m_bWasAlive = pBot->IsAlive();

	if ( GetSummonedTypeNow() == TF_SUMMONED_ROBOT )
	{
		pFactory->EmitSound( "MVM.Robot_Teleporter_Deliver" );
	}

	return true;
}


//----------------------------------------------------------------------------
CTFBot *CTFRobotSquad::CreateMember( CObjectTeleporter *pFactory, CTFPlayer *pEngineer, const RobotSquadMemberDef_t &def, CBaseEntity *pSpawnPoint )
{
	int team = pFactory->GetTeamNumber();
	if ( !HasRoomForAnotherBot( team ) )
		return NULL;

	char szName[ MAX_PLAYER_NAME_LENGTH ];
	FormatMemberName( szName, sizeof( szName ), pEngineer, def );

	CTFBot *pBot = NextBotCreatePlayerBot< CTFBot >( szName, false );
	if ( !pBot )
		return NULL;

	pBot->SetSummoner( pEngineer );
	pBot->SetRobotFactory( pFactory );
	pBot->SetSpawnPoint( pSpawnPoint );

	pBot->ChangeTeam( team, false, true );

	pBot->AllowInstantSpawn();
	pBot->HandleCommand_JoinClass( GetPlayerClassData( def.m_class )->m_szClassName );

	return pBot;
}


//----------------------------------------------------------------------------
// Turn a freshly (re)spawned bot into its squad member. Mirrors CTFBotSpawner::Spawn().
void CTFRobotSquad::ApplyMember( CTFBot *pBot, const RobotSquadMemberDef_t &def, const RobotSquadLoadoutDef_t *pLoadout, const CTFBot::EventChangeAttributes_t &attributes, CObjectTeleporter *pFactory, CTFPlayer *pEngineer )
{
	int nSummonedType = GetSummonedTypeNow();

	pBot->SetSummoner( pEngineer );
	pBot->SetRobotFactory( pFactory );
	pBot->SetRobotSquadOrder( m_nOrder );
	pBot->SetSummonedType( nSummonedType );

	pBot->RemovePlayerAttributes( false );

	pBot->ClearEventChangeAttributes();
	pBot->AddEventChangeAttributes( &attributes );

	int flags = attributes.m_attributeFlags;

	pBot->SetIsMiniBoss( ( flags & CTFBot::MINIBOSS ) != 0 );
	pBot->SetUseBossHealthBar( ( flags & CTFBot::USE_BOSS_HEALTH_BAR ) != 0 );

	if ( flags & CTFBot::BULLET_IMMUNE )
	{
		pBot->m_Shared.AddCond( TF_COND_BULLET_IMMUNE );
	}

	if ( flags & CTFBot::BLAST_IMMUNE )
	{
		pBot->m_Shared.AddCond( TF_COND_BLAST_IMMUNE );
	}

	if ( flags & CTFBot::FIRE_IMMUNE )
	{
		pBot->m_Shared.AddCond( TF_COND_FIRE_IMMUNE );
	}

	pBot->SetScaleOverride( def.m_scale );

	int nHealth = def.m_health > 0 ? def.m_health : pBot->GetMaxHealth();
	pBot->ModifyMaxHealth( nHealth );

	pBot->StartIdleSound();

	pBot->OnEventChangeAttributes( &attributes );

	if ( pBot->HasAttribute( CTFBot::SPAWN_WITH_FULL_CHARGE ) )
	{
		CWeaponMedigun *medigun = dynamic_cast< CWeaponMedigun * >( pBot->Weapon_GetSlot( TF_WPN_TYPE_SECONDARY ) );
		if ( medigun )
		{
			medigun->AddCharge( 1.0f );
		}

		pBot->m_Shared.SetRageMeter( 100.0f );
	}

	ApplyLook( pBot, def, pLoadout, nSummonedType );

	// arrive through the teleporter
	if ( !pBot->IsPlayerClass( TF_CLASS_SPY ) )
	{
		pBot->TeleportEffect();
	}

	float flUberTime = tf_robot_squad_spawn_uber.GetFloat();
	if ( flUberTime > 0.0f )
	{
		pBot->m_Shared.AddCond( TF_COND_INVULNERABLE, flUberTime );
		pBot->m_Shared.AddCond( TF_COND_INVULNERABLE_WEARINGOFF, flUberTime );
	}
}


//----------------------------------------------------------------------------
static bool SetSummonedModel( CTFBot *pBot, const char *pszModel )
{
	// models can only be precached at map start - anything added since falls back to the stock look
	if ( !pszModel || !pszModel[0] || !engine->IsModelPrecached( pszModel ) )
		return false;

	pBot->GetPlayerClass()->SetCustomModel( pszModel, USE_CLASS_ANIMATIONS );
	pBot->UpdateModel();
	return true;
}


//----------------------------------------------------------------------------
// The member's (or its loadout's) own model if it has one, otherwise the stock
// MvM robot - or during Halloween, its undead model or the zombie look
void CTFRobotSquad::ApplyLook( CTFBot *pBot, const RobotSquadMemberDef_t &def, const RobotSquadLoadoutDef_t *pLoadout, int nSummonedType )
{
	int nClassIndex = pBot->GetPlayerClass()->GetClassIndex();

	if ( nSummonedType == TF_SUMMONED_UNDEAD )
	{
		const char *pszUndeadModel = def.m_undeadModel.Get();
		if ( !pszUndeadModel[0] && nClassIndex == TF_CLASS_SNIPER )
		{
			pszUndeadModel = ROBOT_SQUAD_UNDEAD_SNIPER_MODEL;
		}

		if ( SetSummonedModel( pBot, pszUndeadModel ) )
			return;

		// zombies use the original player models with the Voodoo-Cursed Soul skins
		pBot->GetPlayerClass()->SetCustomModel( NULL );
		pBot->UpdateModel();
		pBot->m_nSkin = 4;

		pBot->AddItem( CFmtStr( "Zombie %s", g_aRawPlayerClassNamesShort[ nClassIndex ] ) );
		return;
	}

	const char *pszModel = ( pLoadout && !pLoadout->m_model.IsEmpty() ) ? pLoadout->m_model.Get() : def.m_model.Get();
	if ( SetSummonedModel( pBot, pszModel ) )
	{
		pBot->SetBloodColor( DONT_BLEED );
		return;
	}

	if ( nClassIndex >= TF_CLASS_SCOUT && nClassIndex <= TF_CLASS_ENGINEER )
	{
		bool bGiant = def.m_scale >= tf_mvm_miniboss_scale.GetFloat() || pBot->IsMiniBoss();
		if ( bGiant && g_pFullFileSystem->FileExists( g_szBotBossModels[ nClassIndex ] ) )
		{
			pBot->GetPlayerClass()->SetCustomModel( g_szBotBossModels[ nClassIndex ], USE_CLASS_ANIMATIONS );
			pBot->UpdateModel();
			pBot->SetBloodColor( DONT_BLEED );
		}
		else if ( g_pFullFileSystem->FileExists( g_szBotModels[ nClassIndex ] ) )
		{
			pBot->GetPlayerClass()->SetCustomModel( g_szBotModels[ nClassIndex ], USE_CLASS_ANIMATIONS );
			pBot->UpdateModel();
			pBot->SetBloodColor( DONT_BLEED );
		}
	}
}


//----------------------------------------------------------------------------
void CTFRobotSquad::KickMember( CTFBot *pBot )
{
	if ( !pBot )
		return;

	pBot->ClearEventChangeAttributes();
	pBot->SetRobotFactory( NULL );
	pBot->RemoveSummonedRobot();
}


//----------------------------------------------------------------------------
void CTFRobotSquad::OnFactoryRemoved( void )
{
	if ( gEntList.IsClearingEntities() )
	{
		// level shutdown - the bots are going away too
		m_slots.PurgeAndDeleteElements();
		m_hSpawnPoint = NULL;
		m_hFactory = NULL;
		return;
	}

	CObjectTeleporter *pFactory = m_hFactory;
	CTFPlayer *pEngineer = pFactory ? pFactory->GetBuilder() : NULL;

	// survivors fight on until killed, unless their engineer is gone or the round is being reset
	bool bKickAll = !pEngineer || pEngineer->IsDisconnecting() || !pFactory || pEngineer->GetTeamNumber() != pFactory->GetTeamNumber();

	if ( TFGameRules() )
	{
		gamerules_roundstate_t roundState = TFGameRules()->State_Get();
		if ( roundState != GR_STATE_RND_RUNNING && roundState != GR_STATE_STALEMATE && roundState != GR_STATE_TEAM_WIN )
		{
			bKickAll = true;
		}
	}

	FOR_EACH_VEC( m_slots, i )
	{
		CTFBot *pBot = m_slots[i]->m_hBot;
		if ( !pBot )
			continue;

		if ( bKickAll || !pBot->IsAlive() )
		{
			KickMember( pBot );
			continue;
		}

		// their attributes point into our slots, which are going away
		pBot->ClearEventChangeAttributes();
		pBot->SetRobotFactory( NULL );
		pBot->SetAttribute( CTFBot::REMOVE_ON_DEATH );

		if ( pBot->GetRobotSquadOrder() == TF_ROBOT_SQUAD_ORDER_DEFEND )
		{
			// nothing left to defend
			pBot->SetRobotSquadOrder( TF_ROBOT_SQUAD_ORDER_ATTACK );
			pBot->GetIntentionInterface()->Reset();
		}
	}

	m_slots.PurgeAndDeleteElements();

	if ( m_hSpawnPoint != NULL )
	{
		UTIL_Remove( m_hSpawnPoint );
		m_hSpawnPoint = NULL;
	}

	m_hFactory = NULL;
}
