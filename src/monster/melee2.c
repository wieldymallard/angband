/*
 * File: melee2.c
 * Purpose: Monster AI routines
 *
 * Copyright (c) 1997 Ben Harrison, David Reeve Sward, Keldon Jones.
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "attack.h"
#include "cave.h"
#include "monster/monster.h"
#include "monster/mon-make.h"
#include "monster/mon-spell.h"
#include "monster/mon-timed.h"
#include "monster/mon-util.h"
#include "object/slays.h"
#include "object/tvalsval.h"
#include "spells.h"
#include "squelch.h"

/*
 * Determine if a bolt will arrive, checking that no monsters are in the way
 */
#define clean_shot(Y1, X1, Y2, X2) \
	projectable(Y1, X1, Y2, X2, PROJECT_STOP)

/*
 * And now for Intelligent monster attacks (including spells).
 *
 * Give monsters more intelligent attack/spell selection based on
 * observations of previous attacks on the player, and/or by allowing
 * the monster to "cheat" and know the player status.
 *
 * Maintain an idea of the player status, and use that information
 * to occasionally eliminate "ineffective" spell attacks.  We could
 * also eliminate ineffective normal attacks, but there is no reason
 * for the monster to do this, since he gains no benefit.
 * Note that MINDLESS monsters are not allowed to use this code.
 * And non-INTELLIGENT monsters only use it partially effectively.
 *
 * Actually learn what the player resists, and use that information
 * to remove attacks or spells before using them. 
 *
 * This has the added advantage that attacks and spells are related.
 * The "smart_learn" option means that the monster "learns" the flags
 * that should be set, and "smart_cheat" means that he "knows" them.
 * So "smart_cheat" means that the "smart" field is always up to date,
 * while "smart_learn" means that the "smart" field is slowly learned.
 * Both of them have the same effect on the "choose spell" routine.
 */

/*
 * Remove the "bad" spells from a spell list
 */
static void remove_bad_spells(struct monster *m_ptr, bitflag f[RSF_SIZE])
{
	bitflag f2[RSF_SIZE], ai_flags[OF_SIZE];

	u32b smart = 0L;

	/* Stupid monsters act randomly */
	if (rf_has(m_ptr->race->flags, RF_STUPID)) return;

	/* Take working copy of spell flags */
	rsf_copy(f2, f);

	/* Don't heal if full */
	if (m_ptr->hp >= m_ptr->maxhp) rsf_off(f2, RSF_HEAL);
	
	/* Don't haste if hasted with time remaining */
	if (m_ptr->m_timed[MON_TMD_FAST] > 10) rsf_off(f2, RSF_HASTE);

	/* Don't teleport to if the player is already next to us */
	if (m_ptr->cdis == 1) rsf_off(f2, RSF_TELE_TO);

	/* Update acquired knowledge */
	of_wipe(ai_flags);
	if (OPT(birth_ai_learn))
	{
		/* Occasionally forget player status */
		if (one_in_(100))
			of_wipe(m_ptr->known_pflags);

		/* Use the memorized flags */
		smart = m_ptr->smart;
		of_copy(ai_flags, m_ptr->known_pflags);
	}

	/* Cancel out certain flags based on knowledge */
	if (!of_is_empty(ai_flags))
		unset_spells(f2, ai_flags, m_ptr->race);

	if (smart & SM_IMM_MANA && randint0(100) <
			50 * (rf_has(m_ptr->race->flags, RF_SMART) ? 2 : 1))
		rsf_off(f2, RSF_DRAIN_MANA);

	/* use working copy of spell flags */
	rsf_copy(f, f2);
}


/*
 * Determine if there is a space near the selected spot in which
 * a summoned creature can appear
 */
static bool summon_possible(int y1, int x1)
{
	int y, x;

	/* Start at the location, and check 2 grids in each dir */
	for (y = y1 - 2; y <= y1 + 2; y++)
	{
		for (x = x1 - 2; x <= x1 + 2; x++)
		{
			/* Ignore illegal locations */
			if (!square_in_bounds(cave, y, x)) continue;

			/* Only check a circular area */
			if (distance(y1, x1, y, x) > 2) continue;

			/* Hack: no summon on glyph of warding */
			if (square_iswarded(cave, y, x)) continue;

			/* Require empty floor grid in line of sight */
			if (square_isempty(cave, y, x) && los(y1, x1, y, x))
			{
				return (TRUE);
			}
		}
	}

	return FALSE;
}


/*
 * Have a monster choose a spell to cast.
 *
 * Note that the monster's spell list has already had "useless" spells
 * (bolts that won't hit the player, summons without room, etc.) removed.
 * Perhaps that should be done by this function.
 *
 * Stupid monsters will just pick a spell randomly.  Smart monsters
 * will choose more "intelligently".
 *
 * This function could be an efficiency bottleneck.
 */
static int choose_attack_spell(struct monster *m_ptr, bitflag f[RSF_SIZE])
{
	int num = 0;
	byte spells[RSF_MAX];

	int i;

	/* Extract all spells: "innate", "normal", "bizarre" */
	for (i = FLAG_START, num = 0; i < RSF_MAX; i++)
	{
		if (rsf_has(f, i)) spells[num++] = i;
	}

	/* Paranoia */
	if (num == 0) return 0;

	/* Pick at random */
	return (spells[randint0(num)]);
}


/*
 * Creatures can cast spells, shoot missiles, and breathe.
 *
 * Returns "TRUE" if a spell (or whatever) was (successfully) cast.
 *
 * XXX XXX XXX This function could use some work, but remember to
 * keep it as optimized as possible, while retaining generic code.
 *
 * Verify the various "blind-ness" checks in the code.
 *
 * XXX XXX XXX Note that several effects should really not be "seen"
 * if the player is blind.
 *
 * Perhaps monsters should breathe at locations *near* the player,
 * since this would allow them to inflict "partial" damage.
 *
 * Perhaps smart monsters should decline to use "bolt" spells if
 * there is a monster in the way, unless they wish to kill it.
 *
 * It will not be possible to "correctly" handle the case in which a
 * monster attempts to attack a location which is thought to contain
 * the player, but which in fact is nowhere near the player, since this
 * might induce all sorts of messages about the attack itself, and about
 * the effects of the attack, which the player might or might not be in
 * a position to observe.  Thus, for simplicity, it is probably best to
 * only allow "faulty" attacks by a monster if one of the important grids
 * (probably the initial or final grid) is in fact in view of the player.
 * It may be necessary to actually prevent spell attacks except when the
 * monster actually has line of sight to the player.  Note that a monster
 * could be left in a bizarre situation after the player ducked behind a
 * pillar and then teleported away, for example.
 *
 * Note that this function attempts to optimize the use of spells for the
 * cases in which the monster has no spells, or has spells but cannot use
 * them, or has spells but they will have no "useful" effect.  Note that
 * this function has been an efficiency bottleneck in the past.
 *
 * Note the special "MFLAG_NICE" flag, which prevents a monster from using
 * any spell attacks until the player has had a single chance to move.
 */
bool make_attack_spell(struct monster *m_ptr)
{
	int chance, thrown_spell, rlev, failrate;

	bitflag f[RSF_SIZE];

	monster_lore *l_ptr = get_lore(m_ptr->race);

	char m_name[80], m_poss[80], ddesc[80];

	/* Player position */
	int px = p_ptr->px;
	int py = p_ptr->py;

	/* Extract the blind-ness */
	bool blind = (p_ptr->timed[TMD_BLIND] ? TRUE : FALSE);

	/* Extract the "see-able-ness" */
	bool seen = (!blind && m_ptr->ml);

	/* Assume "normal" target */
	bool normal = TRUE;

	/* Handle "leaving" */
	if (p_ptr->leaving) return FALSE;

	/* Cannot cast spells when confused */
	if (m_ptr->m_timed[MON_TMD_CONF]) return (FALSE);

	/* Cannot cast spells when nice */
	if (m_ptr->mflag & MFLAG_NICE) return FALSE;

	/* Hack -- Extract the spell probability */
	chance = (m_ptr->race->freq_innate + m_ptr->race->freq_spell) / 2;

	/* Not allowed to cast spells */
	if (!chance) return FALSE;

	/* Only do spells occasionally */
	if (randint0(100) >= chance) return FALSE;

	/* Hack -- require projectable player */
	if (normal)
	{
		/* Check range */
		if (m_ptr->cdis > MAX_RANGE) return FALSE;

		/* Check path */
		if (!projectable(m_ptr->fy, m_ptr->fx, py, px, PROJECT_NONE))
			return FALSE;
	}

	/* Extract the monster level */
	rlev = ((m_ptr->race->level >= 1) ? m_ptr->race->level : 1);

	/* Extract the racial spell flags */
	rsf_copy(f, m_ptr->race->spell_flags);

	/* Allow "desperate" spells */
	if (rf_has(m_ptr->race->flags, RF_SMART) &&
	    m_ptr->hp < m_ptr->maxhp / 10 &&
	    randint0(100) < 50)

		/* Require intelligent spells */
		set_spells(f, RST_HASTE | RST_ANNOY | RST_ESCAPE | RST_HEAL | RST_TACTIC | RST_SUMMON);

	/* Remove the "ineffective" spells */
	remove_bad_spells(m_ptr, f);

	/* Check whether summons and bolts are worth it. */
	if (!rf_has(m_ptr->race->flags, RF_STUPID))
	{
		/* Check for a clean bolt shot */
		if (test_spells(f, RST_BOLT) &&
			!clean_shot(m_ptr->fy, m_ptr->fx, py, px))

			/* Remove spells that will only hurt friends */
			set_spells(f, ~RST_BOLT);

		/* Check for a possible summon */
		if (!(summon_possible(m_ptr->fy, m_ptr->fx)))

			/* Remove summoning spells */
			set_spells(f, ~RST_SUMMON);
	}

	/* No spells left */
	if (rsf_is_empty(f)) return FALSE;

	/* Get the monster name (or "it") */
	monster_desc(m_name, sizeof(m_name), m_ptr, MDESC_STANDARD);

	/* Get the monster possessive ("his"/"her"/"its") */
	monster_desc(m_poss, sizeof(m_poss), m_ptr, MDESC_PRO_VIS | MDESC_POSS);

	/* Get the "died from" name */
	monster_desc(ddesc, sizeof(ddesc), m_ptr, MDESC_DIED_FROM);

	/* Choose a spell to cast */
	thrown_spell = choose_attack_spell(m_ptr, f);

	/* Abort if no spell was chosen */
	if (!thrown_spell) return FALSE;

	/* If we see an unaware monster try to cast a spell, become aware of it */
	if (m_ptr->unaware)
		become_aware(m_ptr);

	/* Calculate spell failure rate */
	failrate = 25 - (rlev + 3) / 4;
	if (m_ptr->m_timed[MON_TMD_FEAR])
		failrate += 20;

	/* Stupid monsters will never fail (for jellies and such) */
	if (rf_has(m_ptr->race->flags, RF_STUPID))
		failrate = 0;

	/* Check for spell failure (innate attacks never fail) */
	if ((thrown_spell >= MIN_NONINNATE_SPELL) && (randint0(100) < failrate))
	{
		/* Message */
		msg("%s tries to cast a spell, but fails.", m_name);

		return TRUE;
	}

	/* Cast the spell. */
	disturb(p_ptr, 1, 0);

	/* Special case RSF_HASTE until TMD_* and MON_TMD_* are rationalised */
	if (thrown_spell == RSF_HASTE) {
		if (blind)
			msg("%s mumbles.", m_name);
		else
			msg("%s concentrates on %s body.", m_name, m_poss);

		(void)mon_inc_timed(m_ptr, MON_TMD_FAST, 50, 0, FALSE);
	} else {
		do_mon_spell(thrown_spell, m_ptr, seen);
	}

	/* Remember what the monster did to us */
	if (seen) {
		rsf_on(l_ptr->spell_flags, thrown_spell);

		/* Innate spell */
		if (thrown_spell < MIN_NONINNATE_SPELL) {
			if (l_ptr->cast_innate < MAX_UCHAR)
				l_ptr->cast_innate++;
		} else {
		/* Bolt or Ball, or Special spell */
			if (l_ptr->cast_spell < MAX_UCHAR)
				l_ptr->cast_spell++;
		}
	}
	/* Always take note of monsters that kill you */
	if (p_ptr->is_dead && (l_ptr->deaths < MAX_SHORT)) {
		l_ptr->deaths++;
	}

	/* A spell was cast */
	return TRUE;
}



/*
 * Returns whether a given monster will try to run from the player.
 *
 * Monsters will attempt to avoid very powerful players.  See below.
 *
 * Because this function is called so often, little details are important
 * for efficiency.  Like not using "mod" or "div" when possible.  And
 * attempting to check the conditions in an optimal order.  Note that
 * "(x << 2) == (x * 4)" if "x" has enough bits to hold the result.
 *
 * Note that this function is responsible for about one to five percent
 * of the processor use in normal conditions...
 */
static int mon_will_run(struct monster *m_ptr)
{
	u16b p_lev, m_lev;
	u16b p_chp, p_mhp;
	u16b m_chp, m_mhp;
	u32b p_val, m_val;

	/* Keep monsters from running too far away */
	if (m_ptr->cdis > MAX_SIGHT + 5) return (FALSE);

	/* All "afraid" monsters will run away */
	if (m_ptr->m_timed[MON_TMD_FEAR]) return (TRUE);

	/* Nearby monsters will not become terrified */
	if (m_ptr->cdis <= 5) return (FALSE);

	/* Examine player power (level) */
	p_lev = p_ptr->lev;

	/* Examine monster power (level plus morale) */
	m_lev = m_ptr->race->level + (m_ptr->midx & 0x08) + 25;

	/* Optimize extreme cases below */
	if (m_lev > p_lev + 4) return (FALSE);
	if (m_lev + 4 <= p_lev) return (TRUE);

	/* Examine player health */
	p_chp = p_ptr->chp;
	p_mhp = p_ptr->mhp;

	/* Examine monster health */
	m_chp = m_ptr->hp;
	m_mhp = m_ptr->maxhp;

	/* Prepare to optimize the calculation */
	p_val = (p_lev * p_mhp) + (p_chp << 2);	/* div p_mhp */
	m_val = (m_lev * m_mhp) + (m_chp << 2);	/* div m_mhp */

	/* Strong players scare strong monsters */
	if (p_val * m_mhp > m_val * p_mhp) return (TRUE);

	/* Assume no terror */
	return (FALSE);
}

/* From Will Asher in DJA:
 * Find whether a monster is near a permanent wall
 * this decides whether PASS_WALL & KILL_WALL monsters 
 * use the monster flow code
 */
static bool near_permwall(const monster_type *m_ptr, struct cave *c)
{
	int y, x;
	int my = m_ptr->fy;
	int mx = m_ptr->fx;
	
	/* if PC is in LOS, there's no need to go around walls */
    if (projectable(my, mx, p_ptr->py, p_ptr->px, PROJECT_NONE)) return FALSE;
    
    /* PASS_WALL & KILL_WALL monsters occasionally flow for a turn anyway */
    if (randint0(99) < 5) return TRUE;
    
	/* Search the nearby grids, which are always in bounds */
	for (y = (my - 2); y <= (my + 2); y++)
	{
		for (x = (mx - 2); x <= (mx + 2); x++)
		{
            if (!square_in_bounds_fully(c, y, x)) continue;
            if (square_isperm(c, y, x)) return TRUE;
		}
	}
	return FALSE;
}


/*
 * Choose the "best" direction for "flowing"
 *
 * Note that ghosts and rock-eaters are never allowed to "flow",
 * since they should move directly towards the player.
 *
 * Prefer "non-diagonal" directions, but twiddle them a little
 * to angle slightly towards the player's actual location.
 *
 * Allow very perceptive monsters to track old "spoor" left by
 * previous locations occupied by the player.  This will tend
 * to have monsters end up either near the player or on a grid
 * recently occupied by the player (and left via "teleport").
 *
 * Note that if "smell" is turned on, all monsters get vicious.
 *
 * Also note that teleporting away from a location will cause
 * the monsters who were chasing you to converge on that location
 * as long as you are still near enough to "annoy" them without
 * being close enough to chase directly.  I have no idea what will
 * happen if you combine "smell" with low "aaf" values.
 */
static bool get_moves_aux(struct cave *c, struct monster *m_ptr, int *yp, int *xp)
{
	int py = p_ptr->py;
	int px = p_ptr->px;

	int i, y, x, y1, x1;

	int when = 0;
	int cost = 999;

	/* Monster can go through rocks */
	if (flags_test(m_ptr->race->flags, RF_SIZE, RF_PASS_WALL, RF_KILL_WALL, FLAG_END)){
	
	    /* If monster is near a permwall, use normal pathfinding */
	    if (!near_permwall(m_ptr, c)) return (FALSE);
    }
		
	/* Monster location */
	y1 = m_ptr->fy;
	x1 = m_ptr->fx;

	/* The player is not currently near the monster grid */
	if (c->when[y1][x1] < c->when[py][px])
	{
		/* The player has never been near the monster grid */
		if (c->when[y1][x1] == 0) return (FALSE);
	}

	/* Monster is too far away to notice the player */
	if (c->cost[y1][x1] > MONSTER_FLOW_DEPTH) return (FALSE);
	if (c->cost[y1][x1] > (OPT(birth_small_range) ? m_ptr->race->aaf / 2 : m_ptr->race->aaf)) return (FALSE);

	/* Hack -- Player can see us, run towards him */
	if (player_has_los_bold(y1, x1)) return (FALSE);

	/* Check nearby grids, diagonals first */
	for (i = 7; i >= 0; i--)
	{
		/* Get the location */
		y = y1 + ddy_ddd[i];
		x = x1 + ddx_ddd[i];

		/* Ignore illegal locations */
		if (c->when[y][x] == 0) continue;

		/* Ignore ancient locations */
		if (c->when[y][x] < when) continue;

		/* Ignore distant locations */
		if (c->cost[y][x] > cost) continue;

		/* Save the cost and time */
		when = c->when[y][x];
		cost = c->cost[y][x];

		/* Hack -- Save the "twiddled" location */
		(*yp) = py + 16 * ddy_ddd[i];
		(*xp) = px + 16 * ddx_ddd[i];
	}

	/* No legal move (?) */
	if (!when) return (FALSE);

	/* Success */
	return (TRUE);
}

/*
 * Provide a location to flee to, but give the player a wide berth.
 *
 * A monster may wish to flee to a location that is behind the player,
 * but instead of heading directly for it, the monster should "swerve"
 * around the player so that he has a smaller chance of getting hit.
 */
static bool get_fear_moves_aux(struct cave *c, struct monster *m_ptr, int *yp, int *xp)
{
	int y, x, y1, x1, fy, fx, py, px, gy = 0, gx = 0;
	int when = 0, score = -1;
	int i;

	/* Player location */
	py = p_ptr->py;
	px = p_ptr->px;

	/* Monster location */
	fy = m_ptr->fy;
	fx = m_ptr->fx;

	/* Desired destination */
	y1 = fy - (*yp);
	x1 = fx - (*xp);

	/* The player is not currently near the monster grid */
	if (c->when[fy][fx] < c->when[py][px])
	{
		/* No reason to attempt flowing */
		return (FALSE);
	}

	/* Monster is too far away to use flow information */
	if (c->cost[fy][fx] > MONSTER_FLOW_DEPTH) return (FALSE);
	if (c->cost[fy][fx] > (OPT(birth_small_range) ? m_ptr->race->aaf / 2 : m_ptr->race->aaf)) return (FALSE);

	/* Check nearby grids, diagonals first */
	for (i = 7; i >= 0; i--)
	{
		int dis, s;

		/* Get the location */
		y = fy + ddy_ddd[i];
		x = fx + ddx_ddd[i];

		/* Ignore illegal locations */
		if (c->when[y][x] == 0) continue;

		/* Ignore ancient locations */
		if (c->when[y][x] < when) continue;

		/* Calculate distance of this grid from our destination */
		dis = distance(y, x, y1, x1);

		/* Score this grid */
		s = 5000 / (dis + 3) - 500 / (c->cost[y][x] + 1);

		/* No negative scores */
		if (s < 0) s = 0;

		/* Ignore lower scores */
		if (s < score) continue;

		/* Save the score and time */
		when = c->when[y][x];
		score = s;

		/* Save the location */
		gy = y;
		gx = x;
	}

	/* No legal move (?) */
	if (!when) return (FALSE);

	/* Find deltas */
	(*yp) = fy - gy;
	(*xp) = fx - gx;

	/* Success */
	return (TRUE);
}



/*
 * Hack -- Precompute a bunch of calls to distance() in find_safety() and
 * find_hiding().
 *
 * The pair of arrays dist_offsets_y[n] and dist_offsets_x[n] contain the
 * offsets of all the locations with a distance of n from a central point,
 * with an offset of (0,0) indicating no more offsets at this distance.
 *
 * This is, of course, fairly unreadable, but it eliminates multiple loops
 * from the previous version.
 *
 * It is probably better to replace these arrays with code to compute
 * the relevant arrays, even if the storage is pre-allocated in hard
 * coded sizes.  At the very least, code should be included which is
 * able to generate and dump these arrays (ala "los()").  XXX XXX XXX
 *
 * Also, the storage needs could be reduced by using char.  XXX XXX XXX
 *
 * These arrays could be combined into two big arrays, using sub-arrays
 * to hold the offsets and lengths of each portion of the sub-arrays, and
 * this could perhaps also be used somehow in the "look" code.  XXX XXX XXX
 */


static const int d_off_y_0[] =
{ 0 };

static const int d_off_x_0[] =
{ 0 };


static const int d_off_y_1[] =
{ -1, -1, -1, 0, 0, 1, 1, 1, 0 };

static const int d_off_x_1[] =
{ -1, 0, 1, -1, 1, -1, 0, 1, 0 };


static const int d_off_y_2[] =
{ -1, -1, -2, -2, -2, 0, 0, 1, 1, 2, 2, 2, 0 };

static const int d_off_x_2[] =
{ -2, 2, -1, 0, 1, -2, 2, -2, 2, -1, 0, 1, 0 };


static const int d_off_y_3[] =
{ -1, -1, -2, -2, -3, -3, -3, 0, 0, 1, 1, 2, 2,
  3, 3, 3, 0 };

static const int d_off_x_3[] =
{ -3, 3, -2, 2, -1, 0, 1, -3, 3, -3, 3, -2, 2,
  -1, 0, 1, 0 };


static const int d_off_y_4[] =
{ -1, -1, -2, -2, -3, -3, -3, -3, -4, -4, -4, 0,
  0, 1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 0 };

static const int d_off_x_4[] =
{ -4, 4, -3, 3, -2, -3, 2, 3, -1, 0, 1, -4, 4,
  -4, 4, -3, 3, -2, -3, 2, 3, -1, 0, 1, 0 };


static const int d_off_y_5[] =
{ -1, -1, -2, -2, -3, -3, -4, -4, -4, -4, -5, -5,
  -5, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 5, 5,
  5, 0 };

static const int d_off_x_5[] =
{ -5, 5, -4, 4, -4, 4, -2, -3, 2, 3, -1, 0, 1,
  -5, 5, -5, 5, -4, 4, -4, 4, -2, -3, 2, 3, -1,
  0, 1, 0 };


static const int d_off_y_6[] =
{ -1, -1, -2, -2, -3, -3, -4, -4, -5, -5, -5, -5,
  -6, -6, -6, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
  5, 5, 6, 6, 6, 0 };

static const int d_off_x_6[] =
{ -6, 6, -5, 5, -5, 5, -4, 4, -2, -3, 2, 3, -1,
  0, 1, -6, 6, -6, 6, -5, 5, -5, 5, -4, 4, -2,
  -3, 2, 3, -1, 0, 1, 0 };


static const int d_off_y_7[] =
{ -1, -1, -2, -2, -3, -3, -4, -4, -5, -5, -5, -5,
  -6, -6, -6, -6, -7, -7, -7, 0, 0, 1, 1, 2, 2, 3,
  3, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 0 };

static const int d_off_x_7[] =
{ -7, 7, -6, 6, -6, 6, -5, 5, -4, -5, 4, 5, -2,
  -3, 2, 3, -1, 0, 1, -7, 7, -7, 7, -6, 6, -6,
  6, -5, 5, -4, -5, 4, 5, -2, -3, 2, 3, -1, 0,
  1, 0 };


static const int d_off_y_8[] =
{ -1, -1, -2, -2, -3, -3, -4, -4, -5, -5, -6, -6,
  -6, -6, -7, -7, -7, -7, -8, -8, -8, 0, 0, 1, 1,
  2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
  8, 8, 8, 0 };

static const int d_off_x_8[] =
{ -8, 8, -7, 7, -7, 7, -6, 6, -6, 6, -4, -5, 4,
  5, -2, -3, 2, 3, -1, 0, 1, -8, 8, -8, 8, -7,
  7, -7, 7, -6, 6, -6, 6, -4, -5, 4, 5, -2, -3,
  2, 3, -1, 0, 1, 0 };


static const int d_off_y_9[] =
{ -1, -1, -2, -2, -3, -3, -4, -4, -5, -5, -6, -6,
  -7, -7, -7, -7, -8, -8, -8, -8, -9, -9, -9, 0,
  0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 7,
  7, 8, 8, 8, 8, 9, 9, 9, 0 };

static const int d_off_x_9[] =
{ -9, 9, -8, 8, -8, 8, -7, 7, -7, 7, -6, 6, -4,
  -5, 4, 5, -2, -3, 2, 3, -1, 0, 1, -9, 9, -9,
  9, -8, 8, -8, 8, -7, 7, -7, 7, -6, 6, -4, -5,
  4, 5, -2, -3, 2, 3, -1, 0, 1, 0 };


static const int *dist_offsets_y[10] =
{
	d_off_y_0, d_off_y_1, d_off_y_2, d_off_y_3, d_off_y_4,
	d_off_y_5, d_off_y_6, d_off_y_7, d_off_y_8, d_off_y_9
};

static const int *dist_offsets_x[10] =
{
	d_off_x_0, d_off_x_1, d_off_x_2, d_off_x_3, d_off_x_4,
	d_off_x_5, d_off_x_6, d_off_x_7, d_off_x_8, d_off_x_9
};


/*
 * Choose a "safe" location near a monster for it to run toward.
 *
 * A location is "safe" if it can be reached quickly and the player
 * is not able to fire into it (it isn't a "clean shot").  So, this will
 * cause monsters to "duck" behind walls.  Hopefully, monsters will also
 * try to run towards corridor openings if they are in a room.
 *
 * This function may take lots of CPU time if lots of monsters are fleeing.
 *
 * Return TRUE if a safe location is available.
 */
static bool find_safety(struct cave *c, struct monster *m_ptr, int *yp, int *xp)
{
	int fy = m_ptr->fy;
	int fx = m_ptr->fx;

	int py = p_ptr->py;
	int px = p_ptr->px;

	int i, y, x, dy, dx, d, dis;
	int gy = 0, gx = 0, gdis = 0;

	const int *y_offsets;
	const int *x_offsets;

	/* Start with adjacent locations, spread further */
	for (d = 1; d < 10; d++)
	{
		/* Get the lists of points with a distance d from (fx, fy) */
		y_offsets = dist_offsets_y[d];
		x_offsets = dist_offsets_x[d];

		/* Check the locations */
		for (i = 0, dx = x_offsets[0], dy = y_offsets[0];
		     dx != 0 || dy != 0;
		     i++, dx = x_offsets[i], dy = y_offsets[i])
		{
			y = fy + dy;
			x = fx + dx;

			/* Skip illegal locations */
			if (!square_in_bounds_fully(cave, y, x)) continue;

			/* Skip locations in a wall */
			if (!square_ispassable(cave, y, x)) continue;

			/* Ignore grids very far from the player */
			if (c->when[y][x] < c->when[py][px]) continue;

			/* Ignore too-distant grids */
			if (c->cost[y][x] > c->cost[fy][fx] + 2 * d) continue;

			/* Check for absence of shot (more or less) */
			if (!player_has_los_bold(y,x))
			{
				/* Calculate distance from player */
				dis = distance(y, x, py, px);

				/* Remember if further than previous */
				if (dis > gdis)
				{
					gy = y;
					gx = x;
					gdis = dis;
				}
			}
		}

		/* Check for success */
		if (gdis > 0)
		{
			/* Good location */
			(*yp) = fy - gy;
			(*xp) = fx - gx;

			/* Found safe place */
			return (TRUE);
		}
	}

	/* No safe place */
	return (FALSE);
}




/*
 * Choose a good hiding place near a monster for it to run toward.
 *
 * Pack monsters will use this to "ambush" the player and lure him out
 * of corridors into open space so they can swarm him.
 *
 * Return TRUE if a good location is available.
 */
static bool find_hiding(struct monster *m_ptr, int *yp, int *xp)
{
	int fy = m_ptr->fy;
	int fx = m_ptr->fx;

	int py = p_ptr->py;
	int px = p_ptr->px;

	int i, y, x, dy, dx, d, dis;
	int gy = 0, gx = 0, gdis = 999, min;

	const int *y_offsets, *x_offsets;

	/* Closest distance to get */
	min = distance(py, px, fy, fx) * 3 / 4 + 2;

	/* Start with adjacent locations, spread further */
	for (d = 1; d < 10; d++)
	{
		/* Get the lists of points with a distance d from (fx, fy) */
		y_offsets = dist_offsets_y[d];
		x_offsets = dist_offsets_x[d];

		/* Check the locations */
		for (i = 0, dx = x_offsets[0], dy = y_offsets[0];
		     dx != 0 || dy != 0;
		     i++, dx = x_offsets[i], dy = y_offsets[i])
		{
			y = fy + dy;
			x = fx + dx;

			/* Skip illegal locations */
			if (!square_in_bounds_fully(cave, y, x)) continue;

			/* Skip occupied locations */
			if (!square_isempty(cave, y, x)) continue;

			/* Check for hidden, available grid */
			if (!player_has_los_bold(y, x) && (clean_shot(fy, fx, y, x)))
			{
				/* Calculate distance from player */
				dis = distance(y, x, py, px);

				/* Remember if closer than previous */
				if (dis < gdis && dis >= min)
				{
					gy = y;
					gx = x;
					gdis = dis;
				}
			}
		}

		/* Check for success */
		if (gdis < 999)
		{
			/* Good location */
			(*yp) = fy - gy;
			(*xp) = fx - gx;

			/* Found good place */
			return (TRUE);
		}
	}

	/* No good place */
	return (FALSE);
}


/*
 * Choose "logical" directions for monster movement
 *
 * We store the directions in a special "mm" array
 */
static bool get_moves(struct cave *c, struct monster *m_ptr, int mm[5])
{
	int py = p_ptr->py;
	int px = p_ptr->px;

	int y, ay, x, ax;

	int move_val = 0;

	int y2 = py;
	int x2 = px;

	bool done = FALSE;

	/* Flow towards the player */
	get_moves_aux(c, m_ptr, &y2, &x2);

	/* Extract the "pseudo-direction" */
	y = m_ptr->fy - y2;
	x = m_ptr->fx - x2;



	/* Normal animal packs try to get the player out of corridors. */
	if (rf_has(m_ptr->race->flags, RF_GROUP_AI) &&
	    !flags_test(m_ptr->race->flags, RF_SIZE, RF_PASS_WALL, RF_KILL_WALL, FLAG_END))
	{
		int i, open = 0;

		/* Count empty grids next to player */
		for (i = 0; i < 8; i++)
		{
			int ry = py + ddy_ddd[i];
			int rx = px + ddx_ddd[i];
			/* Check grid around the player for room interior (room walls count)
			   or other empty space */
			if (square_ispassable(cave, ry, rx) || square_isroom(cave, ry, rx))
			{
				/* One more open grid */
				open++;
			}
		}

		/* Not in an empty space and strong player */
		if ((open < 7) && (p_ptr->chp > p_ptr->mhp / 2))
		{
			/* Find hiding place */
			if (find_hiding(m_ptr, &y, &x)) done = TRUE;
		}
	}


	/* Apply fear */
	if (!done && mon_will_run(m_ptr))
	{
		/* Try to find safe place */
		if (!find_safety(c, m_ptr, &y, &x))
		{
			/* This is not a very "smart" method XXX XXX */
			y = (-y);
			x = (-x);
		}

		else
		{
			/* Adjust movement */
			get_fear_moves_aux(c, m_ptr, &y, &x);
		}

		done = TRUE;
	}


	/* Monster groups try to surround the player */
	if (!done && rf_has(m_ptr->race->flags, RF_GROUP_AI))
	{
		int i;

		/* If we are not already adjacent */
		if (m_ptr->cdis > 1)
		{
			/* Find an empty square near the player to fill */
			int tmp = randint0(8);
			for (i = 0; i < 8; i++)
			{
				/* Pick squares near player (pseudo-randomly) */
				y2 = py + ddy_ddd[(tmp + i) & 7];
				x2 = px + ddx_ddd[(tmp + i) & 7];
				
				/* Ignore filled grids */
				if (!square_isempty(cave, y2, x2)) continue;
				
				/* Try to fill this hole */
				break;
			}
		}
		/* Extract the new "pseudo-direction" */
		y = m_ptr->fy - y2;
		x = m_ptr->fx - x2;
	}


	/* Check for no move */
	if (!x && !y) return (FALSE);

	/* Extract the "absolute distances" */
	ax = ABS(x);
	ay = ABS(y);

	/* Do something weird */
	if (y < 0) move_val += 8;
	if (x > 0) move_val += 4;

	/* Prevent the diamond maneuvre */
	if (ay > (ax << 1))
	{
		move_val++;
		move_val++;
	}
	else if (ax > (ay << 1))
	{
		move_val++;
	}

	/* Analyze */
	switch (move_val)
	{
		case 0:
		{
			mm[0] = 9;
			if (ay > ax)
			{
				mm[1] = 8;
				mm[2] = 6;
				mm[3] = 7;
				mm[4] = 3;
			}
			else
			{
				mm[1] = 6;
				mm[2] = 8;
				mm[3] = 3;
				mm[4] = 7;
			}
			break;
		}

		case 1:
		case 9:
		{
			mm[0] = 6;
			if (y < 0)
			{
				mm[1] = 3;
				mm[2] = 9;
				mm[3] = 2;
				mm[4] = 8;
			}
			else
			{
				mm[1] = 9;
				mm[2] = 3;
				mm[3] = 8;
				mm[4] = 2;
			}
			break;
		}

		case 2:
		case 6:
		{
			mm[0] = 8;
			if (x < 0)
			{
				mm[1] = 9;
				mm[2] = 7;
				mm[3] = 6;
				mm[4] = 4;
			}
			else
			{
				mm[1] = 7;
				mm[2] = 9;
				mm[3] = 4;
				mm[4] = 6;
			}
			break;
		}

		case 4:
		{
			mm[0] = 7;
			if (ay > ax)
			{
				mm[1] = 8;
				mm[2] = 4;
				mm[3] = 9;
				mm[4] = 1;
			}
			else
			{
				mm[1] = 4;
				mm[2] = 8;
				mm[3] = 1;
				mm[4] = 9;
			}
			break;
		}

		case 5:
		case 13:
		{
			mm[0] = 4;
			if (y < 0)
			{
				mm[1] = 1;
				mm[2] = 7;
				mm[3] = 2;
				mm[4] = 8;
			}
			else
			{
				mm[1] = 7;
				mm[2] = 1;
				mm[3] = 8;
				mm[4] = 2;
			}
			break;
		}

		case 8:
		{
			mm[0] = 3;
			if (ay > ax)
			{
				mm[1] = 2;
				mm[2] = 6;
				mm[3] = 1;
				mm[4] = 9;
			}
			else
			{
				mm[1] = 6;
				mm[2] = 2;
				mm[3] = 9;
				mm[4] = 1;
			}
			break;
		}

		case 10:
		case 14:
		{
			mm[0] = 2;
			if (x < 0)
			{
				mm[1] = 3;
				mm[2] = 1;
				mm[3] = 6;
				mm[4] = 4;
			}
			else
			{
				mm[1] = 1;
				mm[2] = 3;
				mm[3] = 4;
				mm[4] = 6;
			}
			break;
		}

		default: /* case 12: */
		{
			mm[0] = 1;
			if (ay > ax)
			{
				mm[1] = 2;
				mm[2] = 4;
				mm[3] = 3;
				mm[4] = 7;
			}
			else
			{
				mm[1] = 4;
				mm[2] = 2;
				mm[3] = 7;
				mm[4] = 3;
			}
			break;
		}
	}

	/* Want to move */
	return (TRUE);
}



/*
 * Hack -- compare the "strength" of two monsters XXX XXX XXX
 */
static int compare_monsters(const struct monster *m_ptr, const struct monster *n_ptr)
{
	u32b mexp1 = m_ptr->race->mexp;
	u32b mexp2 = n_ptr->race->mexp;

	/* Compare */
	if (mexp1 < mexp2) return (-1);
	if (mexp1 > mexp2) return (1);

	/* Assume equal */
	return (0);
}

/*
 * Critical blow.  All hits that do 95% of total possible damage,
 * and which also do at least 20 damage, or, sometimes, N damage.
 * This is used only to determine "cuts" and "stuns".
 */
static int monster_critical(int dice, int sides, int dam)
{
	int max = 0;
	int total = dice * sides;

	/* Must do at least 95% of perfect */
	if (dam < total * 19 / 20) return (0);

	/* Weak blows rarely work */
	if ((dam < 20) && (randint0(100) >= dam)) return (0);

	/* Perfect damage */
	if (dam == total) max++;

	/* Super-charge */
	if (dam >= 20)
	{
		while (randint0(100) < 2) max++;
	}

	/* Critical damage */
	if (dam > 45) return (6 + max);
	if (dam > 33) return (5 + max);
	if (dam > 25) return (4 + max);
	if (dam > 18) return (3 + max);
	if (dam > 11) return (2 + max);
	return (1 + max);
}

/*
 * Determine if a monster attack against the player succeeds.
 */
bool check_hit(struct player *p, int power, int level)
{
	int chance, ac;

	/* Calculate the "attack quality" */
	chance = (power + (level * 3));

	/* Total armor */
	ac = p->state.ac + p->state.to_a;

	/* if the monster checks vs ac, the player learns ac bonuses */
	/* XXX Eddie should you only learn +ac on miss, -ac on hit?  who knows */
	object_notice_on_defend(p);

	/* Check if the player was hit */
	return test_hit(chance, ac, TRUE);
}


#define MAX_DESC_INSULT 8

/*
 * Hack -- possible "insult" messages
 */
static const char *desc_insult[MAX_DESC_INSULT] =
{
	"insults you!",
	"insults your mother!",
	"gives you the finger!",
	"humiliates you!",
	"defiles you!",
	"dances around you!",
	"makes obscene gestures!",
	"moons you!!!"
};


#define MAX_DESC_MOAN 8


/*
 * Hack -- possible "insult" messages
 */
static const char *desc_moan[MAX_DESC_MOAN] =
{
	"wants his mushrooms back.",
	"tells you to get off his land.",
	"looks for his dogs. ",
	"says 'Did you kill my Fang?' ",
	"asks 'Do you want to buy any mushrooms?' ",
	"seems sad about something.",
	"asks if you have seen his dogs.",
	"mumbles something about mushrooms."
};

/*
 * Calculate how much damage remains after armor is taken into account
 * (does for a physical attack what adjust_dam does for an elemental attack).
 */
static int adjust_dam_armor(int damage, int ac)
{
	return damage - (damage * ((ac < 240) ? ac : 240) / 400);
}

/**
 * Storage for context information for effect handlers called in make_attack_normal().
 *
 * The members of this struct are initialized in an order-dependent way (to be more cross-
 * platform). If the members change, make sure to change any initializers. Ideally, this
 * should eventually used named initializers.
 */
typedef struct melee_effect_handler_context_s {
	struct player * const p;
	struct monster * const m_ptr;
	const int rlev;
	const int method;
	const int ac;
	const char *ddesc;
	bool obvious;
	bool blinked;
	bool do_break;
	int damage;
} melee_effect_handler_context_t;

/**
 * Melee blow effect handler.
 */
typedef void (*melee_effect_handler_f)(melee_effect_handler_context_t *);

static bool monster_blow_method_physical(int method);

/**
 * Do damage as the result of a melee attack that has an elemental aspect.
 *
 * \param context is information for the current attack.
 * \param type is the GF_ constant for the element.
 * \param pure_element should be TRUE if no side effects (mostly a hack for poison).
 */
static void melee_effect_elemental(melee_effect_handler_context_t *context, int type, bool pure_element)
{
	int physical_dam, elemental_dam;

	if (pure_element) {
		/* Obvious */
		context->obvious = TRUE;
	}

	switch (type) {
		case GF_ACID: msg("You are covered in acid!");
			break;
		case GF_ELEC: msg("You are struck by electricity!");
			break;
		case GF_FIRE: msg("You are enveloped in flames!");
			break;
		case GF_COLD: msg("You are covered with frost!");
			break;
	}

	/* Give the player a small bonus to ac for elemental attacks */
	physical_dam = adjust_dam_armor(context->damage, context->ac + 50);

	/* Some attacks do no physical damage */
	if (!monster_blow_method_physical(context->method))
		physical_dam = 0;

	elemental_dam = adjust_dam(context->p, type, context->damage, RANDOMISE,
							   check_for_resist(context->p, type, context->p->state.flags, TRUE));

	/* Take the larger of physical or elemental damage */
	context->damage = (physical_dam > elemental_dam) ? physical_dam : elemental_dam;

	if (context->damage > 0) take_hit(context->p, context->damage, context->ddesc);
	if (elemental_dam > 0) inven_damage(context->p, type, MIN(elemental_dam * 5, 300));

	if (pure_element) {
		/* Learn about the player */
		monster_learn_resists(context->m_ptr, context->p, type);
	}
}

/**
 * Do damage as the result of a melee attack that has a status effect.
 *
 * \param context is the information for the current attack.
 * \param type is the TMD_ constant for the effect.
 * \param amount is the amount that the timer should be increased by.
 * \param of_flag is the OF_ flag that is passed on to monster learning for this effect.
 * \param attempt_save indicates if a saving throw should be attempted for this effect.
 * \param save_msg is the message that is displayed if the saving throw is successful.
 */
static void melee_effect_timed(melee_effect_handler_context_t *context, int type, int amount, int of_flag, bool attempt_save, const char *save_msg)
{
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);

	/* Perform a saving throw if desired. */
	if (attempt_save && randint0(100) < context->p->state.skills[SKILL_SAVE]) {
		if (save_msg != NULL)
			msg(save_msg);

		context->obvious = TRUE;
	}
	else {
		/* Increase timer for type. */
		if (player_inc_timed(context->p, type, amount, TRUE, TRUE))
			context->obvious = TRUE;
	}

	/* Learn about the player */
	update_smart_learn(context->m_ptr, context->p, of_flag);
}

/**
 * Do damage as the result of a melee attack that drains a stat.
 *
 * \param context is the information for the current attack.
 * \param stat is the A_ constant for the desired stat.
 */
static void melee_effect_stat(melee_effect_handler_context_t *context, int stat)
{
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);

	/* Damage (stat) */
	if (do_dec_stat(stat, FALSE)) context->obvious = TRUE;
}

/**
 * Do damage as the result of an experience draining melee attack.
 *
 * \param context is the information for the current attack.
 * \param chance is the player's chance of resisting drain if they have OF_HOLD_LIFE.
 * \param drain_amount is the base amount of experience to drain.
 */
static void melee_effect_experience(melee_effect_handler_context_t *context, int chance, int drain_amount)
{
	/* Obvious */
	context->obvious = TRUE;

	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
	update_smart_learn(context->m_ptr, context->p, OF_HOLD_LIFE);

	if (check_state(context->p, OF_HOLD_LIFE, context->p->state.flags) && (randint0(100) < chance)) {
		msg("You keep hold of your life force!");
	}
	else {
		s32b d = drain_amount + (context->p->exp/100) * MON_DRAIN_LIFE;
		if (check_state(context->p, OF_HOLD_LIFE, context->p->state.flags)) {
			msg("You feel your life slipping away!");
			player_exp_lose(context->p, d / 10, FALSE);
		}
		else {
			msg("You feel your life draining away!");
			player_exp_lose(context->p, d, FALSE);
		}
	}
}

/**
 * Melee effect handler: Hit the player, but don't do any damage.
 */
static void melee_effect_handler_none(melee_effect_handler_context_t *context)
{
	/* Hack -- Assume obvious */
	context->obvious = TRUE;

	/* Hack -- No damage */
	context->damage = 0;
}

/**
 * Melee effect handler: Hurt the player with no side effects.
 */
static void melee_effect_handler_hurt(melee_effect_handler_context_t *context)
{
	/* Obvious */
	context->obvious = TRUE;

	/* Hack -- Player armor reduces total damage */
	context->damage = adjust_dam_armor(context->damage, context->ac);

	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
}

/**
 * Melee effect handler: Poison the player.
 *
 * We can't use melee_effect_timed(), because this is both and elemental attack and a 
 * status attack. Note the FALSE value for pure_element for melee_effect_elemental().
 */
static void melee_effect_handler_poison(melee_effect_handler_context_t *context)
{
	melee_effect_elemental(context, GF_POIS, FALSE);

	/* Take "poison" effect */
	if (player_inc_timed(context->p, TMD_POISONED, 5 + randint1(context->rlev), TRUE, TRUE))
		context->obvious = TRUE;

	/* Learn about the player */
	monster_learn_resists(context->m_ptr, context->p, GF_POIS);
}

/**
 * Melee effect handler: Disenchant the player.
 */
static void melee_effect_handler_disenchant(melee_effect_handler_context_t *context)
{
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
	
	/* Allow complete resist */
	if (!check_state(context->p, OF_RES_DISEN, context->p->state.flags))
	{
		/* Apply disenchantment */
		if (apply_disenchant(0)) context->obvious = TRUE;
	}
	
	/* Learn about the player */
	monster_learn_resists(context->m_ptr, context->p, GF_DISEN);
}

/**
 * Melee effect handler: Drain charges from the player's inventory.
 */
static void melee_effect_handler_drain_charges(melee_effect_handler_context_t *context)
{
	object_type *o_ptr;
	struct monster *monster = context->m_ptr;
	struct player *player = context->p;
	int item, tries;
	int unpower = 0, newcharge;

	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);

	/* Find an item */
	for (tries = 0; tries < 10; tries++)
	{
		/* Pick an item */
		item = randint0(INVEN_PACK);

		/* Obtain the item */
		o_ptr = &player->inventory[item];

		/* Skip non-objects */
		if (!o_ptr->kind) continue;

		/* Drain charged wands/staves */
		if ((o_ptr->tval == TV_STAFF) ||
			(o_ptr->tval == TV_WAND))
		{
			/* Charged? */
			if (o_ptr->pval[DEFAULT_PVAL])
			{
				/* Get number of charge to drain */
				unpower = (context->rlev / (o_ptr->kind->level + 2)) + 1;

				/* Get new charge value, don't allow negative */
				newcharge = MAX((o_ptr->pval[DEFAULT_PVAL] - unpower),0);

				/* Remove the charges */
				o_ptr->pval[DEFAULT_PVAL] = newcharge;
			}
		}

		if (unpower)
		{
			int heal = context->rlev * unpower;

			msg("Energy drains from your pack!");

			context->obvious = TRUE;

			/* Don't heal more than max hp */
			heal = MIN(heal, monster->maxhp - monster->hp);

			/* Heal */
			monster->hp += heal;

			/* Redraw (later) if needed */
			if (player->health_who == monster)
				player->redraw |= (PR_HEALTH);

			/* Combine / Reorder the pack */
			player->notice |= (PN_COMBINE | PN_REORDER);

			/* Redraw stuff */
			player->redraw |= (PR_INVEN);

			/* Affect only a single inventory slot */
			break;
		}
	}
}

/**
 * Melee effect handler: Take the player's gold.
 */
static void melee_effect_handler_eat_gold(melee_effect_handler_context_t *context)
{
	struct player *player = context->p;

    /* Take damage */
    take_hit(context->p, context->damage, context->ddesc);

    /* Obvious */
    context->obvious = TRUE;

    /* Saving throw (unless paralyzed) based on dex and level */
    if (!player->timed[TMD_PARALYZED] &&
        (randint0(100) < (adj_dex_safe[player->state.stat_ind[A_DEX]] + player->lev)))
    {
        /* Saving throw message */
        msg("You quickly protect your money pouch!");

        /* Occasional blink anyway */
        if (randint0(3)) context->blinked = TRUE;
    }

    /* Eat gold */
    else {
        s32b gold = (player->au / 10) + randint1(25);
        if (gold < 2) gold = 2;
        if (gold > 5000) gold = (player->au / 20) + randint1(3000);
        if (gold > player->au) gold = player->au;
        player->au -= gold;
        if (gold <= 0) {
            msg("Nothing was stolen.");
            return;
        }
        /* Let the player know they were robbed */
        msg("Your purse feels lighter.");
        if (player->au)
            msg("%ld coins were stolen!", (long)gold);
        else
            msg("All of your coins were stolen!");

        /* While we have gold, put it in objects */
        while (gold > 0) {
            int amt;

            /* Create a new temporary object */
            object_type o;
            object_wipe(&o);
            object_prep(&o, objkind_get(TV_GOLD, SV_GOLD), 0, MINIMISE);

            /* Amount of gold to put in this object */
            amt = gold > MAX_PVAL ? MAX_PVAL : gold;
            o.pval[DEFAULT_PVAL] = amt;
            gold -= amt;

            /* Set origin to stolen, so it is not confused with
             * dropped treasure in monster_death */
            o.origin = ORIGIN_STOLEN;

            /* Give the gold to the monster */
            monster_carry(context->m_ptr, &o);
        }
        
        /* Redraw gold */
        player->redraw |= (PR_GOLD);
        
        /* Blink away */
        context->blinked = TRUE;
    }
}

/**
 * Melee effect handler: Take something from the player's inventory.
 */
static void melee_effect_handler_eat_item(melee_effect_handler_context_t *context)
{
	int item, tries;

    /* Take damage */
    take_hit(context->p, context->damage, context->ddesc);

    /* Saving throw (unless paralyzed) based on dex and level */
    if (!context->p->timed[TMD_PARALYZED] &&
        (randint0(100) < (adj_dex_safe[context->p->state.stat_ind[A_DEX]] +
                          context->p->lev)))
    {
        /* Saving throw message */
        msg("You grab hold of your backpack!");

        /* Occasional "blink" anyway */
        context->blinked = TRUE;

        /* Obvious */
        context->obvious = TRUE;

        /* Done */
        return;
    }

    /* Find an item */
    for (tries = 0; tries < 10; tries++)
    {
		object_type *o_ptr, *i_ptr;
		char o_name[80];
        object_type object_type_body;

        /* Pick an item */
        item = randint0(INVEN_PACK);

        /* Obtain the item */
        o_ptr = &context->p->inventory[item];

        /* Skip non-objects */
        if (!o_ptr->kind) continue;

        /* Skip artifacts */
        if (o_ptr->artifact) continue;

        /* Get a description */
        object_desc(o_name, sizeof(o_name), o_ptr, ODESC_FULL);

        /* Message */
        msg("%s %s (%c) was stolen!",
            ((o_ptr->number > 1) ? "One of your" : "Your"),
            o_name, index_to_label(item));

        /* Get local object */
        i_ptr = &object_type_body;

        /* Obtain local object */
        object_copy(i_ptr, o_ptr);

        /* Modify number */
        i_ptr->number = 1;

        /* Hack -- If a rod, staff, or wand, allocate total
         * maximum timeouts or charges between those
         * stolen and those missed. -LM-
         */
        distribute_charges(o_ptr, i_ptr, 1);

        /* Carry the object */
        (void)monster_carry(context->m_ptr, i_ptr);

        /* Steal the items */
        inven_item_increase(item, -1);
        inven_item_optimize(item);
        
        /* Obvious */
        context->obvious = TRUE;
        
        /* Blink away */
        context->blinked = TRUE;
        
        /* Done */
        break;
    }
}

/**
 * Melee effect handler: Eat the player's food.
 */
static void melee_effect_handler_eat_food(melee_effect_handler_context_t *context)
{
	/* Steal some food */
	int item, tries;
	object_type *o_ptr;
	char o_name[80];

	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);

	for (tries = 0; tries < 10; tries++) {
		/* Pick an item from the pack */
		item = randint0(INVEN_PACK);
		
		/* Get the item */
		o_ptr = &context->p->inventory[item];
		
		/* Skip non-objects */
		if (!o_ptr->kind) continue;
		
		/* Skip non-food objects */
		if (o_ptr->tval != TV_FOOD) continue;
		
		if (o_ptr->number == 1) {
			object_desc(o_name, sizeof(o_name), o_ptr, ODESC_BASE);
			msg("Your %s (%c) was eaten!", o_name, index_to_label(item));
		} else {
			object_desc(o_name, sizeof(o_name), o_ptr, ODESC_PREFIX | ODESC_BASE);
			msg("One of your %s (%c) was eaten!", o_name, index_to_label(item));
		}
		
		/* Steal the items */
		inven_item_increase(item, -1);
		inven_item_optimize(item);
		
		/* Obvious */
		context->obvious = TRUE;
		
		/* Done */
		break;
	}
}

/**
 * Melee effect handler: Absorb the player's light.
 */
static void melee_effect_handler_eat_light(melee_effect_handler_context_t *context)
{
	object_type *o_ptr;
	bitflag f[OF_SIZE];

	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);

	/* Get the light, and its flags */
	o_ptr = &context->p->inventory[INVEN_LIGHT];
	object_flags(o_ptr, f);

	/* Drain fuel where applicable */
	if (!of_has(f, OF_NO_FUEL) && (o_ptr->timeout > 0)) {
		/* Reduce fuel */
		o_ptr->timeout -= (250 + randint1(250));
		if (o_ptr->timeout < 1) o_ptr->timeout = 1;

		/* Notice */
		if (!context->p->timed[TMD_BLIND]) {
			msg("Your light dims.");
			context->obvious = TRUE;
		}

		/* Redraw stuff */
		context->p->redraw |= (PR_EQUIP);
	}
}

/**
 * Melee effect handler: Attack the player with acid.
 */
static void melee_effect_handler_acid(melee_effect_handler_context_t *context)
{
	melee_effect_elemental(context, GF_ACID, TRUE);
}

/**
 * Melee effect handler: Attack the player with electricity.
 */
static void melee_effect_handler_elec(melee_effect_handler_context_t *context)
{
	melee_effect_elemental(context, GF_ELEC, TRUE);
}

/**
 * Melee effect handler: Attack the player with fire.
 */
static void melee_effect_handler_fire(melee_effect_handler_context_t *context)
{
	melee_effect_elemental(context, GF_FIRE, TRUE);
}

/**
 * Melee effect handler: Attack the player with cold.
 */
static void melee_effect_handler_cold(melee_effect_handler_context_t *context)
{
	melee_effect_elemental(context, GF_COLD, TRUE);
}

/**
 * Melee effect handler: Blind the player.
 */
static void melee_effect_handler_blind(melee_effect_handler_context_t *context)
{
	melee_effect_timed(context, TMD_BLIND, 10 + randint1(context->rlev), OF_RES_BLIND, FALSE, NULL);
}

/**
 * Melee effect handler: Confuse the player.
 */
static void melee_effect_handler_confuse(melee_effect_handler_context_t *context)
{
	melee_effect_timed(context, TMD_CONFUSED, 3 + randint1(context->rlev), OF_RES_CONFU, FALSE, NULL);
}

/**
 * Melee effect handler: Terrify the player.
 */
static void melee_effect_handler_terrify(melee_effect_handler_context_t *context)
{
	melee_effect_timed(context, TMD_AFRAID, 3 + randint1(context->rlev), OF_RES_FEAR, TRUE, "You stand your ground!");
}

/**
 * Melee effect handler: Paralyze the player.
 */
static void melee_effect_handler_paralyze(melee_effect_handler_context_t *context)
{
	/* Hack -- Prevent perma-paralysis via damage */
	if (context->p->timed[TMD_PARALYZED] && (context->damage < 1)) context->damage = 1;

	melee_effect_timed(context, TMD_PARALYZED, 3 + randint1(context->rlev), OF_FREE_ACT, TRUE, "You resist the effects!");
}

/**
 * Melee effect handler: Drain the player's strength.
 */
static void melee_effect_handler_lose_str(melee_effect_handler_context_t *context)
{
	melee_effect_stat(context, A_STR);
}

/**
 * Melee effect handler: Drain the player's intelligence.
 */
static void melee_effect_handler_lose_int(melee_effect_handler_context_t *context)
{
	melee_effect_stat(context, A_INT);
}

/**
 * Melee effect handler: Drain the player's wisdom.
 */
static void melee_effect_handler_lose_wis(melee_effect_handler_context_t *context)
{
	melee_effect_stat(context, A_WIS);
}

/**
 * Melee effect handler: Drain the player's dexterity.
 */
static void melee_effect_handler_lose_dex(melee_effect_handler_context_t *context)
{
	melee_effect_stat(context, A_DEX);
}

/**
 * Melee effect handler: Drain the player's constitution.
 */
static void melee_effect_handler_lose_con(melee_effect_handler_context_t *context)
{
	melee_effect_stat(context, A_CON);
}

/**
 * Melee effect handler: Drain all of the player's stats.
 */
static void melee_effect_handler_lose_all(melee_effect_handler_context_t *context)
{
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
	
	/* Damage (stats) */
	if (do_dec_stat(A_STR, FALSE)) context->obvious = TRUE;
	if (do_dec_stat(A_DEX, FALSE)) context->obvious = TRUE;
	if (do_dec_stat(A_CON, FALSE)) context->obvious = TRUE;
	if (do_dec_stat(A_INT, FALSE)) context->obvious = TRUE;
	if (do_dec_stat(A_WIS, FALSE)) context->obvious = TRUE;
}

/**
 * Melee effect handler: Cause an earthquake around the player.
 */
static void melee_effect_handler_shatter(melee_effect_handler_context_t *context)
{
	/* Obvious */
	context->obvious = TRUE;
	
	/* Hack -- Reduce damage based on the player armor class */
	context->damage = adjust_dam_armor(context->damage, context->ac);
	
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
	
	/* Radius 8 earthquake centered at the monster */
	if (context->damage > 23) {
		int px_old = context->p->px;
		int py_old = context->p->py;
		
		earthquake(context->m_ptr->fy, context->m_ptr->fx, 8);
		
		/* Stop the blows if the player is pushed away */
		if ((px_old != context->p->px) ||
			(py_old != context->p->py))
			context->do_break = TRUE;
	}
}

/**
 * Melee effect handler: Drain the player's experience.
 */
static void melee_effect_handler_exp_10(melee_effect_handler_context_t *context)
{
	melee_effect_experience(context, 95, damroll(10, 6));
}

/**
 * Melee effect handler: Drain the player's experience.
 */
static void melee_effect_handler_exp_20(melee_effect_handler_context_t *context)
{
	melee_effect_experience(context, 90, damroll(20, 6));
}

/**
 * Melee effect handler: Drain the player's experience.
 */
static void melee_effect_handler_exp_40(melee_effect_handler_context_t *context)
{
	melee_effect_experience(context, 75, damroll(40, 6));
}

/**
 * Melee effect handler: Drain the player's experience.
 */
static void melee_effect_handler_exp_80(melee_effect_handler_context_t *context)
{
	melee_effect_experience(context, 50, damroll(80, 6));
}

/**
 * Melee effect handler: Make the player hallucinate.
 *
 * Note that we don't use melee_effect_timed(), due to the different monster
 * learning function.
 */
static void melee_effect_handler_hallucination(melee_effect_handler_context_t *context)
{
	/* Take damage */
	take_hit(context->p, context->damage, context->ddesc);
	
	/* Increase "image" */
	if (player_inc_timed(context->p, TMD_IMAGE, 3 + randint1(context->rlev / 2), TRUE, TRUE))
		context->obvious = TRUE;
	
	/* Learn about the player */
	monster_learn_resists(context->m_ptr, context->p, GF_CHAOS);
}

/**
 * Return a handler for the given effect.
 *
 * Handlers are associated in a table within the function.
 *
 * \param effect is the RBE_ constant for the effect.
 * \returns a function pointer to handle the effect, or NULL if not found.
 */
melee_effect_handler_f melee_handler_for_blow_effect(int effect)
{
	/* Effect handler table for valid effects. Terminator is {RBE_MAX, NULL}. */
	static const struct blow_handler_s {
		int effect;
		melee_effect_handler_f function;
	} blow_handlers[] = {
		{ RBE_NONE, melee_effect_handler_none },
		{ RBE_HURT, melee_effect_handler_hurt },
		{ RBE_POISON, melee_effect_handler_poison },
		{ RBE_UN_BONUS, melee_effect_handler_disenchant },
		{ RBE_UN_POWER, melee_effect_handler_drain_charges },
		{ RBE_EAT_GOLD, melee_effect_handler_eat_gold },
		{ RBE_EAT_ITEM, melee_effect_handler_eat_item },
		{ RBE_EAT_FOOD, melee_effect_handler_eat_food },
		{ RBE_EAT_LIGHT, melee_effect_handler_eat_light },
		{ RBE_ACID, melee_effect_handler_acid },
		{ RBE_ELEC, melee_effect_handler_elec },
		{ RBE_FIRE, melee_effect_handler_fire },
		{ RBE_COLD, melee_effect_handler_cold },
		{ RBE_BLIND, melee_effect_handler_blind },
		{ RBE_CONFUSE, melee_effect_handler_confuse },
		{ RBE_TERRIFY, melee_effect_handler_terrify },
		{ RBE_PARALYZE, melee_effect_handler_paralyze },
		{ RBE_LOSE_STR, melee_effect_handler_lose_str },
		{ RBE_LOSE_INT, melee_effect_handler_lose_int },
		{ RBE_LOSE_WIS, melee_effect_handler_lose_wis },
		{ RBE_LOSE_DEX, melee_effect_handler_lose_dex },
		{ RBE_LOSE_CON, melee_effect_handler_lose_con },
		{ RBE_LOSE_ALL, melee_effect_handler_lose_all },
		{ RBE_SHATTER, melee_effect_handler_shatter },
		{ RBE_EXP_10, melee_effect_handler_exp_10 },
		{ RBE_EXP_20, melee_effect_handler_exp_20 },
		{ RBE_EXP_40, melee_effect_handler_exp_40 },
		{ RBE_EXP_80, melee_effect_handler_exp_80 },
		{ RBE_HALLU, melee_effect_handler_hallucination },
		{ RBE_MAX, NULL },
	};
	const struct blow_handler_s *current = blow_handlers;

	if (effect < RBE_NONE || effect >= RBE_MAX)
		return NULL;

	while (current->effect != RBE_MAX && current->function != NULL) {
		if (current->effect == effect)
			return current->function;

		current++;
	}

	return NULL;
}


static int monster_blow_effect_power(int effect)
{
	static const int effect_powers[] = {
		#define RBE(x, p, e, d) p,
		#include "list-blow-effects.h"
		#undef RBE
	};

	if (effect < RBE_NONE || effect >= RBE_MAX)
		return 0;

	return effect_powers[effect];
}

static bool monster_blow_method_cut(int method)
{
	static const bool blow_cuts[] = {
		#define RBM(x, c, s, miss, p, m, a, d) c,
		#include "list-blow-methods.h"
		#undef RBM
	};

	if (method < RBM_NONE || method >= RBM_MAX)
		return FALSE;

	return blow_cuts[method];
}

static bool monster_blow_method_stun(int method)
{
	static const bool blow_stuns[] = {
		#define RBM(x, c, s, miss, p, m, a, d) s,
		#include "list-blow-methods.h"
		#undef RBM
	};

	if (method < RBM_NONE || method >= RBM_MAX)
		return FALSE;

	return blow_stuns[method];
}

static int monster_blow_method_message(int method)
{
	static const int blow_messages[] = {
		#define RBM(x, c, s, miss, p, m, a, d) m,
		#include "list-blow-methods.h"
		#undef RBM
	};

	if (method < RBM_NONE || method >= RBM_MAX)
		return MSG_GENERIC;

	return blow_messages[method];
}

static const char *monster_blow_method_action(int method)
{
	static const char *blow_actions[] = {
		#define RBM(x, c, s, miss, p, m, a, d) a,
		#include "list-blow-methods.h"
		#undef RBM
	};
	const char *action = NULL;

	if (method < RBM_NONE || method >= RBM_MAX)
		return NULL;

	action = blow_actions[method];

	if (method == RBM_INSULT && action == NULL) {
		action = desc_insult[randint0(MAX_DESC_INSULT)];
	}
	else if (method == RBM_MOAN && action == NULL) {
		action = desc_moan[randint0(MAX_DESC_MOAN)];
	}

	return action;
}

static bool monster_blow_method_miss(int method)
{
	static const bool blow_misses[] = {
		#define RBM(x, c, s, miss, p, m, a, d) miss,
		#include "list-blow-methods.h"
		#undef RBM
	};

	if (method < RBM_NONE || method >= RBM_MAX)
		return FALSE;

	return blow_misses[method];
}

static bool monster_blow_method_physical(int method)
{
	static const bool blow_physicals[] = {
		#define RBM(x, c, s, miss, p, m, a, d) p,
		#include "list-blow-methods.h"
		#undef RBM
	};

	if (method < RBM_NONE || method >= RBM_MAX)
		return FALSE;

	return blow_physicals[method];
}



/*
 * Attack the player via physical attacks.
 */
static bool make_attack_normal(struct monster *m_ptr, struct player *p)
{
	monster_lore *l_ptr = get_lore(m_ptr->race);
	int ap_cnt;
	int k, tmp, ac, rlev;
	char m_name[80];
	char ddesc[80];
	bool blinked;

	/* Not allowed to attack */
	if (rf_has(m_ptr->race->flags, RF_NEVER_BLOW)) return (FALSE);

	/* Total armor */
	ac = p->state.ac + p->state.to_a;

	/* Extract the effective monster level */
	rlev = ((m_ptr->race->level >= 1) ? m_ptr->race->level : 1);


	/* Get the monster name (or "it") */
	monster_desc(m_name, sizeof(m_name), m_ptr, MDESC_STANDARD);

	/* Get the "died from" information (i.e. "a kobold") */
	monster_desc(ddesc, sizeof(ddesc), m_ptr, MDESC_SHOW | MDESC_IND_VIS);

	/* Assume no blink */
	blinked = FALSE;

	/* Scan through all blows */
	for (ap_cnt = 0; ap_cnt < MONSTER_BLOW_MAX; ap_cnt++)
	{
		bool visible = FALSE;
		bool obvious = FALSE;
		bool do_break = FALSE;

		int power = 0;
		int damage = 0;
		int do_cut = 0;
		int do_stun = 0;
		int sound_msg = MSG_GENERIC;

		const char *act = NULL;

		/* Extract the attack infomation */
		int effect = m_ptr->race->blow[ap_cnt].effect;
		int method = m_ptr->race->blow[ap_cnt].method;
		int d_dice = m_ptr->race->blow[ap_cnt].d_dice;
		int d_side = m_ptr->race->blow[ap_cnt].d_side;

		/* Hack -- no more attacks */
		if (!method) break;

		/* Handle "leaving" */
		if (p->leaving) break;

		/* Extract visibility (before blink) */
		if (m_ptr->ml) visible = TRUE;

		/* Extract visibility from carrying light */
		if (rf_has(m_ptr->race->flags, RF_HAS_LIGHT)) visible = TRUE;

		/* Extract the attack "power" */
		power = monster_blow_effect_power(effect);

		/* Monster hits player */
		if (!effect || check_hit(p, power, rlev)) {
			/* Always disturbing */
			disturb(p, 1, 0);

			/* Hack -- Apply "protection from evil" */
			if (p->timed[TMD_PROTEVIL] > 0)
			{
				/* Learn about the evil flag */
				if (m_ptr->ml)
				{
					rf_on(l_ptr->flags, RF_EVIL);
				}

				if (rf_has(m_ptr->race->flags, RF_EVIL) &&
				    p->lev >= rlev &&
				    randint0(100) + p->lev > 50)
				{
					/* Message */
					msg("%s is repelled.", m_name);

					/* Hack -- Next attack */
					continue;
				}
			}

			/* Describe the attack method */
			act = monster_blow_method_action(method);
			do_cut = monster_blow_method_cut(method);
			do_stun = monster_blow_method_stun(method);
			sound_msg = monster_blow_method_message(method);

			/* Message */
			if (act)
				msgt(sound_msg, "%s %s", m_name, act);

			/* Hack -- assume all attacks are obvious */
			obvious = TRUE;

			/* Roll out the damage */
			if (d_dice > 0 && d_side > 0)
				damage = damroll(d_dice, d_side);
			else
				damage = 0;

			/* Initialize this way so that some values can be const. */
			melee_effect_handler_context_t context = {
				p,
				m_ptr,
				rlev,
				method,
				ac,
				ddesc,
				obvious,
				blinked,
				do_break,
				damage,
			};

			/* Perform the actual effect. */
			melee_effect_handler_f effect_handler = melee_handler_for_blow_effect(effect);

			if (effect_handler != NULL)
				effect_handler(&context);
			else
				bell(format("Effect handler not found for %d.", effect));

			/* Save any changes made in the handler for later use. */
			obvious = context.obvious;
			blinked = context.blinked;
			damage = context.damage;


			/* Hack -- only one of cut or stun */
			if (do_cut && do_stun)
			{
				/* Cancel cut */				if (randint0(100) < 50)
				{
					do_cut = 0;
				}

				/* Cancel stun */
				else
				{
					do_stun = 0;
				}
			}

			/* Handle cut */
			if (do_cut)
			{
				/* Critical hit (zero if non-critical) */
				tmp = monster_critical(d_dice, d_side, damage);

				/* Roll for damage */
				switch (tmp)
				{
					case 0: k = 0; break;
					case 1: k = randint1(5); break;
					case 2: k = randint1(5) + 5; break;
					case 3: k = randint1(20) + 20; break;
					case 4: k = randint1(50) + 50; break;
					case 5: k = randint1(100) + 100; break;
					case 6: k = 300; break;
					default: k = 500; break;
				}

				/* Apply the cut */
				if (k) (void)player_inc_timed(p, TMD_CUT, k, TRUE, TRUE);
			}

			/* Handle stun */
			if (do_stun)
			{
				/* Critical hit (zero if non-critical) */
				tmp = monster_critical(d_dice, d_side, damage);

				/* Roll for damage */
				switch (tmp)
				{
					case 0: k = 0; break;
					case 1: k = randint1(5); break;
					case 2: k = randint1(10) + 10; break;
					case 3: k = randint1(20) + 20; break;
					case 4: k = randint1(30) + 30; break;
					case 5: k = randint1(40) + 40; break;
					case 6: k = 100; break;
					default: k = 200; break;
				}

				/* Apply the stun */
				if (k) (void)player_inc_timed(p, TMD_STUN, k, TRUE, TRUE);
			}
		}
		else {
			/* Visible monster missed player, so notify if appropriate. */
			if (m_ptr->ml && monster_blow_method_miss(method)) {
				/* Disturbing */
				disturb(p, 1, 0);
				msg("%s misses you.", m_name);
			}
		}


		/* Analyze "visible" monsters only */
		if (visible)
		{
			/* Count "obvious" attacks (and ones that cause damage) */
			if (obvious || damage || (l_ptr->blows[ap_cnt] > 10))
			{
				/* Count attacks of this type */
				if (l_ptr->blows[ap_cnt] < MAX_UCHAR)
				{
					l_ptr->blows[ap_cnt]++;
				}
			}
		}

		/* Skip the other blows if necessary */
		if (do_break) break;
	}


	/* Blink away */
	if (blinked)
	{
		msg("There is a puff of smoke!");
		teleport_away(m_ptr, MAX_SIGHT * 2 + 5);
	}


	/* Always notice cause of death */
	if (p->is_dead && (l_ptr->deaths < MAX_SHORT))
	{
		l_ptr->deaths++;
	}


	/* Assume we attacked */
	return (TRUE);
}

/*
 * Process a monster
 *
 * In several cases, we directly update the monster lore
 *
 * Note that a monster is only allowed to "reproduce" if there
 * are a limited number of "reproducing" monsters on the current
 * level.  This should prevent the level from being "swamped" by
 * reproducing monsters.  It also allows a large mass of mice to
 * prevent a louse from multiplying, but this is a small price to
 * pay for a simple multiplication method.
 *
 * XXX Monster fear is slightly odd, in particular, monsters will
 * fixate on opening a door even if they cannot open it.  Actually,
 * the same thing happens to normal monsters when they hit a door
 *
 * In addition, monsters which *cannot* open or bash down a door
 * will still stand there trying to open it...  XXX XXX XXX
 *
 * Technically, need to check for monster in the way combined
 * with that monster being in a wall (or door?) XXX
 */
static void process_monster(struct cave *c, struct monster *m_ptr)
{
	monster_lore *l_ptr = get_lore(m_ptr->race);

	int i, d, oy, ox, ny, nx;

	int mm[5];

	bool woke_up = FALSE;
	bool stagger;

	bool do_turn;
	bool do_move;
	bool do_view;

	char m_name[80];

	/* Get the monster name */
	monster_desc(m_name, sizeof(m_name), m_ptr, MDESC_CAPITAL | MDESC_IND_HID);

	/* Handle "sleep" */
	if (m_ptr->m_timed[MON_TMD_SLEEP]) {
		u32b notice;

		/* Aggravation */
		if (check_state(p_ptr, OF_AGGRAVATE, p_ptr->state.flags)) {
			/* Wake the monster and notify player */
			mon_clear_timed(m_ptr, MON_TMD_SLEEP, MON_TMD_FLG_NOTIFY, FALSE);

			/* Update the health bar */
			if (m_ptr->ml && !m_ptr->unaware) {
				
				/* Hack -- Update the health bar */
				if (p_ptr->health_who == m_ptr) p_ptr->redraw |= (PR_HEALTH);
			}

			/* Efficiency XXX XXX */
			return;
		}

		/* Anti-stealth */
		notice = randint0(1024);

		/* Hack -- See if monster "notices" player */
		if ((notice * notice * notice) <= p_ptr->state.noise) {
			d = 1;

			/* Wake up faster near the player */
			if (m_ptr->cdis < 50) d = (100 / m_ptr->cdis);

			/* Still asleep */
			if (m_ptr->m_timed[MON_TMD_SLEEP] > d) {
				/* Monster wakes up "a little bit" */
				mon_dec_timed(m_ptr, MON_TMD_SLEEP, d , MON_TMD_FLG_NOMESSAGE,
					FALSE);

				/* Notice the "not waking up" */
				if (m_ptr->ml && !m_ptr->unaware) {
					/* Hack -- Count the ignores */
					if (l_ptr->ignore < MAX_UCHAR)
						l_ptr->ignore++;
				}
			} else {
				/* Reset sleep counter */
				woke_up = mon_clear_timed(m_ptr, MON_TMD_SLEEP,
					MON_TMD_FLG_NOMESSAGE, FALSE);

				/* Notice the "waking up" */
				if (m_ptr->ml && !m_ptr->unaware) {
					/* Dump a message */
					msg("%s wakes up.", m_name);

					/* Hack -- Update the health bar */
					if (p_ptr->health_who == m_ptr) p_ptr->redraw |= (PR_HEALTH);

					/* Hack -- Count the wakings */
					if (l_ptr->wake < MAX_UCHAR)
						l_ptr->wake++;
				}
			}
		}

		/* Still sleeping */
		if (m_ptr->m_timed[MON_TMD_SLEEP]) return;
	}

	/* If the monster just woke up, then it doesn't act */
	if (woke_up) return;

	if (m_ptr->m_timed[MON_TMD_FAST])
		mon_dec_timed(m_ptr, MON_TMD_FAST, 1, 0, FALSE);

	if (m_ptr->m_timed[MON_TMD_SLOW])
		mon_dec_timed(m_ptr, MON_TMD_SLOW, 1, 0, FALSE);

	if (m_ptr->m_timed[MON_TMD_STUN]) {
		d = 1;

		/* Make a "saving throw" against stun */
		if (randint0(5000) <= m_ptr->race->level * m_ptr->race->level)
			/* Recover fully */
			d = m_ptr->m_timed[MON_TMD_STUN];

		/* Hack -- Recover from stun */
		if (m_ptr->m_timed[MON_TMD_STUN] > d)
			mon_dec_timed(m_ptr, MON_TMD_STUN, 1, MON_TMD_FLG_NOMESSAGE, FALSE);
		else
			mon_clear_timed(m_ptr, MON_TMD_STUN, MON_TMD_FLG_NOTIFY, FALSE);

		/* Still stunned */
		if (m_ptr->m_timed[MON_TMD_STUN]) return;
	}

	if (m_ptr->m_timed[MON_TMD_CONF]) {
		d = randint1(m_ptr->race->level / 10 + 1);

		/* Still confused */
		if (m_ptr->m_timed[MON_TMD_CONF] > d)
			mon_dec_timed(m_ptr, MON_TMD_CONF, d , MON_TMD_FLG_NOMESSAGE,
				FALSE);
		else
			mon_clear_timed(m_ptr, MON_TMD_CONF, MON_TMD_FLG_NOTIFY, FALSE);
	}

	if (m_ptr->m_timed[MON_TMD_FEAR]) {
		/* Amount of "boldness" */
		d = randint1(m_ptr->race->level / 10 + 1);

		if (m_ptr->m_timed[MON_TMD_FEAR] > d)
			mon_dec_timed(m_ptr, MON_TMD_FEAR, d, MON_TMD_FLG_NOMESSAGE, FALSE);
		else
			mon_clear_timed(m_ptr, MON_TMD_FEAR, MON_TMD_FLG_NOTIFY, FALSE);
	}


	/* Get the origin */
	oy = m_ptr->fy;
	ox = m_ptr->fx;


	/* Attempt to "mutiply" (all monsters are allowed an attempt for lore
	 * purposes, even non-breeders)
	 */
	if (num_repro < MAX_REPRO) {
		int k, y, x;

		/* Count the adjacent monsters */
		for (k = 0, y = oy - 1; y <= oy + 1; y++)
			for (x = ox - 1; x <= ox + 1; x++)
				/* Count monsters */
				if (cave->m_idx[y][x] > 0) k++;

		/* Multiply slower in crowded areas */
		if ((k < 4) && (k == 0 || one_in_(k * MON_MULT_ADJ))) {
			/* Successful breeding attempt, learn about that now */
			if (m_ptr->ml)
				rf_on(l_ptr->flags, RF_MULTIPLY);

			/* Try to multiply (only breeders allowed) */
			if (rf_has(m_ptr->race->flags, RF_MULTIPLY) && multiply_monster(m_ptr)) {
				/* Make a sound */
				if (m_ptr->ml)
					sound(MSG_MULTIPLY);

				/* Multiplying takes energy */
				return;
			}
		}
	}

	/* Mimics lie in wait */
	if (is_mimicking(m_ptr)) return;

	/* Attempt to cast a spell */
	if (make_attack_spell(m_ptr)) return;

	/* Reset */
	stagger = FALSE;

	/* Confused */
	if (m_ptr->m_timed[MON_TMD_CONF])
		/* Stagger */
		stagger = TRUE;

	/* Random movement - always attempt for lore purposes */
	else {
		int roll = randint0(100);

		/* Random movement (25%) */
		if (roll < 25) {
			/* Learn about small random movement */
			if (m_ptr->ml)
				rf_on(l_ptr->flags, RF_RAND_25);

			/* Stagger */
			if (flags_test(m_ptr->race->flags, RF_SIZE, RF_RAND_25, RF_RAND_50, FLAG_END))
				stagger = TRUE;

		/* Random movement (50%) */
		} else if (roll < 50) {
			/* Learn about medium random movement */
			if (m_ptr->ml)
				rf_on(l_ptr->flags, RF_RAND_50);

			/* Stagger */
			if (rf_has(m_ptr->race->flags, RF_RAND_50))
				stagger = TRUE;

		/* Random movement (75%) */
		} else if (roll < 75) {
			/* Stagger */
			if (flags_test_all(m_ptr->race->flags, RF_SIZE, RF_RAND_25, RF_RAND_50, FLAG_END))
				stagger = TRUE;
		}
	}

	/* Normal movement */
	if (!stagger)
		/* Logical moves, may do nothing */
		if (!get_moves(cave, m_ptr, mm)) return;

	/* Assume nothing */
	do_turn = FALSE;
	do_move = FALSE;
	do_view = FALSE;

	/* Process moves */
	for (i = 0; i < 5; i++)	{
		/* Get the direction (or stagger) */
		d = (stagger ? ddd[randint0(8)] : mm[i]);

		/* Get the destination */
		ny = oy + ddy[d];
		nx = ox + ddx[d];

		/* Floor is open? */
		if (square_ispassable(cave, ny, nx))
			/* Go ahead and move */
			do_move = TRUE;

		/* Permanent wall in the way */
		else if (square_iswall(cave, ny, nx) && square_isperm(cave, ny, nx))
		{
			/* Nothing */
		}

		/* Normal wall, door, or secret door in the way */
		else {
			/* There's some kind of feature in the way, so learn about
			 * kill-wall and pass-wall now */
			if (m_ptr->ml) {
				rf_on(l_ptr->flags, RF_PASS_WALL);
				rf_on(l_ptr->flags, RF_KILL_WALL);
			}

			/* Monster moves through walls (and doors) */
			if (rf_has(m_ptr->race->flags, RF_PASS_WALL))
				/* Pass through walls/doors/rubble */
				do_move = TRUE;

			/* Monster destroys walls (and doors) */
			else if (rf_has(m_ptr->race->flags, RF_KILL_WALL)) {
				/* Eat through walls/doors/rubble */
				do_move = TRUE;

				/* Forget the wall */
				sqinfo_off(cave->info[ny][nx], SQUARE_MARK);

				/* Notice */
				square_destroy_wall(c, ny, nx);

				/* Note changes to viewable region */
				if (player_has_los_bold(ny, nx)) do_view = TRUE;
			}

			/* Handle doors and secret doors */
			else if (square_iscloseddoor(cave, ny, nx) || square_issecretdoor(cave, ny, nx)) {
				/* Take a turn */
				do_turn = TRUE;

				/* Learn about door abilities */
				if (m_ptr->ml) {
					rf_on(l_ptr->flags, RF_OPEN_DOOR);
					rf_on(l_ptr->flags, RF_BASH_DOOR);
				}

				/* Creature can open or bash doors */
				if (rf_has(m_ptr->race->flags, RF_OPEN_DOOR) || rf_has(m_ptr->race->flags, RF_BASH_DOOR)) {
					bool may_bash = ((rf_has(m_ptr->race->flags, RF_BASH_DOOR) && one_in_(2))? TRUE: FALSE);

					/* Stuck door -- try to unlock it */
					if (square_islockeddoor(cave, ny, nx)) {
						int k = square_door_power(cave, ny, nx);

						if (randint0(m_ptr->hp / 10) > k) {
							/* Print a message */
							/* XXX This can probably be consolidated, since monster_desc checks m_ptr->ml */
							if (m_ptr->ml) {
								if (may_bash)
									msg("%s slams against the door.", m_name);
								else
									msg("%s fiddles with the lock.", m_name);
							} else {
								if (may_bash)
									msg("Something slams against a door.");
								else
									msg("Something fiddles with a lock.");
							}

							/* Reduce the power of the door by one */
							square_set_feat(c, ny, nx, cave->feat[ny][nx] - 1);
						}
					}

					/* Closed or secret door -- open or bash if allowed */
					else {
						if (may_bash) {
							square_smash_door(c, ny, nx);
							msg("You hear a door burst open!");

							disturb(p_ptr, 0, 0);

							/* Fall into doorway */
							do_move = TRUE;
						} else
							square_open_door(c, ny, nx);

						/* Handle viewable doors */
						if (player_has_los_bold(ny, nx))
							do_view = TRUE;
					}
				}
			}
		}


		/* Hack -- check for Glyph of Warding */
		if (do_move && square_iswarded(cave, ny, nx)) {
			/* Assume no move allowed */
			do_move = FALSE;

			/* Break the ward */
			if (randint1(BREAK_GLYPH) < m_ptr->race->level) {
				/* Describe observable breakage */
				if (sqinfo_has(cave->info[ny][nx], SQUARE_MARK))
					msg("The rune of protection is broken!");

				/* Forget the rune */
				sqinfo_off(cave->info[ny][nx], SQUARE_MARK);

				/* Break the rune */
				square_remove_ward(c, ny, nx);

				/* Allow movement */
				do_move = TRUE;
			}
		}


		/* The player is in the way. */
		if (do_move && (cave->m_idx[ny][nx] < 0)) {
			/* Learn about if the monster attacks */
			if (m_ptr->ml)
				rf_on(l_ptr->flags, RF_NEVER_BLOW);

			/* Some monsters never attack */
			if (rf_has(m_ptr->race->flags, RF_NEVER_BLOW))
				/* Do not move */
				do_move = FALSE;

			/* Otherwise, attack the player */
			else {
				/* Do the attack */
				make_attack_normal(m_ptr, p_ptr);

				/* Do not move */
				do_move = FALSE;

				/* Took a turn */
				do_turn = TRUE;
			}
		}


		/* Some monsters never move */
		if (do_move && rf_has(m_ptr->race->flags, RF_NEVER_MOVE))	{
			/* Learn about lack of movement */
			if (m_ptr->ml)
				rf_on(l_ptr->flags, RF_NEVER_MOVE);

			/* Do not move */
			do_move = FALSE;
		}


		/* A monster is in the way */
		if (do_move && (cave->m_idx[ny][nx] > 0)) {
			monster_type *n_ptr = square_monster(cave, ny, nx);

			/* Kill weaker monsters */
			int kill_ok = rf_has(m_ptr->race->flags, RF_KILL_BODY);

			/* Move weaker monsters if they can swap places */
			/* (not in a wall) */
			int move_ok = (rf_has(m_ptr->race->flags, RF_MOVE_BODY) &&
						   square_ispassable(cave, m_ptr->fy, m_ptr->fx));

			/* Assume no movement */
			do_move = FALSE;

			if (compare_monsters(m_ptr, n_ptr) > 0) 	{
				/* Learn about pushing and shoving */
				if (m_ptr->ml) {
					rf_on(l_ptr->flags, RF_KILL_BODY);
					rf_on(l_ptr->flags, RF_MOVE_BODY);
				}

				if (kill_ok || move_ok) {
					/* Get the names of the monsters involved */
					char m1_name[80];
					char n_name[80];
					monster_desc(m1_name, sizeof(m1_name), m_ptr, MDESC_IND_HID);
					monster_desc(n_name, sizeof(n_name), n_ptr, MDESC_IND_HID);
					my_strcap(m1_name);

					/* Allow movement */
					do_move = TRUE;

					/* Reveal mimics */
					if (is_mimicking(n_ptr))
						become_aware(n_ptr);

					/* Monster ate another monster */
					if (kill_ok) {
						/* Note if visible */
						if (m_ptr->ml && (m_ptr->mflag & (MFLAG_VIEW)))
							msg("%s tramples over %s.", m1_name, n_name);

						delete_monster(ny, nx);
					} else {
						/* Note if visible */
						if (m_ptr->ml && (m_ptr->mflag & (MFLAG_VIEW)))
							msg("%s pushes past %s.", m1_name, n_name);
					}
				}
			}
		}

		/* Creature has been allowed move */
		if (do_move) {
			s16b this_o_idx, next_o_idx = 0;

			/* Learn about no lack of movement */
			if (m_ptr->ml) rf_on(l_ptr->flags, RF_NEVER_MOVE);

			/* Take a turn */
			do_turn = TRUE;

			/* Move the monster */
			monster_swap(oy, ox, ny, nx);

			/* Possible disturb */
			if (m_ptr->ml && (m_ptr->mflag & MFLAG_VIEW) && OPT(disturb_near))
				disturb(p_ptr, 0, 0);

			/* Scan all objects in the grid */
			for (this_o_idx = cave->o_idx[ny][nx]; this_o_idx;
					this_o_idx = next_o_idx) {
				object_type *o_ptr;

				/* Get the object */
				o_ptr = object_byid(this_o_idx);

				/* Get the next object */
				next_o_idx = o_ptr->next_o_idx;

				/* Skip gold */
				if (o_ptr->tval == TV_GOLD) continue;

				/* Learn about item pickup behavior */
				if (m_ptr->ml) {
					rf_on(l_ptr->flags, RF_TAKE_ITEM);
					rf_on(l_ptr->flags, RF_KILL_ITEM);
				}

				/* Take or Kill objects on the floor */
				if (rf_has(m_ptr->race->flags, RF_TAKE_ITEM) ||
						rf_has(m_ptr->race->flags, RF_KILL_ITEM))	{
					bitflag obj_flags[OF_SIZE];
					bitflag mon_flags[RF_SIZE];

					char m1_name[80];
					char o_name[80];

					rf_wipe(mon_flags);

					/* Extract some flags */
					object_flags(o_ptr, obj_flags);

					/* Get the object name */
					object_desc(o_name, sizeof(o_name), o_ptr,
								ODESC_PREFIX | ODESC_FULL);

					/* Get the monster name */
					monster_desc(m1_name, sizeof(m1_name), m_ptr, MDESC_IND_HID | MDESC_CAPITAL);

					/* React to objects that hurt the monster */
					react_to_slay(obj_flags, mon_flags);

					/* The object cannot be picked up by the monster */
					if (o_ptr->artifact || rf_is_inter(m_ptr->race->flags, mon_flags)) {
						/* Only give a message for "take_item" */
						if (rf_has(m_ptr->race->flags, RF_TAKE_ITEM))	{
							/* Describe observable situations */
							if (m_ptr->ml && player_has_los_bold(ny, nx) &&
									!squelch_item_ok(o_ptr))
								/* Dump a message */
								msg("%s tries to pick up %s, but fails.",
									m1_name, o_name);
						}

					/* Pick up the item */
					} else if (rf_has(m_ptr->race->flags, RF_TAKE_ITEM)) {
						object_type *i_ptr;
						object_type object_type_body;

						/* Describe observable situations */
						if (player_has_los_bold(ny, nx) &&
								!squelch_item_ok(o_ptr))
							/* Dump a message */
							msg("%s picks up %s.", m1_name, o_name);

						/* Get local object */
						i_ptr = &object_type_body;

						/* Obtain local object */
						object_copy(i_ptr, o_ptr);

						/* Delete the object */
						delete_object_idx(this_o_idx);

						/* Carry the object */
						monster_carry(m_ptr, i_ptr);

					/* Destroy the item */
					} else {
						/* Describe observable situations */
						if (player_has_los_bold(ny, nx) &&
								!squelch_item_ok(o_ptr))
							/* Dump a message */
							msgt(MSG_DESTROY, "%s crushes %s.", m_name, o_name);

						/* Delete the object */
						delete_object_idx(this_o_idx);
					}
				}
			}
		}

		/* Stop when done */
		if (do_turn) break;
	}

	if (rf_has(m_ptr->race->flags, RF_HAS_LIGHT))
		do_view = TRUE;

	/* Notice changes in view */
	if (do_view) {
		/* Update the visuals */
		p_ptr->update |= (PU_UPDATE_VIEW | PU_MONSTERS);

		/* Fully update the flow XXX XXX XXX */
		p_ptr->update |= (PU_FORGET_FLOW | PU_UPDATE_FLOW);
	}


	/* Hack -- get "bold" if out of options */
	if (!do_turn && !do_move && m_ptr->m_timed[MON_TMD_FEAR])
		mon_clear_timed(m_ptr, MON_TMD_FEAR, MON_TMD_FLG_NOTIFY, FALSE);

	/* If we see an unaware monster do something, become aware of it */
	if (do_turn && m_ptr->unaware)
		become_aware(m_ptr);
}


static bool monster_can_flow(struct cave *c, struct monster *m_ptr)
{
	int fy = m_ptr->fy;
	int fx = m_ptr->fx;

	assert(c);

	/* Check the flow (normal aaf is about 20) */
	if ((c->when[fy][fx] == c->when[p_ptr->py][p_ptr->px]) &&
	    (c->cost[fy][fx] < MONSTER_FLOW_DEPTH) &&
	    (c->cost[fy][fx] < (OPT(birth_small_range) ? m_ptr->race->aaf / 2 : m_ptr->race->aaf)))
		return TRUE;
	return FALSE;
}

/*
 * Process all the "live" monsters, once per game turn.
 *
 * During each game turn, we scan through the list of all the "live" monsters,
 * (backwards, so we can excise any "freshly dead" monsters), energizing each
 * monster, and allowing fully energized monsters to move, attack, pass, etc.
 *
 * Note that monsters can never move in the monster array (except when the
 * "compact_monsters()" function is called by "dungeon()" or "save_player()").
 *
 * This function is responsible for at least half of the processor time
 * on a normal system with a "normal" amount of monsters and a player doing
 * normal things.
 *
 * When the player is resting, virtually 90% of the processor time is spent
 * in this function, and its children, "process_monster()" and "make_move()".
 *
 * Most of the rest of the time is spent in "update_view()" and "light_spot()",
 * especially when the player is running.
 *
 * Note the special "MFLAG_NICE" flag, which prevents "nasty" monsters from
 * using any of their spell attacks until the player gets a turn.
 */
void process_monsters(struct cave *c, byte minimum_energy)
{
	int i;

	/* Process the monsters (backwards) */
	for (i = cave_monster_max(c) - 1; i >= 1; i--)
	{
		monster_type *m_ptr;

		/* Handle "leaving" */
		if (p_ptr->leaving) break;

		/* Get the monster */
		m_ptr = cave_monster(cave, i);

		/* Ignore "dead" monsters */
		if (!m_ptr->race) continue;

		/* Not enough energy to move */
		if (m_ptr->energy < minimum_energy) continue;

		/* Use up "some" energy */
		m_ptr->energy -= 100;

		/* Heal monster? XXX XXX XXX */

		/*
		 * Process the monster if the monster either:
		 * - can "sense" the player
		 * - is hurt
		 * - can "see" the player (checked backwards)
		 * - can "smell" the player from far away (flow)
		 */
		if ((m_ptr->cdis <= (OPT(birth_small_range) ? m_ptr->race->aaf / 2 : m_ptr->race->aaf)) ||
		    (m_ptr->hp < m_ptr->maxhp) ||
		    player_has_los_bold(m_ptr->fy, m_ptr->fx) ||
		    monster_can_flow(c, m_ptr))
		{
			/* Process the monster */
			process_monster(c, m_ptr);
		}
	}
}

/* Test functions */
bool (*testfn_make_attack_normal)(struct monster *m, struct player *p) = make_attack_normal;
