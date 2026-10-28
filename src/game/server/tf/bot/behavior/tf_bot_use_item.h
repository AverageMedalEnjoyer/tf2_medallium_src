//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_use_item.h
// Equip and consume an item
// Michael Booth, July 2011

#ifndef TF_BOT_USE_ITEM_H
#define TF_BOT_USE_ITEM_H

class CTFBotUseItem : public Action< CTFBot >
{
public:
	CTFBotUseItem( CTFWeaponBase *item, CBaseEntity *target = NULL );	// target: who to throw it at
	virtual ~CTFBotUseItem() { }

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );

	virtual const char *GetName( void ) const	{ return "UseItem"; };

private:
	CHandle< CTFWeaponBase > m_item;
	EHANDLE m_target;
	bool m_bHasTarget;
	CountdownTimer m_cooldownTimer;
	CountdownTimer m_aimTimer;			// throw anyway if we can't line it up in time
};


#endif // TF_BOT_USE_ITEM_H
