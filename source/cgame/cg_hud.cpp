/*
Copyright (C) 2006 Pekka Lampila ("Medar"), Damien Deville ("Pb")
and German Garcia Fernandez ("Jal") for Chasseur de bots association.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "qcommon/base.h"
#include "qcommon/string.h"
#include "client/assets.h"
#include "client/renderer/renderer.h"
#include "client/renderer/text.h"
#include "cgame/cg_local.h"

#include "luau/lua.h"
#include "luau/lualib.h"
#include "luau/luacode.h"

static const Vec4 light_gray = sRGBToLinear( RGBA8( 96, 96, 96, 255 ) );
static constexpr Vec4 dark_gray = vec4_dark;

static lua_State * hud_L;

struct constant_numeric_t {
	const char *name;
	int value;
};

static const constant_numeric_t cg_numeric_constants[] = {
	{ "NOT_CHASING", 9999 },

	{ "TEAM_SPECTATOR", TEAM_SPECTATOR },
	{ "TEAM_PLAYERS", TEAM_PLAYERS },
	{ "TEAM_ALPHA", TEAM_ALPHA },
	{ "TEAM_BETA", TEAM_BETA },
	{ "TEAM_ALLY", TEAM_ALLY },
	{ "TEAM_ENEMY", TEAM_ENEMY },

	{ "Gadget_None", Gadget_None },

	{ "Gadget_ThrowingAxe", Gadget_ThrowingAxe },
	{ "Gadget_SuicideBomb", Gadget_SuicideBomb },
	{ "Gadget_StunGrenade", Gadget_StunGrenade },


	{ "Perk_Ninja", Perk_Ninja },
	{ "Perk_Hooligan", Perk_Hooligan },
	{ "Perk_Midget", Perk_Midget },
	{ "Perk_Jetpack", Perk_Jetpack },
	{ "Perk_Boomer", Perk_Boomer },

	{ "Stamina_Normal", Stamina_Normal },
	{ "Stamina_Reloading", Stamina_Reloading },
	{ "Stamina_UsingAbility", Stamina_UsingAbility },
	{ "Stamina_UsedAbility", Stamina_UsedAbility },

	{ "MatchState_Warmup", MatchState_Warmup },
	{ "MatchState_Countdown", MatchState_Countdown },
	{ "MatchState_Playing", MatchState_Playing },
	{ "MatchState_PostMatch", MatchState_PostMatch },
	{ "MatchState_WaitExit", MatchState_WaitExit },

	{ "BombProgress_Nothing", BombProgress_Nothing },
	{ "BombProgress_Planting", BombProgress_Planting },
	{ "BombProgress_Defusing", BombProgress_Defusing },

	{ "RoundType_Normal", RoundType_Normal },
	{ "RoundType_MatchPoint", RoundType_MatchPoint },
	{ "RoundType_Overtime", RoundType_Overtime },
	{ "RoundType_OvertimeMatchPoint", RoundType_OvertimeMatchPoint },
};

//=============================================================================

static int CG_GetPOVnum() {
	return cg.predictedPlayerState.POVnum != cgs.playerNum + 1 ? cg.predictedPlayerState.POVnum : 9999;
}

static int CG_GetSpeed() {
	return Length( cg.predictedPlayerState.pmove.velocity.xy() );
}

static int CG_GetFPS() {
	static int frameTimes[ 32 ];

	if( cg_showFPS->modified ) {
		memset( frameTimes, 0, sizeof( frameTimes ) );
		cg_showFPS->modified = false;
	}

	frameTimes[cg.frameCount % ARRAY_COUNT( frameTimes )] = cls.realFrameTime;

	float average = 0.0f;
	for( size_t i = 0; i < ARRAY_COUNT( frameTimes ); i++ ) {
		average += frameTimes[( cg.frameCount - i ) % ARRAY_COUNT( frameTimes )];
	}
	average /= ARRAY_COUNT( frameTimes );
	return int( 1000.0f / average + 0.5f );
}

static bool CG_IsLagging() {
	int64_t incomingAcknowledged, outgoingSequence;
	CL_GetCurrentState( &incomingAcknowledged, &outgoingSequence, NULL );
	return !cgs.demoPlaying && (outgoingSequence - incomingAcknowledged) >= (CMD_BACKUP - 1);
}

//=============================================================================

#define MAX_OBITUARIES 32

enum obituary_type_t {
	OBITUARY_NONE,
	OBITUARY_NORMAL,
	OBITUARY_SUICIDE,
	OBITUARY_ACCIDENT,
};

struct obituary_t {
	obituary_type_t type;
	int64_t time;
	char victim[MAX_INFO_VALUE];
	int victim_team;
	char attacker[MAX_INFO_VALUE];
	int attacker_team;
	DamageType damage_type;
	bool wallbang;
};

static obituary_t cg_obituaries[MAX_OBITUARIES];
static int cg_obituaries_current = -1;

struct {
	s64 time;
	u64 entropy;
	obituary_type_t type;
	DamageType damage_type;
} self_obituary;

void CG_SC_ResetObituaries() {
	memset( cg_obituaries, 0, sizeof( cg_obituaries ) );
	cg_obituaries_current = -1;
	self_obituary = { };
}

static const char * normal_obituaries[] = {
#include "obituaries.h"
};

static const char * prefixes[] = {
#include "prefixes.h"
};

static const char * suicide_prefixes[] = {
	"AUTO",
	"SELF",
	"SHANKS",
	"SOLO",
	"TIMMA",
};

static const char * void_obituaries[] = {
	"ATE",
	"HOLED",
	"RECLAIMED",
	"TOOK",
};

static const char * spike_obituaries[] = {
	"DISEMBOWELED",
	"GORED",
	"IMPALED",
	"PERFORATED",
	"POKED",
	"SKEWERED",
	"SLASHED",
};

static const char * conjunctions[] = {
	"+",
	"&",
	"&&",
	"ALONG WITH",
	"AND",
	"AS WELL AS",
	"ASSISTED BY",
	"FEAT.",
	"N'",
	"PLUS",
	"UND",
	"W/",
	"WITH",
	"X",
};

static const char * RandomPrefix( RNG * rng, float p ) {
	if( !Probability( rng, p ) )
		return "";
	return RandomElement( rng, prefixes );
}

static char * Uppercase( Allocator * a, const char * str ) {
	char * upper = CopyString( a, str );
	Q_strupr( upper );
	return upper;
}

static char * MakeObituary( Allocator * a, RNG * rng, int type, DamageType damage_type ) {
	Span< const char * > obituaries = StaticSpan( normal_obituaries );
	if( damage_type == WorldDamage_Void ) {
		obituaries = StaticSpan( void_obituaries );
	}
	else if( damage_type == WorldDamage_Spike ) {
		obituaries = StaticSpan( spike_obituaries );
	}

	const char * prefix1 = "";
	if( type == OBITUARY_SUICIDE ) {
		prefix1 = RandomElement( rng, suicide_prefixes );
	}

	// do these in order because arg evaluation order is undefined
	const char * prefix2 = RandomPrefix( rng, 0.05f );
	const char * prefix3 = RandomPrefix( rng, 0.5f );

	return ( *a )( "{}{}{}{}", prefix1, prefix2, prefix3, obituaries[ RandomUniform( rng, 0, obituaries.n ) ] );
}

void CG_SC_Obituary() {
	int victimNum = atoi( Cmd_Argv( 1 ) );
	int attackerNum = atoi( Cmd_Argv( 2 ) );
	int topAssistorNum = atoi( Cmd_Argv( 3 ) );
	DamageType damage_type;
	damage_type.encoded = atoi( Cmd_Argv( 4 ) );
	bool wallbang = atoi( Cmd_Argv( 5 ) ) == 1;
	u64 entropy = StringToU64( Cmd_Argv( 6 ), 0 );

	const char * victim = PlayerName( victimNum - 1 );
	const char * attacker = attackerNum == 0 ? NULL : PlayerName( attackerNum - 1 );
	const char * assistor = topAssistorNum == -1 ? NULL : PlayerName( topAssistorNum - 1 );

	cg_obituaries_current = ( cg_obituaries_current + 1 ) % MAX_OBITUARIES;
	obituary_t * current = &cg_obituaries[ cg_obituaries_current ];
	current->type = OBITUARY_NORMAL;
	current->time = cls.monotonicTime;
	current->damage_type = damage_type;
	current->wallbang = wallbang;

	if( victim != NULL ) {
		Q_strncpyz( current->victim, victim, sizeof( current->victim ) );
		current->victim_team = cg_entities[ victimNum ].current.team;
	}
	if( attacker != NULL ) {
		Q_strncpyz( current->attacker, attacker, sizeof( current->attacker ) );
		current->attacker_team = cg_entities[ attackerNum ].current.team;
	}

	int assistor_team = assistor == NULL ? -1 : cg_entities[ topAssistorNum ].current.team;

	if( cg.view.playerPrediction && ISVIEWERENTITY( victimNum ) ) {
		self_obituary.entropy = 0;
	}

	TempAllocator temp = cls.frame_arena.temp();
	RNG rng = NewRNG( entropy, 0 );

	const char * attacker_name = attacker == NULL ? NULL : temp( "{}{}", ImGuiColorToken( CG_TeamColor( current->attacker_team ) ), Uppercase( &temp, attacker ) );
	const char * victim_name = temp( "{}{}", ImGuiColorToken( CG_TeamColor( current->victim_team ) ), Uppercase( &temp, victim ) );
	const char * assistor_name = assistor == NULL ? NULL : temp( "{}{}", ImGuiColorToken( CG_TeamColor( assistor_team ) ), Uppercase( &temp, assistor ) );

	if( attackerNum == 0 ) {
		current->type = OBITUARY_ACCIDENT;

		if( damage_type == WorldDamage_Void ) {
			attacker_name = temp( "{}{}", ImGuiColorToken( rgba8_black ), "THE VOID" );
		}
		else if( damage_type == WorldDamage_Spike ) {
			attacker_name = temp( "{}{}", ImGuiColorToken( rgba8_black ), "A SPIKE" );
		}
		else {
			return;
		}
	}

	const char * obituary = MakeObituary( &temp, &rng, current->type, damage_type );

	if( cg.view.playerPrediction && ISVIEWERENTITY( victimNum ) ) {
		self_obituary.time = cls.monotonicTime;
		self_obituary.entropy = entropy;
		self_obituary.type = current->type;
		self_obituary.damage_type = damage_type;
	}

	if( assistor == NULL ) {
		CG_AddChat( temp( "{} {}{} {}",
			attacker_name,
			ImGuiColorToken( rgba8_diesel_yellow ), obituary,
			victim_name
		) );
	}
	else {
		const char * conjugation = RandomElement( &rng, conjunctions );
		CG_AddChat( temp( "{} {}{} {} {}{} {}",
			attacker_name,
			ImGuiColorToken( 255, 255, 255, 255 ), conjugation,
			assistor_name,
			ImGuiColorToken( rgba8_diesel_yellow ), obituary,
			victim_name
		) );
	}

	if( ISVIEWERENTITY( attackerNum ) && attacker != victim ) {
		CG_CenterPrint( temp( "{} {}", obituary, Uppercase( &temp, victim ) ) );
	}
}

static const Material * DamageTypeToIcon( DamageType type ) {
	WeaponType weapon;
	GadgetType gadget;
	WorldDamage world;
	DamageCategory category = DecodeDamageType( type, &weapon, &gadget, &world );

	if( category == DamageCategory_Weapon ) {
		return cgs.media.shaderWeaponIcon[ weapon ];
	}

	if( category == DamageCategory_Gadget ) {
		return cgs.media.shaderGadgetIcon[ gadget ];
	}

	switch( world ) {
		case WorldDamage_Slime:
			return FindMaterial( "gfx/slime" );
		case WorldDamage_Lava:
			return FindMaterial( "gfx/lava" );
		case WorldDamage_Crush:
			return FindMaterial( "gfx/crush" );
		case WorldDamage_Telefrag:
			return FindMaterial( "gfx/telefrag" );
		case WorldDamage_Suicide:
			return FindMaterial( "gfx/suicide" );
		case WorldDamage_Explosion:
			return FindMaterial( "gfx/explosion" );
		case WorldDamage_Trigger:
			return FindMaterial( "gfx/trigger" );
		case WorldDamage_Laser:
			return FindMaterial( "gfx/laser" );
		case WorldDamage_Spike:
			return FindMaterial( "gfx/spike" );
		case WorldDamage_Void:
			return FindMaterial( "gfx/void" );
	}

	return FindMaterial( "" );
}

//=============================================================================

static void GlitchText( Span< char > msg ) {
	constexpr const char glitches[] = { '#', '@', '~', '$' };

	RNG rng = NewRNG( cls.monotonicTime / 67, 0 );

	for( char & c : msg ) {
		if( Probability( &rng, 0.03f ) ) {
			c = RandomElement( &rng, glitches );
		}
	}
}

void CG_DrawScope() {
	const WeaponDef * def = GS_GetWeaponDef( cg.predictedPlayerState.weapon );
	if( def->zoom_fov != 0 && cg.predictedPlayerState.zoom_time > 0 ) {
		float frac = cg.predictedPlayerState.zoom_time / float( ZOOMTIME );

		PipelineState pipeline;
		pipeline.pass = frame_static.ui_pass;
		pipeline.shader = &shaders.scope;
		pipeline.depth_func = DepthFunc_Disabled;
		pipeline.blend_func = BlendFunc_Blend;
		pipeline.write_depth = false;
		pipeline.set_uniform( "u_View", frame_static.view_uniforms );
		DrawFullscreenMesh( pipeline );

		if( cg.predictedPlayerState.weapon == Weapon_Sniper ) {
			trace_t trace;
			Vec3 forward = -frame_static.V.row2().xyz();
			Vec3 end = cg.view.origin + forward * 10000.0f;
			CG_Trace( &trace, cg.view.origin, Vec3( 0.0f ), Vec3( 0.0f ), end, cg.predictedPlayerState.POVnum, MASK_SHOT );

			TempAllocator temp = cls.frame_arena.temp();
			float offset = Min2( frame_static.viewport_width, frame_static.viewport_height ) * 0.1f;

			{
				float distance = Length( trace.endpos - cg.view.origin ) + sinf( float( cls.monotonicTime ) / 128.0f ) * 0.5f + sinf( float( cls.monotonicTime ) / 257.0f ) * 0.25f;

				char * msg = temp( "{.2}m", distance / 32.0f );
				GlitchText( Span< char >( msg + strlen( msg ) - 3, 2 ) );

				DrawText( cgs.fontItalic, cgs.textSizeSmall, msg, Alignment_RightTop, frame_static.viewport_width / 2 - offset, frame_static.viewport_height / 2 + offset, vec4_red );
			}

			if( trace.ent > 0 && trace.ent <= MAX_CLIENTS ) {
				Vec4 color = AttentionGettingColor();
				color.w = frac;

				RNG obituary_rng = NewRNG( cls.monotonicTime / 1000, 0 );
				char * msg = temp( "{}?", RandomElement( &obituary_rng, normal_obituaries ) );
				GlitchText( Span< char >( msg, strlen( msg ) - 1 ) );

				DrawText( cgs.fontItalic, cgs.textSizeSmall, msg, Alignment_LeftTop, frame_static.viewport_width / 2 + offset, frame_static.viewport_height / 2 + offset, color, vec4_black );
			}
		}
	}
}

//=============================================================================
// Commands' Functions
//=============================================================================

static float AmmoFrac( const SyncPlayerState * ps, WeaponType weap, int ammo ) {
	const WeaponDef * def = GS_GetWeaponDef( weap );
	if( def->clip_size == 0 )
		return 1.0f;

	if( weap != ps->weapon || ( ps->weapon_state != WeaponState_Reloading && ps->weapon_state != WeaponState_StagedReloading ) ) {
		return float( ammo ) / float( def->clip_size );
	}

	if( def->staged_reload_time != 0 ) {
		u16 t = ps->weapon_state == WeaponState_StagedReloading ? def->staged_reload_time : def->reload_time;
		return ( float( ammo ) + Unlerp01( u16( 0 ), ps->weapon_state_time, t ) ) / float( def->clip_size );
	}

	return Min2( 1.0f, float( ps->weapon_state_time ) / float( def->reload_time ) );
}

static Vec4 AmmoColor( float ammo_frac ) {
	Vec4 color_ammo_max = sRGBToLinear( rgba8_diesel_yellow );
	Vec4 color_ammo_min = sRGBToLinear( RGBA8( 255, 56, 97, 255 ) );

	return Lerp( color_ammo_min, Unlerp( 0.0f, ammo_frac, 1.0f ), color_ammo_max );
}

static void Draw2DBoxPadded( float x, float y, float w, float h, float border, const Material * material, Vec4 color ) {
	Draw2DBox( x - border, y - border, w + border * 2.0f, h + border * 2.0f, material, color );
}

static void DrawWeaponBar( int ix, int iy, int size, int padding, Alignment alignment ) {
	TempAllocator temp = cls.frame_arena.temp();

	const SyncPlayerState * ps = &cg.predictedPlayerState;
	const SyncEntityState * es = &cg_entities[ ps->POVnum ].current;

	int num_weapons = 0;
	for( size_t i = 0; i < ARRAY_COUNT( ps->weapons ); i++ ) {
		if( ps->weapons[ i ].weapon != Weapon_None ) {
			num_weapons++;
		}
	}

	bool has_bomb = ( es->effects & EF_CARRIER ) != 0;

	int columns = num_weapons + ( has_bomb ? 1 : 0 );

	int total_width = columns * size + Max2( 0, columns - 1 ) * padding;

	float border = size * 0.06f;

	float x = CG_HorizontalAlignForWidth( ix, alignment, total_width );
	float y = CG_VerticalAlignForHeight( iy, alignment, size );

	for( int i = 0; i < num_weapons; i++ ) {
		WeaponType weap = ps->weapons[ i ].weapon;
		const WeaponDef * def = GS_GetWeaponDef( weap );
		const Material * icon = cgs.media.shaderWeaponIcon[ weap ];
		Vec2 half_pixel = HalfPixelSize( icon );

		int ammo = ps->weapons[ i ].ammo;

		float ammo_frac = AmmoFrac( ps, weap, ammo );
		Vec4 ammo_color = AmmoColor( ammo_frac );

		bool selected_weapon = ps->pending_weapon != Weapon_None ? weap == ps->pending_weapon : weap == ps->weapon;
		Vec4 selected_weapon_color = selected_weapon ? vec4_white : Vec4( 0.5f, 0.5f, 0.5f, 1.0f );

		// border
		Draw2DBoxPadded( x, y, size, size + size * ( selected_weapon ? 0.35f : 0.27f ), border, cls.white_material, dark_gray );

		// background icon
		Draw2DBox( x, y, size, size, icon, light_gray );

		{
			float y_offset = size * ( 1.0f - ammo_frac );
			float partial_y = y + y_offset;
			float partial_h = size - y_offset;

			// partial ammo color background
			Draw2DBox( x, partial_y, size, partial_h, cls.white_material, ammo_color );

			// partial ammo icon
			Draw2DBoxUV( x, partial_y, size, partial_h,
				Vec2( half_pixel.x, Lerp( half_pixel.y, 1.0f - ammo_frac, 1.0f - half_pixel.y ) ), 1.0f - half_pixel,
				icon, vec4_dark );
		}

		// current ammo
		if( def->clip_size != 0 ) {
			DrawText( cgs.fontBoldItalic, cgs.fontSystemExtraSmallSize, temp( "{}", ammo ), Alignment_LeftTop, x + size * 0.05f, y + size * 0.05f, ammo_color, true );
		}

		// weapon name
		DrawText( cgs.fontBoldItalic, cgs.fontSystemExtraSmallSize, def->name, Alignment_CenterTop, x + size * 0.5f, y + size * (selected_weapon ? 1.15f : 1.1f ), selected_weapon_color, true );

		if( Cvar_Bool( "cg_showHotkeys" ) ) {
			// first try the weapon specific bind
			char bind[ 32 ];
			if( !CG_GetBoundKeysString( temp( "use {}", def->short_name ), bind, sizeof( bind ) ) ) {
				CG_GetBoundKeysString( temp( "weapon {}", i + 1 ), bind, sizeof( bind ) );
			}

			// weapon bind
			DrawText( cgs.fontNormalBold, cgs.fontSystemSmallSize, bind, Alignment_CenterMiddle, x + size * 0.5f, y - size * 0.2f, Vec4( 0.5f, 0.5f, 0.5f, 1.0f ), true );
		}

		x += size + padding;
	}

	if( has_bomb ) {
		Vec4 bg_color = ps->can_plant ? PlantableColor() : dark_gray;
		Vec4 border_color = dark_gray;
		Vec4 bomb_color = ps->can_plant ? dark_gray : vec4_white;
		Vec4 text_color = ps->can_plant ? PlantableColor() : vec4_white;

		Draw2DBoxPadded( x, y, size, size, border, cls.white_material, border_color );
		Draw2DBox( x, y, size, size, cls.white_material, bg_color );
		Draw2DBox( x, y, size, size, FindMaterial( "gfx/bomb" ), bomb_color );
		DrawText( cgs.fontBoldItalic, cgs.fontSystemExtraSmallSize, "BOMB", Alignment_CenterTop, x + size * 0.5f, y + size * 1.15f, text_color, true );

		x += size + padding;
	}
}

static void DrawPerksUtility( int ix, int iy, int size, int padding, Alignment alignment ) {
	TempAllocator temp = cls.frame_arena.temp();

	const SyncPlayerState * ps = &cg.predictedPlayerState;

	bool has_gadget = ps->gadget != Gadget_None;

	int columns = ( has_gadget ? 1 : 0 ) + 1;

	int total_width = columns * size + Max2( 0, columns - 1 ) * padding;

	float border = size * 0.08f;

	float x = CG_HorizontalAlignForWidth( ix, alignment, total_width );
	float y = CG_VerticalAlignForHeight( iy, alignment, size );

	{
		const Material * icon = cgs.media.shaderPerkIcon[ ps->perk ];

		Draw2DBoxPadded( x, y, size, size, border, cls.white_material, dark_gray );
		Draw2DBox( x, y, size, size, cls.white_material, light_gray );
		Draw2DBox( x, y, size, size, icon, vec4_white );
		x += size + padding;
	}

	if( has_gadget ) {
		const GadgetDef * def = GetGadgetDef( ps->gadget );

		float ammo_frac = float( ps->gadget_ammo ) / float( def->uses );
		Vec4 ammo_color = AmmoColor( ammo_frac );

		const Material * icon = cgs.media.shaderGadgetIcon[ ps->gadget ];
		Vec2 half_pixel = HalfPixelSize( icon );

		// border
		Draw2DBoxPadded( x, y, size, size, border, cls.white_material, dark_gray );

		// background icon
		Draw2DBox( x, y, size, size, icon, light_gray );

		{
			float y_offset = size * ( 1.0f - ammo_frac );
			float partial_y = y + y_offset;
			float partial_h = size - y_offset;

			// partial ammo color background
			Draw2DBox( x, partial_y, size, partial_h, cls.white_material, ammo_color );

			// partial ammo icon
			Draw2DBoxUV( x, partial_y, size, partial_h,
				Vec2( half_pixel.x, Lerp( half_pixel.y, 1.0f - ammo_frac, 1.0f - half_pixel.y ) ), 1.0f - half_pixel,
				icon, vec4_dark );
		}

		// current ammo
		DrawText( cgs.fontBoldItalic, cgs.fontSystemExtraSmallSize, temp( "{}", ps->gadget_ammo ), Alignment_LeftTop, x + size * 0.05f, y + size * 0.05f, ammo_color, true );

		if( Cvar_Bool( "cg_showHotkeys" ) ) {
			// bind
			char bind[ 32 ];
			CG_GetBoundKeysString( "+gadget", bind, sizeof( bind ) );
			DrawText( cgs.fontNormalBold, cgs.fontSystemTinySize, bind, Alignment_CenterMiddle, x + size * 0.5f, y - size * 0.25f, Vec4( 0.5f, 0.5f, 0.5f, 1.0f ), true );
		}

		x += size + padding;
	}
}

//=============================================================================

static bool CallWithStackTrace( lua_State * L, int args, int results ) {
	if( lua_pcall( L, args, results, 1 ) != 0 ) {
		Com_Printf( S_COLOR_YELLOW "%s\n", lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
		return false;
	}

	return true;
}

static Span< const char > LuaToSpan( lua_State * L, int idx ) {
	size_t len;
	const char * str = lua_tolstring( L, idx, &len );
	return Span< const char >( str, len );
}

static u8 CheckRGBA8Component( lua_State * L, int idx, int narg ) {
	float val = lua_tonumber( L, idx );
	luaL_argcheck( L, float( u8( val ) ) == val, narg, "RGBA8 colors must have u8 components" );
	return u8( val );
}

static bool IsHex( char c ) {
	return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

static bool ParseHexDigit( u8 * digit, char c ) {
	if( !IsHex( c ) )
		return false;
	if( c >= '0' && c <= '9' ) *digit = c - '0';
	if( c >= 'a' && c <= 'f' ) *digit = 10 + c - 'a';
	if( c >= 'A' && c <= 'F' ) *digit = 10 + c - 'A';
	return true;
}

static bool ParseHexByte( u8 * byte, char a, char b ) {
	u8 x, y;
	if( !ParseHexDigit( &x, a ) || !ParseHexDigit( &y, b ) )
		return false;
	*byte = x * 16 + y;
	return true;
}

static bool ParseHexColor( RGBA8 * rgba, Span< const char > str ) {
	if( str.n == 0 || str[ 1 ] == '#' )
		return false;

	str++;

	char digits[ 8 ];
	digits[ 6 ] = 'f';
	digits[ 7 ] = 'f';

	if( str.n == 3 || str.n == 4 ) {
		// #rgb #rgba
		for( size_t i = 0; i < str.n; i++ ) {
			digits[ i * 2 + 0 ] = str[ i ];
			digits[ i * 2 + 1 ] = str[ i ];
		}
	}
	else if( str.n == 6 || str.n == 8 ) {
		// #rrggbb #rrggbbaa
		for( size_t i = 0; i < str.n; i++ ) {
			digits[ i ] = str[ i ];
		}
	}
	else {
		return false;
	}

	bool ok = true;
	ok = ok && ParseHexByte( &rgba->r, digits[ 0 ], digits[ 1 ] );
	ok = ok && ParseHexByte( &rgba->g, digits[ 2 ], digits[ 3 ] );
	ok = ok && ParseHexByte( &rgba->b, digits[ 4 ], digits[ 5 ] );
	ok = ok && ParseHexByte( &rgba->a, digits[ 6 ], digits[ 7 ] );
	return ok;
}

static Vec4 CheckColor( lua_State * L, int narg ) {
	int type = lua_type( L, narg );
	luaL_argcheck( L, type == LUA_TSTRING || type == LUA_TTABLE, narg, "colors must be strings or tables" );

	if( lua_type( L, narg ) == LUA_TSTRING ) {
		RGBA8 rgba;
		if( !ParseHexColor( &rgba, LuaToSpan( L, narg ) ) ) {
			luaL_error( L, "color doesn't parse as a hex string: %s", lua_tostring( L, narg ) );
		}
		return sRGBToLinear( rgba );
	}

	lua_getfield( L, narg, "srgb" );
	luaL_argcheck( L, lua_isboolean( L, -1 ), narg, "color.srgb must be a boolean" );
	bool srgb = lua_toboolean( L, -1 );
	lua_pop( L, 1 );

	if( srgb ) {
		RGBA8 rgba;

		lua_getfield( L, narg, "r" );
		rgba.r = CheckRGBA8Component( L, -1, narg );
		lua_pop( L, 1 );

		lua_getfield( L, narg, "g" );
		rgba.g = CheckRGBA8Component( L, -1, narg );
		lua_pop( L, 1 );

		lua_getfield( L, narg, "b" );
		rgba.b = CheckRGBA8Component( L, -1, narg );
		lua_pop( L, 1 );

		lua_getfield( L, narg, "a" );
		rgba.a = lua_isnil( L, -1 ) ? 255 : CheckRGBA8Component( L, -1, narg );
		lua_pop( L, 1 );

		return sRGBToLinear( rgba );
	}

	Vec4 linear;

	lua_getfield( L, narg, "r" );
	linear.x = lua_tonumber( L, -1 );
	lua_pop( L, 1 );

	lua_getfield( L, narg, "g" );
	linear.y = lua_tonumber( L, -1 );
	lua_pop( L, 1 );

	lua_getfield( L, narg, "b" );
	linear.z = lua_tonumber( L, -1 );
	lua_pop( L, 1 );

	lua_getfield( L, narg, "a" );
	linear.w = lua_isnil( L, -1 ) ? 1.0f : lua_tonumber( L, -1 );
	lua_pop( L, 1 );

	return linear;
}

static int LuauRGBA8( lua_State * L ) {
	RGBA8 rgba;
	rgba.r = CheckRGBA8Component( L, 1, 1 );
	rgba.g = CheckRGBA8Component( L, 2, 2 );
	rgba.b = CheckRGBA8Component( L, 3, 3 );
	rgba.a = lua_isnoneornil( L, 4 ) ? 255 : CheckRGBA8Component( L, 4, 4 );

	lua_newtable( L );

	lua_pushboolean( L, true );
	lua_setfield( L, -2, "srgb" );

	lua_pushnumber( L, rgba.r );
	lua_setfield( L, -2, "r" );

	lua_pushnumber( L, rgba.g );
	lua_setfield( L, -2, "g" );

	lua_pushnumber( L, rgba.b );
	lua_setfield( L, -2, "b" );

	lua_pushnumber( L, rgba.a );
	lua_setfield( L, -2, "a" );

	return 1;
}

static int LuauRGBALinear( lua_State * L ) {
	lua_newtable( L );

	lua_pushboolean( L, false );
	lua_setfield( L, -2, "srgb" );

	lua_pushvalue( L, 1 );
	lua_setfield( L, -2, "r" );

	lua_pushvalue( L, 2 );
	lua_setfield( L, -2, "g" );

	lua_pushvalue( L, 3 );
	lua_setfield( L, -2, "b" );

	lua_pushvalue( L, 4 );
	lua_setfield( L, -2, "a" );

	return 1;
}

static int LuauPrint( lua_State * L ) {
	Com_Printf( "%s\n", luaL_checkstring( hud_L, 1 ) );
	return 0;
}

static int LuauAsset( lua_State * L ) {
	StringHash hash( luaL_checkstring( hud_L, 1 ) );
	lua_pushlightuserdata( L, checked_cast< void * >( checked_cast< uintptr_t >( hash.hash ) ) );
	return 1;
}

static StringHash CheckHash( lua_State * L, int idx ) {
	if( lua_isnoneornil( L, idx ) )
		return EMPTY_HASH;
	luaL_checktype( L, idx, LUA_TLIGHTUSERDATA );
	return StringHash( checked_cast< u64 >( checked_cast< uintptr_t >( lua_touserdata( L, idx ) ) ) );
}

static int LuauDraw2DBox( lua_State * L ) {
	float x = luaL_checknumber( L, 1 );
	float y = luaL_checknumber( L, 2 );
	float w = luaL_checknumber( L, 3 );
	float h = luaL_checknumber( L, 4 );
	Vec4 color = CheckColor( L, 5 );
	StringHash material = CheckHash( L, 6 );
	Draw2DBox( x, y, w, h, material == EMPTY_HASH ? cls.white_material : FindMaterial( material ), color );
	return 0;
}

static Alignment CheckAlignment( lua_State * L, int idx ) {
	constexpr const Alignment alignments[] = {
		Alignment_LeftTop,
		Alignment_CenterTop,
		Alignment_RightTop,
		Alignment_LeftMiddle,
		Alignment_CenterMiddle,
		Alignment_RightMiddle,
		Alignment_LeftBottom,
		Alignment_CenterBottom,
		Alignment_RightBottom,
	};

	constexpr const char * names[] = {
		"left top",
		"center top",
		"right top",
		"left middle",
		"center middle",
		"right middle",
		"left bottom",
		"center bottom",
		"right bottom",
	};

	return alignments[ luaL_checkoption( L, idx, names[ 0 ], names ) ];
}

static const Font * CheckFont( lua_State * L, int idx ) {
	const Font * fonts[] = {
		cgs.fontNormal,
		cgs.fontNormalBold,
		cgs.fontItalic,
		cgs.fontBoldItalic,
	};

	constexpr const char * names[] = {
		"normal",
		"bold",
		"italic",
		"bolditalic",
	};

	return fonts[ luaL_checkoption( L, idx, names[ 0 ], names ) ];
}

static int LuauDrawText( lua_State * L ) {
	luaL_checktype( L, 1, LUA_TTABLE );

	lua_getfield( L, 1, "color" );
	Vec4 color = CheckColor( L, -1 );
	lua_pop( L, 1 );

	lua_getfield( L, 1, "font" );
	const Font * font = CheckFont( L, -1 );
	lua_pop( L, 1 );

	lua_getfield( L, 1, "font_size" );
	luaL_argcheck( L, lua_type( L, -1 ) == LUA_TNUMBER, 1, "options.font_size must be a number" );
	float font_size = lua_tonumber( L, -1 );
	lua_pop( L, 1 );

	bool border = false;
	Vec4 border_color = vec4_black;
	lua_getfield( L, 1, "border" );
	if( !lua_isnil( L, -1 ) ) {
		border = true;
		border_color = CheckColor( L, -1 );
	}
	lua_pop( L, 1 );

	lua_getfield( L, 1, "alignment" );
	Alignment alignment = CheckAlignment( L, -1 );
	lua_pop( L, 1 );

	float x = luaL_checknumber( L, 2 );
	float y = luaL_checknumber( L, 3 );
	const char * str = luaL_checkstring( hud_L, 4 );

	if( border ) {
		DrawText( font, font_size, str, alignment, x, y, color, border_color );
	}
	else {
		DrawText( font, font_size, str, alignment, x, y, color, false );
	}

	return 0;
}

static int LuauGetBind( lua_State * L ) {
	char keys[ 128 ];
	if( !CG_GetBoundKeysString( luaL_checkstring( L, 1 ), keys, sizeof( keys ) ) ) {
		snprintf( keys, sizeof( keys ), "[%s]", luaL_checkstring( L, 1 ) );
	}

	lua_newtable( L );
	lua_pushstring( L, keys );

	return 1;
}

static int Vec4ToLuauColor( lua_State * L, Vec4 color ) {
	lua_newtable( L );

	lua_pushboolean( L, false );
	lua_setfield( L, -2, "srgb" );

	lua_pushnumber( L, color.x );
	lua_setfield( L, -2, "r" );

	lua_pushnumber( L, color.y );
	lua_setfield( L, -2, "g" );

	lua_pushnumber( L, color.z );
	lua_setfield( L, -2, "b" );

	lua_pushnumber( L, color.w );
	lua_setfield( L, -2, "a" );

	return 1;
}

static int LuauGetTeamColor( lua_State * L ) {
	return Vec4ToLuauColor( L, CG_TeamColorVec4( luaL_checknumber( L, 1 ) ) );
}

static int LuauAttentionGettingColor( lua_State * L ) {
	return Vec4ToLuauColor( L, AttentionGettingColor() );
}

static int LuauGetPlayerName( lua_State * L ) {
	int index = luaL_checknumber( L, 1 ) - 1;

	if( index >= 0 && index < client_gs.maxclients ) {
		lua_newtable( L );
		lua_pushstring( L, PlayerName( index ) );

		return 1;
	}

	return 0;
}


static int HUD_DrawBombIndicators( lua_State * L ) {
	CG_DrawBombHUD( luaL_checknumber( L, 1 ), luaL_checknumber( L, 2 ) );
	return 0;
}

static int HUD_DrawClock( lua_State * L ) {
	CG_DrawClock( luaL_checknumber( L, 1 ), luaL_checknumber( L, 2 ), CheckAlignment( L, 5 ), cgs.fontNormalBold, luaL_checknumber( L, 3 ), CheckColor( L, 4 ), luaL_checknumber( L, 6 ) );
	return 0;
}

static int HUD_DrawCrosshair( lua_State * L ) {
	CG_DrawCrosshair( frame_static.viewport_width / 2, frame_static.viewport_height / 2 );
	return 0;
}

static int HUD_DrawDamageNumbers( lua_State * L ) {
	CG_DrawDamageNumbers( luaL_checknumber( L, 1 ), luaL_checknumber( L, 2 ) );
	return 0;
}

static int HUD_DrawPointed( lua_State * L ) {
	CG_DrawPlayerNames( cgs.fontNormalBold, luaL_checknumber( L, 1 ), CheckColor( L, 2 ), luaL_checknumber( L, 3 ) );
	return 0;
}

static int HUD_DrawObituaries( lua_State * L ) {
	int x = luaL_checknumber( L, 1 );
	int y = luaL_checknumber( L, 2 );
	int width = lua_tonumber( L, 3 );
	int height = lua_tonumber( L, 4 );
	unsigned int icon_size = lua_tonumber( L, 5 );
	float font_size = luaL_checknumber( L, 6 );
	Alignment alignment = CheckAlignment( L, 7 );

	const int icon_padding = 4;

	unsigned line_height = Max2( 1u, Max2( unsigned( font_size ), icon_size ) );
	int num_max = height / line_height;

	const Font * font = cgs.fontNormalBold;

	int next = cg_obituaries_current + 1;
	if( next >= MAX_OBITUARIES ) {
		next = 0;
	}

	int num = 0;
	int i = next;
	do {
		if( cg_obituaries[i].type != OBITUARY_NONE && cls.monotonicTime - cg_obituaries[i].time <= 5000 ) {
			num++;
		}
		if( ++i >= MAX_OBITUARIES ) {
			i = 0;
		}
	} while( i != next );

	int skip;
	if( num > num_max ) {
		skip = num - num_max;
		num = num_max;
	} else {
		skip = 0;
	}

	y = CG_VerticalAlignForHeight( y, alignment, height );
	x = CG_HorizontalAlignForWidth( x, alignment, width );

	int xoffset = 0;
	int yoffset = 0;

	i = next;
	do {
		const obituary_t * obr = &cg_obituaries[i];
		if( ++i >= MAX_OBITUARIES ) {
			i = 0;
		}

		if( obr->type == OBITUARY_NONE || cls.monotonicTime - obr->time > 5000 ) {
			continue;
		}

		if( skip > 0 ) {
			skip--;
			continue;
		}

		const Material * pic = DamageTypeToIcon( obr->damage_type );

		float attacker_width = TextBounds( font, font_size, obr->attacker ).maxs.x;
		float victim_width = TextBounds( font, font_size, obr->victim ).maxs.x;

		int w = 0;
		if( obr->type != OBITUARY_ACCIDENT ) {
			w += attacker_width;
		}
		w += icon_padding;
		w += icon_size;
		w += icon_padding;
		w += victim_width;

		if( obr->wallbang ) {
			w += icon_size;
			w += icon_padding;
		}

		xoffset = width - w;

		int obituary_y = y + yoffset + ( line_height - font_size ) / 2;
		if( obr->type != OBITUARY_ACCIDENT ) {
			Vec4 color = CG_TeamColorVec4( obr->attacker_team );
			DrawText( font, font_size, obr->attacker, x + xoffset, obituary_y, color, true );
			xoffset += attacker_width;
		}

		xoffset += icon_padding;

		Draw2DBox( x + xoffset, y + yoffset + ( line_height - icon_size ) / 2, icon_size, icon_size, pic, AttentionGettingColor() );
		xoffset += icon_size + icon_padding;

		if( obr->wallbang ) {
			Draw2DBox( x + xoffset, y + yoffset + ( line_height - icon_size ) / 2, icon_size, icon_size, FindMaterial( "weapons/wallbang_icon" ), AttentionGettingColor() );
			xoffset += icon_size + icon_padding;
		}

		Vec4 color = CG_TeamColorVec4( obr->victim_team );
		DrawText( font, font_size, obr->victim, x + xoffset, obituary_y, color, true );

		yoffset += line_height;
	} while( i != next );

	if( cg.predictedPlayerState.health <= 0 && cg.predictedPlayerState.team != TEAM_SPECTATOR ) {
		if( self_obituary.entropy != 0 ) {
			float h = 128.0f;
			float yy = frame_static.viewport.y * 0.5f - h * 0.5f;

			float t = float( cls.monotonicTime - self_obituary.time ) / 500.0f;

			Draw2DBox( 0, yy, frame_static.viewport.x, h, cls.white_material, Vec4( 0, 0, 0, Min2( 0.5f, t * 0.5f ) ) );

			if( t >= 1.0f ) {
				RNG rng = NewRNG( self_obituary.entropy, 0 );

				TempAllocator temp = cls.frame_arena.temp();
				const char * obituary = MakeObituary( &temp, &rng, self_obituary.type, self_obituary.damage_type );

				float size = Lerp( h * 0.5f, Unlerp01( 1.0f, t, 20.0f ), h * 5.0f );
				Vec4 color = AttentionGettingColor();
				color.w = Unlerp01( 1.0f, t, 2.0f );
				DrawText( cgs.fontNormal, size, obituary, Alignment_CenterMiddle, frame_static.viewport.x * 0.5f, frame_static.viewport.y * 0.5f, color );
			}
		}
	}

	return 0;
}

static int HUD_DrawWeaponBar( lua_State * L ) {
	int x = luaL_checknumber( L, 1 );
	int y = luaL_checknumber( L, 2 );
	int size = luaL_checknumber( L, 3 );
	int padding = luaL_checknumber( L, 4 );
	Alignment alignment = CheckAlignment( L, 5 );

	DrawWeaponBar( x, y, size, padding, alignment );

	return 0;
}

static int HUD_DrawPerksUtility( lua_State * L ) {
	int x = luaL_checknumber( L, 1 );
	int y = luaL_checknumber( L, 2 );
	int size = luaL_checknumber( L, 3 );
	int padding = luaL_checknumber( L, 4 );
	Alignment alignment = CheckAlignment( L, 5 );

	DrawPerksUtility( x, y, size, padding, alignment );

	return 0;
}

void CG_InitHUD() {
	TracyZoneScopedN( "Luau" );

	hud_L = NULL;

	size_t bytecode_size;
	char * bytecode = luau_compile( AssetString( "huds/hud.lua" ).ptr, AssetBinary( "huds/hud.lua" ).n, NULL, &bytecode_size );
	defer { free( bytecode ); };
	if( bytecode == NULL ) {
		Fatal( "luau_compile" );
	}

	hud_L = luaL_newstate();
	if( hud_L == NULL ) {
		Fatal( "luaL_newstate" );
	}

	constexpr const luaL_Reg cdlib[] = {
		{ "print", LuauPrint },
		{ "asset", LuauAsset },
		{ "box", LuauDraw2DBox },
		{ "text", LuauDrawText },
		{ "getBind", LuauGetBind },
		{ "getTeamColor", LuauGetTeamColor },
		{ "attentionGettingColor", LuauAttentionGettingColor },

		{ "getPlayerName", LuauGetPlayerName },

		{ "drawBombIndicators", HUD_DrawBombIndicators },
		{ "drawCrosshair", HUD_DrawCrosshair },
		{ "drawClock", HUD_DrawClock },
		{ "drawObituaries", HUD_DrawObituaries },
		{ "drawPointed", HUD_DrawPointed },
		{ "drawDamageNumbers", HUD_DrawDamageNumbers },
		{ "drawWeaponBar", HUD_DrawWeaponBar },
		{ "drawPerksUtility", HUD_DrawPerksUtility },

		{ NULL, NULL }
	};

	for( size_t i = 0; i < ARRAY_COUNT( cg_numeric_constants ); i++ ) {
		lua_pushnumber( hud_L, cg_numeric_constants[ i ].value );
		lua_setfield( hud_L, LUA_GLOBALSINDEX, cg_numeric_constants[ i ].name );
	}

	luaL_openlibs( hud_L ); // TODO: don't open all libs?
	luaL_register( hud_L, "cd", cdlib );
	lua_pop( hud_L, 1 );

	lua_pushcfunction( hud_L, LuauRGBA8, "RGBA8" );
	lua_setfield( hud_L, LUA_GLOBALSINDEX, "RGBA8" );

	lua_pushcfunction( hud_L, LuauRGBALinear, "RGBALinear" );
	lua_setfield( hud_L, LUA_GLOBALSINDEX, "RGBALinear" );

	luaL_sandbox( hud_L );

	lua_getglobal( hud_L, "debug" );
	lua_getfield( hud_L, -1, "traceback" );
	lua_remove( hud_L, -2 );

	int ok = luau_load( hud_L, "hud.lua", bytecode, bytecode_size, 0 );
	if( ok == 0 ) {
		if( !CallWithStackTrace( hud_L, 0, 1 ) || lua_type( hud_L, -1 ) != LUA_TFUNCTION ) {
			Com_Printf( S_COLOR_RED "hud.lua must return a function\n" );
			lua_close( hud_L );
			hud_L = NULL;
		}
	}
	else {
		Com_Printf( S_COLOR_RED "Luau compilation error: %s\n", lua_tostring( hud_L, -1 ) );
		lua_close( hud_L );
		hud_L = NULL;
	}
}

void CG_ShutdownHUD() {
	if( hud_L != NULL ) {
		lua_close( hud_L );
	}
}

void CG_DrawHUD() {
	TracyZoneScoped;

	bool hotload = false;
	for( const char * path : ModifiedAssetPaths() ) {
		if( StartsWith( path, "huds/" ) ) {
			hotload = true;
			break;
		}
	}

	if( hotload ) {
		CG_ShutdownHUD();
		CG_InitHUD();
	}

	if( hud_L != NULL ) {
		TracyZoneScopedN( "Luau" );

		lua_pushvalue( hud_L, -1 );
		lua_newtable( hud_L );

		lua_pushboolean( hud_L, cg.predictedPlayerState.ready );
		lua_setfield( hud_L, -2, "ready" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.health );
		lua_setfield( hud_L, -2, "health" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.max_health );
		lua_setfield( hud_L, -2, "max_health" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.perk );
		lua_setfield( hud_L, -2, "perk" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.pmove.stamina );
		lua_setfield( hud_L, -2, "stamina" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.pmove.stamina_state );
		lua_setfield( hud_L, -2, "staminaState" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.team );
		lua_setfield( hud_L, -2, "team" );

		lua_pushboolean( hud_L, cg.predictedPlayerState.carrying_bomb );
		lua_setfield( hud_L, -2, "isCarrier" );

		lua_pushboolean( hud_L, cg.predictedPlayerState.can_plant );
		lua_setfield( hud_L, -2, "canPlant" );

		lua_pushboolean( hud_L, cg.predictedPlayerState.can_change_loadout );
		lua_setfield( hud_L, -2, "canChangeLoadout" );

		lua_pushnumber( hud_L, cg.predictedPlayerState.progress );
		lua_setfield( hud_L, -2, "bomb_progress" );

		lua_pushboolean( hud_L, GS_TeamBasedGametype( &client_gs ) );
		lua_setfield( hud_L, -2, "teambased" );

		lua_pushnumber( hud_L, client_gs.gameState.match_state );
		lua_setfield( hud_L, -2, "matchState" );

		lua_pushnumber( hud_L, client_gs.gameState.teams[ TEAM_ALPHA ].score );
		lua_setfield( hud_L, -2, "scoreAlpha" );

		lua_pushnumber( hud_L, client_gs.gameState.bomb.alpha_players_alive );
		lua_setfield( hud_L, -2, "aliveAlpha" );

		lua_pushnumber( hud_L, client_gs.gameState.bomb.alpha_players_total );
		lua_setfield( hud_L, -2, "totalAlpha" );

		lua_pushnumber( hud_L, client_gs.gameState.teams[ TEAM_BETA ].score );
		lua_setfield( hud_L, -2, "scoreBeta" );

		lua_pushnumber( hud_L, client_gs.gameState.bomb.beta_players_alive );
		lua_setfield( hud_L, -2, "aliveBeta" );

		lua_pushnumber( hud_L, client_gs.gameState.bomb.beta_players_total );
		lua_setfield( hud_L, -2, "totalBeta" );

		lua_pushnumber( hud_L, client_gs.gameState.round_type );
		lua_setfield( hud_L, -2, "roundType" );

		lua_pushnumber( hud_L, CG_GetPOVnum() );
		lua_setfield( hud_L, -2, "chasing" );

		lua_pushstring( hud_L, cl.configstrings[ CS_CALLVOTE ] );
		lua_setfield( hud_L, -2, "vote" );

		lua_pushnumber( hud_L, client_gs.gameState.callvote_required_votes );
		lua_setfield( hud_L, -2, "votesRequired" );

		lua_pushnumber( hud_L, client_gs.gameState.callvote_yes_votes );
		lua_setfield( hud_L, -2, "votesTotal" );

		lua_pushboolean( hud_L, cg.predictedPlayerState.voted );
		lua_setfield( hud_L, -2, "hasVoted" );

		lua_pushboolean( hud_L, CG_IsLagging() );
		lua_setfield( hud_L, -2, "lagging" );

		lua_pushboolean( hud_L, Cvar_Bool( "cg_showFPS" ) );
		lua_setfield( hud_L, -2, "show_fps" );

		lua_pushboolean( hud_L, Cvar_Bool( "cg_showHotkeys" ) );
		lua_setfield( hud_L, -2, "show_hotkeys" );

		lua_pushnumber( hud_L, CG_GetFPS() );
		lua_setfield( hud_L, -2, "fps" );

		lua_pushboolean( hud_L, Cvar_Bool( "cg_showSpeed" ) );
		lua_setfield( hud_L, -2, "show_speed" );

		lua_pushnumber( hud_L, CG_GetSpeed() );
		lua_setfield( hud_L, -2, "speed" );

		lua_pushnumber( hud_L, frame_static.viewport_width );
		lua_setfield( hud_L, -2, "viewport_width" );

		lua_pushnumber( hud_L, frame_static.viewport_height );
		lua_setfield( hud_L, -2, "viewport_height" );

		CallWithStackTrace( hud_L, 1, 0 );
	}
}
