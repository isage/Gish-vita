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

#include "../config.h"
#include "../video/opengl.h"

#if defined(LINUX) || defined(MAC)
#include <unistd.h>
#endif

#include "../game/record.h"
#include "../sdl/event.h"
#include "../video/glfunc.h"

#include <stdio.h>
#include <string.h>

_movie movie;

void recordframe(void)
{
  int count, count2;
  int changeddir;
  char filename[16] = "gish0000.bmp";
  short bmpheader[27]
      = {19778, 4150, 14, 0, 0, 54, 0, 40, 0, 480, 0, 272, 0, 1, 24, 0, 0, 4096, 14, 0, 0, 0, 0, 0, 0, 0};
  int red, green, blue;
  FILE *fp;
  char filepath[256];

#if !defined(GLES)
  glReadBuffer(GL_BACK);
#endif
  glReadPixels(0, 0, windowinfo.resolutionx, windowinfo.resolutiony, GL_RGBA, GL_UNSIGNED_BYTE, screenshotbuffer);

  filename[4] = 48 + (movie.framenum / 1000) % 10;
  filename[5] = 48 + (movie.framenum / 100) % 10;
  filename[6] = 48 + (movie.framenum / 10) % 10;
  filename[7] = 48 + movie.framenum % 10;

  strcpy(filepath, gishDataPath);
  strcat(filepath, "movie/");
  strcat(filepath, filename);

  if ((fp = fopen(filepath, "wb")) != NULL)
  {
    fwrite(bmpheader, 27, 2, fp);

    for (count = 0; count < windowinfo.resolutiony; count++)
    {
      for (count2 = 0; count2 < windowinfo.resolutionx; count2++)
      {
        red   = screenshotbuffer[count * windowinfo.resolutionx + count2] & 0xFF;
        green = (screenshotbuffer[count * windowinfo.resolutionx + count2] >> 8) & 0xFF;
        blue  = (screenshotbuffer[count * windowinfo.resolutionx + count2] >> 16) & 0xFF;
        fputc(blue, fp);
        fputc(green, fp);
        fputc(red, fp);
      }
    }
    fclose(fp);
  }

  movie.framenum++;
}
