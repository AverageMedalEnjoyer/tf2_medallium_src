//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_passtime.cpp
//
// PASSTIME bot behavior
//=============================================================================//

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_gamerules.h"
#include "bot/tf_bot.h"
#include "tf_bot_passtime.h"
#include "triggers.h"
#include "tf_weapon_passtime_gun.h"
#include "tf_passtime_ball.h"

ConVar tf_bot_passtime_escort_distance( "tf_bot_passtime_escort_distance", "132.0f", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"How far away bots will stand while escorting the JACK carrier" );
ConVar tf_bot_debug_passtime_searching( "tf_bot_debug_passtime_searching", "0", FCVAR_CHEAT );

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotGetPasstimeJack::CTFBotGetPasstimeJack( void )
{
	m_path.Invalidate();
	m_jackEntity = NULL;
	m_goalEntity = NULL;
	m_haveJack = false;
	m_lastJackPos = vec3_origin;
	m_scoreMethod = SCORE_WALK;
	m_scoreTarget = NULL;
	m_throwTarget = vec3_origin;
	m_backupPos = vec3_origin;
	m_isChargingThrow = false;
	m_hasSelectedScoreMethod = false;
	m_isPassing = false;
	m_passTarget = NULL;
    m_passLostSightTimer.Invalidate();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
class CPasstimeJackFilter : public INextBotFilter
{
public:
	CPasstimeJackFilter( CTFBot *me ) : m_me( me ), m_area( NULL ) {}

	bool IsSelected( const CBaseEntity *constCandidate ) const
	{
		CBaseEntity *candidate = const_cast< CBaseEntity * >( constCandidate );

		// Not in PASS Time mode
		if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
			return false;

		// No JACK
		if ( !candidate->ClassMatches( "passtime_ball" ) )
			return false;

		// JACK is invisible or gone
		if ( candidate->IsEffectActive( EF_NODRAW ) )
			return false;

		CPasstimeBall *pBall = dynamic_cast< CPasstimeBall * >( candidate );
		if ( pBall && pBall->GetCarrier() )
			return false;

		m_area = (CTFNavArea *)TheNavMesh->GetNearestNavArea( candidate->WorldSpaceCenter() );
		return ( m_area != NULL );
	}

	CTFBot *m_me;
	mutable CTFNavArea *m_area;
};

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotGetPasstimeJack::IsPossible( CTFBot *me )
{
	if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
		return false;

	// Already carrying it
	//if ( me->m_Shared.HasPasstimeBall() )
	//	return false;

	// A teammate has it, switch to escorting/chasing them
	if ( CTFBotFollowPasstimeJackCarrier::IsPossible( me ) )
		return false;

	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CBaseEntity *CTFBotGetPasstimeJack::FindThrowableGoal( const Vector &nearPos, float flMaxDist ) const
{
	float flMaxDistSqr = flMaxDist * flMaxDist;

	CBaseEntity *pEnt = NULL;
	CBaseEntity *pClosest = NULL;
	float flClosest = FLT_MAX;

	while ( ( pEnt = gEntList.FindEntityByClassname( pEnt, "func_respawnroomvisualizer" ) ) != NULL )
	{
		float flDistSqr = ( pEnt->GetAbsOrigin() - nearPos ).LengthSqr();
		if ( flDistSqr < flMaxDistSqr && flDistSqr < flClosest )
		{
			flClosest = flDistSqr;
			pClosest = pEnt;
		}
	}
	return pClosest;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotGetPasstimeJack::SelectScoreMethod( CTFBot *me )
{
	if ( !m_goalEntity )
	{
		m_scoreMethod = SCORE_WALK;
		m_scoreTarget = NULL;
		m_hasSelectedScoreMethod = true;
		return;
	}

	// If the goal we have chosen is within a radius of 128, it is a throw goal. Otherwise, walk it in.
	const float kVeryCloseDist = 128.0f;
	CBaseEntity *pVisualizer = FindThrowableGoal( m_goalEntity->GetAbsOrigin(), kVeryCloseDist );

	if ( pVisualizer )
	{
		m_scoreMethod = SCORE_THROW;
		m_scoreTarget = pVisualizer;
		m_throwTarget = pVisualizer->GetAbsOrigin();

		// Backup position away from the throw goal
		Vector vAway = me->WorldSpaceCenter() - m_throwTarget;
		if ( vAway.LengthSqr() < 1.0f )
			vAway = Vector( RandomFloat( -1.0f, 1.0f ), RandomFloat( -1.0f, 1.0f ), 0.0f );
		vAway.z = 0.0f;
		VectorNormalize( vAway );

		m_backupPos = m_throwTarget + vAway * RandomFloat( 220.0f, 320.0f );

		CNavArea *pArea = TheNavMesh->GetNearestNavArea( m_backupPos );
		if ( pArea )
			m_backupPos = pArea->GetCenter();
	}
	else
	{
		m_scoreMethod = SCORE_WALK;
		m_scoreTarget = NULL;
	}

	m_hasSelectedScoreMethod = true;
	m_isChargingThrow = false;
	m_chargeTimer.Invalidate();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotGetPasstimeJack::GetValidPassTarget( CTFBot *me ) const
{
	if ( !me )
		return NULL;

	const bool bLowHealth = ( me->GetHealth() < 80 );
	CUtlVector< CTFPlayer * > candidates;
	CUtlVector< CTFPlayer * > askers;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );

		// Is our teammate a player? Is our teammate alive? Is our teammate on the other team?
		if ( !pPlayer || pPlayer == me || !pPlayer->IsAlive() || !me->InSameTeam( pPlayer ) )
			continue;

        bool bIsAsker = ( pPlayer->m_Shared.AskForBallTime() > gpGlobals->curtime );

		// Never pass to a Medic, unless they asked for it.
		if ( !bIsAsker && pPlayer->GetPlayerClass()->GetClassIndex() == TF_CLASS_MEDIC )
			continue;

		// Always pass to teammates who are faster then us, unless they asked for it.
        if ( !bIsAsker && pPlayer->MaxSpeed() <= me->MaxSpeed() )
            continue;

		// Rough distance check so we don't try to pass across the whole map
		if ( ( pPlayer->WorldSpaceCenter() - me->WorldSpaceCenter() ).LengthSqr() > ( 1200.0f * 1200.0f ) )
			continue;

		// Pass the ball to our teammate who asked for it
		if ( pPlayer->m_Shared.AskForBallTime() > gpGlobals->curtime )
			askers.AddToTail( pPlayer );
		else
			candidates.AddToTail( pPlayer );
	}

	// If we have a lot of askers, just choose one.
	if ( askers.Count() > 0 )
		return askers[ RandomInt( 0, askers.Count() - 1 ) ];

	if ( candidates.Count() == 0 )
		return NULL;

	// Pass to another teammate if we're about to die.
	// Also pass to a faster teammate (10% chance every check).
	if ( !bLowHealth && RandomInt( 0, 99 ) >= 10 )
		return NULL;

	return candidates[ RandomInt( 0, candidates.Count() - 1 ) ];
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotGetPasstimeJack::StartChargeThrow( CTFBot *me )
{
	if ( !me )
		return;

	m_isChargingThrow = true;
	m_chargeTimer.Start( 1.5f );
	me->PressFireButton( 1.5f );
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotGetPasstimeJack::UpdateChargeThrow( CTFBot *me )
{
	if ( !me || !m_isChargingThrow )
		return;

	Vector aimPos = m_throwTarget;
	if ( m_isPassing && m_passTarget )
		aimPos = m_passTarget->WorldSpaceCenter();

	me->GetBodyInterface()->AimHeadTowards( aimPos, IBody::CRITICAL, 0.5f, NULL, m_isPassing ? "Looking at my pass target!" : "Looking at my score target!" );

	if ( m_chargeTimer.IsElapsed() )
	{
		// For a pass we only release if we can actually see the teammate
		if ( m_isPassing )
		{
			if ( m_passTarget && m_passTarget->IsAlive() && me->IsLineOfSightClear( m_passTarget ) )
			{
				me->ReleaseFireButton();
				m_isChargingThrow = false;
				m_chargeTimer.Invalidate();
				m_isPassing = false;
				m_passTarget = NULL;
			}
			else
			{
				// Still charging
				me->PressFireButton( 0.2f );
			}
		}
		else
		{
			// Normal goal throw
			me->ReleaseFireButton();
			m_isChargingThrow = false;
			m_chargeTimer.Invalidate();
		}
	}
	else
	{
		me->PressFireButton( 0.2f );
	}
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotGetPasstimeJack::CancelThrow( CTFBot *me )
{
	if ( !me )
		return;

	me->PressAltFireButton( 0.1f );
	me->ReleaseFireButton();
	m_isChargingThrow = false;
	m_chargeTimer.Invalidate();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotGetPasstimeJack::InPasstimeGoal( CTFBot *me ) const
{
	if ( !me )
		return false;

	CBaseEntity *pGoal = NULL;
	while ( ( pGoal = gEntList.FindEntityByClassname( pGoal, "func_passtime_goal" ) ) != NULL )
	{
		if ( pGoal->CollisionProp() && pGoal->CollisionProp()->IsPointInBounds( me->WorldSpaceCenter() ) )
			return true;
	}
	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CBaseEntity *CTFBotGetPasstimeJack::SelectBestGoal( CTFBot *me )
{
	CUtlVector< CBaseEntity * > walkGoalList;
	CUtlVector< CBaseEntity * > throwGoalList;
	CBaseEntity *pTrigger = NULL;

	int nTargetTeam = me->GetTeamNumber();

	const float kVeryCloseDist = 128.0f;

	while ( ( pTrigger = gEntList.FindEntityByClassname( pTrigger, "func_passtime_goal" ) ) != NULL )
	{
		// Don't choose a goal that is from the other team
		if ( pTrigger->GetTeamNumber() != nTargetTeam )
			continue;

		// Don't choose disabled goals
		CBaseTrigger *pBaseTrigger = dynamic_cast< CBaseTrigger * >( pTrigger );
		if ( pBaseTrigger && pBaseTrigger->m_bDisabled == true )
			continue;

		// Don't choose goals that are too far above the nav mesh (e.g. incredibly high up goal in pass_brickyard)
		CNavArea *pArea = TheNavMesh->GetNearestNavArea( pTrigger->WorldSpaceCenter() );
		if ( pArea )
		{
			float flHeight = pTrigger->WorldSpaceCenter().z - pArea->GetCenter().z;
			if ( flHeight > 450.0f )
				continue;
		}

		// Check if this specific goal has a respawn room visualizer near it (making it a throw goal)
		CBaseEntity *pVisualizer = FindThrowableGoal( pTrigger->GetAbsOrigin(), kVeryCloseDist );
		if ( pVisualizer )
		{
			throwGoalList.AddToTail( pTrigger );
		}
		else
		{
			walkGoalList.AddToTail( pTrigger );
		}
	}

	CUtlVector< CBaseEntity * > validChoices;
	
	// If we have both types, pick randomly between them
	bool bHasWalk = ( walkGoalList.Count() > 0 );
	bool bHasThrow = ( throwGoalList.Count() > 0 );

	if ( bHasWalk && bHasThrow )
	{
		if ( RandomInt( 0, 1 ) == 0 )
			return walkGoalList[ RandomInt( 0, walkGoalList.Count() - 1 ) ];
		else
			return throwGoalList[ RandomInt( 0, throwGoalList.Count() - 1 ) ];
	}
	else if ( bHasThrow )
	{
		return throwGoalList[ RandomInt( 0, throwGoalList.Count() - 1 ) ];
	}
	else if ( bHasWalk )
	{
		return walkGoalList[ RandomInt( 0, walkGoalList.Count() - 1 ) ];
	}

	return NULL;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CBaseEntity *CTFBotGetPasstimeJack::FindBestBallSpawn( CTFBot *me ) const
{
	CBaseEntity *pBest = NULL;
	float flClosest = FLT_MAX;

	CBaseEntity *pSpawn = NULL;
	while ( ( pSpawn = gEntList.FindEntityByClassname( pSpawn, "info_passtime_ball_spawn" ) ) != NULL )
	{
		float flDistSqr = ( pSpawn->GetAbsOrigin() - me->WorldSpaceCenter() ).LengthSqr();
		if ( flDistSqr < flClosest )
		{
			flClosest = flDistSqr;
			pBest = pSpawn;
		}
	}
	return pBest;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
void CTFBotGetPasstimeJack::UpdateJackTarget( CTFBot *me )
{
	CPasstimeJackFilter filter( me );

	CBaseEntity *pClosest = NULL;
	float flClosest = FLT_MAX;

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "passtime_ball" ) ) != NULL )
	{
		if ( !filter.IsSelected( ent ) )
			continue;

		float flDistSqr = ( ent->WorldSpaceCenter() - me->WorldSpaceCenter() ).LengthSqr();
		if ( flDistSqr < flClosest )
		{
			flClosest = flDistSqr;
			pClosest = ent;
		}

		if ( tf_bot_debug_passtime_searching.GetBool() )
			NDebugOverlay::Cross3D( ent->WorldSpaceCenter(), 12.0f, 0, 255, 0, true, 0.15f );
	}

	if ( pClosest )
	{
		// Go get the ball
		if ( pClosest != m_jackEntity )
		{
			m_jackEntity = pClosest;
			m_lastJackPos = pClosest->WorldSpaceCenter();
			CTFBotPathCost cost( me, DEFAULT_ROUTE );
			m_path.Compute( me, m_lastJackPos, cost );
			m_repathTimer.Start( 0.5f );
		}
		else
		{
			Vector curPos = pClosest->WorldSpaceCenter();
			if ( ( curPos - m_lastJackPos ).LengthSqr() > ( 48.0f * 48.0f ) )
			{
				m_lastJackPos = curPos;
				CTFBotPathCost cost( me, DEFAULT_ROUTE );
				m_path.Compute( me, curPos, cost );
				m_repathTimer.Start( 0.5f );
			}
		}
	}
	else
	{
		// We have no ball, go to the ball spawn and wait like a good bot.
        m_jackEntity = NULL;

		if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
		{
			CBaseEntity *pSpawn = FindBestBallSpawn( me );
			if ( pSpawn )
			{
				CNavArea *pSpawnArea = TheNavMesh->GetNearestNavArea( pSpawn->GetAbsOrigin() );
				Vector goal = pSpawn->GetAbsOrigin();

				if ( pSpawnArea )
				{
					CUtlVector< CNavArea * > nearby;
					CollectSurroundingAreas( &nearby, pSpawnArea, 2500.0f,
						me->GetLocomotionInterface()->GetStepHeight(),
						me->GetLocomotionInterface()->GetDeathDropHeight() );

					if ( nearby.Count() > 0 )
					{
						// Pick a random area so bots spread out
						CNavArea *pChosen = nearby[ RandomInt( 0, nearby.Count() - 1 ) ];
						goal = pChosen->GetCenter();
					}
				}

				CTFBotPathCost cost( me, DEFAULT_ROUTE );
				m_path.Compute( me, goal, cost );
			}
			m_repathTimer.Start( 1.5f );
		}
	}
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotGetPasstimeJack::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	// Not in PASS Time mode
	if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
		return Done( "Not in PASS Time mode" );

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_haveJack = me->m_Shared.HasPasstimeBall();
	m_jackEntity = NULL;
	m_goalEntity = NULL;
	m_repathTimer.Invalidate();
	m_scoreMethod = SCORE_WALK;
	m_scoreTarget = NULL;
	m_throwTarget = vec3_origin;
	m_backupPos = vec3_origin;
	m_isChargingThrow = false;
	m_hasSelectedScoreMethod = false;
	m_chargeTimer.Invalidate();
	m_isPassing = false;
	m_passTarget = NULL;
    m_passLostSightTimer.Invalidate();

	if ( !m_haveJack )
	{
		UpdateJackTarget( me );
	}
	else
	{
		m_path.Invalidate();
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotGetPasstimeJack::Update( CTFBot *me, float interval )
{
	// Not in PASS Time mode
	if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
		return Done( "Not in PASS Time mode" );

	// Teammate has the jack, switch to escorting/chasing them
	if ( CTFBotFollowPasstimeJackCarrier::IsPossible( me ) )
		return ChangeTo( new CTFBotFollowPasstimeJackCarrier(), "Escorting the JACK carrier" );

	// If we don't have the jack, go find it
	if ( !me->m_Shared.HasPasstimeBall() )
	{
		UpdateJackTarget( me );

		if ( m_path.IsValid() )
			m_path.Update( me );

		m_haveJack = false;
		m_goalEntity = NULL;
		m_hasSelectedScoreMethod = false;
		m_isPassing = false;
		m_passTarget = NULL;
		return Continue();
	}

	// We have the jack
	m_haveJack = true;

	if ( !m_isChargingThrow )
		me->ReleaseFireButton();

    // Passing behavior
	if ( !m_isPassing )
	{
		CTFPlayer *pPass = GetValidPassTarget( me );
		if ( pPass )
		{
			// Cancel any throw charge that may be in progress
			if ( m_isChargingThrow )
			{
				CancelThrow( me );
			}

			m_isPassing = true;
			m_passTarget = pPass;
			m_passLostSightTimer.Invalidate();
		}
	}

	if ( m_isPassing )
	{
		bool bCanSee = m_passTarget && m_passTarget->IsAlive() && me->IsLineOfSightClear( m_passTarget );

		if ( !m_passTarget || !m_passTarget->IsAlive() )
		{
			// Target is gone for good
			CancelThrow( me );
			m_isPassing = false;
			m_passTarget = NULL;
			m_passLostSightTimer.Invalidate();
		}
		else if ( !bCanSee )
		{
			if ( !m_passLostSightTimer.HasStarted() )
				m_passLostSightTimer.Start( 5.0f );

			if ( m_passLostSightTimer.IsElapsed() )
			{
				// If our teammate is out of sight for more than 5 seconds, give up
				CancelThrow( me );
				m_isPassing = false;
				m_passTarget = NULL;
				m_passLostSightTimer.Invalidate();
			}
		}
		else
		{
			// We can see them again, reset the grace timer
			m_passLostSightTimer.Invalidate();
		}

		if ( m_isPassing && m_passTarget )
		{
			// Keep looking at the pass target
			me->GetBodyInterface()->AimHeadTowards( m_passTarget->WorldSpaceCenter(), IBody::CRITICAL, 0.3f, NULL, "Looking at pass target" );

			const float flDistSqr = ( me->WorldSpaceCenter() - m_passTarget->WorldSpaceCenter() ).LengthSqr();
			const float kMinPassDist = 180.0f;
			const float kMaxPassDist = 900.0f;

			// Only path toward them while we are still too far
			if ( flDistSqr > ( kMinPassDist * kMinPassDist ) )
			{
				if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
				{
					CTFBotPathCost cost( me, DEFAULT_ROUTE );
					m_path.Compute( me, m_passTarget->WorldSpaceCenter(), cost );
					m_repathTimer.Start( 0.3f );
				}
				if ( m_path.IsValid() )
					m_path.Update( me );
			}

			// Start charging once we are within a reasonable pass range
			if ( !m_isChargingThrow && flDistSqr < ( kMaxPassDist * kMaxPassDist ) )
			{
				StartChargeThrow( me );
			}

			if ( m_isChargingThrow )
				UpdateChargeThrow( me );

			return Continue();
		}
	}

	// Regular scoring behavior
	// 
	// Ensure we always have a valid goal chosen while holding the JACK
	if ( !m_goalEntity )
	{
		m_goalEntity = SelectBestGoal( me );
		m_hasSelectedScoreMethod = false;
	}

	// Pick the scoring method if not done yet
	if ( m_goalEntity && !m_hasSelectedScoreMethod )
		SelectScoreMethod( me );

	if ( m_goalEntity && m_scoreMethod == SCORE_THROW && !m_isChargingThrow )
		me->GetBodyInterface()->AimHeadTowards( m_throwTarget, IBody::CRITICAL, 0.3f, NULL, "Looking to our goal" );

	if ( m_goalEntity )
	{
		// SCORE_THROW: Throw our jack into the goal
		if ( m_scoreMethod == SCORE_THROW )
		{
			if ( m_isChargingThrow )
			{
				UpdateChargeThrow( me );
				return Continue();
			}

			const float kArrivedBackup = 80.0f;
			if ( ( me->WorldSpaceCenter() - m_backupPos ).LengthSqr() > ( kArrivedBackup * kArrivedBackup ) )
			{
				if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
				{
					CTFBotPathCost cost( me, DEFAULT_ROUTE );
					m_path.Compute( me, m_backupPos, cost );
					m_repathTimer.Start( 0.2f );
				}
				if ( m_path.IsValid() )
					m_path.Update( me );
			}
			else
			{
				// Back up a bit, look at our goal, then throw it.
				me->GetBodyInterface()->AimHeadTowards( m_throwTarget, IBody::CRITICAL, 1.0f, NULL, "Preparing to throw my jack into the goal" );
				StartChargeThrow( me );
			}
		}
		else
		{
			// SCORE_WALK: Simply walk into the goal
			if ( !m_path.IsValid() || m_repathTimer.IsElapsed() )
			{
				CTFBotPathCost cost( me, DEFAULT_ROUTE );
				m_path.Compute( me, m_goalEntity->WorldSpaceCenter(), cost );
				m_repathTimer.Start( 1.0f );
			}

			if ( m_path.IsValid() )
			{
				m_path.Update( me );
			}
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotGetPasstimeJack::OnContact( CTFBot *me, CBaseEntity *other, CGameTrace *result )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotGetPasstimeJack::OnStuck( CTFBot *me )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotGetPasstimeJack::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotGetPasstimeJack::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
QueryResultType CTFBotGetPasstimeJack::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
QueryResultType CTFBotGetPasstimeJack::ShouldAttack( const INextBot *me, const CKnownEntity *them ) const
{
	// Don't attack. Attacking makes us throw the JACK at enemies.
	if ( m_haveJack )
	    return ANSWER_NO;

    // But we should attack while we are running toward the jack
    return ANSWER_YES;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
QueryResultType CTFBotGetPasstimeJack::ShouldRetreat( const INextBot *me ) const
{
	// Don't retreat. Retreating breaks our pathing for some reason.
	if ( m_haveJack )
		return ANSWER_NO;

	return ANSWER_UNDEFINED;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotFollowPasstimeJackCarrier::CTFBotFollowPasstimeJackCarrier( void )
{
	m_path.Invalidate();
	m_carrier = NULL;
	m_followPos = vec3_origin;
    m_bTryingMeleeSteal = false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotFollowPasstimeJackCarrier::IsPossible( CTFBot *me )
{
	if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
		return false;

	if ( me->m_Shared.HasPasstimeBall() )
		return false;

	// Look for any living player who currently has the ball (teammate or enemy)
	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "passtime_ball" ) ) != NULL )
	{
		CPasstimeBall *pBall = dynamic_cast< CPasstimeBall * >( ent );
		if ( !pBall )
			continue;

		CTFPlayer *pCarrier = pBall->GetCarrier();
		if ( pCarrier && pCarrier->IsAlive() )
			return true;
	}

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotFollowPasstimeJackCarrier::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_carrier = NULL;
	m_followPos = vec3_origin;
    m_bTryingMeleeSteal = false;
	m_repathTimer.Invalidate();

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "passtime_ball" ) ) != NULL )
	{
		CPasstimeBall *pBall = dynamic_cast< CPasstimeBall * >( ent );
		if ( !pBall )
			continue;

		CTFPlayer *pCarrier = pBall->GetCarrier();
		if ( pCarrier && pCarrier->IsAlive() )
		{
			m_carrier = pCarrier;
			break;
		}
	}

	if ( !m_carrier )
		return Done( "No JACK carrier found" );

	// Initial follow/chase position
	CNavArea *pArea = TheNavMesh->GetNearestNavArea( m_carrier->WorldSpaceCenter() );
	if ( pArea )
	{
		CUtlVector< CNavArea * > nearby;
		CollectSurroundingAreas( &nearby, pArea, 250.0f,
			me->GetLocomotionInterface()->GetStepHeight(),
			me->GetLocomotionInterface()->GetDeathDropHeight() );

		if ( nearby.Count() > 0 )
			m_followPos = nearby[ RandomInt( 0, nearby.Count() - 1 ) ]->GetCenter();
		else
			m_followPos = m_carrier->WorldSpaceCenter();
	}
	else
	{
		m_followPos = m_carrier->WorldSpaceCenter();
	}

	CTFBotPathCost cost( me, DEFAULT_ROUTE );
	m_path.Compute( me, m_followPos, cost );
	m_repathTimer.Start( 1.0f );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotFollowPasstimeJackCarrier::Update( CTFBot *me, float interval )
{
	if ( !TFGameRules() || !TFGameRules()->IsPasstimeMode() )
		return Done( "Not in PASS Time mode" );

	// I am now the JACK carrier
	// or the Jack was dropped, go fetch it
	if ( CTFBotGetPasstimeJack::IsPossible( me ) || me->m_Shared.HasPasstimeBall() )
		return ChangeTo( new CTFBotGetPasstimeJack(), "JACK was dropped/I am now the JACK carrier" );

	// Check if we actually have a carrier (passes, deaths, etc.)
	if ( !m_carrier || !m_carrier->IsAlive() )
	{
		m_carrier = NULL;
		CBaseEntity *ent = NULL;
		while ( ( ent = gEntList.FindEntityByClassname( ent, "passtime_ball" ) ) != NULL )
		{
			CPasstimeBall *pBall = dynamic_cast< CPasstimeBall * >( ent );
			if ( !pBall )
				continue;

			CTFPlayer *pCarrier = pBall->GetCarrier();
			if ( pCarrier && pCarrier->IsAlive() )
			{
				m_carrier = pCarrier;
				break;
			}
		}
	}

	if ( !m_carrier )
		return Done( "Lost the JACK carrier" );

	// If the enemy has the JACK, chase and attack them
    if ( !me->InSameTeam( m_carrier ) )
	{
		const float flDistSqr = ( me->WorldSpaceCenter() - m_carrier->WorldSpaceCenter() ).LengthSqr();
		const float kMeleeStealRange = 120.0f;
		const float kMeleeCancelRange = 350.0f;

		// Already trying a melee steal?
		if ( m_bTryingMeleeSteal )
		{
			if ( flDistSqr > ( kMeleeCancelRange * kMeleeCancelRange ) || !m_carrier->IsAlive() )
			{
				// Too far or carrier died, cancel it
				m_bTryingMeleeSteal = false;
			}
			else
			{
				// Take melee out and run at them wildly
				me->Weapon_Switch( me->Weapon_GetSlot( 2 ) );	// melee slot
				me->PressFireButton( 0.1f );

				if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
				{
					CTFBotPathCost cost( me, DEFAULT_ROUTE );
					m_path.Compute( me, m_carrier->WorldSpaceCenter(), cost );
					m_repathTimer.Start( 0.2f );
				}
				if ( m_path.IsValid() )
					m_path.Update( me );

				return Continue();
			}
		}

		// Not currently stealing – chance to start one when very close
		if ( !m_bTryingMeleeSteal && flDistSqr < ( kMeleeStealRange * kMeleeStealRange ) )
		{
			if ( RandomInt( 0, 1 ) == 0 )	// 50/50
			{
				m_bTryingMeleeSteal = true;
				me->Weapon_Switch( me->Weapon_GetSlot( 2 ) );
				me->PressFireButton( 0.1f );
				return Continue();
			}
		}

		// Normal chase
		me->EquipBestWeaponForThreat( me->GetVisionInterface()->GetPrimaryKnownThreat() );
		if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
		{
			CTFBotPathCost cost( me, DEFAULT_ROUTE );
			m_path.Compute( me, m_carrier->WorldSpaceCenter(), cost );
			m_repathTimer.Start( 0.4f );
		}
		if ( m_path.IsValid() )
			m_path.Update( me );
		return Continue();
	}

	// If our teammate has the JACK, escort them
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if ( threat && threat->GetEntity() )
	{
		float flDistToCarrier = ( m_carrier->WorldSpaceCenter() - threat->GetEntity()->WorldSpaceCenter() ).LengthSqr();
		if ( flDistToCarrier < ( 700.0f * 700.0f ) || CTFBotHasBeenDamaged( static_cast<CTFBot*>( m_carrier.Get() ), 2.0f ) )
		{
			me->EquipBestWeaponForThreat( threat );
			CTFBotPathCost cost( me, DEFAULT_ROUTE );
			m_path.Compute( me, threat->GetEntity()->WorldSpaceCenter(), cost );
			m_path.Update( me );
			return Continue();
		}
	}

	// Defend our jack carrier
	const float kArrived = 100.0f;
	if ( m_repathTimer.IsElapsed() || !m_path.IsValid() ||
		 ( m_followPos - me->WorldSpaceCenter() ).LengthSqr() < ( kArrived * kArrived ) )
	{
		CNavArea *pArea = TheNavMesh->GetNearestNavArea( m_carrier->WorldSpaceCenter() );
		if ( pArea )
		{
			CUtlVector< CNavArea * > nearby;
			CollectSurroundingAreas( &nearby, pArea, 400.0f,
				me->GetLocomotionInterface()->GetStepHeight(),
				me->GetLocomotionInterface()->GetDeathDropHeight() );

			CNavArea *pChosen = NULL;
			if ( nearby.Count() > 0 )
			{
				for ( int attempt = 0; attempt < 8; ++attempt )
				{
					CNavArea *pTest = nearby[ RandomInt( 0, nearby.Count() - 1 ) ];
					float flDist = ( pTest->GetCenter() - m_carrier->WorldSpaceCenter() ).Length2D();
					if ( flDist < 140.0f || flDist > 360.0f )
						continue;

					bool bTooClose = false;
					for ( int p = 1; p <= gpGlobals->maxClients; ++p )
					{
						CBasePlayer *pPlayer = UTIL_PlayerByIndex( p );
						if ( !pPlayer || pPlayer == me || !pPlayer->IsAlive() ||
							 pPlayer->GetTeamNumber() != me->GetTeamNumber() )
							continue;

						if ( ( pPlayer->WorldSpaceCenter() - pTest->GetCenter() ).Length2DSqr() < ( 110.0f * 110.0f ) )
						{
							bTooClose = true;
							break;
						}
					}
					if ( !bTooClose )
					{
						pChosen = pTest;
						break;
					}
				}
				if ( !pChosen )
					pChosen = nearby[ RandomInt( 0, nearby.Count() - 1 ) ];
			}

			if ( pChosen )
			{
				m_followPos = pChosen->GetCenter();
				CTFBotPathCost cost( me, DEFAULT_ROUTE );
				m_path.Compute( me, m_followPos, cost );
			}
		}
		m_repathTimer.Start( 1.4f );
	}

	if ( m_path.IsValid() )
		m_path.Update( me );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotFollowPasstimeJackCarrier::OnStuck( CTFBot *me )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotFollowPasstimeJackCarrier::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotFollowPasstimeJackCarrier::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}