//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifndef C_OBJ_TELEPORTER_H
#define C_OBJ_TELEPORTER_H
#ifdef _WIN32
#pragma once
#endif

#include "c_baseobject.h"
#include "ObjectControlPanel.h"

class C_ObjectTeleporter : public C_BaseObject
{
	DECLARE_CLASS( C_ObjectTeleporter, C_BaseObject );
public:
	DECLARE_CLIENTCLASS();

	C_ObjectTeleporter();

	virtual void OnPreDataChanged( DataUpdateType_t updateType );
	virtual void OnDataChanged( DataUpdateType_t updateType );

	virtual void GetTargetIDDataString( OUT_Z_BYTECAP(iMaxLenInBytes) wchar_t *sDataString, int iMaxLenInBytes );

	virtual void ClientThink( void );

	virtual void UpdateOnRemove();

	virtual CStudioHdr *OnNewModel( void );

	virtual bool IsPlacementPosValid( void );

	float GetChargeTime( void );

	float GetCurrentRechargeDuration( void ) { return m_flCurrentRechargeDuration; }

	int GetState( void ) { return m_iState; }

	int GetTimesUsed( void );

	void StartChargedEffects( void );
	void StopChargedEffects( void );

	void StartActiveEffects( void );
	void StopActiveEffects( void );

	void StartBuildingEffects( void );
	void StopBuildingEffects( void );

	virtual void SetInvisibilityLevel( float flValue );
	void UpdateTeleporterEffects( void );

	virtual void UpdateDamageEffects( BuildingDamageLevel_t damageLevel );

	virtual int		GetUpgradeLevel( void ) { return m_iUpgradeLevel; }
	int				GetUpgradeMetal( void ) { return m_iUpgradeMetal; }
	//virtual int		GetUpgradeMetalRequired( void ) { return GetObjectInfo( GetType() )->m_UpgradeCost; }
	virtual void	UpgradeLevelChanged( void );

	virtual void	OnGoInactive( void ) OVERRIDE;

	// Engineer robot squads
	bool			HasRobotSquad( void ) const { return m_nRobotSquadSize > 0 || m_flRobotSquadNextArrival >= 0.0f; }
	int				GetRobotSquadAlive( void ) const { return m_nRobotSquadAlive; }
	int				GetRobotSquadSize( void ) const { return m_nRobotSquadSize; }
	float			GetRobotSquadNextArrival( void ) const { return m_flRobotSquadNextArrival; }
	int				GetRobotSquadOrder( void ) const { return m_nRobotSquadOrder; }

private:
	int m_iState;
	int m_iOldState;
	float m_flRechargeTime;
	float m_flCurrentRechargeDuration;
	int m_iTimesUsed;
	float m_flYawToExit;
	bool m_bMatchBuilding;
	bool m_bOldMatchBuilding;

	int m_nRobotSquadAlive;
	int m_nRobotSquadSize;
	float m_flRobotSquadNextArrival;
	int m_nRobotSquadOrder;

	int m_iDirectionArrowPoseParam;

	HPARTICLEFFECT	m_hChargedEffect;
	HPARTICLEFFECT	m_hDirectionEffect;

	HPARTICLEFFECT	m_hChargedLeftArmEffect;
	HPARTICLEFFECT	m_hChargedRightArmEffect;

	HPARTICLEFFECT	m_hBuildingLeftArmEffect;
	HPARTICLEFFECT	m_hBuildingRightArmEffect;

	CSoundPatch		*m_pSpinSound;

private:
	C_ObjectTeleporter( const C_ObjectTeleporter & ); // not defined, not accessible
};

#endif	//C_OBJ_TELEPORTER_H
