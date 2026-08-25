//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_passtime.h
//
// PASSTIME bot behavior
//=============================================================================//

#ifndef TF_BOT_PASSTIME_H
#define TF_BOT_PASSTIME_H
#ifdef _WIN32
#pragma once
#endif

//--------------------------------------------------------------------------------------------------------------
inline bool CTFBotHasBeenDamaged( CTFBot *me, float flRecentSeconds = 3.0f )
{
	if ( !me || !me->IsAlive() )
		return false;

	float flTimeSinceInjury = me->GetTimeSinceLastInjury();
	if ( flTimeSinceInjury >= 0.0f && flTimeSinceInjury < flRecentSeconds )
		return true;

	return false;
}
//--------------------------------------------------------------------------------------------------------------

class CTFBotGetPasstimeJack : public Action< CTFBot >
{
public:
	CTFBotGetPasstimeJack( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnContact( CTFBot *me, CBaseEntity *other, CGameTrace *result = NULL );
	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const;
	virtual QueryResultType ShouldAttack(const INextBot* me, const CKnownEntity* them) const;
    virtual QueryResultType ShouldRetreat( const INextBot *me ) const;
	virtual const char *GetName( void ) const	{ return "GetPasstimeJack"; };

private:
	PathFollower			m_path;
	CHandle< CBaseEntity >	m_jackEntity;
	Vector					m_lastJackPos;
	CHandle< CBaseEntity >	m_goalEntity;
	bool					m_haveJack;
	CountdownTimer			m_repathTimer;

	enum ScoreMethod
	{
		SCORE_WALK = 0,
		SCORE_THROW
	};
	ScoreMethod				m_scoreMethod;
	CHandle< CBaseEntity >	m_scoreTarget;
	Vector					m_throwTarget;
	Vector					m_backupPos;
	bool					m_isChargingThrow;
	CountdownTimer			m_chargeTimer;
	bool					m_hasSelectedScoreMethod;

    CHandle< CTFPlayer >	m_passTarget;
	bool					m_isPassing;
	CountdownTimer			m_passLostSightTimer;

	bool					InPasstimeGoal( CTFBot *me ) const;
	CBaseEntity *			SelectBestGoal( CTFBot *me );
	CBaseEntity *			FindBestBallSpawn( CTFBot *me ) const;
	void					UpdateJackTarget( CTFBot *me );
	CBaseEntity *			FindThrowableGoal( const Vector &nearPos, float flMaxDist ) const;
	void					SelectScoreMethod( CTFBot *me );
    CTFPlayer *				GetValidPassTarget( CTFBot *me ) const;
	void					StartChargeThrow( CTFBot *me );
	void					UpdateChargeThrow( CTFBot *me );
	void					CancelThrow( CTFBot *me );
};

class CTFBotFollowPasstimeJackCarrier : public Action< CTFBot >
{
public:
	CTFBotFollowPasstimeJackCarrier( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const	{ return ANSWER_YES; }
	virtual const char *GetName( void ) const { return "FollowPasstimeJackCarrier"; }

private:
	PathFollower			m_path;
	CHandle< CBaseEntity >	m_carrier;
	CountdownTimer			m_repathTimer;
	Vector					m_followPos;
    bool					m_bTryingMeleeSteal;
};

#endif // TF_BOT_PASSTIME_H