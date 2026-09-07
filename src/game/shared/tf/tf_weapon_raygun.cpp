//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_raygun.h"
#include "tf_fx_shared.h"
#include "in_buttons.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "particle_property.h"
#else
#include "tf_player.h"
#include "ndebugoverlay.h"
#include "particle_parse.h"
#include "tf_fx.h"
#include "tf_gamestats.h"
#include "tf_projectile_energy_ring.h"
#endif


//============================

IMPLEMENT_NETWORKCLASS_ALIASED( TFRaygun, DT_WeaponRaygun )

BEGIN_NETWORK_TABLE( CTFRaygun, DT_WeaponRaygun )
#ifdef GAME_DLL
	SendPropBool( SENDINFO( m_bUseNewProjectileCode ) ),
	SendPropFloat( SENDINFO( m_flChargeBeginTime ) ),
	SendPropInt( SENDINFO( m_iChargeEffect ) ),
	SendPropBool( SENDINFO( m_bChargedShot ) ),
#else
	RecvPropBool( RECVINFO( m_bUseNewProjectileCode ) ),
	RecvPropFloat( RECVINFO( m_flChargeBeginTime ) ),
	RecvPropInt( RECVINFO( m_iChargeEffect ) ),
	RecvPropBool( RECVINFO( m_bChargedShot ) ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFRaygun )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_raygun, CTFRaygun );
PRECACHE_WEAPON_REGISTER( tf_weapon_raygun );

//============================
IMPLEMENT_NETWORKCLASS_ALIASED( TFDRGPomson, DT_WeaponDRGPomson )

BEGIN_NETWORK_TABLE( CTFDRGPomson, DT_WeaponDRGPomson )
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFDRGPomson )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_drg_pomson, CTFDRGPomson );
PRECACHE_WEAPON_REGISTER( tf_weapon_drg_pomson );


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFRaygun::CTFRaygun()
{
	m_bUseNewProjectileCode = false;
#ifdef GAME_DLL
	// Goofyness to preserve demos.  Old demos wont have this set on the client
	// so we'll know to use the old code path.
	m_bUseNewProjectileCode = true;
#endif
    m_flIrradiateTime = 0.f;
	m_bEffectsThinking = false;
	m_flChargeBeginTime = 0.f;
	m_bChargedShot = false;
	m_iChargeEffect = 0;
	m_iChargeEffectBase = 0;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFRaygun::Precache()
{
	PrecacheParticleSystem( "drg_bison_impact" );
	PrecacheParticleSystem( "drg_bison_idle" );
	PrecacheParticleSystem( "drg_bison_muzzleflash" );

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
const char *CTFRaygun::GetMuzzleFlashParticleEffect( void )
{
	return "drg_bison_muzzleflash";
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::PrimaryAttack( void )
{
	if ( m_flChargeBeginTime > 0 )
		return;

	if ( !Energy_HasEnergy() )
		return;

	m_bChargedShot = false;
	BaseClass::PrimaryAttack();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::SecondaryAttack( void )
{
	// Ensure weapon has a full clip/energy pool to charge
	if ( !Energy_FullyCharged() )
	{
		Reload();
		return;
	}

	if ( m_flNextPrimaryAttack > gpGlobals->curtime )
		return;

	if ( m_flChargeBeginTime > 0 )
		return;

	if ( !CanAttack() )
	{
		m_flChargeBeginTime = 0;
		return;
	}

	m_bChargedShot = true;
	m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;
	m_flChargeBeginTime = gpGlobals->curtime;

	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		pPlayer->m_Shared.AddCond( TF_COND_AIMING );
		EmitSound( "Weapon_CowMangler.Charging" );
		pPlayer->TeamFortress_SetSpeed();
	}

	m_iChargeEffect++;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::FireChargedShot()
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	if ( !pPlayer->IsAlive() )
		return;

	StopSound( "Weapon_CowMangler.Charging" );

	pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
	pPlayer->TeamFortress_SetSpeed();

    SendWeaponAnim( ACT_VM_PRIMARYATTACK );
	pPlayer->SetAnimation( PLAYER_ATTACK1 );

#ifdef GAME_DLL
	CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, false );
#endif

	CBaseEntity* pProj = FireProjectile( pPlayer );
	ModifyProjectile( pProj );

	float flFireDelay = ApplyFireDelay( m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeFireDelay );
	m_flNextPrimaryAttack = gpGlobals->curtime + flFireDelay;
	SetWeaponIdleTime( gpGlobals->curtime + SequenceDuration() );

	m_flChargeBeginTime = 0.0f;
	m_bChargedShot = false;
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::CreateChargeEffect()
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		DispatchParticleEffect( "drg_cowmangler_muzzleflash_chargeup", PATTACH_POINT_FOLLOW, GetAppropriateWorldOrViewModel(), "muzzle", GetParticleColor( 1 ), GetParticleColor( 2 ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::OnDataChanged( DataUpdateType_t updateType )
{
	BaseClass::OnDataChanged( updateType );

	if ( IsCarrierAlive() && ( WeaponState() == WEAPON_IS_ACTIVE ) )
	{
		if ( m_iChargeEffect != m_iChargeEffectBase )
		{
			CreateChargeEffect();
			m_iChargeEffectBase = m_iChargeEffect;
		}
	}
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ModifyProjectile( CBaseEntity* pProj )
{
#ifdef GAME_DLL
	CTFProjectile_EnergyRing* pEnergyBall = dynamic_cast<CTFProjectile_EnergyRing*>( pProj );
	if ( pEnergyBall == NULL )
	{
		Energy_DrainEnergy();
		return;
	}

	if ( m_bChargedShot )
	{
		pEnergyBall->m_bChargedRing = true;
		pEnergyBall->SetColor( 1, GetParticleColor( 1 ) );
		pEnergyBall->SetColor( 2, GetParticleColor( 2 ) );

		Energy_DrainEnergy( Energy_GetMaxEnergy() );
	}
	else
	{
		pEnergyBall->m_bChargedRing = false;
		pEnergyBall->SetColor( 1, GetParticleColor( 1 ) );
		pEnergyBall->SetColor( 2, GetParticleColor( 2 ) );
		Energy_DrainEnergy();
	}
#else
	Energy_DrainEnergy();
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProgress( void )
{
	return Energy_GetEnergy() / Energy_GetMaxEnergy();
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::DispatchMuzzleFlash( const char* effectName, C_BaseEntity* pAttachEnt )
{
	DispatchParticleEffect( effectName, PATTACH_POINT_FOLLOW, pAttachEnt, "muzzle", GetParticleColor( 1 ), GetParticleColor( 2 ) );
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer && pPlayer->m_Shared.InCond( TF_COND_AIMING ) && !pPlayer->IsRegenerating() )
		return false;

	m_flChargeBeginTime = 0;

	if ( pPlayer )
	{
		pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}

#ifdef CLIENT_DLL
	m_bEffectsThinking = false;
#endif

	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::Deploy( void )
{
	m_flChargeBeginTime = 0;

#ifdef CLIENT_DLL
	m_bEffectsThinking = true;
	SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "EFFECTS_THINK" );
#endif

	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

	if ( m_flChargeBeginTime > 0 )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
		if ( !pPlayer )
			return;

		float flTotalChargeTime = gpGlobals->curtime - m_flChargeBeginTime;
		if ( flTotalChargeTime >= GetChargeForceReleaseTime() )
		{
			FireChargedShot();
		}
	}

#ifdef CLIENT_DLL
	if ( !m_bEffectsThinking )
	{
		m_bEffectsThinking = true;
		SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "EFFECTS_THINK" );
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::WeaponReset( void )
{
	BaseClass::WeaponReset();
	m_flChargeBeginTime = 0.0f;
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ClientEffectsThink( void )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( !pPlayer )
		return;

	if ( !pPlayer->IsLocalPlayer() )
		return;

	if ( !pPlayer->GetViewModel() )
		return;

	if ( !m_bEffectsThinking )
		return;

	SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + 2 + rand() % 5, "EFFECTS_THINK" );

	ParticleProp()->Init( this );
	CNewParticleEffect* pEffect = ParticleProp()->Create( GetIdleParticleEffect(), PATTACH_POINT_FOLLOW, "muzzle" );
	if ( pEffect )
	{
		pEffect->SetControlPoint( CUSTOM_COLOR_CP1, GetParticleColor( 1 ) );
		pEffect->SetControlPoint( CUSTOM_COLOR_CP2, GetParticleColor( 2 ) );
	}
}

#endif

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: Progressively stronger viewmodel shake as the Bison charges
//-----------------------------------------------------------------------------
void CTFRaygun::AddViewmodelBob( CBaseViewModel *viewmodel, Vector &origin, QAngle &angles )
{
	// Keep normal walking bob
	BaseClass::AddViewmodelBob( viewmodel, origin, angles );

	// Only shake while actively charging
	if ( m_flChargeBeginTime <= 0.f )
		return;

	float flChargeTime = gpGlobals->curtime - m_flChargeBeginTime;
	float flProgress   = Clamp( flChargeTime / GetChargeMaxTime(), 0.0f, 1.0f );

	// Amplitude grows from 0 ~2.0 degrees (tune these values)
	const float flMaxAmplitude = 2.0f;
	float flAmplitude = flProgress * flMaxAmplitude;

	// Fast oscillating shake (higher multiplier = faster vibration)
	float flTime = gpGlobals->curtime * 22.0f;

	// Angular shake
	angles[PITCH] += sinf( flTime )          * flAmplitude;
	angles[YAW]   += cosf( flTime * 1.37f )  * flAmplitude * 0.75f;
	angles[ROLL]  += sinf( flTime * 0.83f )  * flAmplitude * 0.55f;

	// Tiny positional jitter so it feels like the gun is vibrating in your hands
	origin.x += sinf( flTime * 1.15f ) * flAmplitude * 0.12f;
	origin.y += cosf( flTime * 0.91f ) * flAmplitude * 0.12f;
	origin.z += sinf( flTime * 1.05f ) * flAmplitude * 0.08f;
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProjectileSpeed( void )
{
	float flSpeed = 1200.f;
	if ( m_bChargedShot )
	{
		flSpeed += 500.f; // 500 faster when charged
	}
	return flSpeed;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProjectileGravity( void )
{
	return 0.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::IsViewModelFlipped( void )
{
	return !BaseClass::IsViewModelFlipped(); 
}

void CTFDRGPomson::Precache()
{
	BaseClass::Precache();

	PrecacheParticleSystem( "drg_pomson_idle" );
	PrecacheParticleSystem( "drg_pomson_impact_drain" );
	PrecacheParticleSystem( "drg_pomson_projectile" );
	PrecacheParticleSystem( "drg_pomson_muzzleflash" );
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFDRGPomson::GetProjectileFireSetup( CTFPlayer *pPlayer, Vector vecOffset, Vector *vecSrc, QAngle *angForward, bool bHitTeammates, float flEndDist )
{
	BaseClass::GetProjectileFireSetup( pPlayer, vecOffset, vecSrc, angForward, bHitTeammates, flEndDist );

	// adjust to line up with the weapon muzzle
	vecSrc->z -= 13.0f;
}
