//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_use_item.h
// Equip and consume an item
// Michael Booth, July 2011

#include "cbase.h"
#include "tf_weaponbase.h"
#include "tf_weapon_jar.h"
#include "bot/tf_bot.h"
#include "bot/behavior/tf_bot_use_item.h"

extern ConVar sv_gravity;


//---------------------------------------------------------------------------------------------
CTFBotUseItem::CTFBotUseItem( CTFWeaponBase *item, CBaseEntity *target )
{
	m_item = item;
	m_target = target;
	m_bHasTarget = ( target != NULL );
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotUseItem::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	// force-equip the item we're going to use
	me->PushRequiredWeapon( m_item );

	m_cooldownTimer.Start( m_item->m_flNextPrimaryAttack - gpGlobals->curtime + 0.25f );

	// give ourselves a moment after it's out to line up a throw
	m_aimTimer.Start( m_cooldownTimer.GetRemainingTime() + 1.0f );

	return Continue();
}


//---------------------------------------------------------------------------------------------
// Where to aim a thrown jar so it comes down on the target
static Vector GetThrowAimSpot( CTFBot *me, CTFWeaponBase *item, CBaseEntity *target )
{
	Vector spot = target->WorldSpaceCenter();

	float flSpeed = 1000.0f;
	CTFJar *jar = dynamic_cast< CTFJar * >( item );
	if ( jar )
	{
		flSpeed = MAX( 1.0f, jar->GetProjectileSpeed() );
	}

	// jars leave the hand with some lift, then drop - aim high by however much more they'll drop than rise
	float flTime = me->GetRangeTo( spot ) / flSpeed;
	float flDrop = 0.5f * sv_gravity.GetFloat() * flTime * flTime - 200.0f * flTime;
	if ( flDrop > 0.0f )
	{
		spot.z += flDrop;
	}

	return spot;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotUseItem::Update( CTFBot *me, float interval )
{
	if ( m_item == NULL )
	{
		return Done( "NULL item" );
	}

	CTFWeaponBase *myCurrentWeapon = me->m_Shared.GetActiveTFWeapon();

	if ( !myCurrentWeapon )
	{
		return Done( "NULL weapon" );
	}

	if ( m_cooldownTimer.HasStarted() )
	{
		bool bReadyToThrow = true;

		if ( m_bHasTarget )
		{
			CBaseEntity *target = m_target;
			if ( !target || !target->IsAlive() )
			{
				return Done( "Throw target is gone" );
			}

			me->GetBodyInterface()->AimHeadTowards( GetThrowAimSpot( me, m_item, target ), IBody::CRITICAL, 0.3f, NULL, "Lining up a throw" );

			bReadyToThrow = me->GetBodyInterface()->IsHeadAimingOnTarget() || m_aimTimer.IsElapsed();
		}

		if ( m_cooldownTimer.IsElapsed() && bReadyToThrow )
		{
			// use it
			me->PressFireButton();
			m_cooldownTimer.Invalidate();
		}
	}
	else // used
	{
		// some items use the taunt system - wait for the taunt to end
		if ( !me->IsTaunting() )
		{
			return Done( "Item used" );
		}
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CTFBotUseItem::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->PopRequiredWeapon();
}
