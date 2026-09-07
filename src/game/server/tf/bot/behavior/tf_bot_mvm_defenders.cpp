//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_mvm_defenders.cpp

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_gamerules.h"
#ifdef CLIENT_DLL
    #include "c_tf_objective_resource.h"
#else
    #include "tf_objective_resource.h"
#endif
#include "bot/tf_bot.h"
#include "tf_bot_mvm_defenders.h"
#include "entity_capture_flag.h"
#include "player_vs_environment/tf_upgrades.h"
#include "tf_upgrades_shared.h"
#include "econ_item_system.h"

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool IsPointInsideInvaderSpawnRoom( const Vector &pos )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "func_respawnroom" );
		  pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "func_respawnroom" ) )
	{
		if ( pEnt->GetTeamNumber() != TF_TEAM_PVE_INVADERS )
			continue;

		Vector mins, maxs;
		pEnt->CollisionProp()->WorldSpaceAABB( &mins, &maxs );
		mins -= Vector( 48.0f, 48.0f, 24.0f );
		maxs += Vector( 48.0f, 48.0f, 24.0f );

		if ( pos.x >= mins.x && pos.x <= maxs.x &&
			 pos.y >= mins.y && pos.y <= maxs.y &&
			 pos.z >= mins.z && pos.z <= maxs.z )
		{
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool IsNavAreaInsideInvaderSpawnRoom( CNavArea *area )
{
	if ( !area )
		return true;

	CTFNavArea *pTFArea = (CTFNavArea *)area;
	if ( !pTFArea )
		return false;

	return pTFArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) ||
		   pTFArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED );
}

// ============================================================================
// Note: This doesn't seem to work??? Will need to make it its own entire behavior class.
static bool TryCollectCurrency( CTFBot *me, PathFollower &path, CountdownTimer &repathTimer )
{
	// Only for Scouts
	if ( me->GetPlayerClass()->GetClassIndex() != TF_CLASS_SCOUT )
		return false;

	CBaseEntity *pClosestPack = NULL;
	float flMinDistSqr = FLT_MAX;
	Vector myPos = me->GetAbsOrigin();

	const char *szCurrencyClasses[] = {
		"item_currencypack_large",
		"item_currencypack_medium",
		"item_currencypack_small"
	};

	for ( int i = 0; i < ARRAYSIZE( szCurrencyClasses ); ++i )
	{
		CBaseEntity *pEntity = NULL;
		while ( ( pEntity = gEntList.FindEntityByClassname( pEntity, szCurrencyClasses[i] ) ) != NULL )
		{
			if ( IsPointInsideInvaderSpawnRoom( pEntity->GetAbsOrigin() ) )
				continue;

			float flDistSqr = ( pEntity->GetAbsOrigin() - myPos ).LengthSqr();
			if ( flDistSqr < flMinDistSqr )
			{
				flMinDistSqr = flDistSqr;
				pClosestPack = pEntity;
			}
		}
	}

	if ( pClosestPack == NULL )
		return false;

	const float flCollectRange = 64.0f;
	float flDist = sqrt( flMinDistSqr );

	if ( flDist < flCollectRange )
	{
		if ( repathTimer.IsElapsed() || !path.IsValid() )
		{
			CTFBotPathCost cost( me, FASTEST_ROUTE );
			path.Compute( me, pClosestPack->WorldSpaceCenter(), cost, 0.0f, true );
			repathTimer.Start( 0.25f );
		}

		if ( path.IsValid() )
		{
			path.Update( me );
			return true;
		}
	}

	if ( repathTimer.IsElapsed() || !path.IsValid() )
	{
		CTFBotPathCost cost( me, SAFEST_ROUTE );
		path.Compute( me, pClosestPack->WorldSpaceCenter(), cost, 0.0f, true );
		repathTimer.Start( 0.5f );
	}

	if ( path.IsValid() )
	{
		path.Update( me );
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
static bool IsPointInsideEntityBounds( const Vector &pos, CBaseEntity *pEnt )
{
	if ( !pEnt )
		return false;

	Vector mins, maxs;
	pEnt->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

	mins.x -= 8.0f;  mins.y -= 8.0f;  mins.z -= 72.0f;
	maxs.x += 8.0f;  maxs.y += 8.0f;  maxs.z += 24.0f;

	return ( pos.x >= mins.x && pos.x <= maxs.x &&
			 pos.y >= mins.y && pos.y <= maxs.y &&
			 pos.z >= mins.z && pos.z <= maxs.z );
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
static bool SelectUpgradeStationGoal( CBaseEntity *pStation, Vector &outGoal )
{
	if ( !pStation )
		return false;

	Vector mins, maxs;
	pStation->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

	Vector center = ( mins + maxs ) * 0.5f;

	CNavArea *pArea = TheNavMesh->GetNearestNavArea( center, false, 256.0f, false, true, TEAM_ANY );
	if ( pArea )
	{
		Vector areaCenter = pArea->GetCenter();
		areaCenter.x = clamp( areaCenter.x, mins.x + 16.0f, maxs.x - 16.0f );
		areaCenter.y = clamp( areaCenter.y, mins.y + 16.0f, maxs.y - 16.0f );
		outGoal = areaCenter;
		return true;
	}

	// Fallback: Randomly try spots inside the upgrade stations until we find a nearby nav area
	for ( int i = 0; i < 24; ++i )
	{
		Vector sample(
			RandomFloat( mins.x + 16.0f, maxs.x - 16.0f ),
			RandomFloat( mins.y + 16.0f, maxs.y - 16.0f ),
			center.z );

		pArea = TheNavMesh->GetNearestNavArea( sample, false, 128.0f, false, true, TEAM_ANY );
		if ( !pArea )
			continue;

		outGoal = pArea->GetCenter();
		outGoal.x = clamp( outGoal.x, mins.x + 8.0f, maxs.x - 8.0f );
		outGoal.y = clamp( outGoal.y, mins.y + 8.0f, maxs.y - 8.0f );
		return true;
	}

	// Fallback, hopefuly won't be used.
	outGoal = center;
	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
// Note: Bots will buy canteens as well the Heavy rage, though they don't currently know how to use them.
static bool HasAffordableUpgrade( CTFBot *me )
{
	if ( !me || !TFGameRules() || !g_hUpgradeEntity.Get() )
		return false;

	const int nUpgradeCount = g_MannVsMachineUpgrades.m_Upgrades.Count();
	if ( nUpgradeCount <= 0 )
		return false;

	const int nClass    = me->GetPlayerClass()->GetClassIndex();
	const int nCurrency = me->GetCurrency();

	for ( int iUpgrade = 0; iUpgrade < nUpgradeCount; ++iUpgrade )
	{
		CMannVsMachineUpgrades upgrade = g_MannVsMachineUpgrades.m_Upgrades[ iUpgrade ];

		CEconItemAttributeDefinition *pAttribDef =
			ItemSystem()->GetStaticDataForAttributeByName( upgrade.szAttrib );
		if ( !pAttribDef )
			continue;

		CUtlVector< int > slots;
		if ( upgrade.nUIGroup == UIGROUP_UPGRADE_ATTACHED_TO_PLAYER )
		{
			slots.AddToTail( LOADOUT_POSITION_INVALID );
		}
		else if ( upgrade.nUIGroup == UIGROUP_POWERUPBOTTLE )
		{
			slots.AddToTail( LOADOUT_POSITION_ACTION );
		}
		else
		{
			for ( int s = LOADOUT_POSITION_PRIMARY; s <= LOADOUT_POSITION_MELEE; ++s )
				slots.AddToTail( s );
		}

		for ( int i = 0; i < slots.Count(); ++i )
		{
			const int iSlot = slots[ i ];

			if ( !TFGameRules()->CanUpgradeWithAttrib(
					me, iSlot, pAttribDef->GetDefinitionIndex(), &upgrade ) )
				continue;

			const int nCost = TFGameRules()->GetCostForUpgrade(
				&upgrade, iSlot, nClass, me );

			if ( nCost > 0 && nCurrency >= nCost )
				return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotMVMDefender::CTFBotMVMDefender( void )
{
	m_bHasGoal = false;
	m_goalPos  = vec3_origin;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefender::IsPossible( CTFBot *me )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return false;

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return false;

	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefender::IsInvalidSpawnRoomArea( CNavArea *area ) const
{
	return IsNavAreaInsideInvaderSpawnRoom( area );
}
//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefender::SelectWanderGoal( CTFBot *me )
{
	CUtlVector< CBaseEntity * > rooms;

	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "func_respawnroom" );
		  pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "func_respawnroom" ) )
	{
		if ( pEnt->GetTeamNumber() == TF_TEAM_PVE_INVADERS )
			rooms.AddToTail( pEnt );
	}

	if ( rooms.Count() == 0 )
	{
		m_bHasGoal = false;
		return false;
	}

	for ( int i = rooms.Count() - 1; i > 0; --i )
	{
		int nSwap = RandomInt( 0, i );
		V_swap( rooms[i], rooms[nSwap] );
	}

	const float flMinRadius      = 350.0f;
	const float flMaxRadius      = 1250.0f;
	const int   kAttemptsPerRoom = 32;
	const int   kMaxCandidates   = 100;

	CUtlVector< CNavArea * > candidates;

	for ( int r = 0; r < rooms.Count() && candidates.Count() < kMaxCandidates; ++r )
	{
		CBaseEntity *pRoom = rooms[r];
		if ( !pRoom )
			continue;

		Vector roomCenter = pRoom->WorldSpaceCenter();

		for ( int attempt = 0; attempt < kAttemptsPerRoom; ++attempt )
		{
			float angle = RandomFloat( 0.0f, 6.2831853f );
			float dist  = RandomFloat( flMinRadius, flMaxRadius );
			Vector offset( cosf( angle ) * dist, sinf( angle ) * dist, 0.0f );
			Vector sample = roomCenter + offset;

			CNavArea *pArea = TheNavMesh->GetNearestNavArea( sample,
															 false, 800.0f, false, true, TEAM_ANY );
			if ( !pArea )
				continue;

			if ( IsInvalidSpawnRoomArea( pArea ) )
				continue;

			Vector mins, maxs;
			pRoom->CollisionProp()->WorldSpaceAABB( &mins, &maxs );
			mins -= Vector( 72.0f, 72.0f, 40.0f );
			maxs += Vector( 72.0f, 72.0f, 40.0f );

			Vector areaCenter = pArea->GetCenter();
			if ( areaCenter.x >= mins.x && areaCenter.x <= maxs.x &&
				 areaCenter.y >= mins.y && areaCenter.y <= maxs.y &&
				 areaCenter.z >= mins.z && areaCenter.z <= maxs.z )
			{
				continue;
			}

			if ( candidates.HasElement( pArea ) )
				continue;

			candidates.AddToTail( pArea );
			if ( candidates.Count() >= kMaxCandidates )
				break;
		}
	}

	if ( candidates.Count() == 0 )
	{
		m_bHasGoal = false;
		return false;
	}

	CNavArea *pChosen = candidates[ RandomInt( 0, candidates.Count() - 1 ) ];
	m_goalPos = pChosen->GetCenter();
	m_bHasGoal = true;
	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotMVMDefender::ComputePathToGoal( CTFBot *me )
{
	if ( !m_bHasGoal )
		return;

	CTFBotPathCost cost( me, FASTEST_ROUTE );
	m_path.Compute( me, m_goalPos, cost, 0.0f, true );
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMDefender::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Invalidate();
	m_bHasGoal = false;

	if ( SelectWanderGoal( me ) )
	{
		ComputePathToGoal( me );
		m_repathTimer.Start( RandomFloat( 1.0f, 2.5f ) );
	}
	else
	{
		m_repathTimer.Start( 1.0f );
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMDefender::Update( CTFBot *me, float interval )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	// No wave in progress, and we have enough currency to buy an upgrade, so go do that.
    if ( CTFBotMVMBuyUpgrades::IsPossible( me ) )
    	return ChangeTo( new CTFBotMVMBuyUpgrades(), "Between waves, buy upgrades" );

	// Scouts should go look for currency
    if ( TryCollectCurrency( me, m_path, m_repathTimer ) )
		return Continue();

	// Bomb threat is active, switch to aggressive bomb defence
	if ( CTFBotMVMDefendBomb::IsPossible( me ) )
		return ChangeTo( new CTFBotMVMDefendBomb(), "Bomb threat active - defend the bomb!" );

	// Normal wander behaviour
	if ( !m_bHasGoal || m_repathTimer.IsElapsed() )
	{
		if ( SelectWanderGoal( me ) )
			ComputePathToGoal( me );

		m_repathTimer.Start( m_bHasGoal ? RandomFloat( 5.0f, 9.0f ) : 1.25f );
	}

	if ( m_bHasGoal )
	{
		const float flStandRange = 250.0f;
		if ( ( me->GetAbsOrigin() - m_goalPos ).AsVector2D().LengthSqr() < ( flStandRange * flStandRange ) )
		{
			// We're ready...
            if ( TFGameRules() && TFGameRules()->UsePlayerReadyStatusMode() )
			{
				if ( !TFGameRules()->IsPlayerReady( me->entindex() ) )
				{
					TFGameRules()->PlayerReadyStatus_UpdatePlayerState( me, true );
				}
			}

			return Continue();
		}

		if ( m_path.IsValid() )
		{
			m_path.Update( me );
		}
		else
		{
			m_repathTimer.Start( 0.8f );
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefender::OnContact( CTFBot *me, CBaseEntity *other, CGameTrace *result )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefender::OnStuck( CTFBot *me )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefender::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefender::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotMVMDefendBomb::CTFBotMVMDefendBomb( void )
{
	m_bomb       = NULL;
	m_carrier    = NULL;
	m_bombPos    = vec3_origin;
	m_standPos   = vec3_origin;
	m_bHasTarget = false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefendBomb::IsPossible( CTFBot *me )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return false;

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return false;

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = gEntList.FindEntityByClassname( pEnt, "item_teamflag" ) ) != NULL )
	{
		CCaptureFlag *pFlag = dynamic_cast< CCaptureFlag * >( pEnt );
		if ( !pFlag )
			continue;

		Vector flagPos = pFlag->WorldSpaceCenter();

		// Case 1: bomb is being carried by a living player outside invader spawn
		if ( pFlag->IsStolen() )
		{
			CTFPlayer *pCarrier = ToTFPlayer( pFlag->GetOwnerEntity() );
			if ( !pCarrier || !pCarrier->IsAlive() )
				continue;

			if ( IsPointInsideInvaderSpawnRoom( pCarrier->WorldSpaceCenter() ) )
				continue;

			CNavArea *pArea = TheNavMesh->GetNearestNavArea( pCarrier->WorldSpaceCenter() );
			if ( IsNavAreaInsideInvaderSpawnRoom( pArea ) )
				continue;

			return true;
		}

		// Case 2: bomb is on the ground and outside invader spawn
		if ( !IsPointInsideInvaderSpawnRoom( flagPos ) )
		{
			CNavArea *pArea = TheNavMesh->GetNearestNavArea( flagPos );
			if ( !IsNavAreaInsideInvaderSpawnRoom( pArea ) )
				return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotMVMDefendBomb::UpdateBombTarget( CTFBot *me )
{
	m_bomb       = NULL;
	m_carrier    = NULL;
	m_bHasTarget = false;

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = gEntList.FindEntityByClassname( pEnt, "item_teamflag" ) ) != NULL )
	{
		CCaptureFlag *pFlag = dynamic_cast< CCaptureFlag * >( pEnt );
		if ( !pFlag )
			continue;

		m_bomb    = pFlag;
		m_bombPos = pFlag->WorldSpaceCenter();

		if ( pFlag->IsStolen() )
		{
			CTFPlayer *pCarrier = ToTFPlayer( pFlag->GetOwnerEntity() );
			if ( pCarrier && pCarrier->IsAlive() &&
				 !IsPointInsideInvaderSpawnRoom( pCarrier->WorldSpaceCenter() ) )
			{
				m_carrier    = pCarrier;
				m_bHasTarget = true;
				return;
			}
		}
		else
		{
			// Dropped bomb that is still outside spawn
			if ( !IsPointInsideInvaderSpawnRoom( m_bombPos ) )
			{
				m_bHasTarget = true;
				return;
			}
		}
	}
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
Vector CTFBotMVMDefendBomb::SelectStandPosAround( const Vector &center, float minRadius, float maxRadius ) const
{
	CNavArea *pSeed = TheNavMesh->GetNearestNavArea( center, false, 600.0f, false, true, TEAM_ANY );
	if ( !pSeed )
		return center;

	CUtlVector< CNavArea * > nearby;
	CollectSurroundingAreas( &nearby, pSeed, maxRadius,
		50.0f, 200.0f );

	CUtlVector< CNavArea * > valid;
	for ( int i = 0; i < nearby.Count(); ++i )
	{
		CNavArea *pArea = nearby[i];
		if ( !pArea || IsNavAreaInsideInvaderSpawnRoom( pArea ) )
			continue;

		float dist = ( pArea->GetCenter() - center ).AsVector2D().Length();
		if ( dist < minRadius || dist > maxRadius )
			continue;

		valid.AddToTail( pArea );
	}

	if ( valid.Count() == 0 )
		return center;

	return valid[ RandomInt( 0, valid.Count() - 1 ) ]->GetCenter();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMDefendBomb::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Invalidate();
	m_bomb       = NULL;
	m_carrier    = NULL;
	m_standPos   = vec3_origin;
	m_bHasTarget = false;

	UpdateBombTarget( me );

	if ( !m_bHasTarget )
		return Done( "No bomb / carrier to defend against" );

	// Initial stand position
	if ( m_carrier )
		m_standPos = SelectStandPosAround( m_carrier->WorldSpaceCenter(), 180.0f, 420.0f );
	else
		m_standPos = SelectStandPosAround( m_bombPos, 120.0f, 350.0f );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
// Doesn't seem to work all the time.
bool CTFBotMVMDefendBomb::ShouldFocusTank( CTFBot *me ) const
{
	// Check if our tank currently exists
	CBaseEntity *pTank = gEntList.FindEntityByClassname( NULL, "tank_boss" );
	if ( !pTank )
		return false;

	// Pyros will ALWAYS focus the tank
	if ( me->GetPlayerClass()->GetClassIndex() == TF_CLASS_PYRO )
		return true;

	// Gather all active defender bots to make 50/50 split to fight the tank or defend/attack the bomb carrier.
	CUtlVector< CTFBot * > defenders;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || !pPlayer->IsBot() || pPlayer->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
			continue;

		CTFBot *pBot = dynamic_cast< CTFBot * >( pPlayer );
		if ( pBot )
			defenders.AddToTail( pBot );
	}

	int totalDefenders = defenders.Count();
	if ( totalDefenders <= 0 )
		return false;

	// Count the non-Pyro defender bots
	CUtlVector< CTFBot * > Defenders;
	int Pyros = 0;
	for ( int i = 0; i < totalDefenders; ++i )
	{
		if ( defenders[i]->GetPlayerClass()->GetClassIndex() == TF_CLASS_PYRO )
			Pyros++;
		else
			Defenders.AddToTail( defenders[i] );
	}

	int targetTankGroupSize = totalDefenders / 2;
	int nonPyrosNeededForTank = clamp( targetTankGroupSize - Pyros, 0, Defenders.Count() );

	int meIndex = -1;
	for ( int i = 0; i < Defenders.Count(); ++i )
	{
		if ( Defenders[i] == me )
		{
			meIndex = i;
			break;
		}
	}

	if ( meIndex >= 0 && meIndex < nonPyrosNeededForTank )
		return true;

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMDefendBomb::Update( CTFBot *me, float interval )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	// No wave in progress, and we have enough currency to buy an upgrade, so go do that.
    if ( CTFBotMVMBuyUpgrades::IsPossible( me ) )
    	return ChangeTo( new CTFBotMVMBuyUpgrades(), "Between waves, buy upgrades" );

	// Scouts should go look for currency
    if ( TryCollectCurrency( me, m_path, m_repathTimer ) )
		return Continue();

	UpdateBombTarget( me );

	// Threat gone, return to normal approach-zone defence
	if ( !m_bHasTarget || !CTFBotMVMDefendBomb::IsPossible( me ) )
		return ChangeTo( new CTFBotMVMDefender(), "Bomb threat gone, resume normal defence" );

	Vector focusPos;
	bool bFocusingTank = ShouldFocusTank( me );

	if ( bFocusingTank )
	{
		CBaseEntity *pTank = gEntList.FindEntityByClassname( NULL, "tank_boss" );
		if ( pTank )
		{
			focusPos = pTank->WorldSpaceCenter();
		}
		else
		{
			bFocusingTank = false; // Tank is gone, fallback to bomb carrier
			focusPos = ( m_carrier != NULL ) ? m_carrier->WorldSpaceCenter() : m_bombPos;
		}
	}
	else
	{
		if ( m_carrier != NULL )
		{
			focusPos = m_carrier->WorldSpaceCenter();
		}
		else
		{
			focusPos = m_bombPos;
		}
	}

	const float flArrived = 140.0f;
	if ( m_repathTimer.IsElapsed() ||
		 ( me->GetAbsOrigin() - m_standPos ).AsVector2D().LengthSqr() < ( flArrived * flArrived ) )
	{
		if ( bFocusingTank )
		{
			// Wander around and flank the tank boss closely
			m_standPos = SelectStandPosAround( focusPos, 100.0f, 300.0f );
		}
		else if ( m_carrier )
		{
			m_standPos = SelectStandPosAround( focusPos, 180.0f, 780.0f );
		}
		else
		{
			m_standPos = SelectStandPosAround( focusPos, 100.0f, 950.0f );
		}

		CTFBotPathCost cost( me, FASTEST_ROUTE );
		m_path.Compute( me, m_standPos, cost, 0.0f, true );
		m_repathTimer.Start( bFocusingTank ? RandomFloat( 1.0f, 2.0f ) : ( m_carrier ? RandomFloat( 1.2f, 2.2f ) : RandomFloat( 2.0f, 3.5f ) ) );
	}

	if ( m_path.IsValid() )
		m_path.Update( me );
	else
		m_repathTimer.Start( 0.5f );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefendBomb::OnStuck( CTFBot *me )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMDefendBomb::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

EventDesiredResult< CTFBot > CTFBotMVMDefendBomb::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
QueryResultType CTFBotMVMDefendBomb::ShouldAttack( const INextBot *me, const CKnownEntity *them ) const
{
	return ANSWER_YES;
}

//---------------------------------------------------------------------------------------------
// CTFBotMVMBuyUpgrades
//---------------------------------------------------------------------------------------------
CTFBotMVMBuyUpgrades::CTFBotMVMBuyUpgrades( void )
{
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMBuyUpgrades::IsPossible( CTFBot *me )
{
	if ( !TFGameRules() || !TFObjectiveResource() )
		return false;

	// Only between waves
	if ( !TFObjectiveResource()->GetMannVsMachineIsBetweenWaves() )
		return false;

	// Must have at least one upgrade station on the map
	if ( gEntList.FindEntityByClassname( NULL, "func_upgradestation" ) == NULL )
		return false;

	// Make sure we can actually afford anythng
	if ( !HasAffordableUpgrade( me ) )
		return false;

	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMBuyUpgrades::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Invalidate();
	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMBuyUpgrades::Update( CTFBot *me, float interval )
{
	if ( !TFGameRules() || !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	if ( !IsPossible( me ) )
	{
		if ( CTFBotMVMDefendBomb::IsPossible( me ) )
			return ChangeTo( new CTFBotMVMDefendBomb(), "Bomb carrier active" );

		return ChangeTo( new CTFBotMVMDefender(), "Finished upgrading, wave started" );
	}

	// Find nearest upgrade station
	CBaseEntity *pClosestStation = NULL;
	float flMinDistSqr = FLT_MAX;
	Vector myPos = me->GetAbsOrigin();

	CBaseEntity *pEntity = NULL;
	while ( ( pEntity = gEntList.FindEntityByClassname( pEntity, "func_upgradestation" ) ) != NULL )
	{
		float flDistSqr = ( pEntity->WorldSpaceCenter() - myPos ).LengthSqr();
		if ( flDistSqr < flMinDistSqr )
		{
			flMinDistSqr = flDistSqr;
			pClosestStation = pEntity;
		}
	}

	if ( !pClosestStation )
		return ChangeTo( new CTFBotMVMDefender(), "No upgrade station found" );

	// Already inside the station volume?
	bool bIsInsideStation = IsPointInsideEntityBounds( myPos, pClosestStation )
						 || me->m_Shared.IsInUpgradeZone();

	if ( !bIsInsideStation )
	{
		// Path to a walkable point inside the brush
		if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
		{
			Vector goal;
			if ( !SelectUpgradeStationGoal( pClosestStation, goal ) )
				return ChangeTo( new CTFBotMVMDefender(), "Could not find goal to upgrade station" );

			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, goal, cost, 0.0f, true );
			m_repathTimer.Start( 1.0f );
		}

		if ( m_path.IsValid() )
		{
			m_path.Update( me );
			return Continue();
		}

		return Continue();
	}

	// We are inside the upgrade station, so we can start buying upgrades.
	me->m_Shared.SetInUpgradeZone( true );

	if ( g_MannVsMachineUpgrades.m_Upgrades.Count() > 0 && g_hUpgradeEntity.Get() )
	{
		me->BeginPurchasableUpgrades();

		bool bSuccess = false;
		for ( int attempt = 0; attempt < 8; ++attempt )
		{
			int nRandomUpgradeIndex = RandomInt( 0, g_MannVsMachineUpgrades.m_Upgrades.Count() - 1 );
			const CMannVsMachineUpgrades &upgradeDef = g_MannVsMachineUpgrades.m_Upgrades[nRandomUpgradeIndex];

			int iTargetSlot;
			if ( upgradeDef.nUIGroup == UIGROUP_UPGRADE_ATTACHED_TO_PLAYER )
			{
				iTargetSlot = LOADOUT_POSITION_INVALID;
			}
			else if ( upgradeDef.nUIGroup == UIGROUP_POWERUPBOTTLE )
			{
				iTargetSlot = LOADOUT_POSITION_ACTION;
			}
			else
			{
				iTargetSlot = RandomInt( LOADOUT_POSITION_PRIMARY, LOADOUT_POSITION_MELEE );
			}

			bSuccess = g_hUpgradeEntity->PlayerPurchasingUpgrade(
				me, iTargetSlot, nRandomUpgradeIndex, false, false, false );

			if ( bSuccess )
				break;
		}

		me->EndPurchasableUpgrades();

		if ( bSuccess )
		{
			m_repathTimer.Start( 2.0f );
			return Continue();
		}
	}

	m_repathTimer.Start( 1.0f );
	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMBuyUpgrades::OnStuck( CTFBot *me )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMBuyUpgrades::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMVMBuyUpgrades::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}