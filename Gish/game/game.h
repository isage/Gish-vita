#ifndef GISH_GAME_GAME_H
#define GISH_GAME_GAME_H
/*
Copyright (C) 2005, 2010 - Cryptic Sea

This file is part of Gish.

Gish is free software; you can redistribute it and/or
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

void gameloop(void);
void renderlevel(void);
void simulation(void);
void getinputs(void);

typedef enum
{
  GAMEEXIT_NONE       = 0,
  GAMEEXIT_INGAMEMENU = 1,
  GAMEEXIT_EXITGAME   = 2,
  GAMEEXIT_DIED       = 3,
  GAMEEXIT_WON        = 4,
  GAMEXIT_WARPZONE    = 5
} gameexit;

typedef struct
{
  float position[3];
  float orientation[3][3];
  float zoom;
  int zoomx;
  int zoomy;
} _view;

typedef struct
{
  int pause;
  int framenum;
  int levelnum;
  int bosslevel;
  int dialog;
  int dialogdelay;
  int songnum;
  int currentsongnum;
  int time;
  int score[4];
  int combo;
  int combodelay;
  int totalscore;
  int difficulty;
  int bonus[16];
  int numofbonus[16];
  int scoredelay;
  int startdelay;
  int exitdelay;
  int over;
  int numoflives;
  char text[256];
  gameexit exit;
  int oldschool;
  int oldschoolsound;
  int turbomode;
  int supersize;
  unsigned int simspeed;
  int playreplay;
  int editing;
  int godmode;
  int godparticle;
} _game;

extern _view view;
extern _game game;

extern int fps_enabled;
extern int touchcontrols;
extern int lighting_enabled;
extern int shadow_enabled;
extern int cache_fix;
extern int disable_frame;
extern int fix_boss_logic_when_lights_off;
extern float z_zoom;

#if defined(GISH_INSTRUMENT)
/* Timing instrumentation - sub-timers for bondsimulation2().  Plain C types
   only: this header is included from files that do not include SDL.  With
   GISH_INSTRUMENT off (the release default) the probes disappear entirely, so
   physics.c can keep its call sites unconditionally. */
enum
{
  PERFBOND_PRE = 0,  /* forceapplied zeroing + bondnormal/veclength/oomass  */
  PERFBOND_COPY,     /* the 32x particle prevvelocity copy                  */
  PERFBOND_GLOBAL,   /* the 32x plain bond[] solver loop                    */
  PERFBOND_TEMP,     /* the 32x physicstemp[] contact-bond solver loop      */
  PERFBOND_REST,     /* loop overhead / everything else                     */
  PERFBOND_CALIB,    /* empty probes: measures the probe cost itself        */
  PERFBOND_SLOTS
};
#define PERFBOND_TYPES 7 /* contact bonds counted per type: 0,1,2,4,5, and -1 */

extern unsigned long long perf_bond_us[PERFBOND_SLOTS];
extern unsigned long long perf_bond_tick;
extern int                perf_bond_max;    /* peak physicstemp.numofbonds   */
extern long long          perf_bond_temps;  /* sum of physicstemp.numofbonds */
extern long long          perf_bond_itcnt[PERFBOND_TYPES];
void                      perf_bond_start(void);
void                      perf_bond_probe(int slot);
#else
#define perf_bond_start()  ((void)0)
#define perf_bond_probe(s) ((void)0)
#endif

#endif /* GISH_GAME_GAME_H */
