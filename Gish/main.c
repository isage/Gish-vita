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

#include "audio/audio.h"
#include "config.h"
#include "game/config.h"
#include "game/high.h"
#include "game/mainmenu.h"
#include "game/options.h"
#include "game/player.h"
#include "input/joystick.h"
#include "menu/menu.h"
#include "sdl/endian.h"
#include "sdl/event.h"
#include "sdl/sdl.h"
#include "sdl/video.h"
#include "video/opengl.h"
#include "video/text.h"
#include "video/texture.h"

//#include <psp2/kernel/modulemgr.h>
#include <psp2/power.h>
//#include <services.h>


int _newlib_heap_size_user   = 100 * 1024 * 1024;
unsigned int sceLibcHeapSize = 50 * 1024 * 1024;

#ifdef PC_GLES

EGLDisplay eglDisplay;
EGLConfig glConfig;
EGLContext eglContext;
EGLSurface eglSurface;
const char *gl_vendor, *gl_renderer, *gl_version, *gl_extensions;
#endif

char *gishDataPath = (char *)"ux0:/data/gish/";

int main(int argc, char *argv[])
{
  int count;
  int flags;
  const char *temp;
/*
  sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, NULL, 0, NULL, NULL);
  sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, NULL, 0, NULL, NULL);
  sceKernelLoadStartModule("app0:libgpu_es4_ext.suprx", 0, NULL, 0, NULL, NULL);
  sceKernelLoadStartModule("app0:libIMGEGL.suprx", 0, NULL, 0, NULL, NULL);

  PVRSRV_PSP2_APPHINT hint;

  PVRSRVInitializeAppHint(&hint);
  PVRSRVCreateVirtualAppHint(&hint);
*/
  scePowerSetArmClockFrequency(444);
  scePowerSetGpuClockFrequency(222);

  checkbigendian();

  loadconfig();
  loadscores();
  loadplayers();

  flags = SDL_INIT_VIDEO | SDL_INIT_TIMER;

  SDL_Init(flags);

  int display_count = 0, display_index = 0, mode_index = 0;
  SDL_DisplayMode mode = {SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0};

  if ((display_count = SDL_GetNumVideoDisplays()) < 1)
  {
    TO_DEBUG_LOG("SDL_GetNumVideoDisplays returned: %i\n", display_count);
    return 1;
  }

  if (SDL_GetDisplayMode(display_index, mode_index, &mode) != 0)
  {
    TO_DEBUG_LOG("SDL_GetDisplayMode failed: %s\n", SDL_GetError());
    return 1;
  }

  TO_DEBUG_LOG("SDL_GetDisplayMode(0, 0, &mode): %i bpp %ix%i\n", SDL_BITSPERPIXEL(mode.format), mode.w, mode.h);

  if (SDL_BITSPERPIXEL(mode.format) == 16)
    config.bitsperpixel = 16;

  saveconfig();

  SDL_ShowCursor(SDL_DISABLE);

  listvideomodes();

  TO_DEBUG_LOG("Main.c Opening screen %dx%dx%d depth: %d, stencil: %d\n", windowinfo.resolutionx,
               windowinfo.resolutiony, windowinfo.bitsperpixel, windowinfo.depthbits, windowinfo.stencilbits);
/*
  EGLint egl_config_attr[] = {
    EGL_BUFFER_SIZE,  16,
    EGL_DEPTH_SIZE,   16,
    EGL_STENCIL_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };

  EGLint numConfigs, majorVersion, minorVersion;
*/
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

  globalwindow = SDL_CreateWindow("Gish GLES", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowinfo.resolutionx,
                                  windowinfo.resolutiony, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);

  SDL_GLContext glc = SDL_GL_CreateContext(globalwindow);
/*

  eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(eglDisplay, &majorVersion, &minorVersion);
  eglChooseConfig(eglDisplay, egl_config_attr, &glConfig, 1, &numConfigs);
  SDL_SysWMinfo sysInfo;
  SDL_VERSION(&sysInfo.version); // Set SDL version
  SDL_GetWindowWMInfo(globalwindow, &sysInfo);

  eglSurface = eglCreateWindowSurface(eglDisplay, glConfig, (EGLNativeWindowType)0, NULL);
  eglContext = eglCreateContext(eglDisplay, glConfig, EGL_NO_CONTEXT, NULL);
  eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
  eglSwapInterval(eglDisplay, (EGLint)1); // VSYNC
*/
  if (globalwindow == NULL)
  {
    printf("No SDL screen\n");
  }

  loadglextentions();

  for (count = 0; count < 2048; count++)
    glGenTextures(1, &texture[count].glname);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);

  if (config.joystick)
  {
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    numofjoysticks = SDL_NumJoysticks();
    for (count = 0; count < numofjoysticks; count++)
    {
      joy[count] = SDL_JoystickOpen(count);
      temp       = SDL_JoystickName(joy[count]);
      strcpy(joystick[count].name, temp);
      joystick[count].numofbuttons = SDL_JoystickNumButtons(joy[count]);
      joystick[count].numofhats    = SDL_JoystickNumHats(joy[count]);
    }

    SDL_JoystickEventState(SDL_IGNORE);
  }

  font.texturenum = 0;
  font.cursornum  = 0;
  font.sizex      = 640;
  font.sizey      = 480;

  loadtexturetga_app(1000, "font00.tga", 0, GL_CLAMP, GL_CLAMP, GL_LINEAR, GL_LINEAR);
  loadtexturetga(1001, "font01.tga", 0, GL_CLAMP, GL_CLAMP, GL_LINEAR, GL_LINEAR);

  loadtexturetga(768, "mouse00.tga", 0, GL_CLAMP, GL_CLAMP, GL_LINEAR, GL_LINEAR);
  loadtexturetga(769, "mouse00.tga", 0, GL_CLAMP, GL_CLAMP, GL_LINEAR, GL_LINEAR);

  setupmenuitems();

  if (!glext.multitexture)
  {
    notsupportedmenu();

    SDL_MinimizeWindow(globalwindow);
    SDL_Quit();
    return (0);
  }

  if (config.sound)
    setupaudio();

  mainmenu();

  saveconfig();
  savescores();
  saveplayers();

  if (config.sound)
    shutdownaudio();

//  eglDestroySurface(eglDisplay, eglSurface);
//  eglDestroyContext(eglDisplay, eglContext);
//  eglTerminate(eglDisplay);

  SDL_Quit();

  return (0);
}
