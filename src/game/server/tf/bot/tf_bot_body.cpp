//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_body.cpp
// Team Fortress NextBot body interface
// Michael Booth, May 2010

#include "cbase.h"

#include "tf_bot.h"
#include "tf_bot_body.h"


// 
// Return how often we should sample our target's position and 
// velocity to update our aim tracking, to allow realistic slop in tracking
//
float CTFBotBody::GetHeadAimTrackingInterval( void ) const
{
	CTFBot *me = (CTFBot *)GetBot();

	// Don't let Spies in MvM mode aim too precisely
	if ( TFGameRules()->IsMannVsMachineMode() &&
		 me->GetTeamNumber() != TF_TEAM_PVE_DEFENDERS &&
		 me->IsPlayerClass( TF_CLASS_SPY ) )
	{
		return 0.15f;
	}

	// Adjust tracking interval based on bot difficulty
	switch ( me->GetDifficulty() )
	{
	case CTFBot::EXPERT:	return 0.033f;
	case CTFBot::HARD:		return 0.05f;
	case CTFBot::NORMAL:	return 0.08f;
	case CTFBot::EASY:		return 0.15f;
	}

	return 0.05f;
}

//-----------------------------------------------------------------------------
// Lead our shots based on the speed of our target and the bot's difficulty level
//-----------------------------------------------------------------------------
float CTFBotBody::GetHeadAimSubjectLeadTime( void ) const
{
	CTFBot *me = (CTFBot *)GetBot();
	if ( !me )
		return 0.0f;

	// Hitscan weapons usually hit the target instantly, so we don't need to lead our shots at all.
	CTFWeaponBase *pWeapon = me->GetActiveTFWeapon();
	if ( pWeapon && me->IsHitScanWeapon( pWeapon ) )
		return 0.0f;

	// Only lead when we actually have a enemy to lead.
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( false );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
		return 0.0f;

	CBaseEntity *pSubject = threat->GetEntity();
	Vector vel = pSubject->GetAbsVelocity();
	float speed = vel.Length2D();

	if ( speed < 10.0f )			// Basically stationary
		return 0.0f;

	float flLead = clamp( speed / 300.0f, 0.05f, 0.35f );

	// Adjust lead based on bot difficulty
	switch ( me->GetDifficulty() )
	{
	case CTFBot::EXPERT:	flLead *= 1.25f;	break;
	case CTFBot::HARD:		flLead *= 1.45f;	break;
	case CTFBot::NORMAL:	flLead *= 0.65f;	break;
	case CTFBot::EASY:		flLead *= 0.85f;	break;
	}

	// Get the distance to our subject
	Vector eyePos = me->EyePosition();
	Vector subjectPos = pSubject->WorldSpaceCenter();
	float flDistToSubject = ( subjectPos - eyePos ).Length();

	const int kMaxIterations = 8;
	for ( int i = 0; i < kMaxIterations && flLead > 0.02f; ++i )
	{
		Vector predictedPos = subjectPos + vel * flLead;

		trace_t tr;
		UTIL_TraceLine( eyePos, predictedPos, MASK_SHOT, me->GetEntity(), COLLISION_GROUP_NONE, &tr );

		// Do we have a clear line of sight to the predicted position? If not, reduce the lead time and try again.
		if ( !tr.DidHit() || ( tr.endpos - eyePos ).Length() >= flDistToSubject * 0.92f )
			break;

		flLead *= 0.65f;
	}

	return flLead;
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

float CTFBotBody::GetMaxHeadAngularVelocity( void ) const
{
	CTFBot *me = (CTFBot *)GetBot();
	switch ( me->GetDifficulty() )
	{
	case CTFBot::EXPERT:	return 720.0f;
	case CTFBot::HARD:		return 540.0f;
	case CTFBot::NORMAL:	return 400.0f;
	case CTFBot::EASY:		return 280.0f;
	}
	return 500.0f;
}