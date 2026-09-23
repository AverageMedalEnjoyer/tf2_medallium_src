//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_civilian_boost.cpp
//
// Civilian boost bot behavior
//=============================================================================//

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_gamerules.h"
#include "bot/tf_bot.h"
#include "tf_weaponbase_melee.h"
#include "tf_player_shared.h"
#include "tf_bot_civilian_boost.h"

extern ConVar tf2m_teammate_boost_range;

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFBotCivilianBoost::CTFBotCivilianBoost( void )
{
	m_bTryingToBoost = false;
}

//---------------------------------------------------------------------------------------------
// Purpose: Returns the umbrella (or any melee with altfire_boosts_teammates)
//---------------------------------------------------------------------------------------------
CTFWeaponBaseMelee *CTFBotCivilianBoost::GetBoostWeapon( CTFBot *me )
{
	for ( int i = 0; i < MAX_WEAPONS; ++i )
	{
		CBaseCombatWeapon *pBase = me->GetWeapon( i );
		if ( !pBase )
			continue;

		CTFWeaponBase *pWpn = dynamic_cast< CTFWeaponBase * >( pBase );
		if ( !pWpn )
			continue;

		CTFWeaponBaseMelee *pMelee = dynamic_cast< CTFWeaponBaseMelee * >( pWpn );
		if ( !pMelee )
			continue;

		int iBoost = 0;
		CALL_ATTRIB_HOOK_INT_ON_OTHER( pMelee, iBoost, altfire_boosts_teammates );
		if ( iBoost > 0 )
			return pMelee;
	}
	return NULL;
}

//---------------------------------------------------------------------------------------------
// Purpose: Get the radius we should stand around teammates
//---------------------------------------------------------------------------------------------
float CTFBotCivilianBoost::GetAuraRadius( CTFBot *me )
{
	float flRadius = TF_BUFF_RADIUS;	// 450
	CALL_ATTRIB_HOOK_FLOAT_ON_OTHER( me, flRadius, mult_healaura_radius );
	return flRadius;
}

//---------------------------------------------------------------------------------------------
// Purpose: Check that our boost is ready (not recharging or draining)
//---------------------------------------------------------------------------------------------
bool CTFBotCivilianBoost::IsBoostReady( CTFBot *me )
{
	CTFWeaponBaseMelee *pUmbrella = GetBoostWeapon( me );
	if ( !pUmbrella )
		return false;

	if ( pUmbrella->IsBoostMeterDraining() )
		return false;

	if ( pUmbrella->GetEffectBarProgress() < 1.0f )
		return false;

	return true;
}

//---------------------------------------------------------------------------------------------
// Purpose: Check that we are in combat (or we can see the enemy)
//---------------------------------------------------------------------------------------------
bool CTFBotCivilianBoost::IsInCombat( CTFBot *me )
{
    const CKnownEntity *pKnown = me->GetVisionInterface()->GetPrimaryKnownThreat( false );
	if ( pKnown && pKnown->GetEntity() && pKnown->GetEntity()->IsAlive() && pKnown->IsVisibleRecently() )
		return true;

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotCivilianBoost::SelectBoostTarget( CTFBot *me )
{
	const float flMaxRange = GetAuraRadius( me ) * 1.25f;

	CUtlVector< CTFPlayer * > candidates;
	CollectPlayers( &candidates, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );

	// Remove self and anyone already boosted / too far
	for ( int i = candidates.Count() - 1; i >= 0; --i )
	{
		CTFPlayer *p = candidates[i];
		if ( p == me || !p->IsAlive() )
		{
			candidates.Remove( i );
			continue;
		}

		if ( ( p->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr() > flMaxRange * flMaxRange )
		{
			candidates.Remove( i );
			continue;
		}

		// Prefer players that don’t already have a recent boost
		if ( p->m_Shared.InCond( TF2M_COND_BOOST_MINICRITS ) ||
			 p->m_Shared.InCond( TF2M_COND_BOOST_REFLECT ) )
		{
			continue;
		}
	}

	if ( candidates.Count() == 0 )
		return NULL;

	CTFPlayer *pBest = NULL;
	float flBestScore = -FLT_MAX;

	const int priority[] = 
	{
		TF_CLASS_SOLDIER,
		TF_CLASS_DEMOMAN,
		TF_CLASS_PYRO,
		TF_CLASS_HEAVYWEAPONS,
		TF_CLASS_MEDIC
	};

	for ( int i = 0; i < candidates.Count(); ++i )
	{
		CTFPlayer *p = candidates[i];
		float flDist = ( p->GetAbsOrigin() - me->GetAbsOrigin() ).Length();
		float flScore = -flDist;	// closer is better

		// Class bonus
		for ( int prio = 0; prio < ARRAYSIZE( priority ); ++prio )
		{
			if ( p->IsPlayerClass( priority[prio] ) )
			{
				flScore += 1000.0f - ( prio * 50.0f );	// Strong preference
				break;
			}
		}

		// Slight preference for lower health teammates
		flScore += ( 1.0f - p->HealthFraction() ) * 200.0f;

		if ( flScore > flBestScore )
		{
			flBestScore = flScore;
			pBest = p;
		}
	}

	return pBest;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotCivilianBoost::SelectAuraAnchor( CTFBot *me )
{
	const float flRadius = GetAuraRadius( me );

	CUtlVector< CTFPlayer * > teammates;
	CollectPlayers( &teammates, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );

	if ( teammates.Count() <= 1 )	// only ourselves
		return NULL;

	// Remove self
	teammates.FindAndRemove( me );

	// Score each possible “anchor” by (number of nearby teammates) + (inverse health)
	CTFPlayer *pBest = NULL;
	float flBestScore = -FLT_MAX;

	for ( int i = 0; i < teammates.Count(); ++i )
	{
		CTFPlayer *pAnchor = teammates[i];
		int nNearby = 0;
		float flHealthFactor = 1.0f - pAnchor->HealthFraction();

		for ( int j = 0; j < teammates.Count(); ++j )
		{
			if ( i == j )
				continue;
			if ( ( teammates[j]->GetAbsOrigin() - pAnchor->GetAbsOrigin() ).LengthSqr() < flRadius * flRadius )
				++nNearby;
		}

		// Crowd size is primary, low health is secondary
		float flScore = nNearby * 100.0f + flHealthFactor * 50.0f;

		if ( flScore > flBestScore )
		{
			flBestScore = flScore;
			pBest = pAnchor;
		}
	}

	return pBest;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
bool CTFBotCivilianBoost::IsPossible( CTFBot *me )
{
	if ( !me->IsPlayerClass( TF_CLASS_CIVILIAN ) )
		return false;

	if ( !GetBoostWeapon( me ) )
		return false;

	// Need at least one living teammate in aura range
	CUtlVector< CTFPlayer * > teammates;
	CollectPlayers( &teammates, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );
	teammates.FindAndRemove( me );

	const float flRadius = GetAuraRadius( me );
	for ( int i = 0; i < teammates.Count(); ++i )
	{
		if ( ( teammates[i]->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr() < flRadius * flRadius )
			return true;
	}

	return false;
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotCivilianBoost::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Invalidate();
	m_boostAttemptTimer.Invalidate();
	m_lookAtTimer.Invalidate();
	m_hBoostTarget = NULL;
	m_hAuraAnchor = NULL;
	m_bTryingToBoost = false;

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotCivilianBoost::Update( CTFBot *me, float interval )
{
	CTFWeaponBaseMelee *pUmbrella = GetBoostWeapon( me );
	if ( !pUmbrella )
		return Done( "No boost weapon" );

	const bool bInCombat   = IsInCombat( me );
	const bool bBoostReady = IsBoostReady( me );

	// Attempt a boost
	if ( m_bTryingToBoost )
	{
		CTFPlayer *pTarget = m_hBoostTarget;

		bool bAbort = false;
		const char *pszAbortReason = NULL;

		if ( !pTarget || !pTarget->IsAlive() )
		{
			bAbort = true;
			pszAbortReason = "Target gone";
		}
		else if ( !bBoostReady )
		{
			bAbort = true;
			pszAbortReason = "Boost applied or recharging";
		}
		else if ( m_boostAttemptTimer.IsElapsed() )
		{
			bAbort = true;
			pszAbortReason = "Timeout (10 secs)";
		}
		else
		{
			const float flMaxRange = GetAuraRadius( me ) * 1.5f;
			if ( ( pTarget->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr() > flMaxRange * flMaxRange )
			{
				bAbort = true;
				pszAbortReason = "Target too far";
			}
		}

		if ( bAbort )
		{
			m_bTryingToBoost = false;
			m_hBoostTarget   = NULL;
			me->ReleaseFireButton();
			me->ReleaseAltFireButton();
			m_path.Invalidate();

            CTFWeaponBase *pPrimary = me->Weapon_GetWeaponByType( TF_WPN_TYPE_PRIMARY );
			if ( pPrimary )
				me->Weapon_Switch( pPrimary );
		}
		else
		{
			me->GetBodyInterface()->AimHeadTowards( pTarget, IBody::CRITICAL, 0.2f, NULL, "Boosting teammate" );

			if ( me->GetActiveTFWeapon() != pUmbrella )
				me->Weapon_Switch( pUmbrella );

			me->PressAltFireButton();

			// Actively chase boost target
			const float flDesiredDist = 256.0f;
			const float flSlack       = 48.0f;

			Vector vToTarget = pTarget->GetAbsOrigin() - me->GetAbsOrigin();
			float  flDist    = vToTarget.Length();

			Vector vGoal = pTarget->GetAbsOrigin();

			if ( flDist > flDesiredDist + flSlack )
			{
				if ( flDist > 1.0f )
					vToTarget /= flDist;
				vGoal = pTarget->GetAbsOrigin() - vToTarget * flDesiredDist;
			}
			else if ( flDist < flDesiredDist - flSlack )
			{
				if ( flDist > 1.0f )
					vToTarget /= flDist;
				vGoal = me->GetAbsOrigin() - vToTarget * 64.0f;
			}

			if ( m_path.GetAge() > 0.4f || !m_path.IsValid() )
			{
				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, vGoal, cost );
			}

			m_path.Update( me );
			return Continue();
		}
	}

	// Should we actually boost?
	if ( bInCombat && bBoostReady )
	{
		CTFPlayer *pTarget = SelectBoostTarget( me );
		if ( pTarget )
		{
			m_hBoostTarget = pTarget;
			m_bTryingToBoost = true;
			m_boostAttemptTimer.Start( 10.0f );
			m_vLastKnownTargetPos = pTarget->GetAbsOrigin();
			m_path.Invalidate();

			if ( me->GetActiveTFWeapon() != pUmbrella )
				me->Weapon_Switch( pUmbrella );

			return Continue();
		}
	}

	// Stay near teammates (non-boost behavior).
	CTFWeaponBase *pPrimary = me->Weapon_GetWeaponByType( TF_WPN_TYPE_PRIMARY );
	if ( pPrimary && me->GetActiveTFWeapon() != pPrimary )
	{
		me->Weapon_Switch( pPrimary );
	}

	if ( m_repathTimer.IsElapsed() || !m_hAuraAnchor || !m_hAuraAnchor->IsAlive() )
	{
		m_hAuraAnchor = SelectAuraAnchor( me );
		m_repathTimer.Start( 1.5f );
	}

	Vector vGoal;
	bool   bHaveGoal = false;

	if ( m_hAuraAnchor )
	{
		vGoal = m_hAuraAnchor->GetAbsOrigin();
		Vector vToMe = me->GetAbsOrigin() - vGoal;
		float flDist = vToMe.Length();
		const float flDesired = GetAuraRadius( me ) * 0.6f;

		if ( flDist > flDesired )
		{
			vToMe.NormalizeInPlace();
			vGoal += vToMe * flDesired;
		}
		bHaveGoal = true;
	}
	else
	{
		// We have no teammates in our aura radius, retreat and find more teammates to stand by.
		CUtlVector< CTFPlayer * > teammates;
		CollectPlayers( &teammates, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );
		teammates.FindAndRemove( me );

		CTFPlayer *pNearest = NULL;
		float flNearestDistSqr = FLT_MAX;

		for ( int i = 0; i < teammates.Count(); ++i )
		{
			float flDistSqr = ( teammates[i]->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr();
			if ( flDistSqr < flNearestDistSqr )
			{
				flNearestDistSqr = flDistSqr;
				pNearest = teammates[i];
			}
		}

		if ( pNearest )
		{
			vGoal = pNearest->GetAbsOrigin();
			bHaveGoal = true;
		}
	}

	if ( bHaveGoal )
	{
		if ( m_path.GetAge() > 1.0f || !m_path.IsValid() )
		{
			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, vGoal, cost );
		}
		m_path.Update( me );
	}
	else
	{
		me->GetLocomotionInterface()->Stop();
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCivilianBoost::OnStuck( CTFBot *me )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCivilianBoost::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotCivilianBoost::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	return TryContinue();
}