//========= Copyright Valve Corporation, All rights reserved. ============//
// MVM Defender behavior for bots

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_gamerules.h"
#include "bot/tf_bot.h"
#include "tf_bot_mvm_defenders.h"

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotMVMDefender::CTFBotMVMDefender( void )
{
	m_bHasGoal = false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefender::IsPossible( CTFBot *me )
{
	// Not in MVM
	if ( TFGameRules() && !TFGameRules()->IsMannVsMachineMode() )
		return false;

	// Not on DEFENDERS team
	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return false;

	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotMVMDefender::SelectRandomDefenderSpawnGoal( CTFBot *me )
{
	CUtlVector< CBaseEntity * > rooms;

	// Collect every func_respawnroom that belongs to the ROBOTS team
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "func_respawnroom" );
		  pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "func_respawnroom" ) )
	{
		if ( pEnt->GetTeamNumber() == TF_TEAM_PVE_INVADERS )	// team 3
		{
			rooms.AddToTail( pEnt );
		}
	}

	if ( rooms.Count() == 0 )
	{
		m_bHasGoal = false;
		return false;
	}

	for ( int i = rooms.Count() - 1; i > 0; --i )
	{
		int nSwap = RandomInt( 0, i );
		CBaseEntity *pTemp = rooms[i];
		rooms[i] = rooms[nSwap];
		rooms[nSwap] = pTemp;
	}

	CTFBotPathCost cost( me, FASTEST_ROUTE );

	const float minRadius = 180.0f;
	const float maxRadius = 450.0f;
	const int   kMaxAttemptsPerRoom = 12;

	for ( int i = 0; i < rooms.Count(); ++i )
	{
		CBaseEntity *pRoom = rooms[i];
		if ( !pRoom )
			continue;

		Vector roomCenter = pRoom->WorldSpaceCenter();

		// Get the actual trigger bounds so we can reject points that land inside
		Vector mins, maxs;
		pRoom->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

		for ( int attempt = 0; attempt < kMaxAttemptsPerRoom; ++attempt )
		{
			float angle = RandomFloat( 0.0f, 6.2831853f );
			float dist  = RandomFloat( minRadius, maxRadius );
			Vector offset( cosf( angle ) * dist, sinf( angle ) * dist, 0.0f );
			Vector candidateCenter = roomCenter + offset;

			CNavArea *pArea = TheNavMesh->GetNearestNavArea( candidateCenter,
															 false,			// anyZ
															 600.0f,		// maxDist
															 true,			// checkLOS
															 true,			// checkGround
															 me->GetTeamNumber() );
			if ( !pArea )
				continue;

			Vector candidatePos;
			pArea->GetClosestPointOnArea( candidateCenter, &candidatePos );

			if ( candidatePos.x >= mins.x && candidatePos.x <= maxs.x &&
				 candidatePos.y >= mins.y && candidatePos.y <= maxs.y &&
				 candidatePos.z >= mins.z && candidatePos.z <= maxs.z )
			{
				continue;
			}

			// Path-validate
			Path testPath;
			testPath.Compute( me, candidatePos, cost, 0.0f, true );
			if ( testPath.IsValid() )
			{
				m_goalPos = candidatePos;
				m_bHasGoal = true;
				return true;
			}
		}
	}

	m_bHasGoal = false;
	return false;
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
	// Not in MVM
	if ( TFGameRules() && !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	// Not on DEFENDERS team
	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Invalidate();
	m_bHasGoal = false;

	if ( SelectRandomDefenderSpawnGoal( me ) )
	{
		ComputePathToGoal( me );
		m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotMVMDefender::Update( CTFBot *me, float interval )
{
	// Not in MVM
	if ( TFGameRules() && !TFGameRules()->IsMannVsMachineMode() )
		return Done( "Not in MVM." );

	// Not on DEFENDERS team
	if ( me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		return Done( "Not on DEFENDERS team." );

	// Periodically re-choose a goal
	if ( !m_bHasGoal || m_repathTimer.IsElapsed() )
	{
		if ( SelectRandomDefenderSpawnGoal( me ) )
		{
			ComputePathToGoal( me );
		}
		m_repathTimer.Start( RandomFloat( 2.0f, 4.0f ) );
	}

	if ( m_bHasGoal )
	{
		const float standRange = 280.0f;
		if ( ( me->GetAbsOrigin() - m_goalPos ).AsVector2D().LengthSqr() < standRange * standRange )
		{
			return Continue();
		}

		if ( m_path.IsValid() )
		{
			m_path.Update( me );
		}
		else
		{
			m_repathTimer.Start( 0.5f );
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
	// Force a repath when stuck
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
	// Force a repath
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
QueryResultType CTFBotMVMDefender::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}