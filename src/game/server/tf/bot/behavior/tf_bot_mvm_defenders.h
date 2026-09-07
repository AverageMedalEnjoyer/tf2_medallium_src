//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_mvm_defenders.h

#ifndef TF_BOT_MVM_DEFENDERS_H
#define TF_BOT_MVM_DEFENDERS_H
#ifdef _WIN32
#pragma once
#endif

//---------------------------------------------------------------------
// Helper used by both behaviours
//---------------------------------------------------------------------
bool IsPointInsideInvaderSpawnRoom( const Vector &pos );
bool IsNavAreaInsideInvaderSpawnRoom( CNavArea *area );

//---------------------------------------------------------------------
// Normal approach-zone wander behaviour
//---------------------------------------------------------------------
class CTFBotMVMDefender : public Action< CTFBot >
{
public:
	CTFBotMVMDefender( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnContact( CTFBot *me, CBaseEntity *other, CGameTrace *result = NULL );
	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const	{ return ANSWER_UNDEFINED; }
	virtual const char *GetName( void ) const	{ return "MVMDefender"; }

private:
	bool	IsInvalidSpawnRoomArea( CNavArea *area ) const;
	bool	SelectWanderGoal( CTFBot *me );
	void	ComputePathToGoal( CTFBot *me );

	PathFollower		m_path;
	CountdownTimer		m_repathTimer;
	Vector				m_goalPos;
	bool				m_bHasGoal;
};

class CTFBotMVMDefendBomb : public Action< CTFBot >
{
public:
	CTFBotMVMDefendBomb( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const	{ return ANSWER_UNDEFINED; }
	virtual QueryResultType ShouldAttack( const INextBot *me, const CKnownEntity *them ) const;
	virtual const char *GetName( void ) const	{ return "MVMDefendBomb"; }

private:
	void	UpdateBombTarget( CTFBot *me );
	Vector	SelectStandPosAround( const Vector &center, float minRadius, float maxRadius ) const;
    bool	ShouldFocusTank( CTFBot *me ) const;

	PathFollower			m_path;
	CountdownTimer			m_repathTimer;
	CHandle< CBaseEntity >	m_bomb;
	CHandle< CTFPlayer >	m_carrier;
	Vector					m_bombPos;
	Vector					m_standPos;
	bool					m_bHasTarget;
};

class CTFBotMVMBuyUpgrades : public Action< CTFBot >
{
public:
	CTFBotMVMBuyUpgrades( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const	{ return ANSWER_UNDEFINED; }
	virtual const char *GetName( void ) const	{ return "MVMBuyUpgrades"; }

private:
	PathFollower		m_path;
	CountdownTimer		m_repathTimer;
};

#endif // TF_BOT_MVM_DEFENDERS_H