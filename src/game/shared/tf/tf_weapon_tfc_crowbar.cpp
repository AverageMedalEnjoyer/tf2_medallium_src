//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======
//
// Purpose: 
//
//=============================================================================

#include "cbase.h"
#include "tf_weapon_tfc_crowbar.h"
#include "decals.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "c_ai_basenpc.h"
// Server specific.
#else
#include "tf_player.h"
#include "ai_basenpc.h"
#endif

//=============================================================================
//
// Weapon Crowbar tables.
//
CREATE_SIMPLE_WEAPON_TABLE(TFCCrowbar, tf_weapon_tfc_crowbar)
//CREATE_SIMPLE_WEAPON_TABLE( TFCUmbrella, tf_weapon_tfc_umbrella )

//=============================================================================
//
// Umbrella Weapon tables.
//

ConVar fc_civilian_buff_range("fc_civilian_buff_range", "256.0", FCVAR_NONE, "Sets the distance Civilian can buff people using the umbrella.");

IMPLEMENT_NETWORKCLASS_ALIASED(TFCUmbrella, DT_TFC_Umbrella)

BEGIN_NETWORK_TABLE(CTFCUmbrella, DT_TFC_Umbrella)
#ifdef CLIENT_DLL
	//RecvPropTime(RECVINFO(m_flNextFireTime)),
	RecvPropBool(RECVINFO(m_bFiring)),
#else
	//SendPropTime(SENDINFO(m_flNextFireTime)),
	SendPropBool(SENDINFO(m_bFiring)),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA(CTFCUmbrella)
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS(tf_weapon_tfc_umbrella, CTFCUmbrella);
PRECACHE_WEAPON_REGISTER(tf_weapon_tfc_umbrella);

//=============================================================================
//
// Weapon Crowbar functions.
//

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFCCrowbar::CTFCCrowbar()
{
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFCUmbrella::CTFCUmbrella()
{
	UseClientSideAnimation();
	StartEffectBarRegen();
}


// Purpose: Swing the umbrella
//-----------------------------------------------------------------------------
void CTFCUmbrella::PrimaryAttack(void)
{
	if (m_flNextPrimaryAttack > gpGlobals->curtime)
		return;

	BaseClass::PrimaryAttack();
}

//-----------------------------------------------------------------------------
// Purpose: Alt. Fire
//-----------------------------------------------------------------------------
void CTFCUmbrella::SecondaryAttack(void)
{
	if (m_flNextPrimaryAttack > gpGlobals->curtime || m_flNextSecondaryAttack > gpGlobals->curtime || m_bFiring)
		return;

	CTFPlayer *pPlayer = GetTFPlayerOwner();

	if ((!pPlayer || !pPlayer->CanAttack()) || GetEffectBarProgress() < 1.0f )
		return;

	trace_t tr;
	Vector vecStart, vecEnd, vecDir;
	AngleVectors(pPlayer->EyeAngles(), &vecDir);

	vecStart = pPlayer->EyePosition();
	vecEnd = vecStart + (vecDir * fc_civilian_buff_range.GetFloat());

	//CTraceFilterIgnorePlayers *pFilter = new CTraceFilterIgnorePlayers( this, COLLISION_GROUP_NONE );
	CTraceFilterSimple *pFilter = new CTraceFilterSimple(pPlayer, COLLISION_GROUP_NONE);

	UTIL_TraceLine(vecStart, vecEnd, MASK_ALL, pFilter, &tr);

	//UTIL_Portal_TraceRay( ray, MASK_ALL, pFilter, &tr );

	// A wall is stopping our fire
	if (tr.DidHitWorld() || !tr.m_pEnt)
		return;

	CTFPlayer *pTarget = ToTFPlayer(tr.m_pEnt);
	//CAI_BaseNPC *pNPC = tr.m_pEnt->MyNPCPointer(); // no NPC-ally support in this project yet

	if (pPlayer->InSameTeam(tr.m_pEnt) || ( pTarget && ( ( pTarget->m_Shared.InCond( TF_COND_DISGUISED ) ) && ( pTarget->m_Shared.GetDisguiseTeam() == pPlayer->GetTeamNumber() ) ) ) )
	{
		if (pTarget)
		{
			SendWeaponAnim(ACT_VM_SECONDARYATTACK);
			pTarget->m_Shared.AddCond((ETFCond)GetBuffType( GetUmbrellaType() ), 8.0f);
			//SetEffectBarProgress(-15.0f);
		}
		//else if (pNPC)
		//{
		//	SendWeaponAnim(ACT_VM_SECONDARYATTACK);
		//	pNPC->AddCond(GetBuffType( GetUmbrellaType() ), 8.0f);
		//	//SetEffectBarProgress(-15.0f);
		//}

		SendWeaponAnim(ACT_MP_GESTURE_VC_FINGERPOINT_MELEE);
		pPlayer->DoAnimationEvent(PLAYERANIMEVENT_CUSTOM_GESTURE, ACT_MP_GESTURE_VC_FINGERPOINT_MELEE);
	}
	else
	{
		// If we didn't have a valid target, don't set next fire time or recharge time
		return;
	}

	m_flNextPrimaryAttack = gpGlobals->curtime + 1.0f;
	// Wait until next fully charged time?
	m_flNextSecondaryAttack = gpGlobals->curtime + InternalGetEffectBarRechargeTime();
	// This is done in SendWeaponAnim by animation time
	//SetWeaponIdleTime(m_flNextFireTime);

	// There is nothing that un-sets this
	//m_bFiring = true;

	StartEffectBarRegen();

#ifdef GAME_DLL
	if (pPlayer->m_Shared.InCond(TF_COND_STEALTHED))
		pPlayer->RemoveInvisibility();
#endif
}

float CTFCUmbrella::InternalGetEffectBarRechargeTime(void)
{
	if (CAttributeManager::AttribHookValue<float>(0, "item_meter_charge_rate", this) > 0)
		return (CAttributeManager::AttribHookValue<float>(0, "item_meter_charge_rate", this));

	return 15.0f;
}

int CTFCUmbrella::GetBuffType( int iMode )
{

	switch ( iMode )
	{

	case 1:
		return (int)FC_COND_CIVILIAN_ENERGY_BUFF;

	case 2:
		return (int)TF_COND_REGENONDAMAGEBUFF;

	default:
		return (int)FC_COND_CIVILIAN_ENERGY_BUFF;
	}

}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
float CTFCUmbrella::GetMeleeDamage( CBaseEntity *pTarget, int *piDamageType, int *piCustomDamage )
{
	// Start with our base damage. We use this to generate our custom damage flags,
	// if any. We may trash the damage amount.
	float fDamage = BaseClass::GetMeleeDamage( pTarget, piDamageType, piCustomDamage );

	// The katana is a weapon of honor!!!! (Hitting someone wielding a katana with
	// your katana results in a massive damage boost, a one-hit kill.)
	if ( IsHonorBound() )
	{
		CTFPlayer *pTFPlayerTarget = ToTFPlayer( pTarget );
		if ( pTFPlayerTarget )
		{
			// If our victim is wielding the weapon we're looking for, bump the damage way up.
			if ( pTFPlayerTarget->GetActiveTFWeapon() && pTFPlayerTarget->GetActiveTFWeapon()->IsHonorBound() )
			{
				fDamage = MAX( fDamage, pTFPlayerTarget->GetHealth() * 3 );
				*piDamageType |= DMG_DONT_COUNT_DAMAGE_TOWARDS_CRIT_RATE;
				//piCustomDamage = TF_DMG_CUSTOM_DECAPITATION;
			}
		}
	}

	return fDamage;
}
