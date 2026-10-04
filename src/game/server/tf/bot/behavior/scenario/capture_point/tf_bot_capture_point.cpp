//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_capture_point.cpp
// Move to and try to capture the next point
// Michael Booth, February 2009

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_player.h"
#include "tf_gamerules.h"
#include "team_control_point_master.h"
#include "trigger_area_capture.h"
#include "bot/tf_bot.h"
#include "bot/behavior/scenario/capture_point/tf_bot_capture_point.h"
#include "bot/behavior/scenario/capture_point/tf_bot_defend_point.h"
#include "bot/behavior/tf_bot_seek_and_destroy.h"


extern ConVar tf_bot_path_lookahead_range;
ConVar tf_bot_offense_must_push_time( "tf_bot_offense_must_push_time", "120", FCVAR_CHEAT, "If timer is less than this, bots will push hard to cap" );

ConVar tf_bot_capture_seek_and_destroy_min_duration( "tf_bot_capture_seek_and_destroy_min_duration", "15", FCVAR_CHEAT, "If a capturing bot decides to go hunting, this is the min duration he will hunt for before reconsidering" );
ConVar tf_bot_capture_seek_and_destroy_max_duration( "tf_bot_capture_seek_and_destroy_max_duration", "30", FCVAR_CHEAT, "If a capturing bot decides to go hunting, this is the max duration he will hunt for before reconsidering" );

CTriggerAreaCapture *GetCaptureTriggerForPoint( CTeamControlPoint *point )
{
	if ( !point )
		return NULL;

	for ( int i = 0; i < ITriggerAreaCaptureAutoList::AutoList().Count(); ++i )
	{
		CTriggerAreaCapture *trigger = static_cast< CTriggerAreaCapture * >(
			ITriggerAreaCaptureAutoList::AutoList()[i] );
		if ( trigger && trigger->GetControlPoint() == point )
			return trigger;
	}

	// Fallback
	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "trigger_capture_area" ) ) != NULL )
	{
		CTriggerAreaCapture *trigger = dynamic_cast< CTriggerAreaCapture * >( ent );
		if ( trigger && trigger->GetControlPoint() == point )
			return trigger;
	}

	return NULL;
}

bool GetControlPointCaptureExtent( CTeamControlPoint *point, Extent &outExtent )
{
	CTriggerAreaCapture *trigger = GetCaptureTriggerForPoint( point );
	if ( trigger )
	{
		outExtent.Init( static_cast< CBaseEntity * >( trigger ) );
		return true;
	}

	if ( point )
	{
		outExtent.Init( static_cast< CBaseEntity * >( point ) );
		return true;
	}
	return false;
}

int CountTeammatesOnPoint( CTFBot *me, CTeamControlPoint *point )
{
	if ( !me || !point )
		return 0;

	Extent extent;
	if ( !GetControlPointCaptureExtent( point, extent ) )
		return 0;

	int count = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *player = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !player || !player->IsAlive() || player->GetTeamNumber() != me->GetTeamNumber() )
			continue;

		if ( extent.Contains( player->GetAbsOrigin() ) )
			++count;
	}
	return count;
}

int CountLivingTeammates( CTFBot *me )
{
	if ( !me )
		return 0;

	int count = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *player = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( player && player->IsAlive() && player->GetTeamNumber() == me->GetTeamNumber() )
			++count;
	}
	return count;
}

Vector SelectRandomPointInCaptureZone( CTeamControlPoint *point )
{
	if ( !point )
		return vec3_origin;

	const CUtlVector< CTFNavArea * > *areas =
		TheTFNavMesh()->GetControlPointAreas( point->GetPointIndex() );
	if ( areas && areas->Count() > 0 )
	{
		CTFNavArea *area = areas->Element( RandomInt( 0, areas->Count() - 1 ) );
		if ( area )
			return area->GetRandomPoint();
	}

	Extent extent;
	if ( GetControlPointCaptureExtent( point, extent ) )
	{
		Vector p;
		p.x = RandomFloat( extent.lo.x, extent.hi.x );
		p.y = RandomFloat( extent.lo.y, extent.hi.y );
		p.z = ( extent.lo.z + extent.hi.z ) * 0.5f;
		return p;
	}

	return point->GetAbsOrigin();
}

CTeamControlPoint *FindThreatenedFriendlyPoint( CTFBot *me )
{
	if ( !me )
		return NULL;

	CTeamControlPointMaster *master =
		g_hControlPointMasters.Count() ? g_hControlPointMasters[0] : NULL;
	if ( !master )
		return NULL;

	CTeamControlPoint *closest = NULL;
	float closestDistSq = FLT_MAX;
	const float kMaxReactRangeSq = 800.0f * 800.0f;

	for ( int i = 0; i < master->GetNumPoints(); ++i )
	{
		CTeamControlPoint *point = master->GetControlPoint( i );
		if ( !point || point->GetOwner() != me->GetTeamNumber() )
			continue;

		float distSq = ( point->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr();
		if ( distSq > kMaxReactRangeSq )
			continue;

		bool bContested = false;

		if ( point->LastContestedAt() > 0.0f &&
			 ( gpGlobals->curtime - point->LastContestedAt() ) < 5.0f )
		{
			bContested = true;
		}
		else if ( point->GetTeamCapPercentage( me->GetTeamNumber() ) < 1.0f )
		{
			bContested = true;
		}
		else
		{
			Extent extent;
			if ( GetControlPointCaptureExtent( point, extent ) )
			{
				for ( int p = 1; p <= gpGlobals->maxClients; ++p )
				{
					CTFPlayer *enemy = ToTFPlayer( UTIL_PlayerByIndex( p ) );
					if ( !enemy || !enemy->IsAlive() || enemy->GetTeamNumber() == me->GetTeamNumber() )
						continue;

					if ( extent.Contains( enemy->GetAbsOrigin() ) )
					{
						bContested = true;
						break;
					}
				}
			}
		}

		if ( !bContested )
			continue;

		if ( distSq < closestDistSq )
		{
			closestDistSq = distSq;
			closest = point;
		}
	}
	return closest;
}

//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotCapturePoint::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	VPROF_BUDGET( "CTFBotCapturePoint::OnStart", "NextBot" );

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_path.Invalidate();

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotCapturePoint::Update( CTFBot *me, float interval )
{
	if ( TFGameRules()->InSetup() )
	{
		// wait until the gates open, then path
		m_path.Invalidate();
		m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

		return Continue();
	}

	CTeamControlPoint *threatenedFriendly = FindThreatenedFriendlyPoint( me );
	if ( threatenedFriendly )
	{
		me->ClearMyControlPoint();		// force re-evaluation
		return ChangeTo( new CTFBotDefendPoint, "Friendly point is under attack, defending!" );
	}

	CTeamControlPoint *point = me->GetMyControlPoint();

	if ( point == NULL )
	{
		const float roamTime = 10.0f;
		return SuspendFor( new CTFBotSeekAndDestroy( roamTime ), "Seek and destroy until a point becomes available" );
	}

	if ( point->GetTeamNumber() == me->GetTeamNumber() )
	{
		me->ClearMyControlPoint();
		return Continue();
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if ( threat && threat->IsVisibleRecently() &&
        me->IsLineOfFireClear( threat->GetEntity()->EyePosition() ) )
	{
		// prepare to fight
		me->EquipBestWeaponForThreat( threat );
	}

	bool isPushingToCapture = ( me->IsPointBeingCaptured( point ) && !me->IsInCombat() ) ||			// a friend is capturing
							   me->IsCapturingPoint() ||												// we're capturing
							   // me->m_Shared.InCond( TF_COND_INVULNERABLE ) ||						// we're ubered
							   TFGameRules()->InOvertime() ||											// the game is in overtime
							   me->GetTimeLeftToCapture() < tf_bot_offense_must_push_time.GetFloat() ||	// nearly out of tim
							   TFGameRules()->IsInTraining() ||											// teach newbies to capture
							   me->IsNearPoint( point );


	// if we see an enemy at a good combat range, stop and engage them unless we're running out of time
	if ( !isPushingToCapture )
	{
		if ( threat && threat->IsVisibleRecently() &&
            me->IsLineOfFireClear( threat->GetEntity()->EyePosition() ) )
		{
			return SuspendFor( new CTFBotSeekAndDestroy(
				RandomFloat( tf_bot_capture_seek_and_destroy_min_duration.GetFloat(),
							 tf_bot_capture_seek_and_destroy_max_duration.GetFloat() ) ),
				"Too early to capture - hunting" );
		}
	}

	const int livingTeam   = CountLivingTeammates( me );
	const int neededOnPoint = Max( 1, ( livingTeam + 1 ) / 2 );	// ~half, at least 1
	const int onPoint       = CountTeammatesOnPoint( me, point );

	Extent captureExtent;
	const bool haveExtent = GetControlPointCaptureExtent( point, captureExtent );
	const bool iAmOnPoint = haveExtent && captureExtent.Contains( me->GetAbsOrigin() );

	if ( onPoint >= neededOnPoint && iAmOnPoint == false )
	{
		// Enough people are already capping patrol the perimeter
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 3.0f, 5.0f ) );

			// Random point on a ring ~800-2000 units from the live centre
			Vector center = haveExtent ? ( captureExtent.lo + captureExtent.hi ) * 0.5f
									   : point->GetAbsOrigin();
			float angle = RandomFloat( 0.0f, 2.0f * M_PI );
			float radius = RandomFloat( 800.0f, 2000.0f );
			Vector patrolGoal = center + Vector( cos( angle ) * radius, sin( angle ) * radius, 0.0f );

			CTFBotPathCost cost( me, DEFAULT_ROUTE );
			m_path.Compute( me, patrolGoal, cost );
		}
		m_path.Update( me );
		return Continue();
	}

	if ( me->IsCapturingPoint() || iAmOnPoint )
	{
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 2.0f, 4.0f ) );	// change position every few seconds

			Vector goal = SelectRandomPointInCaptureZone( point );
			CTFBotPathCost cost( me, DEFAULT_ROUTE );
			m_path.Compute( me, goal, cost );
		}
		m_path.Update( me );
	}
	else
	{
		// move toward the point, periodically repathing to account for changing situation
		if ( m_repathTimer.IsElapsed() )
		{
			VPROF_BUDGET( "CTFBotCapturePoint::Update( repath )", "NextBot" );

			Vector goal = SelectRandomPointInCaptureZone( point );
			CTFBotPathCost cost( me, SAFEST_ROUTE );
			m_path.Compute( me, goal, cost );
			m_repathTimer.Start( RandomFloat( 2.0f, 3.0f ) );
		}

		if ( TFGameRules()->IsInTraining() && !me->IsAnyPointBeingCaptured() )
		{
			// stop short of capturing until the human trainee starts it
			if ( m_path.GetLength() < 1000.0f )
			{
				// hold here and yell at player to get on the point
				me->SpeakConceptIfAllowed( MP_CONCEPT_PLAYER_GO );

				return Continue();
			}
		}

		// move towards next capture point
		m_path.Update( me );
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotCapturePoint::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	m_repathTimer.Invalidate();
	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnStuck( CTFBot *me )
{
	m_repathTimer.Invalidate();
	me->GetLocomotionInterface()->ClearStuckStatus();

	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	m_repathTimer.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnTerritoryContested( CTFBot *me, int territoryID )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnTerritoryCaptured( CTFBot *me, int territoryID )
{
	// we got it, move on
	m_repathTimer.Invalidate();

	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCapturePoint::OnTerritoryLost( CTFBot *me, int territoryID )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
QueryResultType	CTFBotCapturePoint::ShouldRetreat( const INextBot *bot ) const
{
	CTFBot *me = (CTFBot *)bot->GetEntity();

	// if we're running out of time, we have to go for it
	if ( me->GetTimeLeftToCapture() < tf_bot_offense_must_push_time.GetFloat() )
		return ANSWER_NO;

	return ANSWER_UNDEFINED;
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotCapturePoint::ShouldHurry( const INextBot *bot ) const
{
	CTFBot *me = (CTFBot *)bot->GetEntity();

	// if we're running out of time, we have to go for it
	if ( me->GetTimeLeftToCapture() < tf_bot_offense_must_push_time.GetFloat() )
		return ANSWER_YES;

	return ANSWER_UNDEFINED;
}

