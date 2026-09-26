//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#include "cbase.h"
#include "sceneentity_shared.h"
#include "choreoscene.h"
#include "filesystem.h"
#include "utlbuffer.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar scene_print( "scene_print", "0", FCVAR_REPLICATED, "When playing back a scene, print timing and event info to console." );
ConVar scene_clientflex( "scene_clientflex", "1", FCVAR_REPLICATED, "Do client side flex animation." );

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pFormat - 
//			... - 
// Output : static void
//-----------------------------------------------------------------------------
void Scene_Printf( const char *pFormat, ... )
{
	int val = scene_print.GetInt();
	if ( !val )
		return;

	if ( val >= 2 )
	{
		if ( CBaseEntity::IsServer() && val != 2 )
		{
			return;
		}
        else if ( !CBaseEntity::IsServer() && val != 3 )
		{
			return;
		}
	}

	va_list marker;
	char msg[8192];

	va_start(marker, pFormat);
	Q_vsnprintf(msg, sizeof(msg), pFormat, marker);
	va_end(marker);	
	
	Msg( "%8.3f[%d] %s:  %s", gpGlobals->curtime, gpGlobals->tickcount, CBaseEntity::IsServer() ? "sv" : "cl", msg );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : const char
//-----------------------------------------------------------------------------
const char *CSceneTokenProcessor::CurrentToken( void )
{
	return m_szToken;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : crossline - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSceneTokenProcessor::GetToken( bool crossline )
{
	// NOTE: crossline is ignored here, may need to implement if needed
	m_pBuffer = engine->ParseFile( m_pBuffer, m_szToken, sizeof( m_szToken ) );
	if ( m_szToken[0] )
		return true;
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSceneTokenProcessor::TokenAvailable( void )
{
	char const *search_p = m_pBuffer;

	while ( *search_p <= 32)
	{
		if (*search_p == '\n')
			return false;
		search_p++;
		if ( !*search_p )
			return false;

	}

	if (*search_p == ';' || *search_p == '#' ||		 // semicolon and # is comment field
		(*search_p == '/' && *((search_p)+1) == '/')) // also make // a comment field
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *fmt - 
//			... - 
//-----------------------------------------------------------------------------
void CSceneTokenProcessor::Error( const char *fmt, ... )
{
	char string[ 2048 ];
	va_list argptr;
	va_start( argptr, fmt );
	Q_vsnprintf( string, sizeof(string), fmt, argptr );
	va_end( argptr );

	Warning( "%s", string );
	Assert(0);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *buffer - 
//-----------------------------------------------------------------------------
void CSceneTokenProcessor::SetBuffer( char *buffer )
{
	m_pBuffer = buffer;
}

CSceneTokenProcessor g_TokenProcessor;

//-----------------------------------------------------------------------------
// Raw VCD support, following Mapbase's compiled-scene-first loading policy.
// Keep the buffer local so playback and metadata queries release it alike.
//-----------------------------------------------------------------------------
CChoreoScene *LoadLooseScene( const char *filename, IChoreoEventCallback *pCallback )
{
	char loadfile[MAX_PATH];
	Q_strncpy( loadfile, filename, sizeof( loadfile ) );
	Q_SetExtension( loadfile, ".vcd", sizeof( loadfile ) );
	Q_FixSlashes( loadfile );

	CUtlBuffer buffer( 0, 0, CUtlBuffer::TEXT_BUFFER );
	if ( !filesystem->ReadFile( loadfile, "MOD", buffer ) || buffer.TellPut() == 0 )
		return NULL;

	// ReadFile() only guarantees the bytes read from disk - no trailing null.
	// engine->ParseFile() (via CSceneTokenProcessor::GetToken) walks the buffer
	// as a plain C-string and relies on a terminator to know where to stop, so
	// without this it reads past the end into whatever memory follows once it
	// hits the last real token - surfacing as garbled "expecting X got Y"
	// parse errors on essentially-random files.
	buffer.PutChar( '\0' );

	if ( IsBufferBinaryVCD( (char *)buffer.Base(), buffer.TellPut() ) )
	{
		Warning( "LoadLooseScene: Expected a text VCD in '%s'\n", loadfile );
		return NULL;
	}

	g_TokenProcessor.SetBuffer( (char *)buffer.Base() );
	CChoreoScene *pScene = ChoreoLoadScene( loadfile, pCallback, &g_TokenProcessor, Scene_Printf );
	g_TokenProcessor.SetBuffer( NULL );
	return pScene;
}
