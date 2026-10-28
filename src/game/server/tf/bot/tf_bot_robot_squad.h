//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_robot_squad.h
// Engineer robot squads: teleporter exits that summon a squad of TFBots
// for the engineer's team outside of Mann vs. Machine.

#ifndef TF_BOT_ROBOT_SQUAD_H
#define TF_BOT_ROBOT_SQUAD_H

#include "tf_bot.h"

class CObjectTeleporter;

//----------------------------------------------------------------------------
// One loadout a squad member can carry. Each time the member comes through the
// exit it picks among its loadouts, favoring the ones that counter the enemy team.
struct RobotSquadLoadoutDef_t
{
	RobotSquadLoadoutDef_t()
	{
		m_weight = 1.0f;
		V_memset( m_counterClassWeight, 0, sizeof( m_counterClassWeight ) );
		m_counterSentryWeight = 0;
		m_counterGiantWeight = 0;
		m_nOrderMask = 0;
	}

	CUtlString m_name;
	float m_weight;
	int m_counterClassWeight[ TF_CLASS_COUNT_ALL ];	// how much each enemy of this class counts toward picking this loadout
	int m_counterSentryWeight;
	int m_counterGiantWeight;
	int m_nOrderMask;								// bit per ETFRobotSquadOrder this loadout suits, 0 for none in particular
	CUtlString m_model;								// replaces the member's model while carrying this loadout
	CTFBot::EventChangeAttributes_t m_attributes;	// items and attributes added on top of the member's own
};

//----------------------------------------------------------------------------
// One squad member, parsed from popfile-style TFBot syntax
struct RobotSquadMemberDef_t
{
	RobotSquadMemberDef_t()
	{
		m_class = TF_CLASS_UNDEFINED;
		m_health = -1;
		m_scale = -1.0f;
		m_count = 1;
		m_minLevel = 1;
		m_maxLevel = 3;
		m_spawnTime = -1.0f;
	}

	bool IsGiant( void ) const	{ return ( m_attributes.m_attributeFlags & CTFBot::MINIBOSS ) != 0; }

	int m_class;
	CUtlString m_name;
	int m_health;
	float m_scale;
	int m_count;
	int m_minLevel;					// teleporter levels this member shows up at
	int m_maxLevel;
	float m_spawnTime;				// seconds the exit spends sending this member through, < 0 uses the squad's
	CUtlString m_model;				// custom robot model, otherwise the stock MvM robot
	CUtlString m_undeadModel;		// model during Halloween / Full Moon, otherwise the zombie look
	CTFBot::EventChangeAttributes_t m_attributes;
	CCopyableUtlVector< const RobotSquadLoadoutDef_t * > m_loadouts;	// owned by CTFRobotSquadTemplates
};

//----------------------------------------------------------------------------
// A squad template, picked by the engineer from the robot factory menu
struct RobotSquadDef_t
{
	RobotSquadDef_t()
	{
		m_respawnDelay = -1.0f;
		m_spawnInterval = -1.0f;
	}

	const char *GetDisplayName( void ) const	{ return m_displayName.IsEmpty() ? m_name.Get() : m_displayName.Get(); }

	CUtlString m_name;
	CUtlString m_displayName;
	float m_respawnDelay;			// seconds before a dead member's place is refilled, < 0 uses tf_robot_squad_respawn_time
	float m_spawnInterval;			// seconds to send each member through, < 0 uses tf_robot_squad_spawn_interval
	CUtlVector< RobotSquadMemberDef_t > m_members;
};

//----------------------------------------------------------------------------
// Loads scripts/robot_squads.txt, then scripts/robot_squads/<map>.txt
class CTFRobotSquadTemplates : public CAutoGameSystem
{
public:
	CTFRobotSquadTemplates();
	virtual ~CTFRobotSquadTemplates();

	virtual void LevelInitPreEntity( void ) OVERRIDE;
	virtual void LevelShutdownPostEntity( void ) OVERRIDE;

	bool Load( void );
	void Clear( void );
	void PrecacheModels( void );		// at map start, since models can't be precached once the game is running

	int GetSquadCount( void );
	const RobotSquadDef_t *GetSquad( int i );
	const RobotSquadDef_t *FindSquad( const char *pszName );
	const RobotSquadDef_t *FindForEngineer( CTFPlayer *pEngineer );

	// increments whenever the squads are reloaded, so live squads know their templates changed
	int GetGeneration( void ) const		{ return m_nGeneration; }

private:
	void EnsureLoaded( void );
	bool ParseFile( KeyValues *pFileKV, KeyValues *pTemplatesKV, const char *pszFile );
	bool ParseSquad( KeyValues *pSquadKV, KeyValues *pTemplatesKV, RobotSquadDef_t *pSquad );
	bool ParseMember( KeyValues *pMemberKV, KeyValues *pTemplatesKV, RobotSquadMemberDef_t &member, int nDepth );
	const RobotSquadLoadoutDef_t *ParseLoadout( KeyValues *pLoadoutKV );

	CUtlVector< RobotSquadDef_t * > m_squads;
	CUtlVector< RobotSquadLoadoutDef_t * > m_loadouts;	// every member's loadouts, shared by copies of the member
	bool m_bLoaded;
	int m_nGeneration;
};

CTFRobotSquadTemplates &TheRobotSquadTemplates( void );

// Give orders to the squads of this engineer's exits (nOrder < 0 cycles). Returns false if there are none.
bool TFRobotSquad_OrderSquad( CTFPlayer *pEngineer, int nOrder );

// The robot factory menu, opened by right-clicking with the build PDA
void TFRobotSquad_OpenMenu( CTFPlayer *pEngineer, int nPage = -1 );
bool TFRobotSquad_HandleMenuSelect( CTFPlayer *pEngineer, int nSlot );	// true if it was our menu


//----------------------------------------------------------------------------
// The squad owned by one teleporter exit. Like an MvM engineer's teleporter it
// keeps sending robots through one at a time until the squad is full, and
// refills each member's place a while after they die.
class CTFRobotSquad
{
public:
	CTFRobotSquad( CObjectTeleporter *pFactory );
	~CTFRobotSquad();

	void Update( void );						// called periodically by the factory
	void OnFactoryRemoved( void );				// factory is going away - release or kick members

	void SetOrder( int nOrder );				// ETFRobotSquadOrder
	int GetOrder( void ) const					{ return m_nOrder; }

	int GetAliveCount( void ) const;
	int GetSize( void ) const;					// members in the current roster
	float GetNextArrivalTime( void ) const		{ return m_flChargeDoneTime; }	// < 0 while nobody is on the way

	static bool IsEligibleFactory( CObjectTeleporter *pFactory );

	static int GetSummonedTypeNow( void );		// robots, or undead during Halloween / Full Moon

private:
	// one place in the squad
	struct Slot_t
	{
		Slot_t( const RobotSquadMemberDef_t &def, int nMemberIndex, int nCopy ) : m_def( def )
		{
			m_nMemberIndex = nMemberIndex;
			m_nCopy = nCopy;
			m_flReadyTime = 0.0f;
			m_bWasAlive = false;
			m_bRetired = false;
			m_pSpawnAttributes = NULL;
			m_pLoadout = NULL;
		}

		~Slot_t()
		{
			delete m_pSpawnAttributes;
		}

		RobotSquadMemberDef_t m_def;
		CTFBot::EventChangeAttributes_t *m_pSpawnAttributes;	// the member's own plus its loadout's - the bot points in here
		const RobotSquadLoadoutDef_t *m_pLoadout;				// what it carried last time it came through
		int m_nMemberIndex;						// which TFBot block and copy of it this place came from
		int m_nCopy;
		CHandle< CTFBot > m_hBot;
		float m_flReadyTime;					// when this place can be refilled
		bool m_bWasAlive;
		bool m_bRetired;						// not in the roster any more - goes away once its bot dies
	};

	void RefreshRoster( CObjectTeleporter *pFactory, CTFPlayer *pEngineer );
	void TrackDeaths( CObjectTeleporter *pFactory );
	void ReleaseRetiredSlots( void );
	Slot_t *FindSlotToFill( void ) const;
	bool CanSpawn( CObjectTeleporter *pFactory, CTFPlayer *pEngineer ) const;
	bool HasRoomForAnotherBot( int nTeam ) const;
	bool SpawnSlot( Slot_t *pSlot, CObjectTeleporter *pFactory, CTFPlayer *pEngineer );
	CTFBot *CreateMember( CObjectTeleporter *pFactory, CTFPlayer *pEngineer, const RobotSquadMemberDef_t &def, CBaseEntity *pSpawnPoint );
	const RobotSquadLoadoutDef_t *PickLoadout( const RobotSquadMemberDef_t &def, CObjectTeleporter *pFactory ) const;
	void ApplyMember( CTFBot *pBot, const RobotSquadMemberDef_t &def, const RobotSquadLoadoutDef_t *pLoadout, const CTFBot::EventChangeAttributes_t &attributes, CObjectTeleporter *pFactory, CTFPlayer *pEngineer );
	void ApplyLook( CTFBot *pBot, const RobotSquadMemberDef_t &def, const RobotSquadLoadoutDef_t *pLoadout, int nSummonedType );
	void KickMember( CTFBot *pBot );
	float GetLevelMult( CObjectTeleporter *pFactory ) const;
	float GetSpawnTime( const Slot_t *pSlot ) const;
	void RecoverStuckMembers( CObjectTeleporter *pFactory );
	CBaseEntity *GetSpawnPoint( CObjectTeleporter *pFactory );

	CHandle< CObjectTeleporter > m_hFactory;
	EHANDLE m_hSpawnPoint;								// sits on top of the exit
	CUtlVector< Slot_t * > m_slots;

	// what the roster was built from
	const RobotSquadDef_t *m_pRosterDef;
	int m_nRosterLevel;
	int m_nRosterGeneration;

	Slot_t *m_pChargingSlot;							// the member on the way, or NULL
	float m_flChargeDoneTime;
	float m_flFirstSpawnTime;
	int m_nOrder;
	bool m_bEngineerWasAlive;
};

#endif // TF_BOT_ROBOT_SQUAD_H
