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

#include "../game/game.h"

#include "../audio/audio.h"
#include "../config.h"
#include "../game/animation.h"
#include "../game/boss.h"
#include "../game/config.h"
#include "../game/custom.h"
#include "../game/editor.h"
#include "../game/english.h"
#include "../game/gameaudio.h"
#include "../game/gameobject.h"
#include "../game/level.h"
#include "../game/lighting.h"
#include "../game/logic.h"
#include "../game/mainmenu.h"
#include "../game/music.h"
#include "../game/objfunc.h"
#include "../game/options.h"
#include "../game/physics.h"
#include "../game/prerender.h"
#include "../game/random.h"
#include "../game/record.h"
#include "../game/render.h"
#include "../game/replay.h"
#include "../game/setup.h"
#include "../game/sprite.h"
#include "../input/joystick.h"
#include "../input/keyboard.h"
#include "../input/mouse.h"
#include "../math/vector.h"
#include "../menu/menu.h"
#include "../physics/bond.h"
#include "../physics/particle.h"
#include "../sdl/event.h"
#include "../sdl/sdl.h"
#include "../sdl/video.h"
#include "../video/glfunc.h"
#include "../video/opengl.h"
#include "../video/text.h"
#include "../video/texture.h"

#include <stdlib.h>
#include <time.h>

_view view;
_game game;

// EXL: Some Gish settings
int fps_enabled                    = 0;
int lighting_enabled               = 0;
int shadow_enabled                 = 0;
int touchcontrols                  = 0;
int cache_fix                      = 0;
int disable_frame                  = 0;
float z_zoom                       = 10.0f;
int fix_boss_logic_when_lights_off = 0;

int fps, fps_cache = 0, fps_renew = 0, fps_mean;

#if defined(GISH_INSTRUMENT)
/* ---------------------------------------------------------------------------
   PERF INSTRUMENTATION - simulation breakdown

   Compiled only when configured with -DGISH_INSTRUMENT=ON (see CMakeLists.txt).
   A release build contains none of this: no timers, no counters, no report
   strings.

   The render side is ticked inside gameloop(); the sim side is ticked inside
   simulation(), so these counters have to live at file scope.  Each slot is
   ticked once per simulation step, so the reported averages are ms per step.

   The perf lines are mirrored to <gishDataPath>perf.log as well, because a
   dropped Cat-A-Log connection silently loses whole sections of a session.

   Remove this block and every PERF_SIM/PERF_EMIT call before a public release.
   --------------------------------------------------------------------------- */

enum
{
  PERFSIM_PARTICLES = 0, /* particlesimulation, lifetime, gravity, particlecollision */
  PERFSIM_OBJWORLD,      /* objectcollision() loop    - objects vs level blocks   */
  PERFSIM_OBJOBJ,        /* objectcollisionobject()   - objects vs each other      */
  PERFSIM_CYCLE,         /* objectcycle() + objectanimation()                      */
  PERFSIM_BONDS,         /* bondsimulation2()                                      */
  PERFSIM_CHECKBONDS,    /* checkbonds()                                           */
  PERFSIM_OBJTTL,        /* objecttimetolive()                                     */
  PERFSIM_SPRITES,       /* spritesimulation() + spritetimetolive()                */
  PERFSIM_BOSS,          /* bosssimulation() + bosstimetolive()                    */
  PERFSIM_DAMAGE,        /* damage / particle-spawn loop at the end of simulation  */
  PERFSIM_LOGIC,         /* gamelogic(), ticked from the step loop in gameloop()   */
  PERFSIM_SLOTS
};

static Uint64 perf_simsec[PERFSIM_SLOTS], perf_simsec_max[PERFSIM_SLOTS];
static Uint64 perf_simtick;
static int perf_maxobjs, perf_maxbonds, perf_maxparts;

#define PERF_SIM(i)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    Uint64 _sn = SDL_GetPerformanceCounter(), _sd = _sn - perf_simtick;                                                \
    perf_simsec[i] += _sd;                                                                                             \
    if (_sd > perf_simsec_max[i])                                                                                      \
      perf_simsec_max[i] = _sd;                                                                                        \
    perf_simtick = _sn;                                                                                                \
  } while (0)

/* PERF INSTRUMENTATION (temporary) - bondsimulation2() sub-timers.
   A probe charges the time since the previous probe to one slot.  Every probe
   costs one sceKernelGetProcessTimeWide() read (~0.2 us, 1 us resolution), so
   the empty calibration loop below measures that cost and it can be
   subtracted from the reported numbers. */
unsigned long long perf_bond_us[PERFBOND_SLOTS];
unsigned long long perf_bond_tick;
int                perf_bond_max;
long long          perf_bond_temps;
long long          perf_bond_itcnt[PERFBOND_TYPES];

void perf_bond_start(void) { perf_bond_tick = SDL_GetPerformanceCounter(); }

void perf_bond_probe(int slot)
{
  Uint64 now    = SDL_GetPerformanceCounter();
  perf_bond_us[slot] += now - perf_bond_tick;
  perf_bond_tick = now;
}

/* stderr -> Cat-A-Log.  Deliberately no file mirror: writing to ux0: costs
   many milliseconds and was visible in the profile as a hitch every 1-3 s.

   One write per report, not one per line: each line sent to Cat-A-Log is a
   blocking socket write costing ~2 ms, so six separate writes cost ~9 ms of
   one frame every second.  Lines are therefore formatted into one buffer and
   sent in a single fprintf. */
static char perf_linebuf[2048];
static int  perf_linelen;

static void perfreport(const char *text) { TO_DEBUG_LOG("%s", text); }

/* append one perf line to the report buffer */
#define PERF_EMIT(...)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
    int _n = snprintf(perf_linebuf + perf_linelen, sizeof(perf_linebuf) - (size_t)perf_linelen, __VA_ARGS__);           \
    if (_n > 0)                                                                                                        \
    {                                                                                                                  \
      perf_linelen += _n;                                                                                              \
      if (perf_linelen > (int)sizeof(perf_linebuf) - 1)                                                                \
        perf_linelen = (int)sizeof(perf_linebuf) - 1;                                                                  \
    }                                                                                                                  \
  } while (0)

#else /* !GISH_INSTRUMENT */

/* Release build: every instrumentation call site stays in the source but
   compiles to nothing, so physics.c/render.c need no #ifdefs of their own. */
#define PERF_SIM(i)    do { } while (0)
#define PERF_SEC(i)    do { } while (0)
#define PERF_EMIT(...) do { } while (0)

#endif /* GISH_INSTRUMENT */

void gameloop(void)
{
  int count, count2;
  unsigned int simtimer;
  int simcount;
  int frametimer;
  // float vec[3];
  // char filename[13]="text000.tga";
  int scorenum;
  // unsigned int x;

  game.godparticle = -1;

  game.oldschool = 0;
  if (game.levelnum == 64)
    game.oldschool = 1;
  if (game.levelnum == 65)
  {
    game.oldschool      = 2;
    game.oldschoolsound = -200;
  }
  if (game.levelnum == 66)
    game.oldschool = 3;

  srand(time(NULL));

  setuplevel();
  setupgame();

  /* Retro warp levels 64/65 sample every texture with GL_NEAREST.  These two
     levels show much more of the world at once than the normal ones, so the
     GL_LINEAR/mipmapped sampling blurs the pixel art; nearest keeps the
     texels sharp.  Textures uploaded later in the level pick the flag up in
     setuptexture(); the ones already on the GPU are switched here.  This runs
     once per level, never per frame. */
  nearestfiltering = (game.oldschool == 1 || game.oldschool == 2);
  {
    static int filtering_applied = -1;

    if (filtering_applied != nearestfiltering)
    {
      settexturefiltering(nearestfiltering);
      filtering_applied = nearestfiltering;
    }
  }

  simtimer = SDL_GetTicks();

  game.exit = GAMEEXIT_NONE;

  scorenum = -1;

  resetmenuitems();

  view.zoom = z_zoom;
  if (game.oldschool == 1)
    view.zoom = 10.0f;
  if (game.oldschool == 2)
    view.zoom = 16.0f;
  if (game.oldschool == 3)
    view.zoom = 26.0f;

  if (level.gametype == GAMETYPE_2COLLECTION)
    view.zoom = 24.0f;
  if (level.gametype == GAMETYPE_2RACING)
    view.zoom = 24.0f;
  if (level.gametype == GAMETYPE_4FOOTBALL || level.gametype == GAMETYPE_4SUMO)
    view.zoom = 14.0f;

#if defined(GISH_INSTRUMENT)
  /* PERF INSTRUMENTATION */
  static Uint64 perf_freq = 0, perf_prev = 0, perf_window = 0;
  static Uint64 perf_render = 0, perf_sim = 0, perf_sim_max = 0, perf_frame_max = 0;
  static Uint64 perf_sec[12], perf_sec_max[12], perf_tick = 0;
  static int perf_frames = 0, perf_steps = 0, perf_catchup = 0;
  static int perf_maxleveltiles, perf_maxleveldraws, perf_maxlevelflush;
  static int perf_maxrenders, perf_maxlights;
  static Uint32 perf_tick0 = 0;
  if (perf_freq == 0)
  {
    perf_freq  = SDL_GetPerformanceFrequency();
    perf_tick0 = SDL_GetTicks();
  }
/* slots: 0=clear/bg 1=light 2=setupobj 3=shadow 4=lvlbak 5=part 6=obj 7=part2 8=lvl 9=lvlfore 10=rest 11=pixelation */
#define PERF_SEC(i)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    Uint64 _pn = SDL_GetPerformanceCounter(), _pd = _pn - perf_tick;                                                   \
    perf_sec[i] += _pd;                                                                                                \
    if (_pd > perf_sec_max[i])                                                                                         \
      perf_sec_max[i] = _pd;                                                                                           \
    perf_tick = _pn;                                                                                                   \
  } while (0)
#endif

  while ((game.exit < GAMEEXIT_EXITGAME || game.exitdelay > 0) && !windowinfo.shutdown)
  {
#if defined(GISH_INSTRUMENT)
    Uint64 perf_now, perf_t_sim0, perf_t_sim1;
#endif
    frametimer = SDL_GetTicks();
#if defined(GISH_INSTRUMENT)
    perf_now   = SDL_GetPerformanceCounter();
    perf_tick  = perf_now;

    /* level pass counters are filled by render.c during this frame */
    perf_leveltiles = 0;
    perf_leveldraws = 0;
    perf_levelflush = 0;
#endif

    /* Music streaming: exactly one call per frame, at the top of the frame,
       so a decode slice never lands inside the sim or the draw loop. */
    updateogg();

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(~0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glStencilMask(0);

    setuptextdisplay();
#if defined(GLES)
    glColor4f(level.ambient[3][0], level.ambient[3][1], level.ambient[3][2], 1.0f);
#else
    glColor3fv(level.ambient[3]);
#endif
    if (level.background[0] != 0)
      displaybackground(660);
    PERF_SEC(0);

    if (game.over != 0 && level.gametype < GAMETYPE_2FOOTBALL)
      if (game.exit == GAMEEXIT_NONE)
      {
        if (game.over >= 3 && game.over <= 5)
        {
          game.exit      = GAMEXIT_WARPZONE;
          game.exitdelay = 100;
        }
        if (game.over == 2)
        {
          game.exit      = GAMEEXIT_WON;
          game.exitdelay = 100;
        }
        if (game.over == 1)
        {
          game.exit      = GAMEEXIT_DIED;
          game.exitdelay = 100;
          if (game.levelnum == 65)
            game.exitdelay = 200;
        }
      }

    numofmenuitems = 0;
    if (game.exit == GAMEEXIT_NONE)
    {
      createmenuitem("", 0, 0, 16, 1.0f, 1.0f, 1.0f, 1.0f);
      setmenuitem(MO_HOTKEY, SCAN_ESC);
      setmenuitem(MO_SET, &game.exit, GAMEEXIT_INGAMEMENU);
    }
    if (game.exit == GAMEEXIT_INGAMEMENU)
    {
      count = 240;

      if (game.over == 0)
      {
        createmenuitem(TXT_RETURN_TO_GAME, (320 | TEXT_CENTER), count, 16, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_HOTKEY, SCAN_ESC);
        setmenuitem(MO_SET, &game.exit, GAMEEXIT_NONE);
      }
      else
        createmenuitemempty();
      count += 16;

      if (game.levelnum < 64)
      {
        if (level.gametype == GAMETYPE_CAMPAIGN && (game.levelnum > 0 || mappack.active) && !game.playreplay)
          createmenuitem(TXT_RESETLEVEL_MINUSONE, (320 | TEXT_CENTER), count, 16, 1.0f, 1.0f, 1.0f, 1.0f);
        else
          createmenuitem(TXT_RESETLEVEL, (320 | TEXT_CENTER), count, 16, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_HOTKEY, SCAN_R);
        if (level.gametype == GAMETYPE_CAMPAIGN)
          setmenuitem(MO_SET, &game.exit, GAMEEXIT_DIED);
        count += 16;
      }
      else
        createmenuitemempty();

      if (game.over == 0 && game.levelnum < 64 && level.gametype == GAMETYPE_CAMPAIGN && game.levelnum > 0
          && !game.playreplay)
        createmenuitem(TXT_EXITGAME_MINUSONE, (320 | TEXT_CENTER), count, 16, 1.0f, 1.0f, 1.0f, 1.0f);
      else
        createmenuitem(TXT_EXITGAME, (320 | TEXT_CENTER), count, 16, 1.0f, 1.0f, 1.0f, 1.0f);
      setmenuitem(MO_HOTKEY, SCAN_E);
      setmenuitem(MO_SET, &game.exit, GAMEEXIT_EXITGAME);
      count += 16;
    }
    if (game.exit == GAMEEXIT_DIED)
    {
      if (game.time > 0)
      {
        createmenuitem("       ", (320 | TEXT_CENTER), (240 | TEXT_CENTER), 20, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_SET, &game.exitdelay, 0);
      }
      else
      {
        createmenuitem(TXT_TIMEUP, (320 | TEXT_CENTER), (240 | TEXT_CENTER), 20, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_SET, &game.exitdelay, 0);
      }
    }
    if (game.exit == GAMEEXIT_WON)
    {
      createmenuitem(TXT_COMPLETE, 524 | TEXT_CENTER, 266, 20, 1.0f, 1.0f, 1.0f, 1.0f);
      setmenuitem(MO_IMAGE, 462);
      setmenuitem(MO_RESIZE, (320 | TEXT_CENTER), (240 | TEXT_CENTER), 256, 128);
      setmenuitem(MO_SET, &game.exitdelay, 0);
    }
    if (game.exit == GAMEXIT_WARPZONE)
    {
      if (game.levelnum != 34)
      {
        createmenuitem(TXT_WARPZONE, (320 | TEXT_CENTER), (240 | TEXT_CENTER), 24, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_SET, &game.exitdelay, 0);
      }
      else
      {
        // createmenuitem("OOPS!",(320|TEXT_CENTER),(240|TEXT_CENTER),24,1.0f,1.0f,1.0f,1.0f);
        // setmenuitem(MO_SET,&game.exitdelay,0);
        createmenuitem(TXT_OOPS, (320 | TEXT_CENTER), (240 | TEXT_CENTER), 24, 1.0f, 1.0f, 1.0f, 1.0f);
        setmenuitem(MO_IMAGE, 465);
        setmenuitem(MO_RESIZE, (320 | TEXT_CENTER), (240 | TEXT_CENTER), 256, 128);
        setmenuitem(MO_SET, &game.exitdelay, 0);
      }
    }

    checksystemmessages();
    checkkeyboard();
    checkmouse();
    checkjoystick();
    checkmenuitems();

    if (game.exit == GAMEEXIT_INGAMEMENU)
      if (level.gametype != GAMETYPE_CAMPAIGN)
        if (menuitem[1].active)
        {
          setuplevel();
          setupgame();

          game.exit          = GAMEEXIT_NONE;
          menuitem[1].active = 0;
        }

    if (game.dialog > 0)
    {
      if (game.levelnum != 68 || (game.dialog != 13 && game.dialog != 14))
      {
        if (game.dialogdelay > 0)
          game.dialogdelay--;

        count2 = 0;
        if (game.dialogdelay == 0)
          count2 = 1;
        for (count = 4; count < KEYALIAS_LENGTH; count++)
          if (keyboard[control[0].key[count]] && !prevkeyboard[control[0].key[count]])
            count2 = 1;
        if (control[0].joysticknum != -1)
          for (count = 4; count < 8; count++)
            if (control[0].button[count] != -1)
              if (joystick[control[0].joysticknum].button[control[0].button[count]]
                  && !prevjoystick[control[0].joysticknum].button[control[0].button[count]])
                count2 = 1;

        if (count2 == 1)
        {
          game.dialog--;
          game.dialogdelay = 1000;

          if (game.levelnum == 68)
            if (game.dialog == 4)
            {
              game.numoflives = 99;
              game.exit       = GAMEEXIT_WON;
              game.exitdelay  = 0;
            }
        }
      }
      else
      {
        count2 = 0;
        if (keyboard[control[0].key[KEYALIAS_DOWN]] && !prevkeyboard[control[0].key[KEYALIAS_DOWN]])
          count2 = 1;
        if (keyboard[control[0].key[KEYALIAS_UP]] && !prevkeyboard[control[0].key[KEYALIAS_UP]])
          count2 = 1;
        if (control[0].joysticknum != -1)
        {
          if (joystick[control[0].joysticknum].axis[1] >= 0.5f && prevjoystick[control[0].joysticknum].axis[1] < 0.5f)
            count2 = 1;
          if (joystick[control[0].joysticknum].axis[1] <= -0.5f && prevjoystick[control[0].joysticknum].axis[1] > -0.5f)
            count2 = 1;
        }
        if (count2 == 1)
        {
          if (game.dialog == 14)
            game.dialog = 13;
          else
            game.dialog = 14;
        }

        count2 = 0;
        for (count = 4; count < KEYALIAS_LENGTH; count++)
          if (keyboard[control[0].key[count]] && !prevkeyboard[control[0].key[count]])
            count2 = 1;
        if (control[0].joysticknum != -1)
          for (count = 4; count < 8; count++)
            if (control[0].button[count] != -1)
              if (joystick[control[0].joysticknum].button[control[0].button[count]]
                  && !prevjoystick[control[0].joysticknum].button[control[0].button[count]])
                count2 = 1;

        if (count2 == 1)
        {
          if (game.dialog == 14)
            game.dialog = 12;
          else
            game.dialog = 1;
        }
      }
    }
    checkmusic();

    if (game.levelnum >= 1 && game.levelnum <= 7)
      game.songnum = 0;
    if (game.levelnum >= 8 && game.levelnum <= 14)
      game.songnum = 1;
    if (game.levelnum >= 15 && game.levelnum <= 21)
      game.songnum = 2;
    if (game.levelnum >= 22 && game.levelnum <= 28)
      game.songnum = 3;
    if (game.levelnum >= 29 && game.levelnum <= 32)
      game.songnum = 4;
    if (game.bosslevel)
      game.songnum = 5;
    if (game.levelnum == 64)
      game.songnum = 6;
    if (game.levelnum == 67)
      game.songnum = 2;

    if (game.levelnum == 0)
    {
      if (game.songnum == -1)
        game.songnum = rand() % 5;
    }
    if (level.gametype == GAMETYPE_2SUMO)
      game.songnum = 7;
    /*
    if (level.gametype==GAMETYPE_2FOOTBALL)
      game.songnum=4;
    if (level.gametype==GAMETYPE_2SUMO)
      game.songnum=5;
    if (level.gametype==GAMETYPE_2GREED)
      game.songnum=4;
    */
    if (game.levelnum == 0)
      if (keyboard[SCAN_F5] && !prevkeyboard[SCAN_F5])
      {
        setuplevel();
        setupgame();
      }
#ifndef ANDROID_NDK
    if (keyboard[SCAN_P] && !prevkeyboard[SCAN_P] && game.exit == GAMEEXIT_NONE)
      game.pause ^= 1;
#else
    if (keyboard[SCAN_A] && !prevkeyboard[SCAN_A] && game.exit == GAMEEXIT_NONE)
      game.pause = 0;
#endif
    // if (keyboard[SCAN_R] && !prevkeyboard[SCAN_R])
    //  movie.record^=1;

    if (keyboard[SCAN_F] && !prevkeyboard[SCAN_F] && game.exit == GAMEEXIT_NONE)
      fps_enabled ^= 1;
    if (keyboard[SCAN_M] && !prevkeyboard[SCAN_M] && game.exit == GAMEEXIT_NONE)
      shadow_enabled ^= 1;

    if (keyboard[SCAN_9] || keyboard[SCAN_MINUS] || keyboard[SCAN_NUMMINUS])
      view.zoom *= 1.01f;
    if (view.zoom > 50.0f)
      view.zoom = 50.0f;
    if (keyboard[SCAN_0] || keyboard[SCAN_EQUALS] || keyboard[SCAN_NUMPLUS])
      view.zoom /= 1.01f;
    if (view.zoom < 4.0f)
      view.zoom = 4.0f;

    view.zoomy = view.zoom * 0.80f + 0.5f;
    // view.zoomy=view.zoom*0.5625f+0.5f;

    if (config.keepaspect == 2)
    {
      /* Expand mode: widen the world box to the render-area aspect, so one
         world unit stays the same number of pixels horizontally and
         vertically.  The vertical extent does not change - the extra width is
         the whole point.  view.zoomx follows, so the tile and light loops and
         the object culling in prerender.c cover the wider view. */
      float halfwidth = view.zoom * 0.75f * (float)renderareaw / (float)renderareah;

      view.zoomx = halfwidth + 0.5f;
      setuporthoviewport(0, 0, 640, 480, halfwidth, view.zoom * 0.75f, 20.0f);
    }
    else
    {
      view.zoomx = view.zoom + 0.5f;
      setuporthoviewport(0, 0, 640, 480, view.zoom, view.zoom * 0.75f, 20.0f);
    }
    // setuporthoviewport(0,0,640,480,view.zoom,view.zoom*0.5625f,20.0f);
    setupviewpoint(view.position, view.orientation);

    /* Retro pixelation (config.txt: warpzonepixelation=).  When it is on, the
       level is drawn into a 256x256 (level 64) or 128x128 (level 65) corner of
       the framebuffer, read back into a texture and drawn back magnified - see
       the copy and the quad below.  When it is off, the viewport set by
       setuporthoviewport() above is left alone, so the level renders
       full-screen at native resolution and the expensive read-back in
       glCopyTexImage2D never happens. */
    if (config.warpzonepixelation)
    {
      if (game.oldschool == 1) // || game.oldschool==3)
        glViewport(0, 0, 256, 256);
      if (game.oldschool == 2)
        glViewport(0, 0, 128, 128);
    }

    soundsimulation(view.position, view.orientation);

    if (lighting_enabled)
    {
      setupframelighting();
    }
    else if (fix_boss_logic_when_lights_off)
    { // cave6.lvl boss
      setupframelighting_fix_cave6_boss();
    }
    PERF_SEC(1);

    setuprenderobjects();
    PERF_SEC(2);

    if (shadow_enabled)
      rendershadows();
    PERF_SEC(3);

    renderlevelback();
    PERF_SEC(4);

    renderparticles();
    PERF_SEC(5);

    // if (!keyboard[SCAN_B])
    renderobjects();
    PERF_SEC(6);

    renderparticles2();
    PERF_SEC(7);

    renderlevel();
    PERF_SEC(8);
    renderlevelfore();
    PERF_SEC(9);

    /* Retro pixelation read-back, only with warpzonepixelation=1.  This single
       glCopyTexImage2D costs about 54 ms per frame on the Vita (the driver
       reads the framebuffer back in software), which is the whole difference
       between 60 fps and 13 fps in these two levels.  With the option off the
       level has already been rendered full-screen above, so there is nothing
       to read back or magnify. */
    if (config.warpzonepixelation)
    {
      if (game.oldschool == 1) // || game.oldschool==3)
      {
        glBindTexture(GL_TEXTURE_2D, texture[334].glname);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, 256, 256, 0);
      }
      if (game.oldschool == 2)
      {
        glBindTexture(GL_TEXTURE_2D, texture[333].glname);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, 128, 128, 0);
      }

      if (game.oldschool == 1 || game.oldschool == 2) // || game.oldschool==3)
      {
        setuptextdisplay();

        /* The magnified quad is eye-space at z = -1 too, so in expand mode the
           widened HUD frustum would leave it at 75.5% of the width.  Scale it
           back out, exactly like the full-screen fades below.  The next
           setuptextdisplay() (after the retro block) resets the modelview. */
        glMatrixMode(GL_MODELVIEW);
        glScalef(hudfrustumscale, 1.0f, 1.0f);

        if (game.oldschool == 1) // || game.oldschool==3)
          glBindTexture(GL_TEXTURE_2D, texture[334].glname);
        if (game.oldschool == 2)
          glBindTexture(GL_TEXTURE_2D, texture[333].glname);

#if defined(GLES)
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        GLfloat quad[] = {-1.0f, 0.75f, -1.0f, 1.0f, 0.75f, -1.0f, 1.0f, -0.75f, -1.0f, -1.0f, -0.75f, -1.0f};
        GLfloat tx[]   = {0, 1, 1, 1, 1, 0, 0, 0};
        glVertexPointer(3, GL_FLOAT, 0, quad);
        glTexCoordPointer(2, GL_FLOAT, 0, tx);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
#else
        glBegin(GL_QUADS);

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

        glTexCoord2f(0.0f, 1.0f);
        glVertex3f(-1.0f, 0.75f, -1.0f);

        glTexCoord2f(1.0f, 1.0f);
        glVertex3f(1.0f, 0.75f, -1.0f);

        glTexCoord2f(1.0f, 0.0f);
        glVertex3f(1.0f, -0.75f, -1.0f);

        glTexCoord2f(0.0f, 0.0f);
        glVertex3f(-1.0f, -0.75f, -1.0f);

        glEnd();
#endif
      }
    }
    /* the retro levels render into a 256x256 / 128x128 texture and blit it
       back fullscreen: its own section, it used to disappear inside "rest" */
    PERF_SEC(11);

    setuptextdisplay();

    if (fps_enabled)
    {
      fps_mean += fps;
      fps_renew--;
      if (fps_renew <= 0)
      {
        fps_cache = (float)fps_mean / 20.0;
        fps_renew = 20;
        fps_mean  = 0;
      }
      drawtext("/i", 40, 64, 16, 1.0f, 1.0f, 0.0f, 1.0f, fps_cache);
    }

    if (game.exit == GAMEEXIT_WON || game.exit == GAMEXIT_WARPZONE)
    {
      glDisable(GL_TEXTURE_2D);

      /* These quads are eye-space at z = -1, so the widened expand-mode HUD
         frustum would leave them at 75.5% of the screen width: scale the
         modelview back out.  hudfrustumscale is 1.0 outside expand mode. */
      glMatrixMode(GL_MODELVIEW);
      glScalef(hudfrustumscale, 1.0f, 1.0f);

#if !defined(GLES)
      glBegin(GL_QUADS);
#endif
      glColor4f(0.0f, 0.0f, 0.0f, (float)(100 - game.exitdelay) * 0.01f);
#if defined(GLES)
      glEnableClientState(GL_VERTEX_ARRAY);
      GLfloat quad[] = {-1.0f, 0.75f, -1.0f, 1.0f, 0.75f, -1.0f, 1.0f, -0.75f, -1.0f, -1.0f, -0.75f, -1.0f};
      glVertexPointer(3, GL_FLOAT, 0, quad);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      glDisableClientState(GL_VERTEX_ARRAY);
#else
      glVertex3f(-1.0f, 0.75f, -1.0f);
      glVertex3f(1.0f, 0.75f, -1.0f);
      glVertex3f(1.0f, -0.75f, -1.0f);
      glVertex3f(-1.0f, -0.75f, -1.0f);

      glEnd();
#endif
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glMatrixMode(GL_TEXTURE);
      glEnable(GL_TEXTURE_2D);
    }
    if (game.exit == GAMEEXIT_DIED)
    {
      glDisable(GL_TEXTURE_2D);

      glMatrixMode(GL_MODELVIEW);
      glScalef(hudfrustumscale, 1.0f, 1.0f);

#if !defined(GLES)
      glBegin(GL_QUADS);
#endif
      if (game.exitdelay > 50)
        glColor4f(0.5f, 0.0f, 0.0f, (float)(100 - game.exitdelay) * 0.01f);
      else
        glColor4f(1.0f - (float)(100 - game.exitdelay) * 0.01f, 0.0f, 0.0f, (float)(100 - game.exitdelay) * 0.01f);
#if defined(GLES)
      glEnableClientState(GL_VERTEX_ARRAY);
      GLfloat quad[] = {-1.0f, 0.75f, -1.0f, 1.0f, 0.75f, -1.0f, 1.0f, -0.75f, -1.0f, -1.0f, -0.75f, -1.0f};
      glVertexPointer(3, GL_FLOAT, 0, quad);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      glDisableClientState(GL_VERTEX_ARRAY);
#else
      glVertex3f(-1.0f, 0.75f, -1.0f);
      glVertex3f(1.0f, 0.75f, -1.0f);
      glVertex3f(1.0f, -0.75f, -1.0f);
      glVertex3f(-1.0f, -0.75f, -1.0f);

      glEnd();
#endif
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glMatrixMode(GL_TEXTURE);
      glEnable(GL_TEXTURE_2D);
    }

    if (game.oldschool == 0 && !game.bosslevel && level.gametype != GAMETYPE_2COLLECTION)
      rendersprites();

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    if (!disable_frame)
    {
      if (config.keepaspect == 2)
      {
        /* 16:9 border.  The art is 847x480 and the expand viewport is
           847.45 design units wide (960px at 1.1328 px/unit), so drawing the
           tiles 1:1 and shifting the run left by (847.45-640)/2 = 103.5 units
           centres it exactly on the screen.  drawbackground() masks negative
           x with 0xFFFF, so the shift is done with the modelview instead.
           The last column (x=591) and row (y=224) overlap the previous tile
           on purpose: every source block then lies inside the image, and the
           only double-drawn area is the transparent hole. */
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(-103.5f / 320.0f, 0.0f, 0.0f);

        drawbackground(640, 0, 0, 256, 256, 640, 480);
        drawbackground(641, 256, 0, 256, 256, 640, 480);
        drawbackground(642, 512, 0, 256, 256, 640, 480);
        drawbackground(643, 591, 0, 256, 256, 640, 480);
        drawbackground(644, 0, 224, 256, 256, 640, 480);
        drawbackground(645, 256, 224, 256, 256, 640, 480);
        drawbackground(646, 512, 224, 256, 256, 640, 480);
        drawbackground(647, 591, 224, 256, 256, 640, 480);

        glLoadIdentity();
        glMatrixMode(GL_TEXTURE); /* the state the rest of the HUD expects */
      }
      else
      {
        drawbackground(640, 0, 0, 256, 256, 640, 480);
        drawbackground(641, 256, 0, 256, 256, 640, 480);
        drawbackground(642, 512, 0, 256, 256, 640, 480);
        drawbackground(643, 0, 256, 256, 256, 640, 480);
        drawbackground(644, 256, 256, 256, 256, 640, 480);
        drawbackground(645, 512, 256, 256, 256, 640, 480);
      }
    }

    // if (game.turbomode)
    //  drawbackground(529,28,64,64,32,640,480);

    gamedisplay();

    drawmenuitems();

    if (game.exit == GAMEEXIT_DIED || game.exit == GAMEEXIT_WON || game.exit == GAMEXIT_WARPZONE)
      if (game.exitdelay < 20)
      {
        glDisable(GL_TEXTURE_2D);

        glMatrixMode(GL_MODELVIEW);
        glScalef(hudfrustumscale, 1.0f, 1.0f);
#if !defined(GLES)
        glBegin(GL_QUADS);
#endif
        glColor4f(0.0f, 0.0f, 0.0f, (float)(20 - game.exitdelay) * 0.05f);
#if defined(GLES)
        glEnableClientState(GL_VERTEX_ARRAY);
        GLfloat quad[] = {-1.0f, 0.75f, -1.0f, 1.0f, 0.75f, -1.0f, 1.0f, -0.75f, -1.0f, -1.0f, -0.75f, -1.0f};
        glVertexPointer(3, GL_FLOAT, 0, quad);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableClientState(GL_VERTEX_ARRAY);
#else
        glVertex3f(-1.0f, 0.75f, -1.0f);
        glVertex3f(1.0f, 0.75f, -1.0f);
        glVertex3f(1.0f, -0.75f, -1.0f);
        glVertex3f(-1.0f, -0.75f, -1.0f);

        glEnd();
#endif
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glMatrixMode(GL_TEXTURE);
        glEnable(GL_TEXTURE_2D);
      }
    if (movie.record)
    {
      if (movie.framenum < game.framenum / 2)
        recordframe();
      drawtext("RECORD", 0, 64, 16, 1.0f, 0.0f, 0.0f, 1.0f);
    }

    if (game.playreplay)
      drawtext(TXT_REPLAY, (612 | TEXT_END), 64, 16, 1.0f, 1.0f, 0.0f, 1.0f);

    if (game.pause && game.exit == GAMEEXIT_NONE)
    {
      drawtext(TXT_PAUSED, (320 | TEXT_CENTER), 240, 16, 1.0f, 1.0f, 1.0f, 1.0f);
      drawtext(TXT_PRESS_P, (320 | TEXT_CENTER), 256, 12, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    if (game.exit != GAMEEXIT_NONE || game.godmode)
      drawmousecursor(768 + font.cursornum, mouse.x, mouse.y, 16, 1.0f, 1.0f, 1.0f, 1.0f);

    PERF_SEC(10);
#if defined(GISH_INSTRUMENT)
    perf_t_sim0 = SDL_GetPerformanceCounter();
#endif
    simcount      = 0;
    game.simspeed = 20;
    if (game.turbomode)
      game.simspeed = 15;

    // Unlock Maximum FPS
    //    while (SDL_GetTicks()-simtimer<=game.simspeed)
    //      SDL_Delay(1);

    while (SDL_GetTicks() - simtimer > game.simspeed && simcount < 3)
    {
      simcount++;
      count    = SDL_GetTicks() - simtimer - game.simspeed;
      simtimer = SDL_GetTicks() - count;
      if (simcount == 3)
        simtimer = SDL_GetTicks();

      if (game.exitdelay > 0)
        if (!game.editing)
          game.exitdelay--;
      if (keyboard[SCAN_ESC])
        game.exitdelay = 0;

      if (game.exit == GAMEEXIT_NONE && !game.pause && game.dialog == 0 && !game.over)
      {
        getinputs();

        if (!game.playreplay)
          saveinputs();
        else
          loadinputs();

#if defined(GISH_INSTRUMENT)
        perf_simtick = SDL_GetPerformanceCounter();
#endif
        if (game.startdelay == 0)
          simulation();
        gamelogic();
        PERF_SIM(PERFSIM_LOGIC);
      }
    }
#if defined(GISH_INSTRUMENT)
    perf_t_sim1 = SDL_GetPerformanceCounter();
    {
      Uint64 dt = perf_t_sim1 - perf_t_sim0;
      perf_render += perf_t_sim0 - perf_now;
      perf_sim    += dt;
      if (dt > perf_sim_max)
        perf_sim_max = dt;
      perf_steps  += simcount;
      if (simcount >= 3)
        perf_catchup++;
    }
#endif

    if (game.levelnum == 0 && game.editing)
      if (keyboard[SCAN_F1] && !prevkeyboard[SCAN_F1])
      {
        game.songnum = -1;
        checkmusic();

        for (count = numofsounds - 1; count >= 0; count--)
          deletesound(count);

        setuptextdisplay();

        drawtext(TXT_LOADINGEDITOR, (320 | TEXT_CENTER), 240, 16, 1.0f, 1.0f, 1.0f, 1.0f);

#if defined(PC_GLES)
        eglSwapBuffers(eglDisplay, eglSurface);
#else
        SDL_GL_SwapWindow(globalwindow);
#endif

        for (count = 0; count < 20; count++)
          if (animation[count].loaded == 0)
            animation[count].loaded = 2;

        loadanimations();

        editlevel();

        savelevel("backup.lvl");

        simtimer = SDL_GetTicks();
      }

#if defined(PC_GLES)
    eglSwapBuffers(eglDisplay, eglSurface);
#else
    SDL_GL_SwapWindow(globalwindow);
#endif

    if ((SDL_GetTicks() - frametimer) != 0)
      fps = 1000 / (SDL_GetTicks() - frametimer);
#if defined(GISH_INSTRUMENT)
    {
      Uint64 perf_end  = SDL_GetPerformanceCounter();
      Uint64 frameus   = perf_end - perf_now; /* "frame" is the global light/scene struct */
      if (perf_prev != 0)
      {
        perf_frames++;
        if (frameus > perf_frame_max)
          perf_frame_max = frameus;
        /* PERF INSTRUMENTATION (temporary): peak scene/draw numbers */
        if (perf_leveltiles > perf_maxleveltiles)
          perf_maxleveltiles = perf_leveltiles;
        if (perf_leveldraws > perf_maxleveldraws)
          perf_maxleveldraws = perf_leveldraws;
        if (perf_levelflush > perf_maxlevelflush)
          perf_maxlevelflush = perf_levelflush;
        if (numofobjectrenders > perf_maxrenders)
          perf_maxrenders = numofobjectrenders;
        if (frame.numoflights > perf_maxlights)
          perf_maxlights = frame.numoflights;
        /* One report every 5 s, not every 1 s: a report costs ~14 ms of one
           frame (stderr -> Cat-A-Log socket writes), i.e. a visible hitch. */
        if (perf_end - perf_window >= perf_freq * 5)
        {
          double k   = 1000.0 / (double)perf_freq;
          double n   = (double)perf_frames;
          double avg = k * (double)(perf_end - perf_window) / n;
          double r   = k * (double)perf_render / n;
          double s   = k * (double)perf_sim / n;
          double v[12];
          int i;
          int nsteps = perf_steps > 0 ? perf_steps : 1;
          double sv[PERFSIM_SLOTS], wv[PERFSIM_SLOTS], stot = 0.0;
          static Uint64 perf_rep_max  = 0; /* worst report cost seen      */
          static Uint64 perf_rep_prev = 0; /* cost of the previous report */
          Uint64        perf_rep0;
          Uint32        perf_tickms;
          double        perf_winms = k * (double)(perf_end - perf_window);
          for (i = 0; i < 12; i++)
            v[i] = k * (double)perf_sec[i] / n;
          for (i = 0; i < PERFSIM_SLOTS; i++)
          {
            sv[i] = k * (double)perf_simsec[i] / (double)nsteps;
            wv[i] = k * (double)perf_simsec_max[i];
            stot += sv[i];
          }
          /* cost of producing this report itself - the one thing that can make
             logging look like a game hitch */
          perf_rep0  = SDL_GetPerformanceCounter();
          perf_linelen = 0;
          PERF_EMIT("PERF %d fps | frame %.2f (max %.2f) | render %.2f | sim %.2f (max %.2f) | wait %.2f | steps %d catchup %d\n",
                    perf_frames, avg, k * (double)perf_frame_max, r, s,
                    k * (double)perf_sim_max, avg - r - s, perf_steps, perf_catchup);
          PERF_EMIT("PERF2 bg %.2f light %.2f setup %.2f shad %.2f | lvlb %.2f part %.2f obj %.2f part2 %.2f | lvl %.2f lvlf %.2f pix %.2f rest %.2f\n",
                    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[11], v[10]);
          PERF_EMIT("PERF3 worst-ms bg %.2f setup %.2f lvlb %.2f obj %.2f lvl %.2f lvlf %.2f pix %.2f rest %.2f\n",
                    k * (double)perf_sec_max[0], k * (double)perf_sec_max[2],
                    k * (double)perf_sec_max[4], k * (double)perf_sec_max[6],
                    k * (double)perf_sec_max[8], k * (double)perf_sec_max[9],
                    k * (double)perf_sec_max[11], k * (double)perf_sec_max[10]);
          PERF_EMIT("SIM2 ms/step objworld %.2f objobj %.2f bonds %.2f parts %.2f | cycle %.2f check %.2f ttl %.2f | sprite %.2f boss %.2f dmg %.2f | logic %.2f | sum %.2f\n",
                    sv[PERFSIM_OBJWORLD], sv[PERFSIM_OBJOBJ], sv[PERFSIM_BONDS], sv[PERFSIM_PARTICLES],
                    sv[PERFSIM_CYCLE], sv[PERFSIM_CHECKBONDS], sv[PERFSIM_OBJTTL],
                    sv[PERFSIM_SPRITES], sv[PERFSIM_BOSS], sv[PERFSIM_DAMAGE],
                    sv[PERFSIM_LOGIC], stot);
          PERF_EMIT("SIM3 worst-ms objworld %.2f objobj %.2f bonds %.2f parts %.2f | cycle %.2f check %.2f ttl %.2f | sprite %.2f boss %.2f dmg %.2f | logic %.2f\n",
                    wv[PERFSIM_OBJWORLD], wv[PERFSIM_OBJOBJ], wv[PERFSIM_BONDS], wv[PERFSIM_PARTICLES],
                    wv[PERFSIM_CYCLE], wv[PERFSIM_CHECKBONDS], wv[PERFSIM_OBJTTL],
                    wv[PERFSIM_SPRITES], wv[PERFSIM_BOSS], wv[PERFSIM_DAMAGE],
                    wv[PERFSIM_LOGIC]);
          PERF_EMIT("BOND ms/step pre %.2f copy %.2f glob %.2f temp %.2f rest %.2f calib %.3f | ctmp avg %.0f peak %d | "
                    "it/step t0 %.0f t1 %.0f t2 %.0f t4 %.0f t5 %.0f bro %.0f\n",
                    k * (double)perf_bond_us[PERFBOND_PRE] / (double)nsteps,
                    k * (double)perf_bond_us[PERFBOND_COPY] / (double)nsteps,
                    k * (double)perf_bond_us[PERFBOND_GLOBAL] / (double)nsteps,
                    k * (double)perf_bond_us[PERFBOND_TEMP] / (double)nsteps,
                    k * (double)perf_bond_us[PERFBOND_REST] / (double)nsteps,
                    k * (double)perf_bond_us[PERFBOND_CALIB] / (double)nsteps,
                    (double)perf_bond_temps / (double)nsteps, perf_bond_max,
                    (double)perf_bond_itcnt[0] / (double)nsteps,
                    (double)perf_bond_itcnt[1] / (double)nsteps,
                    (double)perf_bond_itcnt[2] / (double)nsteps,
                    (double)perf_bond_itcnt[4] / (double)nsteps,
                    (double)perf_bond_itcnt[5] / (double)nsteps,
                    (double)perf_bond_itcnt[6] / (double)nsteps);
          /* SDL_GetTicks vs the hi-res counter over the same window: the step
             loop runs on the former, every ms figure below on the latter. */
          perf_tickms = SDL_GetTicks() - perf_tick0;
          perf_tick0  = SDL_GetTicks();
          PERF_EMIT("SCENE max objs %d bonds %d parts %d | tiles %d draws %d flush %d | renders %d lights %d | level %d zoom %.1f old %d | report %.2f (max %.2f) | ticks %u/%.0f\n",
                    perf_maxobjs, perf_maxbonds, perf_maxparts,
                    perf_maxleveltiles, perf_maxleveldraws, perf_maxlevelflush,
                    perf_maxrenders, perf_maxlights,
                    game.levelnum, (double)view.zoom, game.oldschool,
                    k * (double)perf_rep_prev, k * (double)perf_rep_max,
                    (unsigned)perf_tickms, perf_winms);
          perfreport(perf_linebuf);
          perf_rep_prev = SDL_GetPerformanceCounter() - perf_rep0;
          if (perf_rep_prev > perf_rep_max)
            perf_rep_max = perf_rep_prev;
          perf_frames = perf_steps = perf_catchup = 0;
          perf_render = perf_sim = perf_sim_max = perf_frame_max = 0;
          perf_maxobjs = perf_maxbonds = perf_maxparts = 0;
          perf_maxleveltiles = perf_maxleveldraws = perf_maxlevelflush = 0;
          perf_maxrenders = perf_maxlights = 0;
          for (i = 0; i < 12; i++)
            perf_sec[i] = perf_sec_max[i] = 0;
          for (i = 0; i < PERFSIM_SLOTS; i++)
            perf_simsec[i] = perf_simsec_max[i] = 0;
          for (i = 0; i < PERFBOND_SLOTS; i++)
            perf_bond_us[i] = 0;
          for (i = 0; i < PERFBOND_TYPES; i++)
            perf_bond_itcnt[i] = 0;
          perf_bond_max   = 0;
          perf_bond_temps = 0;
          perf_window     = perf_end;
        }
      }
      else
      {
        perf_window = perf_end;
      }
      perf_prev = perf_end;
    }
#endif /* GISH_INSTRUMENT */
  }

  game.songnum = -1;
  checkmusic();

  for (count = numofsounds - 1; count >= 0; count--)
    deletesound(count);

  resetmenuitems();
}

void simulation(void)
{
  int count, count2;
  float vec[3], vec2[3];
  // float intersectpoint[3];
  // float normal[3];
  float scale;

  game.framenum++;

  if (game.godmode)
    if (!game.playreplay)
    {
      vec[0] = view.position[0] + (float)(mouse.x - 320) / 32.0f;
      vec[1] = view.position[1] + (float)(240 - mouse.y) / 32.0f;
      vec[2] = 0.0f;

      if (mouse.lmb)
      {
        if (game.godparticle == -1)
        {
          game.godparticle = numofparticles;
          createparticle(16, vec, NULL, 10000.0f, -1, 10000);

          for (count = 0; count < numofparticles; count++)
            if (count != game.godparticle)
              if (particle[count].type == 3)
              {
                subtractvectors(vec2, vec, particle[count].position);
                if (vectorlength(vec2) < 3.0f)
                  createbond(count, game.godparticle, 16, -1);
              }
        }
        else
        {
          subtractvectors(vec2, vec, particle[game.godparticle].position);
          scaleaddvectors2(vec2, vec2, particle[game.godparticle].velocity, -4.0f);

          scale = vectorlength(vec2);
          normalizevector(vec2, vec2);
          scale *= 0.01f;

          scaleaddvectors2(particle[game.godparticle].velocity, particle[game.godparticle].velocity, vec2, scale);
        }
      }
      else
      {
        if (game.godparticle != -1)
        {
          deleteparticle(game.godparticle);
          game.godparticle = -1;
        }
      }
    }

  particlesimulation();
  particletimetolive();

  // physicstemp.numofbonds=0;

  for (count = 0; count < numofobjects; count++)
    object[count].prevhitpoints = object[count].hitpoints;

  if (level.gametype != GAMETYPE_2RACING)
    for (count = 0; count < numofparticles; count++)
      particle[count].velocity[1] -= particle[count].gravity;

  for (count = 0; count < numofparticles; count++)
    if (particle[count].levelcollision)
      particlecollision(count);
  PERF_SIM(PERFSIM_PARTICLES);

  for (count = 0; count < numofobjects; count++)
  {
    objectcollision(count);
  }
  PERF_SIM(PERFSIM_OBJWORLD);

  for (count = 0; count < numofobjects; count++)
  {
    objectcollisionobject(count);
  }
  PERF_SIM(PERFSIM_OBJOBJ);

  objectcycle();
  objectanimation();
  PERF_SIM(PERFSIM_CYCLE);

  bondsimulation2();
  PERF_SIM(PERFSIM_BONDS);

  checkbonds();
  PERF_SIM(PERFSIM_CHECKBONDS);

  objecttimetolive();
  PERF_SIM(PERFSIM_OBJTTL);

  spritesimulation();
  spritetimetolive();
  PERF_SIM(PERFSIM_SPRITES);

  bosssimulation();
  bosstimetolive();
  PERF_SIM(PERFSIM_BOSS);

  for (count = 0; count < numofobjects; count++)
  {
    if (object[count].type == 1)
    {
      if (object[count].hitpoints < 0)
        object[count].hitpoints = 0;

      if (object[count].hitpoints < object[count].prevhitpoints)
      {
        for (count2 = 0; count2 <= (object[count].prevhitpoints - object[count].hitpoints) / 20; count2++)
        {
          vec[0] = (float)((rnd() & 255) - 127) / 1270.0f;
          vec[1] = (float)((rnd() & 255) - 127) / 1270.0f;
          vec[2] = 0.0f;
          addvectors(vec, vec, object[count].velocity);

          createparticle(5, object[count].position, vec, 0.25f, -1, 100 + (rnd() & 63));
          particle[numofparticles - 1].rendersize = 0.125 + (float)(rnd() & 127) / 1000.0f;
          if (count == 0)
            particle[numofparticles - 1].texturenum = 368;
          else if (count == 1)
            particle[numofparticles - 1].texturenum = 363;
          else if (count == 2)
            particle[numofparticles - 1].texturenum = 383 + 40;
          else
            particle[numofparticles - 1].texturenum = 383 + 60;
        }
        object[count].frame      = 12;
        object[count].framedelay = 0.0f;
        playsound(16, object[count].position, NULL, 0.5f, 0, 1.0f, count, 2);
#ifdef ANDROID_NDK
        doVibrateFromJNI(10);
#endif
      }
    }
  }
  PERF_SIM(PERFSIM_DAMAGE);

#if defined(GISH_INSTRUMENT)
  /* peak scene size in this window */
  if (numofobjects > perf_maxobjs)
    perf_maxobjs = numofobjects;
  if (numofbonds > perf_maxbonds)
    perf_maxbonds = numofbonds;
  if (numofparticles > perf_maxparts)
    perf_maxparts = numofparticles;
#endif
}

void getinputs(void)
{
  int count;

  for (count = 0; count < 4; count++)
  {
    object[count].axis[0] = 0.0f;
    object[count].axis[1] = 0.0f;
    object[count].button  = 0;
  }
  for (count = 0; count < CONTROLS_LENGTH; count++)
  {
    if (keyboard[control[count].key[KEYALIAS_LEFT]])
      object[count].axis[0] -= 1.0f;
    if (keyboard[control[count].key[KEYALIAS_RIGHT]])
      object[count].axis[0] += 1.0f;
    if (keyboard[control[count].key[KEYALIAS_DOWN]])
      object[count].axis[1] -= 1.0f;
    if (keyboard[control[count].key[KEYALIAS_UP]])
      object[count].axis[1] += 1.0f;
    if (keyboard[control[count].key[KEYALIAS_STICK]])
      object[count].button |= 1;
    if (keyboard[control[count].key[KEYALIAS_JUMP]])
      object[count].button |= 2;
    if (keyboard[control[count].key[KEYALIAS_SLIDE]])
      object[count].button |= 4;
    if (keyboard[control[count].key[KEYALIAS_HEAVY]])
      object[count].button |= 8;

    if (control[count].joysticknum != -1)
    {
      object[count].axis[0] += joystick[control[count].joysticknum].axis[0];
      object[count].axis[1] += joystick[control[count].joysticknum].axis[1];

      if (control[count].button[0] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[0]])
          object[count].axis[0] -= 1.0f;
      if (control[count].button[1] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[1]])
          object[count].axis[0] += 1.0f;
      if (control[count].button[2] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[2]])
          object[count].axis[1] -= 1.0f;
      if (control[count].button[3] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[3]])
          object[count].axis[1] += 1.0f;

      if (control[count].button[4] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[4]])
          object[count].button |= 1;
      if (control[count].button[5] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[5]])
          object[count].button |= 2;
      if (control[count].button[6] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[6]])
          object[count].button |= 4;
      if (control[count].button[7] != -1)
        if (joystick[control[count].joysticknum].button[control[count].button[7]])
          object[count].button |= 8;
    }
    if (object[count].axis[0] < -1.0f)
      object[count].axis[0] = -1.0f;
    if (object[count].axis[0] > 1.0f)
      object[count].axis[0] = 1.0f;
    if (object[count].axis[1] < -1.0f)
      object[count].axis[1] = -1.0f;
    if (object[count].axis[1] > 1.0f)
      object[count].axis[1] = 1.0f;
  }
}
