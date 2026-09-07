//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_heal.cpp
// Heal a teammate
// Michael Booth, February 2009

#include "cbase.h"
#include "team.h"
#include "tf_player.h"
#include "tf_gamerules.h"
#include "tf_weapon_medigun.h"
#include "bot/tf_bot.h"
#include "bot/behavior/tf_bot_mvm_defenders.h"
#include "bot/behavior/medic/tf_bot_medic_heal.h"
#include "bot/behavior/medic/tf_bot_medic_retreat.h"
#include "bot/behavior/tf_bot_use_teleporter.h"
#include "bot/behavior/scenario/capture_the_flag/tf_bot_fetch_flag.h"
#include "nav_mesh.h"
#include "tier0/vprof.h"

extern ConVar tf_bot_path_lookahead_range;

ConVar tf_bot_medic_stop_follow_range( "tf_bot_medic_stop_follow_range", "75", FCVAR_CHEAT );			// 100
ConVar tf_bot_medic_start_follow_range( "tf_bot_medic_start_follow_range", "250", FCVAR_CHEAT );		// 300
ConVar tf_bot_medic_max_heal_range( "tf_bot_medic_max_heal_range", "600", FCVAR_CHEAT );
ConVar tf_bot_medic_cover_debug( "tf_bot_medic_cover_debug", "0", FCVAR_CHEAT );
ConVar tf_bot_medic_max_call_response_range( "tf_bot_medic_max_call_response_range", "1000", FCVAR_CHEAT );
ConVar tf_bot_medic_opportunistic_range( "tf_bot_medic_opportunistic_range", "2500", FCVAR_CHEAT );

//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotMedicHeal::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_chasePath.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_patient = NULL;
	m_coverArea = NULL;
	m_patientAnchorPos = vec3_origin;
	m_isPatientRunningTimer.Invalidate();
    m_opportunisticPatient = NULL;
	m_opportunisticStickTimer.Invalidate();
	m_bUberLocked = false;

	return Continue();
}


//---------------------------------------------------------------------------------------------
/**
 * Choose a player as our "primary" patient. The guy we're going to tether ourselves to
 * and keep alive as long as we can.
 */
class CSelectPrimaryPatient : public IVision::IForEachKnownEntity
{
public:
	CSelectPrimaryPatient( CTFBot *me, CTFPlayer *currentPatient )
	{
		m_me = me;
		m_medigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );

		m_selected = currentPatient;
	}

	CTFPlayer *SelectPreferred( CTFPlayer *current, CTFPlayer *contender )
	{
		// in order of preference
		static int preferredClass[] = 
		{
			TF_CLASS_HEAVYWEAPONS,
			TF_CLASS_SOLDIER,
			TF_CLASS_PYRO,
			TF_CLASS_DEMOMAN,

//			TF_CLASS_SCOUT,
// 			TF_CLASS_ENGINEER,		
// 			TF_CLASS_SNIPER,
// 			TF_CLASS_SPY,
// 			TF_CLASS_MEDIC,

			TF_CLASS_UNDEFINED
		};

		int i;

		if ( TFGameRules()->IsInTraining() )
		{
			// in training mode, stay on the human trainee
			if ( !current || current->IsBot() )
				return contender;

			return current;
		}

		if ( !current )
		{
			return contender;
		}
		else if ( !contender )
		{
			return current;
		}

		// if we are in a squad, always heal the squad leader
		if ( m_me->IsInASquad() && m_me->GetSquad()->GetLeader() )
		{
			if ( m_me->GetSquad()->GetLeader()->entindex() == current->entindex() )
			{
				return current;
			}

			if ( m_me->GetSquad()->GetLeader()->entindex() == contender->entindex() )
			{
				return contender;
			}
		}

        int currentHealers = 0;
    	int contenderHealers = 0;

	    for ( i = 0; i < current->m_Shared.GetNumHealers(); ++i )
	    {
	    	CBaseEntity *healer = current->m_Shared.GetHealerByIndex( i );
	    	if ( healer && healer->IsPlayer() && !m_me->IsSelf( healer ) )
	    		++currentHealers;
	    }

	    for ( i = 0; i < contender->m_Shared.GetNumHealers(); ++i )
	    {
		    CBaseEntity *healer = contender->m_Shared.GetHealerByIndex( i );
	    	if ( healer && healer->IsPlayer() && !m_me->IsSelf( healer ) )
	    		++contenderHealers;
    	}

    	// Avoid anyone who already has a Medic
	    if ( currentHealers > 0 && contenderHealers == 0 )
		    return contender;

	    if ( contenderHealers > 0 && currentHealers == 0 )
		    return current;

		// if current already has another medic (not a dispenser) on him, select contender
		int numHealers = current->m_Shared.GetNumHealers();
		for ( i=0; i<numHealers; ++i )
		{
			CBaseEntity *medic = current->m_Shared.GetHealerByIndex(i);

			if ( medic && medic->IsPlayer() && !m_me->IsSelf( medic ) )
				return contender;
		}

		// if contender already has another medic (not a dispenser) on him, ignore him
		numHealers = contender->m_Shared.GetNumHealers();
		for ( i=0; i<numHealers; ++i )
		{
			CBaseEntity *medic = contender->m_Shared.GetHealerByIndex(i);

			if ( medic && medic->IsPlayer() && !m_me->IsSelf( medic ) )
				return current;
		}

		// respond to calls for help
		// NOTE: For now, only attend to HUMAN calls for help
		CTFPlayer *currentCaller = NULL;
		CTFPlayer *contenderCaller = NULL;
		CTFBotPathCost cost( m_me, FASTEST_ROUTE );
		
		if ( /*!current->IsBot() &&*/ current->IsCallingForMedic() && m_me->IsRangeLessThan( current, tf_bot_medic_max_call_response_range.GetFloat() ) )
		{
			// check actual travel range
			if ( NavAreaTravelDistance( m_me->GetLastKnownArea(), current->GetLastKnownArea(), cost, 1.5f * tf_bot_medic_max_call_response_range.GetFloat() ) >= 0.0 )
			{
				currentCaller = current;
			}
		}

		if ( /*!contender->IsBot() &&*/ contender->IsCallingForMedic() && m_me->IsRangeLessThan(contender, tf_bot_medic_max_call_response_range.GetFloat()))
		{
			// check actual travel range
			if ( NavAreaTravelDistance( m_me->GetLastKnownArea(), contender->GetLastKnownArea(), cost, 1.5f * tf_bot_medic_max_call_response_range.GetFloat() ) >= 0.0 )
			{
				contenderCaller = contender;
			}
		}

		if ( currentCaller )
		{
			if ( contenderCaller )
			{
				// both are calling for me, and in range - choose most recent caller
				if ( currentCaller->GetTimeSinceCalledForMedic() < contender->GetTimeSinceCalledForMedic() )
				{
					return current;
				}
				else
				{
					return contender;
				}
			}
			else
			{
				return current;
			}
		}
		else if ( contenderCaller )
		{
			return contender;
		}


		int currentRank = 999, contenderRank = 999;
		for( i=0; preferredClass[i] != TF_CLASS_UNDEFINED; ++i )
		{
			// for now, heavy, solider, and pyro are equivalent choices
			if ( current->GetPlayerClass()->GetClassIndex() == preferredClass[i] )
				currentRank = (i < 3) ? 0 : i;

			if ( contender->GetPlayerClass()->GetClassIndex() == preferredClass[i] )
				contenderRank = (i < 3) ? 0 : i;
		}

		if ( currentRank == contenderRank )
		{
			// unless contender is much closer, keep current guy
			const float tolerance = 100.0f;
			return ( m_me->GetDistanceBetween( current ) - m_me->GetDistanceBetween( contender ) > tolerance ) ? contender : current;
		}

		if ( currentRank > contenderRank )
		{
			// switch to contender unless he's far away
			const float nearbyRange = 750.0f;
			if ( m_me->GetDistanceBetween( contender ) < nearbyRange )
			{
				return contender;
			}
		}

		return current;
	}

	bool Inspect( const CKnownEntity &known )
	{
		if ( !known.GetEntity() || !known.GetEntity()->IsPlayer() || !known.GetEntity()->IsAlive() || !m_me->IsFriend( known.GetEntity() ) )
			return true;

		CTFPlayer *player = dynamic_cast< CTFPlayer * >( known.GetEntity() );
		if ( player == NULL )
			return true;

		if ( m_me->IsSelf( player ) )
			return true;

		// always heal the flag carrier, regardless of class
		// squads always heal the leader
		if ( !player->HasTheFlag() && !m_me->IsInASquad() )
		{
			if ( player->IsPlayerClass( TF_CLASS_MEDIC ) ||
				 player->IsPlayerClass( TF_CLASS_SNIPER ) ||
				 player->IsPlayerClass( TF_CLASS_ENGINEER ) ||
				 player->IsPlayerClass( TF_CLASS_SPY ) )
			{
				// these classes can't be our primary heal target (although they will get opportunistic healing
				return true;
			}
		}

		// select primary patient for long-term healing
		m_selected = SelectPreferred( m_selected, player );

		return true;
	}

	CTFBot *m_me;
	CWeaponMedigun *m_medigun;
	CTFPlayer *m_selected;
};


//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotMedicHeal::SelectPatient( CTFBot *me, CTFPlayer *current )
{
	CWeaponMedigun *medigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );

	if ( medigun )
	{
		if ( current == NULL || !current->IsAlive() )
		{
			current = ToTFPlayer( medigun->GetHealTarget() );
		}

		if ( medigun->IsReleasingCharge() )
		{
			// don't change targets when using uber
			return current;
		}

		if ( IsReadyToDeployUber( medigun ) && current && IsGoodUberTarget( current ) )
		{
			// don't change targets if we're ready to uber and we have a good target
			return current;
		}
	}

	CSelectPrimaryPatient choose( me, current );

	if ( TFGameRules()->IsPVEModeActive() )
	{
		// assume perfect knowledge
		CUtlVector< CTFPlayer * > livePlayerVector;
		CollectPlayers( &livePlayerVector, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );

		for( int i=0; i<livePlayerVector.Count(); ++i )
		{
			CKnownEntity known( livePlayerVector[i] );
			known.UpdatePosition();

			choose.Inspect( known );
		}
	}
	else
	{
		me->GetVisionInterface()->ForEachKnownEntity( choose );
	}

	return choose.m_selected;
}


//---------------------------------------------------------------------------------------------
/**
 * Return true if the given patient is healthy and safe for now
 */
bool CTFBotMedicHeal::IsStable( CTFPlayer *patient ) const
{
	const float safeTime = 3.0f;

	// if they are in combat, they are not stable
	if ( patient->GetTimeSinceLastInjury( GetEnemyTeam( patient->GetTeamNumber() ) ) < safeTime )
		return false;

	const float healthyRatio = 1.0f; // can be buffed higher
	if ( ( (float)patient->GetHealth() / (float)patient->GetMaxHealth() ) < healthyRatio )
		return false;

	if ( patient->m_Shared.InCond( TF_COND_BURNING ) )
		return false;

	if ( patient->m_Shared.InCond( TF_COND_BLEEDING ) )
		return false;

	return true;
}


//---------------------------------------------------------------------------------------------
class CFindMostInjuredNeighbor : public IVision::IForEachKnownEntity
{
public:
	CFindMostInjuredNeighbor( CTFBot *me, float maxRange, bool isInCombat )
	{
		m_me = me;
		m_mostInjured = NULL;
		m_injuredHealthRatio = 1.0f;
		m_isOnFire = false;
		m_maxRange = maxRange;
		m_isInCombat = isInCombat;
	}

    bool Inspect( const CKnownEntity &known )
	{
		if ( !known.GetEntity() || !known.GetEntity()->IsPlayer() )
			return true;

		CTFPlayer *player = ToTFPlayer( known.GetEntity() );
		if ( !player )
			return true;

		// Never consider ourselves
		if ( m_me->IsSelf( player ) )
			return true;

		// Must be alive teammate
		if ( !player->IsAlive() || !player->InSameTeam( m_me ) )
			return true;

		// Never heal cloaked/disguised Spies
		if ( player->m_Shared.IsStealthed() || player->m_Shared.InCond( TF_COND_DISGUISED ) )
			return true;

		// Never heal a teammate if they are already being healed
        bool bAlreadyHasMedic = false;
	        for ( int i = 0; i < player->m_Shared.GetNumHealers(); ++i )
	    {
	    	CBaseEntity *healer = player->m_Shared.GetHealerByIndex( i );
	    	if ( healer && healer->IsPlayer() && !m_me->IsSelf( healer ) )
	    	{
			bAlreadyHasMedic = true;
	        		break;
	    	}
	    }

	    if ( bAlreadyHasMedic )
		    return true;

		// These classes are less crucial to heal as they are usually far away anyways
		// Basically, Medics will look 50% less for these classes
		float effectiveRange = m_maxRange;
        if ( player->IsPlayerClass( TF_CLASS_SNIPER ) ||
			 player->IsPlayerClass( TF_CLASS_SPY ) ||
			 player->IsPlayerClass( TF_CLASS_MEDIC ) ||
			 player->IsPlayerClass( TF_CLASS_ENGINEER ) )
		{
			effectiveRange *= 0.5f;
		}

		if ( m_me->IsRangeGreaterThan( player, effectiveRange ) )
			return true;

		//if ( !m_me->IsLineOfFireClear( player->EyePosition() ) )
		//	return true;

		// ----- Existing health-ratio logic (most injured first) -----
		float maxHealth = m_isInCombat ? player->GetMaxHealth() : player->m_Shared.GetMaxBuffedHealth();
		float healthRatio = (float)player->GetHealth() / maxHealth;

		if ( m_isOnFire )
		{
			// only others on fire who have less health can trump
			if ( player->m_Shared.InCond( TF_COND_BURNING ) && healthRatio < m_injuredHealthRatio )
			{
				m_mostInjured = player;
				m_injuredHealthRatio = healthRatio;
			}
		}
		else
		{
			if ( player->m_Shared.InCond( TF_COND_BURNING ) )
			{
				// fire trumps
				m_mostInjured = player;
				m_injuredHealthRatio = healthRatio;
				m_isOnFire = true;
			}
			else
			{
				if ( healthRatio < m_injuredHealthRatio )
				{
					m_mostInjured = player;
					m_injuredHealthRatio = healthRatio;
				}
			}
		}

		return true;
	}

	CTFBot *m_me;
	CTFPlayer *m_mostInjured;
	float m_injuredHealthRatio;
	bool m_isOnFire;
	float m_maxRange;
	bool m_isInCombat;
};


//---------------------------------------------------------------------------------------------
bool CTFBotMedicHeal::CanDeployUber( CTFBot *me, const CWeaponMedigun* pMedigun ) const
{

	return true;
}


//---------------------------------------------------------------------------------------------
//
// Return true if we our charge is full, and it is an appropriate time to release uber.
// Don't use uber in setup.
// We don't pay attention to our patient here, because we might need to pop uber to save ourselves.
//
bool CTFBotMedicHeal::IsReadyToDeployUber( const CWeaponMedigun* pMedigun ) const
{
	if( !pMedigun )
		return false;

	if ( pMedigun->GetChargeLevel() < pMedigun->GetMinChargeAmount() )
		return false;
	
	if ( TFGameRules()->InSetup() )
		return false;
	
	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotMedicHeal::IsGoodUberTarget( CTFPlayer *who ) const
{
	if ( !who )
		return false;

	// Power classes are good Uber targets
	if ( who->IsPlayerClass( TF_CLASS_HEAVYWEAPONS ) ||
		 who->IsPlayerClass( TF_CLASS_SOLDIER ) ||
		 who->IsPlayerClass( TF_CLASS_PYRO ) ||
		 who->IsPlayerClass( TF_CLASS_DEMOMAN ) )
	{
		return true;
	}

	return false;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotMedicHeal::Update( CTFBot *me, float interval )
{
	// if we're in a squad, and the only other members are medics, disband the squad
	if ( me->IsInASquad() )
	{
		CTFBotSquad *squad = me->GetSquad();
		if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() && squad->IsLeader( me ) )
		{
			return ChangeTo( new CTFBotFetchFlag, "I'm now a squad leader! Going for the flag!" );
		}

		if ( !squad->ShouldPreserveSquad() )
		{
			CUtlVector< CTFBot * > memberVector;
			squad->CollectMembers( &memberVector );

			int i;
			for( i=0; i<memberVector.Count(); ++i )
			{
				if ( !memberVector[i]->IsPlayerClass( TF_CLASS_MEDIC ) )
				{
					break;
				}
			}

			if ( i == memberVector.Count() )
			{
				// squad is obsolete
				for( i=0; i<memberVector.Count(); ++i )
				{
					memberVector[i]->LeaveSquad();
				}
			}
		}
	}
	else
	{
		// not in a squad - for now, assume whatever mission I was on is over
		me->SetMission( CTFBot::NO_MISSION, MISSION_DOESNT_RESET_BEHAVIOR_SYSTEM );
	}

	m_patient = SelectPatient( me, m_patient );

    if ( m_bUberLocked )
	{
		CWeaponMedigun *pMed = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );
		if ( !pMed || ( !pMed->IsReleasingCharge() && !me->m_Shared.InCond( TF_COND_INVULNERABLE ) ) )
		{
			m_bUberLocked = false;
		}
	}

	// prevent a group of medic healing each other in a loop. always heal the top guy in the chain
	if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() && m_patient != NULL && m_patient->IsPlayerClass( TF_CLASS_MEDIC ) && me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
	{
		CUtlVector< CBaseEntity* > seenPatients;
		seenPatients.AddToTail( m_patient );

		while ( CBaseEntity* pTestPatient = m_patient->MedicGetHealTarget() )
		{
			if ( !pTestPatient->IsPlayer() || seenPatients.Find( pTestPatient ) != seenPatients.InvalidIndex() )
			{
				break;
			}

			seenPatients.AddToTail( pTestPatient );
			m_patient = ToTFPlayer( pTestPatient );
		}
	}

	if ( m_patient == NULL )
	{
		// no patients

		if ( TFGameRules()->IsMannVsMachineMode() && me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
		{
			// no-one is left to heal - get the flag!
			return ChangeTo( new CTFBotFetchFlag, "Everyone is gone! Going for the flag" );
		}

		if ( TFGameRules()->IsPVEModeActive() )
		{
			// don't retreat, just wait
			return Continue();
		}

		// no patients - retreat to spawn to find another one
		return SuspendFor( new CTFBotMedicRetreat, "Retreating to find another patient to heal" );
	}

	const float anchorRadius = 200.0f;
	if ( ( m_patient->GetAbsOrigin() - m_patientAnchorPos ).IsLengthGreaterThan( anchorRadius ) )
	{
		// our patient is on the move
		m_patientAnchorPos = m_patient->GetAbsOrigin();
		m_isPatientRunningTimer.Start( 3.0f );
	}

	// if our patient is teleporting away - follow them!
	if ( m_patient->m_Shared.InCond( TF_COND_SELECTED_TO_TELEPORT ) )
	{
		// find closest teleporter entrance to patient's location
		CObjectTeleporter *closeTeleporter = NULL;
		float closeRangeSq = FLT_MAX;

		CUtlVector< CBaseObject * > objVector;
		TheTFNavMesh()->CollectBuiltObjects( &objVector, me->GetTeamNumber() );

		for( int i=0; i<objVector.Count(); ++i )
		{
			if ( objVector[i]->GetType() == OBJ_TELEPORTER )
			{
				CObjectTeleporter *teleporter = (CObjectTeleporter *)objVector[i];

				if ( teleporter->IsEntrance() && teleporter->IsReady() )
				{
					float rangeSq = ( teleporter->GetAbsOrigin() - m_patient->GetAbsOrigin() ).LengthSqr();

					if ( rangeSq < closeRangeSq )
					{
						closeRangeSq = rangeSq;
						closeTeleporter = teleporter;
					}
				}
			}
		}

		if ( closeTeleporter )
		{
			return SuspendFor( new CTFBotUseTeleporter( closeTeleporter, CTFBotUseTeleporter::ALWAYS_USE ), "Following my patient through a teleporter" );
		}
	}


	CTFPlayer *actualHealTarget = m_patient;
	bool isHealTargetBlocked = true;
	bool isActivelyHealing = false;
	bool isUsingProjectileShield = false;
	const CKnownEntity *knownThreat = me->GetVisionInterface()->GetPrimaryKnownThreat();

	CWeaponMedigun *medigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );
	if ( medigun )
	{
		if( medigun->GetMedigunType() == MEDIGUN_RESIST )
		{
			// If I'm a Vaccinnator medic and am told to prefer a certain type of resist, then cycle to that resist
			while( ( me->HasAttribute( CTFBot::PREFER_VACCINATOR_BULLETS )	&& medigun->GetResistType() != MEDIGUN_BULLET_RESIST )
				|| ( me->HasAttribute( CTFBot::PREFER_VACCINATOR_BLAST )	&& medigun->GetResistType() != MEDIGUN_BLAST_RESIST )
				|| ( me->HasAttribute( CTFBot::PREFER_VACCINATOR_FIRE )		&& medigun->GetResistType() != MEDIGUN_FIRE_RESIST ) )
			{
				medigun->CycleResistType();
			}
		}

        // Opportunistic healing: look farther (1500 units) and stick until the target is healthy
        if ( !medigun->IsReleasingCharge() && !TFGameRules()->IsInTraining() && !me->IsInASquad() && !m_bUberLocked )
		{
			// Still have a previous opportunistic patient?
			if ( m_opportunisticPatient != NULL && m_opportunisticPatient->IsAlive() )
			{
				float healthRatio = (float)m_opportunisticPatient->GetHealth() / (float)m_opportunisticPatient->GetMaxHealth();
				bool bStillInRange = me->IsRangeLessThan( m_opportunisticPatient, tf_bot_medic_opportunistic_range.GetFloat() );
				bool bCanSee       = me->IsLineOfFireClear( m_opportunisticPatient->EyePosition() );

				if ( healthRatio < 0.99f && bStillInRange && ( bCanSee || !m_opportunisticStickTimer.IsElapsed() ) )
				{
					actualHealTarget = m_opportunisticPatient;
				}
				else
				{
					// Finished or lost them
					m_opportunisticPatient = NULL;
					m_opportunisticStickTimer.Invalidate();
				}
			}

			// Look for a new most-injured teammate (lowest health first)
			if ( m_opportunisticPatient == NULL )
			{
				bool isInCombat = actualHealTarget ? actualHealTarget->GetTimeSinceWeaponFired() < 1.0f : false;

				CFindMostInjuredNeighbor neighbor( me, tf_bot_medic_opportunistic_range.GetFloat(), isInCombat );
				me->GetVisionInterface()->ForEachKnownEntity( neighbor );

				if ( neighbor.m_mostInjured )
				{
					float healthRatio = neighbor.m_injuredHealthRatio;
					if ( healthRatio < 0.95f )		// meaningfully hurt
					{
						m_opportunisticPatient = neighbor.m_mostInjured;
						m_opportunisticStickTimer.Start( 10.0f );	// grace period
						actualHealTarget = m_opportunisticPatient;
					}
				}
			}
		}

		// juice 'em
		me->GetBodyInterface()->AimHeadTowards( actualHealTarget, IBody::CRITICAL, 1.0f, NULL, "Aiming at my patient" );

		if ( medigun->GetHealTarget() == NULL || medigun->GetHealTarget() == actualHealTarget )
		{
			// only hold fire button if we're healing who we think we're healing
			me->PressFireButton();
			isHealTargetBlocked = false;
			isActivelyHealing = ( medigun->GetHealTarget() != NULL );
		}
		else
		{
			// we're not healing who we want to, but we don't want to spam the medigun on/off so much
			if ( m_changePatientTimer.IsElapsed() )
			{
				// stop pressing fire for a moment to allow the medigun to select a new target
				m_changePatientTimer.Start( RandomFloat( 1.0f, 2.0f ) );
			}
			else
			{
				// keep building uber on wrong patient at least
				me->PressFireButton();
			}
		}

		// use uber if we've got it and we're under threat, or our patient was just hurt
		bool useUber = false;
		if ( IsReadyToDeployUber( medigun ) && CanDeployUber( me, medigun ) )
		{
			if( medigun->GetMedigunType() == MEDIGUN_RESIST )
			{
				// uber if I'm getting low and have recently taken damage
				if ( me->GetTimeSinceLastInjury( GetEnemyTeam( me->GetTeamNumber() ) ) < 1.0f )
				{
					useUber = true;
				}

				if( m_patient->GetTimeSinceLastInjury( GetEnemyTeam( m_patient->GetTeamNumber() ) ) < 1.0f )
				{
					useUber = true;
				}
			}
			else
			{
				// use uber if our patient's health is getting low
				const float healthyRatio = 0.5f;
				useUber = ( ( (float)m_patient->GetHealth() / (float)m_patient->GetMaxHealth() ) < healthyRatio );

				// don't uber our patient if he's already uber from some other source
				if ( m_patient->m_Shared.InCond( TF_COND_INVULNERABLE ) || m_patient->m_Shared.InCond( TF_COND_MEGAHEAL ) )
				{
					useUber = false;
				}

				// uber if I'm getting low and have recently taken damage
				if ( me->GetHealth() < me->GetUberHealthThreshold() )
				{
					if ( me->GetTimeSinceLastInjury( GetEnemyTeam( me->GetTeamNumber() ) ) < 1.0f || ( TFGameRules()->IsMannVsMachineMode() && me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS ) )
					{
						useUber = true;
					}
				}

				// also uber if I'm about to die!
				if ( me->GetHealth() < 25 )
				{
					useUber = true;
				}

				// special case for bots in mvm spawn zones
				if ( TFGameRules()->IsMannVsMachineMode() && me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS )
				{
					if ( m_patient->m_Shared.InCond( TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED ) && 
						 me->m_Shared.InCond( TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED ) )
					{
						useUber = false;
					}
				}
			}

            if ( useUber )
			{
				if ( !m_bUberLocked )
				{
					// Prefer current patient if it's a power class, otherwise reselect
					if ( m_patient && IsGoodUberTarget( m_patient ) )
					{
						// Already good
					}
					else
					{
						// Favor power classes when ubering
						m_patient = SelectPatient( me, NULL );
					}

					m_bUberLocked = true;
				}

				if ( !m_delayUberTimer.HasStarted() )
				{
					m_delayUberTimer.Start( me->GetUberDeployDelayDuration() );
				}
				
				if ( m_delayUberTimer.IsElapsed() )
				{
					m_delayUberTimer.Invalidate();
					me->PressAltFireButton();
				}
			}
		}
		
		// try to activate shield when I'm not using uber so I don't waste it
		if ( TFGameRules()->IsMannVsMachineMode() && me->HasAttribute( CTFBot::PROJECTILE_SHIELD ) )
		{
			isUsingProjectileShield = me->m_Shared.IsRageDraining();
			// when the rage is ready to deploy and we're not using uber
			if ( me->m_Shared.GetRageMeter() >= 100.f && !isUsingProjectileShield && !useUber )
			{
				// use shield if me or my patient is getting attacked
				if ( me->GetTimeSinceLastInjury( GetEnemyTeam( me->GetTeamNumber() ) ) < 1.0f || m_patient->GetTimeSinceLastInjury( GetEnemyTeam( m_patient->GetTeamNumber() ) ) < 1.0f )
				{
					me->PressSpecialFireButton();
					isUsingProjectileShield = true;
				}
			}
		}
	}

    bool isThreatened = false;
	if ( knownThreat && knownThreat->IsVisibleRecently() && knownThreat->GetEntity() )
	{
		if ( actualHealTarget )
		{
			float patientRangeSq = me->GetRangeSquaredTo( actualHealTarget );
			float threatRangeSq  = me->GetRangeSquaredTo( knownThreat->GetEntity() );
			isThreatened = threatRangeSq < patientRangeSq;
		}
		else
		{
			isThreatened = true;
		}
	}

	// Only consider "out of range" when we are significantly beyond heal range
	bool outOfHealRange = me->IsRangeGreaterThan( actualHealTarget, 1.25f * tf_bot_medic_max_heal_range.GetFloat() );
	bool isPatientObscured = actualHealTarget ? !me->IsLineOfFireClear( actualHealTarget->EyePosition() ) : true;

	// Prefer staying on the Medigun while we are trying to reach someone
	bool bWantCombatWeapon = !IsReadyToDeployUber( medigun )
						  && !me->m_Shared.InCond( TF_COND_INVULNERABLE )
						  && !isActivelyHealing
						  && !isUsingProjectileShield
						  && ( isThreatened || ( outOfHealRange && isPatientObscured ) );

	if ( bWantCombatWeapon )
	{
		me->EquipBestWeaponForThreat( knownThreat );
		if ( knownThreat && knownThreat->GetEntity() )
		{
			me->GetBodyInterface()->AimHeadTowards( knownThreat->GetEntity(), IBody::IMPORTANT, 1.0f, NULL, "Aiming at an enemy" );
		}
	}
	else
	{
		CBaseCombatWeapon *gun = me->Weapon_GetSlot( TF_WPN_TYPE_SECONDARY );
		if ( gun )
		{
			me->Weapon_Switch( gun );
		}
	}

    CTFPlayer *followTarget = m_patient;
	bool bForceChase = false;

	if ( m_opportunisticPatient != NULL && m_opportunisticPatient->IsAlive() )
	{
		// Actively leave the power class and go heal the injured teammate
		followTarget = m_opportunisticPatient;
		//bForceChase  = true;
	}

	if ( me->m_Shared.InCond( TF_COND_INVULNERABLE ) || isHealTargetBlocked || bForceChase )
	{
		// Chase the current priority target (opportunistic or main)
		if ( me->IsRangeGreaterThan( followTarget, tf_bot_medic_stop_follow_range.GetFloat() ) ||
			 !me->IsAbleToSee( followTarget, CBaseCombatCharacter::DISREGARD_FOV ) )
		{
			CTFBotPathCost cost( me, SAFEST_ROUTE ); // We are a VERY important guy... We need to be VERY safe...
			m_chasePath.Update( me, followTarget, cost );
		}
	}
	else
	{
		// Normal cover-following of the main patient
		if ( m_coverTimer.IsElapsed() || IsVisibleToEnemy( me, me->EyePosition() ) )
		{
			m_coverTimer.Start( RandomFloat( 0.5f, 1.0f ) );

		    CTFPlayer *oldPatient = m_patient;
		    if ( followTarget != m_patient )
		    	m_patient = followTarget;

	    	ComputeFollowPosition( me );

	    	m_patient = oldPatient;		// Restore
			CTFBotPathCost cost( me, SAFEST_ROUTE ); // We are a VERY important guy... We need to be VERY safe...
			m_coverPath.Compute( me, m_followGoal, cost );
		}
		m_coverPath.Update( me );
	}

    // MVM defender Medics: Ready up only when we have full Uber
	if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() &&
		 me->GetTeamNumber() == TF_TEAM_PVE_DEFENDERS )
	{
		CWeaponMedigun *pMedigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );
		if ( pMedigun && pMedigun->GetChargeLevel() >= 1.0f && m_patient != NULL )
		{
			// Ready up...
			if ( TFGameRules()->UsePlayerReadyStatusMode() &&
				 !TFGameRules()->IsPlayerReady( me->entindex() ) )
			{
				TFGameRules()->PlayerReadyStatus_UpdatePlayerState( me, true );
			}
		}
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotMedicHeal::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	m_chasePath.Invalidate();

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMedicHeal::OnStuck( CTFBot *me )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMedicHeal::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMedicHeal::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotMedicHeal::OnActorEmoted( CTFBot *me, CBaseCombatCharacter *emoter, int emote )
{
	if ( !emoter->IsPlayer() )
		return TryContinue();
	
	CTFPlayer *emotingPlayer = ToTFPlayer( emoter );

	switch( emote )
	{
	case MP_CONCEPT_PLAYER_MEDIC:
		// emoter is calling to be healed by a Medic
		// this is handled in SelectPatient()
		break;

	case MP_CONCEPT_PLAYER_GO:
	case MP_CONCEPT_PLAYER_ACTIVATECHARGE:
		// if our patient said this, and we have charge, deploy it!
		if ( m_patient && emotingPlayer && m_patient->entindex() == emotingPlayer->entindex() )
		{
			CWeaponMedigun *medigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );
			if ( IsReadyToDeployUber( medigun ) && CanDeployUber( me, medigun ) )
			{
				// start the uber
				me->PressAltFireButton();
			}
		}
		break;
	}


	return TryContinue();
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotMedicHeal::ShouldHurry( const INextBot *me ) const
{
	// never abandon our patient
	return ANSWER_YES;
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotMedicHeal::ShouldAttack( const INextBot *bot, const CKnownEntity *them ) const
{
	CTFBot *me = (CTFBot *)bot->GetEntity();

	// only attack if we're not wielding the medigun
	return me->IsCombatWeapon( MY_CURRENT_GUN ) ? ANSWER_YES : ANSWER_NO;
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotMedicHeal::ShouldRetreat( const INextBot *bot ) const
{
	CTFBot *me = (CTFBot *)bot->GetEntity();

	// retreat if stunned
	if ( me->m_Shared.IsControlStunned() || me->m_Shared.IsLoserStateStunned() )
		return ANSWER_YES;

	// never abandon our patient
	return ANSWER_NO;
}


//---------------------------------------------------------------------------------------------
class CKnownCollector:  public IVision::IForEachKnownEntity
{
public:
	virtual bool Inspect( const CKnownEntity &known )
	{
		m_vector.AddToTail( &known );
		return true;
	}

	CUtlVector< const CKnownEntity * > m_vector;
};


//---------------------------------------------------------------------------------------------
ConVar tf_bot_medic_cover_test_resolution( "tf_bot_medic_cover_test_resolution", "8", FCVAR_CHEAT );

void CTFBotMedicHeal::ComputeFollowPosition( CTFBot *me )
{
	VPROF_BUDGET( "CTFBotMedicHeal::ComputeFollowPosition", "NextBot" );

	m_followGoal = me->GetAbsOrigin();

	if ( m_patient == NULL )
		return;

	bool isExposed = true;

	if ( TFGameRules()->IsMannVsMachineMode() && me->GetTeamNumber() == TF_TEAM_PVE_INVADERS )
	{
		// robot medics in MvM don't care if the enemy sees them
		isExposed = false;
	}
	else
	{
		isExposed = IsVisibleToEnemy( me, me->EyePosition() );
	}

	Vector patientForward;
	m_patient->EyeVectors( &patientForward );
	patientForward.z = 0.0f;
	patientForward.NormalizeInPlace();

	bool isNearPatient = me->IsRangeLessThan( m_patient, tf_bot_medic_start_follow_range.GetFloat() ) &&
						 me->IsAbleToSee( m_patient, CBaseCombatCharacter::DISREGARD_FOV );

	// If we are NOT exposed, just try to stay near
	if ( !isExposed )
	{
		if ( isNearPatient )
		{
			// Move behind the patient if we are in front of him and he hasn't fired recently
			Vector toPatient = m_patient->GetAbsOrigin() - me->GetAbsOrigin();
			if ( !TFGameRules()->InSetup() &&
				 m_patient->GetTimeSinceWeaponFired() > 5.0f &&
				 DotProduct( patientForward, toPatient ) < 0.0f )
			{
				m_followGoal = m_patient->GetAbsOrigin() - tf_bot_medic_stop_follow_range.GetFloat() * patientForward;
			}
			else
			{
				// we're good where we are
				m_followGoal = me->GetAbsOrigin();
			}
		}
		else
		{
			// get closer to our patient
			m_followGoal = m_patient->GetAbsOrigin();
		}
		return;
	}

	// We ARE exposed, search for cover around our patient
	Vector bestCoverPos = vec3_origin;
	float bestCoverRangeSq = FLT_MAX;
	bool bFoundCover = false;

	trace_t trace;
	NextBotTraceFilterIgnoreActors traceFilter( NULL, COLLISION_GROUP_NONE );

	const float inc = M_PI / tf_bot_medic_cover_test_resolution.GetFloat();
	const float radiusInc = 75.0f;

	float maxRadius = tf_bot_medic_max_heal_range.GetFloat();
	CWeaponMedigun *medigun = dynamic_cast< CWeaponMedigun * >( me->m_Shared.GetActiveTFWeapon() );

	// stay close if our patient is on the move, or we have an uber ready
	if ( IsPatientRunning() || IsReadyToDeployUber( medigun ) )
	{
		maxRadius = tf_bot_medic_start_follow_range.GetFloat();
	}

    const float minCoverRadius = 10.0f;

    for ( float radius = minCoverRadius + RandomFloat( 0.0f, radiusInc );
	      radius <= maxRadius;
	      radius += radiusInc )
	{
		for ( float angle = 0.0f; angle <= 2.0f * M_PI; angle += inc )
		{
			Vector offset;
			SinCos( angle, &offset.y, &offset.x );
			offset.z = 0.0f;

			Vector testPos = m_patient->WorldSpaceCenter() + radius * offset;

			// Trace from patient to the potential cover spot
			UTIL_TraceLine( m_patient->WorldSpaceCenter(), testPos,
							MASK_OPAQUE | CONTENTS_IGNORE_NODRAW_OPAQUE | CONTENTS_MONSTER,
							&traceFilter, &trace );

			Vector actualPos = trace.endpos;

			// Back up a little so the Medic has room to stand
			if ( trace.DidHit() )
			{
				actualPos -= 0.6f * me->GetBodyInterface()->GetHullWidth() * offset;
			}

			// Put it on the ground
			TheNavMesh->GetSimpleGroundHeight( actualPos, &actualPos.z );

			// Skip spots that are too low relative to the patient
			if ( m_patient->GetAbsOrigin().z - actualPos.z > me->GetLocomotionInterface()->GetStepHeight() )
				continue;

			// Eye height for visibility tests
			Vector eyePos = actualPos + Vector( 0, 0, HumanEyeHeight );

			// IMPORTANT: Must still be able to see the patient from this cover
			if ( !me->IsLineOfFireClear( m_patient->EyePosition(), eyePos ) )
				continue;

			// Must NOT be visible to any known enemy
			if ( IsVisibleToEnemy( me, eyePos ) )
			{
				if ( tf_bot_medic_cover_debug.GetBool() )
				{
					NDebugOverlay::Cross3D( actualPos, 5.0f, 255, 0, 0, true, 1.0f );
					NDebugOverlay::Line( m_patient->WorldSpaceCenter(), actualPos, 255, 0, 0, true, 1.0f );
				}
				continue;
			}

			// This is a valid cover spot, keep the closest one to our current position
			float rangeSq = ( me->EyePosition() - eyePos ).LengthSqr();
			if ( rangeSq < bestCoverRangeSq )
			{
				bestCoverRangeSq = rangeSq;
				bestCoverPos = actualPos;
				bFoundCover = true;
			}

			if ( tf_bot_medic_cover_debug.GetBool() )
			{
				NDebugOverlay::Cross3D( actualPos, 5.0f, 0, 255, 0, true, 1.0f );
				NDebugOverlay::Line( m_patient->WorldSpaceCenter(), actualPos, 0, 255, 0, true, 1.0f );
			}
		}
	}

	//  Decide cover goal
	if ( bFoundCover )
	{
		// We found real cover that still lets us see our patient
		m_followGoal = bestCoverPos;
	}
	else
	{
		// No cover available, stand directly behind our patient
		m_followGoal = m_patient->GetAbsOrigin() - ( tf_bot_medic_stop_follow_range.GetFloat() + 20.0f ) * patientForward;
		TheNavMesh->GetSimpleGroundHeight( m_followGoal, &m_followGoal.z );
	}
}


//---------------------------------------------------------------------------------------------
bool CTFBotMedicHeal::IsVisibleToEnemy( CTFBot *me, const Vector &where ) const
{
	CKnownCollector known;
	me->GetVisionInterface()->ForEachKnownEntity( known );

	trace_t trace;

	for( int i=0; i<known.m_vector.Count(); ++i )
	{
		CBaseCombatCharacter *threat = known.m_vector[i]->GetEntity()->MyCombatCharacterPointer();

		if ( threat && me->IsEnemy( threat ) )
		{
			if ( threat->IsLineOfSightClear( where, CBaseCombatCharacter::IGNORE_ACTORS ) )
			{
				return true;
			}
		}
	}

	return false;
}
