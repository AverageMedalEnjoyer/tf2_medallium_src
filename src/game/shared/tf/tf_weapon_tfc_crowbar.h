//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======
//
// Purpose: 
//
//=============================================================================
#ifndef TF_WEAPON_CROWBAR_H
#define TF_WEAPON_CROWBAR_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_melee.h"
#include "tf_weaponbase_gun.h"

#ifdef CLIENT_DLL
#define CTFCCrowbar C_TFCCrowbar
#define CTFCUmbrella C_TFCUmbrella
#endif

//=============================================================================
//
// Crowbar class.
//
class CTFCCrowbar : public CTFWeaponBaseMelee
{
public:

	DECLARE_CLASS( CTFCCrowbar, CTFWeaponBaseMelee );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	CTFCCrowbar();
	virtual int			GetWeaponID( void ) const { return TF_WEAPON_TFC_CROWBAR; }

private:

	CTFCCrowbar( const CTFCCrowbar & ) {}
};

//class CTFCUmbrella : public CTFWeaponBaseMelee
//{
//public:
//
//	DECLARE_CLASS( CTFCUmbrella, CTFWeaponBaseMelee );
//	DECLARE_NETWORKCLASS();
//	DECLARE_PREDICTABLE();
//
//	CTFCUmbrella();
//	virtual int			GetWeaponID( void ) const { return TF_WEAPON_TFC_UMBRELLA; }
//
//private:
//
//	CTFCUmbrella( const CTFCUmbrella & ) {}
//};

class CTFCUmbrella : public CTFWeaponBaseMelee
{
public:

	DECLARE_CLASS(CTFCUmbrella, CTFWeaponBaseMelee);
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	CTFCUmbrella();

	//virtual void		Precache(void);
	virtual int			GetWeaponID(void) const					{ return TF_WEAPON_TFC_UMBRELLA; }

	int					GetUmbrellaType(void) const { int iMode = 0; CALL_ATTRIB_HOOK_INT(iMode, set_buff_type); return iMode; };

	int					GetBuffType( int iMode );

	//virtual bool		Deploy(void);
	virtual void		PrimaryAttack(void);
	virtual void		SecondaryAttack(void);
	//virtual void		ItemPostFrame(void);

	virtual bool		HasChargeBar(void)						{ return true; }
	virtual float		InternalGetEffectBarRechargeTime(void);
	virtual const char	*GetEffectLabelText(void) { return "Boost"; }

	virtual float	GetMeleeDamage( CBaseEntity *pTarget, int *piDamageType, int *piCustomDamage );

	//virtual bool       	SendWeaponAnim(int iActivity);

	//virtual bool		CanCreateBuffCiv(CTFPlayer *pPlayer);

private:
	CTFCUmbrella(const CTFCUmbrella &) {}

	// prediction
	CNetworkVar(float, m_flNextFireTime);
	CNetworkVar(bool, m_bFiring);
};

#endif // TF_WEAPON_CROWBAR_H
