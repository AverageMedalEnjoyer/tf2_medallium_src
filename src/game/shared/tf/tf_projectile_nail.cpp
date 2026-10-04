//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF Nail
//
//=============================================================================
#include "cbase.h"
#include "tf_projectile_nail.h"
#include "tf_gamerules.h"

#ifdef CLIENT_DLL
#include "c_basetempentity.h"
#include "c_te_legacytempents.h"
#include "c_te_effect_dispatch.h"
#include "input.h"
#include "c_tf_player.h"
#include "cliententitylist.h"
#endif

#ifdef GAME_DLL
#include "tf_player.h"
#endif

#define BLUTSAUGER_INDEX 36


//=============================================================================
//
// TF Syringe Projectile functions (Server specific).
//
#define SYRINGE_MODEL				"models/weapons/w_models/w_syringe_proj.mdl"
#define SYRINGE_DISPATCH_EFFECT		"ClientProjectile_Syringe"
#define SYRINGE_LEECH_MODEL			"models/weapons/c_models/c_leechgun/c_leech_proj.mdl"

LINK_ENTITY_TO_CLASS( tf_projectile_syringe, CTFProjectile_Syringe );
PRECACHE_REGISTER( tf_projectile_syringe );


short g_sModelIndexSyringe;
void PrecacheSyringe(void *pUser)
{
	g_sModelIndexSyringe = modelinfo->GetModelIndex( SYRINGE_MODEL );
}

PRECACHE_REGISTER_FN(PrecacheSyringe);


short g_sModelIndexSyringeLeech;
void PrecacheSyringeLeech(void *pUser)
{
	g_sModelIndexSyringeLeech = modelinfo->GetModelIndex( SYRINGE_LEECH_MODEL );
}

PRECACHE_REGISTER_FN(PrecacheSyringeLeech);

//-----------------------------------------------------------------------------
// CTFProjectile_Syringe
//-----------------------------------------------------------------------------
#define SYRINGE_GRAVITY		0.3f
#define SYRINGE_VELOCITY	1000.0f
// Purpose:
//-----------------------------------------------------------------------------
CTFBaseProjectile *CTFProjectile_Syringe::Create( 
	const Vector &vecOrigin, 
	const QAngle &vecAngles, 
	CTFWeaponBaseGun *pLauncher /*= NULL*/,
	CBaseEntity *pOwner /*= NULL*/, 
	CBaseEntity *pScorer /*= NULL*/, 
	bool bCritical /*= false */
)
{
	int nModelIndex = g_sModelIndexSyringe;

	if ( pLauncher )
	{
		CEconItemView *pItem = pLauncher->GetAttributeContainer()
			? pLauncher->GetAttributeContainer()->GetItem() : NULL;

		if ( pItem && pItem->GetItemDefIndex() == BLUTSAUGER_INDEX )
			nModelIndex = g_sModelIndexSyringeLeech;
	}

	// Safety: never pass -1 - IDK why this fixes Client Projectile error, but it works!
	if ( nModelIndex <= 0 )
		nModelIndex = g_sModelIndexSyringe;

	return CTFBaseProjectile::Create( "tf_projectile_syringe", vecOrigin, vecAngles, pOwner, SYRINGE_VELOCITY, nModelIndex, SYRINGE_DISPATCH_EFFECT, pScorer, bCritical );
}

//-----------------------------------------------------------------------------
// Purpose: For Syringe Projectiles, Returns the correct projectile model path for the syringe based on the owner’s active weapon.
//-----------------------------------------------------------------------------
const char *CTFProjectile_Syringe::GetProjectileModelName( void )
{
	// Fallback; Create already chooses the index, but this is useful if anything else queries the name
	CBaseEntity *pOwner = GetOwnerEntity();
	if ( pOwner )
	{
		CTFPlayer *pPlayer = ToTFPlayer( pOwner );
		if ( pPlayer )
		{
			CTFWeaponBase *pWeapon = pPlayer->GetActiveTFWeapon();
			if ( pWeapon )
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer() ? pWeapon->GetAttributeContainer()->GetItem() : NULL;
				if ( pItem && pItem->GetItemDefIndex() == BLUTSAUGER_INDEX )
					return SYRINGE_LEECH_MODEL;
			}
		}
	}
	return SYRINGE_MODEL;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
unsigned int CTFProjectile_Syringe::PhysicsSolidMaskForEntity( void ) const
{ 
	return BaseClass::PhysicsSolidMaskForEntity() | CONTENTS_REDTEAM | CONTENTS_BLUETEAM;
}

//-----------------------------------------------------------------------------
float CTFProjectile_Syringe::GetGravity( void )
{
	return SYRINGE_GRAVITY;
}


#ifdef CLIENT_DLL

//-----------------------------------------------------------------------------
void GetSyringeTrailParticleName( CTFPlayer *pPlayer, CAttribute_String *attrParticleName, bool bCritical )
{
	int iTeamNumber = TF_TEAM_RED;	
	if ( pPlayer )
	{
		iTeamNumber = pPlayer->GetTeamNumber();
		CTFWeaponBase *pWeapon = pPlayer->GetActiveTFWeapon();
		if ( pWeapon )
		{
			static CSchemaAttributeDefHandle pAttrDef_ParticleName( "projectile particle name" );
			CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if ( pAttrDef_ParticleName && pItem )
			{
				if ( pItem->FindAttribute( pAttrDef_ParticleName, attrParticleName ) )
				{
					const char * pParticleName = attrParticleName->value().c_str();
					if ( iTeamNumber == TF_TEAM_BLUE && V_stristr( pParticleName, "_teamcolor_red" ))
					{
						static char pBlue[256];
						V_StrSubst( attrParticleName->value().c_str(), "_teamcolor_red", "_teamcolor_blue", pBlue, 256 );
						attrParticleName->set_value( pBlue );
					}
					return;
				}
			}
		}
	}
	
	if ( iTeamNumber == TF_TEAM_BLUE )
	{
		attrParticleName->set_value( bCritical ? "nailtrails_medic_blue_crit" : "nailtrails_medic_blue" );
	}
	else
	{
		attrParticleName->set_value( bCritical ? "nailtrails_medic_red_crit" : "nailtrails_medic_red" );
	}
	return;
}

//-----------------------------------------------------------------------------
// Purpose: For Syringe Projectiles, Add effects
//-----------------------------------------------------------------------------
void ClientsideProjectileSyringeCallback( const CEffectData &data )
{
	// Get the syringe and add it to the client entity list, so we can attach a particle system to it.
	C_TFPlayer *pPlayer = dynamic_cast<C_TFPlayer*>( ClientEntityList().GetBaseEntityFromHandle( data.m_hEntity ) );
	if ( !pPlayer )
		return;

	C_LocalTempEntity *pSyringe = ClientsideProjectileCallback( data, SYRINGE_GRAVITY );
	if ( !pSyringe )
		return;

	// Choose the correct visual model for local player
	const char *pszModel = SYRINGE_MODEL;
	CTFWeaponBase *pWeapon = pPlayer->GetActiveTFWeapon();
	if ( pWeapon )
	{
		CEconItemView *pItem = pWeapon->GetAttributeContainer()
			? pWeapon->GetAttributeContainer()->GetItem() : NULL;
		if ( pItem && pItem->GetItemDefIndex() == BLUTSAUGER_INDEX )
			pszModel = SYRINGE_LEECH_MODEL;
	}

	// Force the model
	pSyringe->SetModel( pszModel );

	pSyringe->m_nSkin = ( pPlayer->GetTeamNumber() == TF_TEAM_RED ) ? 0 : 1;

	bool bCritical = ( ( data.m_nDamageType & DMG_CRITICAL ) != 0 );
	CAttribute_String attrParticleName;
	GetSyringeTrailParticleName( pPlayer, &attrParticleName, bCritical );

	pSyringe->AddParticleEffect( attrParticleName.value().c_str() );
	pSyringe->AddEffects( EF_NOSHADOW );
	pSyringe->flags |= FTENT_USEFASTCOLLISIONS;
}

DECLARE_CLIENT_EFFECT( SYRINGE_DISPATCH_EFFECT, ClientsideProjectileSyringeCallback );

#endif
