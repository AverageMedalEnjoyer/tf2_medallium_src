//========= Copyright Valve Corporation, All rights reserved. ============//
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

/*
//=============================================================================
//
// Umbrella Weapon tables.
//

ConVar tf2m_civilian_buff_range("tf2m_civilian_buff_range", "3000.0", FCVAR_NONE, "Sets the distance Civilian can buff people using the umbrella.");

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
*/

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
