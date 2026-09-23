//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_civilian_boost.h
//
// Civilian boost bot behavior
//=============================================================================//

#ifndef TF_BOT_CIVILIAN_BOOST_H
#define TF_BOT_CIVILIAN_BOOST_H
#ifdef _WIN32
#pragma once
#endif

class CTFWeaponBaseMelee;

//--------------------------------------------------------------------------------------------------------------

class CTFBotCivilianBoost : public Action< CTFBot >
{
public:
	CTFBotCivilianBoost( void );

	static bool IsPossible( CTFBot *me );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );

	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const	{ return ANSWER_UNDEFINED; }
	virtual const char *GetName( void ) const { return "BotCivilianBoost"; }

private:
	static CTFWeaponBaseMelee *GetBoostWeapon( CTFBot *me );
	static float GetAuraRadius( CTFBot *me );
	static CTFPlayer *SelectBoostTarget( CTFBot *me );
	static CTFPlayer *SelectAuraAnchor( CTFBot *me );
	static bool IsBoostReady( CTFBot *me );
	static bool IsInCombat( CTFBot *me );

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_boostAttemptTimer;
	CountdownTimer m_lookAtTimer;

	CHandle< CTFPlayer > m_hBoostTarget;
	CHandle< CTFPlayer > m_hAuraAnchor;

	bool m_bTryingToBoost;
	Vector m_vLastKnownTargetPos;
};

#endif // TF_BOT_CIVILIAN_BOOST_H