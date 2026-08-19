//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_mvm_defenders.h

#ifndef TF_BOT_MVM_DEFENDERS_H
#define TF_BOT_MVM_DEFENDERS_H
#ifdef _WIN32
#pragma once
#endif

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

	virtual QueryResultType ShouldHurry( const INextBot *me ) const;					// are we in a hurry?
	virtual const char *GetName( void ) const	{ return "MVMDefender"; };

private:
	bool SelectRandomDefenderSpawnGoal( CTFBot *me );
	void ComputePathToGoal( CTFBot *me );

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	Vector m_goalPos;
	bool m_bHasGoal;
};

#endif // TF_BOT_MVM_DEFENDERS_H