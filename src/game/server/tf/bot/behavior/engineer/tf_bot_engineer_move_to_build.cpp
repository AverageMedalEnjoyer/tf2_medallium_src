//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_move_to_build.cpp
// Engineer moving into position to build
// Michael Booth, February 2009

#include "cbase.h"
#include "nav_mesh/tf_nav_mesh.h"
#include "tf_player.h"
#include "tf_gamerules.h"
#include "tf_obj_sentrygun.h"
#include "tf_weapon_builder.h"
#include "team_train_watcher.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_build.h"
#include "bot/behavior/engineer/tf_bot_engineer_move_to_build.h"
#include "bot/behavior/engineer/tf_bot_engineer_building.h"
#include "bot/map_entities/tf_bot_hint_sentrygun.h"
#include "bot/behavior/tf_bot_get_ammo.h"
#include "bot/behavior/tf_bot_retreat_to_cover.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_teleport_entrance.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_teleport_exit.h"
#include "bot/behavior/tf_bot_mvm_defenders.h"
#include "trigger_area_capture.h"
#include "entity_capture_flag.h"

#include "raid/tf_raid_logic.h"


extern ConVar tf_bot_path_lookahead_range;

ConVar tf_bot_debug_sentry_placement( "tf_bot_debug_sentry_placement", "0", FCVAR_CHEAT );
ConVar tf_bot_max_teleport_exit_travel_to_point( "tf_bot_max_teleport_exit_travel_to_point", "2500", FCVAR_CHEAT, "In an offensive engineer bot's tele exit is farther from the point than this, destroy it" );
ConVar tf_bot_min_teleport_travel( "tf_bot_min_teleport_travel", "3000", FCVAR_CHEAT, "Minimum travel distance between teleporter entrance and exit before engineer bot will build one" );

//--------------------------------------------------------------------------------------------------------
static Vector s_pointCentroid;

int CompareRangeToPoint( CTFNavArea * const *area1, CTFNavArea * const *area2 )
{
	float d1 = ( (*area1)->GetCenter() - s_pointCentroid ).LengthSqr();
	float d2 = ( (*area2)->GetCenter() - s_pointCentroid ).LengthSqr();

	// reversed so farthest is sorted first in the vector
	if ( d1 < d2 )
		return 1;

	if ( d1 > d2 )
		return -1;

	return 0;
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerMoveToBuild::CollectBuildAreas( CTFBot *me )
{
	// if we have a predesignated build area, we're done
	if ( me->GetHomeArea() )
		return;

	m_sentryAreaVector.RemoveAll();

	CUtlVector< CTFNavArea * > pointAreaVector;
	Vector pointCentroid = vec3_origin;
	float pointEnemyIncursion = 0.0f;
	int i;

	int myTeam = me->GetTeamNumber();
	int enemyTeam = ( myTeam == TF_TEAM_BLUE ) ? TF_TEAM_RED : TF_TEAM_BLUE;

	CCaptureZone *zone = me->GetFlagCaptureZone();
	if ( zone )
	{
		// NOTE: Not strictly the right thing - should defend location of our team's flag
		CTFNavArea *zoneArea = (CTFNavArea *)TheTFNavMesh()->GetNearestNavArea( zone->WorldSpaceCenter(), false, 500.0f, true );
		if ( zoneArea )
		{
			pointAreaVector.AddToTail( zoneArea );
			pointCentroid += zoneArea->GetCenter();
			pointEnemyIncursion += zoneArea->GetIncursionDistance( enemyTeam );
		}
	}
	else if ( TFGameRules()->GetGameType() == TF_GAMETYPE_ESCORT )
	{
		CTeamTrainWatcher *trainWatcher;

		if ( myTeam == TF_TEAM_BLUE )
		{
			trainWatcher = TFGameRules()->GetPayloadToPush( me->GetTeamNumber() );
		}
		else
		{
			trainWatcher = TFGameRules()->GetPayloadToBlock( me->GetTeamNumber() );
		}

		if ( trainWatcher )
		{
			Vector checkpointPos = trainWatcher->GetNextCheckpointPosition();

			CTFNavArea *checkpointArea = (CTFNavArea *)TheTFNavMesh()->GetNearestNavArea( checkpointPos, false, 500.0f, true );
			if ( checkpointArea )
			{
				pointAreaVector.AddToTail( checkpointArea );
				pointCentroid += checkpointArea->GetCenter();
				pointEnemyIncursion += checkpointArea->GetIncursionDistance( enemyTeam );
			}
		}
	}
	else
	{
		// collect all areas overlapping the point
		CTeamControlPoint *ctrlPoint = me->GetMyControlPoint();
		if ( !ctrlPoint )
			return;

		const CUtlVector< CTFNavArea * > *ctrlPointAreaVector = TheTFNavMesh()->GetControlPointAreas( ctrlPoint->GetPointIndex() );

		if ( ctrlPointAreaVector )
		{
			for( i=0; i<ctrlPointAreaVector->Count(); ++i )
			{
				CTFNavArea *area = ctrlPointAreaVector->Element(i);

				pointAreaVector.AddToTail( area );
				pointCentroid += area->GetCenter();
				pointEnemyIncursion += area->GetIncursionDistance( enemyTeam );
			}
		}
	}

	if ( pointAreaVector.Count() == 0 )
		return;

	pointCentroid /= pointAreaVector.Count();
	pointEnemyIncursion /= pointAreaVector.Count();


	// collect all areas that can see the point
	CUtlVector< CTFNavArea * > exposedAreaVector;
	for( i=0; i<pointAreaVector.Count(); ++i )
	{
		CTFAreaCollector collect;
		pointAreaVector[i]->ForAllPotentiallyVisibleAreas( collect );

		for( int j=0; j<collect.m_vector.Count(); ++j )
		{
			CTFNavArea *visibleArea = collect.m_vector[j];


			if ( visibleArea->GetIncursionDistance( myTeam ) < 0 || visibleArea->GetIncursionDistance( enemyTeam ) < 0 )
				continue;

			if ( TFGameRules()->IsInKothMode() )
			{
				// ignore areas the enemy can reach first
				if ( visibleArea->GetIncursionDistance( myTeam ) >= visibleArea->GetIncursionDistance( enemyTeam ) )
					continue;
			}

// incursion flow is badly behaved at cap #1, stage #2 in dustbowl
// 			else
// 			{
// 				if ( pointEnemyIncursion > visibleArea->GetIncursionDistance( enemyTeam ) )
// 					continue;
// 			}

			if ( TFGameRules()->GetGameType() == TF_GAMETYPE_CP )
			{
				// don't build directly on the point
				if ( visibleArea->HasAttributeTF( TF_NAV_CONTROL_POINT ) )
					continue;

				// ignore areas below the point
				const float tooFarBelow = 150.0f;
				if ( visibleArea->GetCenter().z < pointCentroid.z - tooFarBelow )
					continue;

				// ignore areas too far from the point for the sentry gun to reach
				const float tolerance = 1.1f;
				if ( ( visibleArea->GetCenter() - pointCentroid ).IsLengthGreaterThan( SENTRY_MAX_RANGE * tolerance ) )
					continue;
			}

			// ignore areas that don't have clear line of FIRE (not sight)
			const float sentryEyeHeight = 60.0f;
			const float pointFlagHeight = 70.0f; // 100.0f;
			if ( !me->IsLineOfFireClear( visibleArea->GetCenter() + Vector( 0, 0, sentryEyeHeight ), pointCentroid + Vector( 0, 0, pointFlagHeight ) ) )
				continue;

			if ( !exposedAreaVector.HasElement( visibleArea ) )
				exposedAreaVector.AddToTail( visibleArea );
		}
	}

	// keep the farthest away areas
	const float keepRatio = 1.0f; // 0.5f;
	s_pointCentroid = pointCentroid;
	exposedAreaVector.Sort( CompareRangeToPoint );

	for( i=0; i<exposedAreaVector.Count() * keepRatio; ++i )
	{
		CTFNavArea *usableArea = exposedAreaVector[i];

		m_sentryAreaVector.AddToTail( usableArea );
	}

	// calculate total surface area
	m_totalSurfaceArea = 0.0f;
	FOR_EACH_VEC( m_sentryAreaVector, it )
	{
		CTFNavArea *area = m_sentryAreaVector[ it ];

		m_totalSurfaceArea += area->GetSizeX() * area->GetSizeY();

		if ( tf_bot_debug_sentry_placement.GetBool() )
		{
			TheNavMesh->AddToSelectedSet( area );
		}
	}
}


//---------------------------------------------------------------------------------------------
/**
 * Doesn't recompute the potential areas, just reselected from the list
 */
void CTFBotEngineerMoveToBuild::SelectBuildLocation( CTFBot *me )
{
	m_path.Invalidate();

	m_sentryBuildHint = NULL;
	m_sentryBuildLocation = vec3_origin;


	// if we have a build spot, use it
	if ( me->GetHomeArea() )
	{
		m_sentryBuildLocation = me->GetHomeArea()->GetCenter();
		return;
	}

	// MVM defender Engineers: Build near the invader spawn room, the bomb carrier, or the bomb if it's dropped.
    if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() &&
         me->GetTeamNumber() == TF_TEAM_PVE_DEFENDERS )
    {
        CBaseObject *pSentry    = me->GetObjectOfType( OBJ_SENTRYGUN );
        CBaseObject *pDispenser = me->GetObjectOfType( OBJ_DISPENSER );

        if ( !pSentry && !pDispenser )
        {
            CCaptureFlag *pBomb   = NULL;
            CTFPlayer    *pCarrier = NULL;
            Vector        bombPos  = vec3_origin;
            bool          bCarried = false;

            for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "item_teamflag" );
                  pEnt;
                  pEnt = gEntList.FindEntityByClassname( pEnt, "item_teamflag" ) )
            {
                CCaptureFlag *pFlag = dynamic_cast< CCaptureFlag * >( pEnt );
                if ( !pFlag )
                    continue;

                Vector flagPos = pFlag->WorldSpaceCenter();

                if ( pFlag->IsStolen() )
                {
                    CTFPlayer *pOwner = ToTFPlayer( pFlag->GetOwnerEntity() );
                    if ( pOwner && pOwner->IsAlive() &&
                         !IsPointInsideInvaderSpawnRoom( pOwner->WorldSpaceCenter() ) )
                    {
                        CNavArea *pArea = TheNavMesh->GetNearestNavArea( pOwner->WorldSpaceCenter() );
                        if ( !IsNavAreaInsideInvaderSpawnRoom( pArea ) )
                        {
                            pBomb    = pFlag;
                            pCarrier = pOwner;
                            bombPos  = pOwner->WorldSpaceCenter();
                            bCarried = true;
                            break;
                        }
                    }
                }
                else if ( !IsPointInsideInvaderSpawnRoom( flagPos ) )
                {
                    CNavArea *pArea = TheNavMesh->GetNearestNavArea( flagPos );
                    if ( !IsNavAreaInsideInvaderSpawnRoom( pArea ) )
                    {
                        pBomb   = pFlag;
                        bombPos = flagPos;
                        bCarried = false;
                        break;
                    }
                }
            }

            if ( pBomb )
            {
                // Choose radius based on whether the bomb is being carried
                float flMinRadius, flMaxRadius;
                if ( bCarried )
                {
                    flMinRadius = 450.0f;
                    flMaxRadius = 850.0f;
                }
                else
                {
                    // dropped, random range of 400
                    flMinRadius = 0.0f;
                    flMaxRadius = 400.0f;
                }

				// Attempt to find a valid build location around the bomb
                for ( int attempt = 0; attempt < 48; ++attempt )
                {
                    float angle = RandomFloat( 0.0f, 6.2831853f );
                    float dist  = RandomFloat( flMinRadius, flMaxRadius );
                    Vector offset( cosf( angle ) * dist, sinf( angle ) * dist, 0.0f );
                    Vector candidate = bombPos + offset;

                    CNavArea *pArea = TheNavMesh->GetNearestNavArea( candidate,
                                                                     false, 600.0f, false, true, TEAM_ANY );
                    if ( !pArea )
                        continue;

                    // Never build inside an invader spawnroom
                    if ( IsNavAreaInsideInvaderSpawnRoom( pArea ) )
                        continue;

                    if ( IsPointInsideInvaderSpawnRoom( pArea->GetCenter() ) )
                        continue;

                    Vector buildPos = pArea->GetCenter();
                    buildPos.z += 8.0f;

                    m_sentryBuildLocation = buildPos;
                    return;
                }
            }
        }

		CUtlVector< CBaseEntity * > rooms;
		for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "func_respawnroom" );
			  pEnt;
			  pEnt = gEntList.FindEntityByClassname( pEnt, "func_respawnroom" ) )
		{
			if ( pEnt->GetTeamNumber() == TF_TEAM_PVE_INVADERS )
				rooms.AddToTail( pEnt );
		}

		if ( rooms.Count() > 0 )
		{
			for ( int attempt = 0; attempt < 64; ++attempt )
			{
				CBaseEntity *pRoom = rooms[ RandomInt( 0, rooms.Count() - 1 ) ];
				Vector roomOrigin = pRoom->WorldSpaceCenter();

				// Get the actual brush bounds
				Vector mins, maxs;
				pRoom->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

				float angle = RandomFloat( 0.0f, 6.2831853f );
				float dist  = RandomFloat( 420.0f, 1100.0f );	// a bit farther so we stay clear of walls
				Vector offset( cosf( angle ) * dist, sinf( angle ) * dist, 0.0f );
				Vector candidate = roomOrigin + offset;

				CNavArea *pArea = TheNavMesh->GetNearestNavArea( candidate,
																 false, 700.0f, false, true, TEAM_ANY );
				if ( !pArea )
					continue;

				// Never build inside a spawnroom nav area
				CTFNavArea *pTFArea = (CTFNavArea *)pArea;
				if ( pTFArea && ( pTFArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) ||
								  pTFArea->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED ) ) )
					continue;

				Vector buildPos = pArea->GetCenter();
				buildPos.z += 8.0f;

				// Also reject if the candidate is still inside an expanded version of the room brush
				Vector expandedMins = mins - Vector( 80, 80, 40 );
				Vector expandedMaxs = maxs + Vector( 80, 80, 40 );
				if ( buildPos.x >= expandedMins.x && buildPos.x <= expandedMaxs.x &&
					 buildPos.y >= expandedMins.y && buildPos.y <= expandedMaxs.y &&
					 buildPos.z >= expandedMins.z && buildPos.z <= expandedMaxs.z )
				{
					continue;
				}

				Vector testPoints[5];
				testPoints[0] = roomOrigin;
				testPoints[1] = Vector( mins.x, mins.y, (mins.z + maxs.z) * 0.5f );
				testPoints[2] = Vector( maxs.x, mins.y, (mins.z + maxs.z) * 0.5f );
				testPoints[3] = Vector( mins.x, maxs.y, (mins.z + maxs.z) * 0.5f );
				testPoints[4] = Vector( maxs.x, maxs.y, (mins.z + maxs.z) * 0.5f );

				bool bCanSeeBrush = false;
				Vector eye = buildPos + Vector( 0, 0, 60 );

				for ( int t = 0; t < 5; ++t )
				{
					trace_t tr;
					UTIL_TraceLine( eye, testPoints[t] + Vector( 0, 0, 20 ),
									MASK_SOLID_BRUSHONLY, me, COLLISION_GROUP_NONE, &tr );

					if ( tr.fraction > 0.85f )
					{
						bCanSeeBrush = true;
						break;
					}
				}

				if ( !bCanSeeBrush )
					continue;

				// Spot that can see the invader spawn brush
				m_sentryBuildLocation = buildPos;
				return;
			}
		}

		// Fallback
		// Note: Seems to break Engineer bots in a few MVM maps, where they will attempt to build inside of spawn. Not sure why this happens yet.
		m_sentryBuildLocation = me->GetAbsOrigin();
		return;
	}

	// if we have a set of specific build locations, pick one of them
	CUtlVector< CTFBotHintSentrygun * > sentryHintVector;

	CTFBotHintSentrygun *sentryHint;
	for( sentryHint = static_cast< CTFBotHintSentrygun * >( gEntList.FindEntityByClassname( NULL, "bot_hint_sentrygun" ) );
		 sentryHint;
		 sentryHint = static_cast< CTFBotHintSentrygun * >( gEntList.FindEntityByClassname( sentryHint, "bot_hint_sentrygun" ) ) )
	{
		// clear the previous owner if it is us
		if ( sentryHint->GetPlayerOwner() == me )
		{
			sentryHint->SetPlayerOwner( NULL );
		}
		if ( sentryHint->IsAvailableForSelection( me ) )
		{
			sentryHintVector.AddToTail( sentryHint );
		}
	}

	if ( sentryHintVector.Count() > 0 )
	{
		int which = RandomInt( 0, sentryHintVector.Count()-1 );

		m_sentryBuildHint = sentryHintVector[ which ];
		m_sentryBuildHint->SetPlayerOwner( me );
		m_sentryBuildLocation = m_sentryBuildHint->GetAbsOrigin();

		return;
	}


	// collect nav area candidates
	CollectBuildAreas( me );

	// choose based on surface area to avoid biasing finely subdivided areas of the mesh
	float which = RandomFloat( 0.0f, m_totalSurfaceArea - 1.0f );
	float soFar = 0.0f;
	FOR_EACH_VEC( m_sentryAreaVector, sit )
	{
		CTFNavArea *area = m_sentryAreaVector[ sit ];

		soFar += area->GetSizeX() * area->GetSizeY();

		if ( which < soFar )
		{
			m_sentryBuildLocation = area->GetRandomPoint();
			return;
		}
	}

	if ( !HushAsserts() )
	{
		Assert( !"Failed to find a build location" );
	}
	m_sentryBuildLocation = me->GetAbsOrigin();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerMoveToBuild::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

#ifdef TF_RAID_MODE
	if ( TFGameRules()->IsRaidMode() )
	{
		if ( me->GetHomeArea() && TFGameRules()->GetRaidLogic() )
		{
			// try to pick a new area
			CTFNavArea *sentryArea = TFGameRules()->GetRaidLogic()->SelectRaidSentryArea();
			if ( sentryArea )
			{
				me->SetHomeArea( sentryArea );
			}
		}
	}
#endif // TF_RAID_MODE

	SelectBuildLocation( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerMoveToBuild::Update( CTFBot *me, float interval )
{
	if ( m_fallBackTimer.HasStarted() )
	{
		if ( m_fallBackTimer.IsElapsed() )
		{
			SelectBuildLocation( me );
			m_fallBackTimer.Invalidate();
		}
		else
		{
			// wait a moment while we decide where to build near fallback point
			return Continue();
		}
	}

	CBaseObject	*mySentry = me->GetObjectOfType( OBJ_SENTRYGUN );
	if ( mySentry )
	{
		// we already have a sentry from a previous life - continue what we were doing

		// if we used a sentry hint last time, reuse it
		CTFBotHintSentrygun *sentryHint;
		for( sentryHint = static_cast< CTFBotHintSentrygun * >( gEntList.FindEntityByClassname( NULL, "bot_hint_sentrygun" ) );
			 sentryHint;
			 sentryHint = static_cast< CTFBotHintSentrygun * >( gEntList.FindEntityByClassname( sentryHint, "bot_hint_sentrygun" ) ) )
		{
			if ( sentryHint->GetPlayerOwner() == me )
			{
				return ChangeTo( new CTFBotEngineerBuilding( sentryHint ), "Going back to my existing sentry nest and reusing a sentry hint" );
			}
		}

		return ChangeTo( new CTFBotEngineerBuilding, "Going back to my existing sentry nest" );
	}

	// offensive engineers need to place a forward teleporter
	if ( ( TFGameRules()->IsAttackDefenseMode() && me->GetTeamNumber() == TF_TEAM_BLUE ) ||
		 ( TFGameRules()->GetGameType() == TF_GAMETYPE_CP && !TFGameRules()->IsAttackDefenseMode() && !TFGameRules()->IsInKothMode() ) )
	{
		CObjectTeleporter *myTeleportExit = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_EXIT );
		int myTeam = me->GetTeamNumber();

		if ( myTeleportExit )
		{
			// if exit is too far from the point, destroy it and try again
			CTeamControlPoint *point = me->GetMyControlPoint();
			if ( point )
			{
				CTFNavArea *pointArea = TheTFNavMesh()->GetControlPointCenterArea( point->GetPointIndex() );

				myTeleportExit->UpdateLastKnownArea();
				CTFNavArea *exitArea = (CTFNavArea *)myTeleportExit->GetLastKnownArea();

				if ( pointArea && exitArea )
				{
					float travelToPoint = fabs( exitArea->GetIncursionDistance( myTeam ) - pointArea->GetIncursionDistance( myTeam ) );

					if ( travelToPoint > tf_bot_max_teleport_exit_travel_to_point.GetFloat() )
					{
						// too far, destroy it
						myTeleportExit->DestroyObject();
						myTeleportExit = NULL;
					}
				}
			}
		}
		else
		{
			CObjectTeleporter *myTeleportEntrance = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_ENTRANCE );
			CTFNavArea *myArea = me->GetLastKnownArea();

			bool shouldBuildExit = true;

			// if we have a teleporter entrance, don't place the exit too close to it
			if ( myTeleportEntrance && myArea )
			{
				myTeleportEntrance->UpdateLastKnownArea();
				CTFNavArea *enterArea = (CTFNavArea *)myTeleportEntrance->GetLastKnownArea();

				if ( enterArea )
				{
					float travelBetween = fabs( enterArea->GetIncursionDistance( myTeam ) - myArea->GetIncursionDistance( myTeam ) );

					if ( travelBetween < tf_bot_min_teleport_travel.GetFloat() )
					{
						shouldBuildExit = false;
					}
				}
			}

			if ( shouldBuildExit )
			{
				// no exit yet - need to place one
				// when we see the enemy, retreat to cover and build the exit there
				if ( me->GetVisionInterface()->GetPrimaryKnownThreat( true ) )
				{
					if ( !me->m_Shared.InCond( TF_COND_INVULNERABLE ) && ShouldRetreat( me ) != ANSWER_NO )
					{
						Action< CTFBot > *nextActionWhenInCover = new CTFBotEngineerBuildTeleportExit;
						return SuspendFor( new CTFBotRetreatToCover( nextActionWhenInCover ), "Retreating to a safe place to build my teleporter exit" );
					}
				}
			}
		}
	}

    // MVM defender Engineers: Place a teleporter exit near the sentry once we have one
	if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() &&
		 me->GetTeamNumber() == TF_TEAM_PVE_DEFENDERS )
	{
		CObjectTeleporter *pExit = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_EXIT );
		CBaseObject *pSentry = me->GetObjectOfType( OBJ_SENTRYGUN );
		CBaseObject *pDispenser = me->GetObjectOfType( OBJ_DISPENSER );

		if ( !pExit && pSentry && pDispenser )
		{
			// We already have a sentry and dispenser up front, go place an exit nearby
			// Note: Doesn't seem to work.
			return SuspendFor( new CTFBotEngineerBuildTeleportExit, "Placing teleporter exit near sentry" );
		}
	}

	// move to build position
	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

		CTFBotPathCost cost( me, SAFEST_ROUTE );
		m_path.Compute( me, m_sentryBuildLocation, cost );
	}

	Vector forward;
	me->EyeVectors( &forward );
	forward.z = 0.0f;
	forward.NormalizeInPlace();

	Vector myBlueprintPosition = me->GetAbsOrigin() + 50.0f * forward;

	const float closeToHome = 25.0f;
	Vector toBuild = m_sentryBuildLocation - myBlueprintPosition;
	Vector toMe = m_sentryBuildLocation - me->GetAbsOrigin();

	if ( me->GetLocomotionInterface()->IsOnGround() )
	{
		// we need to wait until we're on the ground since the Build action assumes our position OnStart is where we are going to build
		if ( toMe.AsVector2D().IsLengthLessThan( closeToHome ) || toBuild.AsVector2D().IsLengthLessThan( closeToHome ) )
		{
			if ( m_sentryBuildHint != NULL )
			{
				return ChangeTo( new CTFBotEngineerBuilding( m_sentryBuildHint ), "Reached my precise build location" );
			}

			return ChangeTo( new CTFBotEngineerBuilding, "Reached my build location" );
		}

		m_path.Update( me );
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerMoveToBuild::OnStuck( CTFBot *me )
{
//	SelectBuildLocation( me );
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerMoveToBuild::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerMoveToBuild::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	SelectBuildLocation( me );

	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerMoveToBuild::OnTerritoryLost( CTFBot *me, int territoryID )
{
	// we have to wait a moment until contested point changes to select a new build spot
	m_fallBackTimer.Start( 0.2f );

	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerMoveToBuild::OnTerritoryCaptured( CTFBot *me, int territoryID )
{
	// we have to wait a moment until contested point changes to select a new build spot
	m_fallBackTimer.Start( 0.2f );

	return TryContinue();
}
