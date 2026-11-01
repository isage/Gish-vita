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

#include "../game/render.h"

#include "../audio/audio.h"
#include "../config.h"
#include "../game/block.h"
#include "../game/editor.h"
#include "../game/game.h"
#include "../game/gameobject.h"
#include "../game/level.h"
#include "../game/lighting.h"
#include "../game/physics.h"
#include "../game/prerender.h"
#include "../game/sprite.h"
#include "../math/vector.h"
#include "../physics/bond.h"
#include "../physics/particle.h"
#include "../sdl/event.h"
#include "../video/opengl.h"
#include "../video/text.h"
#include "../video/texture.h"

#include <math.h>

_frame frame;

/* ---------------------------------------------------------------------------
   Level tile batching

   The level passes used to issue one glDrawArrays per visible cell (~700 per
   layer at ~8 us each, i.e. ~5 ms per layer).  Every cell of a layer draws
   exactly its own 1x1 cell with axis-aligned geometry, so tiles inside one
   layer never overlap and their drawing order cannot matter: whatever the
   order, each cell ends up covered by its own tile only.  That lets us group
   the visible cells by block number - which is also the texture index - and
   emit one draw call per texture instead of one per tile.

   Usage: levelbatch_begin(), levelbatch_add() per visible cell, then
   levelbatch_flush().  The caller owns the colour and the client arrays:
   colour is set once per layer, GL_VERTEX_ARRAY / GL_TEXTURE_COORD_ARRAY are
   enabled before begin() and disabled after flush().  Nothing is allocated.
   --------------------------------------------------------------------------- */

#define LEVELBATCH_MAX 1024    /* ~840 visible cells at the largest zoom */
#define LEVELBATCH_BLOCKS 1024 /* block[] bound, block number == texture index */

static int   levelbatch_block[LEVELBATCH_MAX];
static float levelbatch_x[LEVELBATCH_MAX];
static float levelbatch_y[LEVELBATCH_MAX];
static int   levelbatch_num;

static int   levelbatch_start[LEVELBATCH_BLOCKS];
static int   levelbatch_count[LEVELBATCH_BLOCKS];
static int   levelbatch_cursor[LEVELBATCH_BLOCKS];
static int   levelbatch_order[LEVELBATCH_MAX];
static float levelbatch_verts[LEVELBATCH_MAX * 12];
static float levelbatch_texco[LEVELBATCH_MAX * 12];

static void levelbatch_sort(void)
{
  int blocknum, count, i, num;

  num = levelbatch_num;
  if (num == 0)
    return;

  /* counting sort by block number, which doubles as the texture index */
  for (blocknum = 0; blocknum < LEVELBATCH_BLOCKS; blocknum++)
    levelbatch_count[blocknum] = 0;
  for (i = 0; i < num; i++)
    levelbatch_count[levelbatch_block[i]]++;

  count = 0;
  for (blocknum = 0; blocknum < LEVELBATCH_BLOCKS; blocknum++)
  {
    levelbatch_start[blocknum]  = count;
    levelbatch_cursor[blocknum] = count;
    count += levelbatch_count[blocknum];
  }
  for (i = 0; i < num; i++)
    levelbatch_order[levelbatch_cursor[levelbatch_block[i]]++] = i;
}

static void levelbatch_flush(void)
{
  int blocknum, count, i, num;
  int first, numquads;
  float *vp, *tp;

  num = levelbatch_num;
  if (num == 0)
    return;

  levelbatch_sort();

  for (blocknum = 1; blocknum < LEVELBATCH_BLOCKS; blocknum++)
  {
    numquads = levelbatch_count[blocknum];
    first    = levelbatch_start[blocknum];
    if (numquads == 0)
      continue;

    glBindTexture(GL_TEXTURE_2D, texture[blocknum].glname);

#if defined(GLES)
    /* same vertices, same texcoords and same winding as the old
       GL_TRIANGLE_FAN, emitted as two triangles per cell */
    vp = levelbatch_verts;
    tp = levelbatch_texco;
    for (i = 0; i < numquads; i++)
    {
      int   q = levelbatch_order[first + i];
      float x = levelbatch_x[q];
      float y = levelbatch_y[q];

      *vp++ = x;     *vp++ = y + 1.0f; *tp++ = 0.0f; *tp++ = 0.0f;
      *vp++ = x + 1.0f; *vp++ = y + 1.0f; *tp++ = 1.0f; *tp++ = 0.0f;
      *vp++ = x + 1.0f; *vp++ = y;     *tp++ = 1.0f; *tp++ = 1.0f;
      *vp++ = x;     *vp++ = y + 1.0f; *tp++ = 0.0f; *tp++ = 0.0f;
      *vp++ = x + 1.0f; *vp++ = y;     *tp++ = 1.0f; *tp++ = 1.0f;
      *vp++ = x;     *vp++ = y;     *tp++ = 0.0f; *tp++ = 1.0f;
    }
    glVertexPointer(2, GL_FLOAT, 0, levelbatch_verts);
    glTexCoordPointer(2, GL_FLOAT, 0, levelbatch_texco);
    glDrawArrays(GL_TRIANGLES, 0, numquads * 6);
#else
    glBegin(GL_QUADS);
    for (i = 0; i < numquads; i++)
    {
      int   q = levelbatch_order[first + i];
      float x = levelbatch_x[q];
      float y = levelbatch_y[q];

      glTexCoord2f(0.0f, 0.0f);
      glVertex3f(x, y + 1.0f, 0.0f);
      glTexCoord2f(1.0f, 0.0f);
      glVertex3f(x + 1.0f, y + 1.0f, 0.0f);
      glTexCoord2f(1.0f, 1.0f);
      glVertex3f(x + 1.0f, y, 0.0f);
      glTexCoord2f(0.0f, 1.0f);
      glVertex3f(x, y, 0.0f);
    }
    glEnd();
#endif
  }

  levelbatch_num = 0;
}

static void levelbatch_begin(void)
{
  levelbatch_num = 0;
}

static void levelbatch_add(int blocknum, int x, int y)
{
  /* block number doubles as the texture index; out of range means the level
     data is broken, in which case the old code read past block[] - skip it */
  if (blocknum <= 0 || blocknum >= LEVELBATCH_BLOCKS)
    return;

  if (levelbatch_num >= LEVELBATCH_MAX)
    levelbatch_flush();

  levelbatch_block[levelbatch_num] = blocknum;
  levelbatch_x[levelbatch_num]     = (float)x;
  levelbatch_y[levelbatch_num]     = (float)y;
  levelbatch_num++;
}

/* ---------------------------------------------------------------------------
   Level light pass batching

   Both level light passes used to issue one glDrawArrays per lit tile (and in
   renderlevel() one per tile edge) plus a glBindTexture whenever the tile
   texture changed - thousands of driver calls per frame at ~10 us each, which
   made these two passes 16 of the 28 ms a lit frame cost.

   The tile scan is shared with the ambient pass: levelbatch_add() collects the
   lit cells, levelbatch_sort() groups them by block number (= texture index),
   and each group is emitted into one interleaved vertex array and drawn with a
   single glDrawArrays.  Two layouts are needed:

     renderlevelback(): pos(2) unit0 texcoord(2) unit1 light-gradient(2)
     renderlevel():     pos(2) unit0 texcoord(2) RGBA colour(4)

   Both are switched to GL_TRIANGLES: the old code drew GL_TRIANGLE_FAN of 4
   vertices, and the same corners are kept in the same order, so 0,1,2 + 0,2,3
   covers exactly the same pixels.
   --------------------------------------------------------------------------- */

#define LIGHTBATCH_MAXQUADS 512

/* renderlevelback(): gradient light over whole tiles */
static float gradbatch_verts[LIGHTBATCH_MAXQUADS * 6 * 6];
static int   gradbatch_quads;

static void gradbatch_flush(void)
{
  if (gradbatch_quads == 0)
    return;

#if defined(GLES)
  glClientActiveTexture(GL_TEXTURE1);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 6 * sizeof(float), gradbatch_verts + 4);
  glClientActiveTexture(GL_TEXTURE0);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 6 * sizeof(float), gradbatch_verts + 2);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 6 * sizeof(float), gradbatch_verts);
  glDrawArrays(GL_TRIANGLES, 0, gradbatch_quads * 6);
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY); /* unit 0 */
  glClientActiveTexture(GL_TEXTURE1);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY); /* unit 1 */
  glClientActiveTexture(GL_TEXTURE0);
#endif

  gradbatch_quads = 0;
}

static void gradbatch_add(float x, float y, const float *lightpos, float inv)
{
#if defined(GLES)
  static const float u[4]   = {0.0f, 1.0f, 1.0f, 0.0f};
  static const float v[4]   = {0.0f, 0.0f, 1.0f, 1.0f};
  static const int   idx[6] = {0, 1, 2, 0, 2, 3};
  float px[4], py[4];
  float *p;
  int i, q;

  if (gradbatch_quads >= LIGHTBATCH_MAXQUADS)
    gradbatch_flush();

  px[0] = x;        py[0] = y + 1.0f;
  px[1] = x + 1.0f; py[1] = y + 1.0f;
  px[2] = x + 1.0f; py[2] = y;
  px[3] = x;        py[3] = y;

  p = gradbatch_verts + gradbatch_quads * 36;
  for (i = 0; i < 6; i++)
  {
    q = idx[i];
    *p++ = px[q];
    *p++ = py[q];
    *p++ = u[q];
    *p++ = v[q];
    *p++ = (px[q] - lightpos[0]) * inv + 0.5f;
    *p++ = (py[q] - lightpos[1]) * inv + 0.5f;
  }
  gradbatch_quads++;
#else
  (void)x;
  (void)y;
  (void)lightpos;
  (void)inv;
#endif
}

/* renderlevel(): per-edge strips with a per-vertex colour */
static float edgebatch_verts[LIGHTBATCH_MAXQUADS * 6 * 8];
static int   edgebatch_quads;

static void edgebatch_flush(void)
{
  if (edgebatch_quads == 0)
    return;

#if defined(GLES)
  glActiveTextureARB(GL_TEXTURE0_ARB);
  glClientActiveTexture(GL_TEXTURE0);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 8 * sizeof(float), edgebatch_verts + 2);
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_FLOAT, 8 * sizeof(float), edgebatch_verts + 4);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 8 * sizeof(float), edgebatch_verts);
  glDrawArrays(GL_TRIANGLES, 0, edgebatch_quads * 6);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
#endif

  edgebatch_quads = 0;
}

static void edgebatch_add(const float *quad, const float *tx, const float *color)
{
#if defined(GLES)
  static const int idx[6] = {0, 1, 2, 0, 2, 3};
  float *p;
  int i, q;

  if (edgebatch_quads >= LIGHTBATCH_MAXQUADS)
    edgebatch_flush();

  p = edgebatch_verts + edgebatch_quads * 48;
  for (i = 0; i < 6; i++)
  {
    q = idx[i];
    *p++ = quad[q * 2 + 0];
    *p++ = quad[q * 2 + 1];
    *p++ = tx[q * 2 + 0];
    *p++ = tx[q * 2 + 1];
    *p++ = color[q * 4 + 0];
    *p++ = color[q * 4 + 1];
    *p++ = color[q * 4 + 2];
    *p++ = color[q * 4 + 3];
  }
  edgebatch_quads++;
#else
  (void)quad;
  (void)tx;
  (void)color;
#endif
}

void renderlevelback(void)
{
  int count, count2, count3;
  int lightcount;
  int blocknum;
  int lightrange;
  float vec[3];
  // float texcoord[2];

#if defined(GLES)
  glColor4f(level.ambient[0][0], level.ambient[0][1], level.ambient[0][2], 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#else
  glColor3fv(level.ambient[0]);
#endif
  levelbatch_begin();

  for (count = view.position[1] - view.zoomy; count <= view.position[1] + view.zoomy; count++)
    if (count >= 0 && count < 256)
      for (count2 = view.position[0] - view.zoomx; count2 <= view.position[0] + view.zoomx; count2++)
        if (count2 >= 0 && count2 < 256)
        {
          blocknum = level.backgrid[count][count2];

          if (block[blocknum].animation != 0)
            if (block[blocknum].animationspeed != 0)
              blocknum += (game.framenum / block[blocknum].animationspeed) % block[blocknum].animation;

          if (!game.godmode)
            if (!editor.active || !editor.showgrid)
              if (level.grid[count][count2] != 0 && level.gridmod[count][count2] == 0)
                if (!texture[level.grid[count][count2]].isalpha)
                  blocknum = 0;

          if (!game.godmode)
            if (!editor.active || !editor.showgrid)
              if (level.foregrid[count][count2] != 0)
                if (!texture[level.foregrid[count][count2]].isalpha)
                  blocknum = 0;

          if (blocknum != 0)
            levelbatch_add(blocknum, count2, count);
        }

  levelbatch_flush();
#if defined(GLES)
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
#endif

  glBlendFunc(GL_SRC_ALPHA, GL_ONE);

  glEnable(GL_STENCIL_TEST);
  glActiveTextureARB(GL_TEXTURE1_ARB);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, texture[332].glname);
  glDisable(GL_TEXTURE_2D);
  glActiveTextureARB(GL_TEXTURE0_ARB);

  for (lightcount = 0; lightcount < frame.numoflights; lightcount++)
  {
    float inv = (frame.light[lightcount].intensity != 0.0f)
                    ? 1.0f / frame.light[lightcount].intensity
                    : 0.0f;

    glStencilMask((1 << lightcount));
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_NOTEQUAL, (1 << lightcount), (1 << lightcount));

    // ptitSeb optimizations for Pandora
    glActiveTextureARB(GL_TEXTURE1_ARB);
    glEnable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0_ARB);

    glColor4f(frame.light[lightcount].color[0], frame.light[lightcount].color[1],
              frame.light[lightcount].color[2], 1.0f);

    levelbatch_begin();
    for (count = view.position[1] - view.zoomy; count <= view.position[1] + view.zoomy; count++)
      if (count >= 0 && count < 256)
        for (count2 = view.position[0] - view.zoomx; count2 <= view.position[0] + view.zoomx; count2++)
          if (count2 >= 0 && count2 < 256)
          {
            blocknum = level.backgrid[count][count2];

            if (block[blocknum].animation != 0)
              if (block[blocknum].animationspeed != 0)
                blocknum += (game.framenum / block[blocknum].animationspeed) % block[blocknum].animation;

            if (!editor.active || !editor.showgrid)
              if (level.grid[count][count2] != 0 && level.gridmod[count][count2] == 0)
                if (!texture[level.grid[count][count2]].isalpha)
                  blocknum = 0;

            if (!editor.active || !editor.showgrid)
              if (level.foregrid[count][count2] != 0)
                if (!texture[level.foregrid[count][count2]].isalpha)
                  blocknum = 0;

            lightrange = frame.light[lightcount].intensity * 0.5f;
            if ((count2 - frame.light[lightcount].position[0]) > lightrange + 1)
              blocknum = 0;
            if ((count - frame.light[lightcount].position[1]) > lightrange + 1)
              blocknum = 0;

            if (blocknum != 0)
              levelbatch_add(blocknum, count2, count);
          }

    levelbatch_sort();

    for (blocknum = 1; blocknum < LEVELBATCH_BLOCKS; blocknum++)
    {
      int first    = levelbatch_start[blocknum];
      int numquads = levelbatch_count[blocknum];

      if (numquads == 0)
        continue;

      glBindTexture(GL_TEXTURE_2D, texture[blocknum].glname);

      for (count3 = 0; count3 < numquads; count3++)
      {
        int   q = levelbatch_order[first + count3];
        float x = levelbatch_x[q];
        float y = levelbatch_y[q];

#if defined(GLES)
        gradbatch_add(x, y, frame.light[lightcount].position, inv);
#else
        glBegin(GL_QUADS);

        vec[0] = x;
        vec[1] = y + 1.0f;
        vec[2] = 0.0f;
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, 0.0f);
        setuplighttexcoord(lightcount, vec);
        glVertex3f(vec[0], vec[1], 0.0f);

        vec[0] = x + 1.0f;
        vec[1] = y + 1.0f;
        vec[2] = 0.0f;
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0f, 0.0f);
        setuplighttexcoord(lightcount, vec);
        glVertex3f(vec[0], vec[1], 0.0f);

        vec[0] = x + 1.0f;
        vec[1] = y;
        vec[2] = 0.0f;
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0f, 1.0f);
        setuplighttexcoord(lightcount, vec);
        glVertex3f(vec[0], vec[1], 0.0f);

        vec[0] = x;
        vec[1] = y;
        vec[2] = 0.0f;
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, 1.0f);
        setuplighttexcoord(lightcount, vec);
        glVertex3f(vec[0], vec[1], 0.0f);

        glEnd();
#endif
      }

#if defined(GLES)
      gradbatch_flush();
#endif
    }

    // ptitSeb optimizations for Pandora
    glActiveTextureARB(GL_TEXTURE1_ARB);
    glDisable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0_ARB);
  }

  glDisable(GL_STENCIL_TEST);

  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void renderlevel(void)
{
  int count, count2;
  int lightcount;
  int blocknum;
  int lightrange;
  float vec[3], vec2[3], vec3[3];
  float normal[3];
  float scale;

#if defined(GLES)
  glColor4f(level.ambient[1][0], level.ambient[1][1], level.ambient[1][2], 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#else
  glColor3fv(level.ambient[1]);
#endif
  levelbatch_begin();

  for (count = view.position[1] - view.zoomy; count <= view.position[1] + view.zoomy; count++)
    if (count >= 0 && count < 256)
      for (count2 = view.position[0] - view.zoomx; count2 <= view.position[0] + view.zoomx; count2++)
        if (count2 >= 0 && count2 < 256)
        {
          blocknum = level.grid[count][count2];

          if (block[blocknum].animation != 0)
            if (block[blocknum].animationspeed != 0)
              blocknum += (game.framenum / block[blocknum].animationspeed) % block[blocknum].animation;

          if (level.gridmod[count][count2] == 1)
            blocknum = 0;

          if (blocknum >= 240 && blocknum < 248)
            if (level.gridmod[count][count2] == 2)
              blocknum += 8;

          if (!editor.active || !editor.showgrid)
            if (level.foregrid[count][count2] != 0)
              if (!texture[level.foregrid[count][count2]].isalpha)
                blocknum = 0;

          if (blocknum != 0)
            levelbatch_add(blocknum, count2, count);
        }

  levelbatch_flush();
#if defined(GLES)
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
#endif

  glBlendFunc(GL_ONE, GL_ONE);

  glEnable(GL_STENCIL_TEST);

  for (lightcount = 0; lightcount < frame.numoflights; lightcount++)
  {
    glStencilMask((1 << lightcount));
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_NOTEQUAL, (1 << lightcount), (1 << lightcount));

    glActiveTextureARB(GL_TEXTURE0_ARB);

    levelbatch_begin();
    for (count = view.position[1] - view.zoomy; count <= view.position[1] + view.zoomy; count++)
      if (count >= 0 && count < 256)
        for (count2 = view.position[0] - view.zoomx; count2 <= view.position[0] + view.zoomx; count2++)
          if (count2 >= 0 && count2 < 256)
          {
            blocknum = level.grid[count][count2];

            if (block[blocknum].animation != 0)
              if (block[blocknum].animationspeed != 0)
                blocknum += (game.framenum / block[blocknum].animationspeed) % block[blocknum].animation;

            if (level.gridmod[count][count2] == 1)
              blocknum = 0;

            if (blocknum >= 240 && blocknum < 248)
              if (level.gridmod[count][count2] == 2)
                blocknum += 8;

            if (!editor.active || !editor.showgrid)
              if (level.foregrid[count][count2] != 0)
                if (!texture[level.foregrid[count][count2]].isalpha)
                  blocknum = 0;

            lightrange = frame.light[lightcount].intensity * 0.5f;

            if ((count2 - frame.light[lightcount].position[0]) > lightrange + 1)
              blocknum = 0;
            if ((count - frame.light[lightcount].position[1]) > lightrange + 1)
              blocknum = 0;

            if (blocknum != 0)
              levelbatch_add(blocknum, count2, count);
          }

    levelbatch_sort();

    for (blocknum = 1; blocknum < LEVELBATCH_BLOCKS; blocknum++)
    {
      int first    = levelbatch_start[blocknum];
      int numtiles = levelbatch_count[blocknum];
      int tile, line;

      if (numtiles == 0)
        continue;

      glBindTexture(GL_TEXTURE_2D, texture[blocknum].glname);

      for (tile = 0; tile < numtiles; tile++)
      {
        int q  = levelbatch_order[first + tile];
        int gx = (int)levelbatch_x[q];
        int gy = (int)levelbatch_y[q];

        for (line = 0; line < block[blocknum].numoflines; line++)
          if (((level.gridflags[gy][gx] >> line) & 1) == 0)
          {
            normal[0] = -(block[blocknum].line[line][3] - block[blocknum].line[line][1]);
            normal[1] = (block[blocknum].line[line][2] - block[blocknum].line[line][0]);
            normal[2] = 0.0f;
            normalizevector(normal, normal);

#if defined(GLES)
            {
              GLfloat color[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
              GLfloat quad[]  = {(float)gx + block[blocknum].line[line][0],
                                (float)gy + block[blocknum].line[line][1],
                                (float)gx + block[blocknum].line[line][2],
                                (float)gy + block[blocknum].line[line][3],
                                (float)gx + block[blocknum].line[line][2] - normal[0] * 0.125f,
                                (float)gy + block[blocknum].line[line][3] - normal[1] * 0.125f,
                                (float)gx + block[blocknum].line[line][0] - normal[0] * 0.125f,
                                (float)gy + block[blocknum].line[line][1] - normal[1] * 0.125f};
              GLfloat tx[]    = {block[blocknum].line[line][0],
                              1.0f - block[blocknum].line[line][1],
                              block[blocknum].line[line][2],
                              1.0f - block[blocknum].line[line][3],
                              (block[blocknum].line[line][2] - normal[0] * 0.125f),
                              1.0f - (block[blocknum].line[line][3] - normal[1] * 0.125f),
                              (block[blocknum].line[line][0] - normal[0] * 0.125f),
                              1.0f - (block[blocknum].line[line][1] - normal[1] * 0.125f)};

              vec[0] = (float)gx + block[blocknum].line[line][0];
              vec[1] = (float)gy + block[blocknum].line[line][1];
              vec[2] = 0.0f;
              subtractvectors(vec2, frame.light[lightcount].position, vec);
              normalizevector(vec3, vec2);
              scale = frame.light[lightcount].intensity * dotproduct(vec3, normal)
                      / (vec2[0] * vec2[0] + vec2[1] * vec2[1]);
              scalevector(vec2, frame.light[lightcount].color, scale);
              color[0] = vec2[0];
              color[1] = vec2[1];
              color[2] = vec2[2];

              vec[0] = (float)gx + block[blocknum].line[line][2];
              vec[1] = (float)gy + block[blocknum].line[line][3];
              vec[2] = 0.0f;
              subtractvectors(vec2, frame.light[lightcount].position, vec);
              normalizevector(vec3, vec2);
              scale = frame.light[lightcount].intensity * dotproduct(vec3, normal)
                      / (vec2[0] * vec2[0] + vec2[1] * vec2[1]);
              scalevector(vec2, frame.light[lightcount].color, scale);
              color[4] = vec2[0];
              color[5] = vec2[1];
              color[6] = vec2[2];

              edgebatch_add(quad, tx, color);
            }
#else
            glBegin(GL_QUADS);

            vec[0] = (float)gx + block[blocknum].line[line][0];
            vec[1] = (float)gy + block[blocknum].line[line][1];
            vec[2] = 0.0f;
            subtractvectors(vec2, frame.light[lightcount].position, vec);
            normalizevector(vec3, vec2);
            scale = frame.light[lightcount].intensity * dotproduct(vec3, normal)
                    / (vec2[0] * vec2[0] + vec2[1] * vec2[1]);
            scalevector(vec2, frame.light[lightcount].color, scale);
            glColor3fv(vec2);
            glTexCoord2f(block[blocknum].line[line][0], 1.0f - block[blocknum].line[line][1]);
            glVertex3f(vec[0], vec[1], 0.0f);

            vec[0] = (float)gx + block[blocknum].line[line][2];
            vec[1] = (float)gy + block[blocknum].line[line][3];
            vec[2] = 0.0f;
            subtractvectors(vec2, frame.light[lightcount].position, vec);
            normalizevector(vec3, vec2);
            scale = frame.light[lightcount].intensity * dotproduct(vec3, normal)
                    / (vec2[0] * vec2[0] + vec2[1] * vec2[1]);
            scalevector(vec2, frame.light[lightcount].color, scale);
            glColor3fv(vec2);
            glTexCoord2f(block[blocknum].line[line][2], 1.0f - block[blocknum].line[line][3]);
            glVertex3f(vec[0], vec[1], 0.0f);

            vec[0] = (float)gx + block[blocknum].line[line][2] - normal[0] * 0.125f;
            vec[1] = (float)gy + block[blocknum].line[line][3] - normal[1] * 0.125f;
            glColor3f(0.0f, 0.0f, 0.0f);
            glTexCoord2f((block[blocknum].line[line][2] - normal[0] * 0.125f),
                         1.0f - (block[blocknum].line[line][3] - normal[1] * 0.125f));
            glVertex3f(vec[0], vec[1], 0.0f);

            vec[0] = (float)gx + block[blocknum].line[line][0] - normal[0] * 0.125f;
            vec[1] = (float)gy + block[blocknum].line[line][1] - normal[1] * 0.125f;
            glColor3f(0.0f, 0.0f, 0.0f);
            glTexCoord2f((block[blocknum].line[line][0] - normal[0] * 0.125f),
                         1.0f - (block[blocknum].line[line][1] - normal[1] * 0.125f));
            glVertex3f(vec[0], vec[1], 0.0f);

            glEnd();
#endif
          }
      }

#if defined(GLES)
      edgebatch_flush();
#endif
    }
  }

  glDisable(GL_STENCIL_TEST);

  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void renderlevelfore(void)
{
  int count, count2;
  int blocknum;

  /* This is the layer that carries alpha content (grates, water, decals).
     Batching it is still exact: a fore tile draws only its own cell, so two
     fore tiles never cover the same pixel and their order cannot matter; and
     the whole layer is still drawn after the mid layer. */
#if defined(GLES)
  glColor4f(level.ambient[2][0], level.ambient[2][1], level.ambient[2][2], 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#else
  glColor3fv(level.ambient[2]);
#endif
  levelbatch_begin();

  for (count = view.position[1] - view.zoomy; count <= view.position[1] + view.zoomy; count++)
    if (count >= 0 && count < 256)
      for (count2 = view.position[0] - view.zoomx; count2 <= view.position[0] + view.zoomx; count2++)
        if (count2 >= 0 && count2 < 256)
        {
          blocknum = level.foregrid[count][count2];

          if (block[blocknum].animation != 0)
            if (block[blocknum].animationspeed != 0)
              blocknum += (game.framenum / block[blocknum].animationspeed) % block[blocknum].animation;

          if (blocknum != 0)
            levelbatch_add(blocknum, count2, count);
        }

  levelbatch_flush();
#if defined(GLES)
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
#endif
}

void renderbonds(void)
{
  int count;
  float vec[3];

  glDisable(GL_TEXTURE_2D);

#if defined(GLES)
  int index = -1;
  GLfloat vtxA[numofbonds * 6];

  glEnableClientState(GL_VERTEX_ARRAY);
#else
  glBegin(GL_LINES);
#endif

  glColor4f(0.0f, 1.0f, 0.0f, 1.0f);

  for (count = 0; count < numofbonds; count++)
  {
#if defined(GLES)
    vtxA[++index] = particle[bond[count].part1].position[0];
    vtxA[++index] = particle[bond[count].part1].position[1];
    vtxA[++index] = particle[bond[count].part1].position[2];

    vtxA[++index] = particle[bond[count].part2].position[0];
    vtxA[++index] = particle[bond[count].part2].position[1];
    vtxA[++index] = particle[bond[count].part2].position[2];
#else
    vec[0] = particle[bond[count].part1].position[0];
    vec[1] = particle[bond[count].part1].position[1];
    vec[2] = particle[bond[count].part1].position[2];
    glVertex3fv(vec);

    vec[0] = particle[bond[count].part2].position[0];
    vec[1] = particle[bond[count].part2].position[1];
    vec[2] = particle[bond[count].part2].position[2];
    glVertex3fv(vec);
#endif
  }

#if defined(GLES)
  glVertexPointer(3, GL_FLOAT, 0, vtxA);
  glDrawArrays(GL_LINES, 0, numofbonds * 2);
  glDisableClientState(GL_VERTEX_ARRAY);
#endif

  glColor4f(1.0f, 1.0f, 0.0f, 1.0f);

#if defined(GLES)
  index = -1;
  GLfloat vtxB[physicstemp.numofbonds * 6];

  glEnableClientState(GL_VERTEX_ARRAY);
#endif
  for (count = 0; count < physicstemp.numofbonds; count++)
  {
    if (physicstemp.bond[count].type == 0)
    {
#if defined(GLES)
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[0];
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[1];
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[2];

      vtxB[++index] = physicstemp.bond[count].point[0];
      vtxB[++index] = physicstemp.bond[count].point[1];
      vtxB[++index] = physicstemp.bond[count].point[2];
#else
      glVertex3fv(particle[physicstemp.bond[count].part1].position);
      glVertex3fv(physicstemp.bond[count].point);
#endif
    }
    if (physicstemp.bond[count].type == 1)
    {
#if defined(GLES)
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[0];
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[1];
      vtxB[++index] = particle[physicstemp.bond[count].part1].position[2];

      vtxB[++index] = physicstemp.bond[count].point[0];
      vtxB[++index] = physicstemp.bond[count].point[1];
      vtxB[++index] = physicstemp.bond[count].point[2];
#else
      glVertex3fv(particle[physicstemp.bond[count].part1].position);
      glVertex3fv(physicstemp.bond[count].point);
#endif
    }
    if (physicstemp.bond[count].type == 5)
    {
      scalevector(vec, particle[physicstemp.bond[count].part1].position, physicstemp.bond[count].force[0]);
      scaleaddvectors(vec, vec, particle[physicstemp.bond[count].part2].position, physicstemp.bond[count].force[1]);
#if defined(GLES)
      vtxB[++index] = vec[0];
      vtxB[++index] = vec[1];
      vtxB[++index] = vec[2];

      vtxB[++index] = particle[physicstemp.bond[count].part3].position[0];
      vtxB[++index] = particle[physicstemp.bond[count].part3].position[1];
      vtxB[++index] = particle[physicstemp.bond[count].part3].position[2];
#else
      glVertex3fv(vec);
      glVertex3fv(particle[physicstemp.bond[count].part3].position);
#endif
    }
  }
#if defined(GLES)
  glVertexPointer(3, GL_FLOAT, 0, vtxB);
  glDrawArrays(GL_LINES, 0, physicstemp.numofbonds * 2);
  glDisableClientState(GL_VERTEX_ARRAY);
#else
  glEnd();
#endif

  glEnable(GL_TEXTURE_2D);
}

void renderobjectinvisible(int objectnum)
{
  int count, count2;
  float vec[3], vec2[3];
  float windowsize;

  windowsize = 2.0f * 640.0f / 800.0f;

  vec[0] = object[objectrender[objectnum].objectnum].position[0] - view.position[0];
  vec[1] = object[objectrender[objectnum].objectnum].position[1] - view.position[1];
  vec[0] /= 10.0f;
  vec[1] /= 10.0f;
  count2 = (windowinfo.resolutionx >> 1) + vec[0] * (windowinfo.resolutionx >> 1);
  count  = (windowinfo.resolutiony >> 1) + vec[1] * (windowinfo.resolutionx >> 1);
  count2 -= 64;
  count -= 64;
  if (count2 < 0)
    count2 = 0;
  if (count < 0)
    count = 0;
  if (count2 > windowinfo.resolutionx - 128)
    count2 = windowinfo.resolutionx - 128;
  if (count > windowinfo.resolutiony - 128)
    count = windowinfo.resolutiony - 128;

  glBindTexture(GL_TEXTURE_2D, texture[331].glname);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, count2, count, 128, 128, 0);

  glBlendFunc(GL_ONE, GL_ZERO);

  // glDisable(GL_ALPHA_TEST);

  glBindTexture(GL_TEXTURE_2D, texture[331].glname);

#if defined(GLES)
  int index_vtx = -1;
  int index_tex = -1;
  GLfloat vtx[32 * 9];
  GLfloat tex[32 * 6];

  glColor4f(0.75f, 0.75f, 0.75f, 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#else
  glBegin(GL_TRIANGLES);

  glColor3f(0.75f, 0.75f, 0.75f);
#endif

  for (count2 = 0; count2 < 32; count2++)
  {
    subtractvectors(vec2, objectrender[objectnum].vertex[count2], object[objectrender[objectnum].objectnum].position);
    normalizevector(vec2, vec2);
#if defined(GLES)
    tex[++index_tex] = 0.5f + vec2[0] * 0.3f;
    tex[++index_tex] = 0.5f + vec2[1] * 0.3f;
    vtx[++index_vtx] = objectrender[objectnum].vertex[count2][0];
    vtx[++index_vtx] = objectrender[objectnum].vertex[count2][1];
    vtx[++index_vtx] = objectrender[objectnum].vertex[count2][2];
#else
    glTexCoord2f(0.5f + vec2[0] * 0.3f, 0.5f + vec2[1] * 0.3f);
    glVertex3fv(objectrender[objectnum].vertex[count2]);
#endif

    subtractvectors(vec2, objectrender[objectnum].vertex[((count2 + 1) & 31)],
                    object[objectrender[objectnum].objectnum].position);
    normalizevector(vec2, vec2);
#if defined(GLES)
    tex[++index_tex] = 0.5f + vec2[0] * 0.3f;
    tex[++index_tex] = 0.5f + vec2[1] * 0.3f;
    vtx[++index_vtx] = objectrender[objectnum].vertex[((count2 + 1) & 31)][0];
    vtx[++index_vtx] = objectrender[objectnum].vertex[((count2 + 1) & 31)][1];
    vtx[++index_vtx] = objectrender[objectnum].vertex[((count2 + 1) & 31)][2];

    tex[++index_tex] = 0.5f;
    tex[++index_tex] = 0.5f;
    vtx[++index_vtx] = objectrender[objectnum].vertex[32][0];
    vtx[++index_vtx] = objectrender[objectnum].vertex[32][1];
    vtx[++index_vtx] = objectrender[objectnum].vertex[32][2];
#else
    glTexCoord2f(0.5f + vec2[0] * 0.3f, 0.5f + vec2[1] * 0.3f);
    glVertex3fv(objectrender[objectnum].vertex[((count2 + 1) & 31)]);

    glTexCoord2f(0.5f, 0.5f);
    glVertex3fv(objectrender[objectnum].vertex[32]);
#endif
  }

#if defined(GLES)
  glVertexPointer(3, GL_FLOAT, 0, vtx);
  glTexCoordPointer(2, GL_FLOAT, 0, tex);
  glDrawArrays(GL_TRIANGLES, 0, 32 * 3);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
#else
  glEnd();
#endif

  /*
  for (count=0;count<16;count++)
    {
    glBegin(GL_TRIANGLES);

    glColor3f(0.5f,0.5f,0.5f);

    subtractvectors(vec2,particle[object[objectnum].particle[count]].position,object[objectnum].position);
    normalizevector(vec2,vec2);
    glTexCoord2f(0.5f+vec2[0]*0.3f,0.5f+vec2[1]*0.3f);
    glVertex3fv(particle[object[objectnum].particle[count]].position);

    subtractvectors(vec2,particle[object[objectnum].particle[((count+1)&15)]].position,object[objectnum].position);
    normalizevector(vec2,vec2);
    glTexCoord2f(0.5f+vec2[0]*0.3f,0.5f+vec2[1]*0.3f);
    glVertex3fv(particle[object[objectnum].particle[((count+1)&15)]].position);

    glTexCoord2f(0.5f,0.5f);
    glVertex3fv(object[objectnum].position);

    glEnd();
    }
  */
  // glEnable(GL_ALPHA_TEST);

  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void setuplighttexcoord(int lightcount, float position[3])
{
  float texcoord[2];

  texcoord[0] = (position[0] - frame.light[lightcount].position[0]) / frame.light[lightcount].intensity + 0.5f;
  texcoord[1] = (position[1] - frame.light[lightcount].position[1]) / frame.light[lightcount].intensity + 0.5f;

  glMultiTexCoord2fARB(GL_TEXTURE1_ARB, texcoord[0], texcoord[1]);
}

float calclight(int lightcount, float position[3], float normal[3])
{
  float vec[3], vec2[3];
  float scale;

  subtractvectors(vec, frame.light[lightcount].position, position);
  normalizevector(vec2, vec);
  scale = frame.light[lightcount].intensity * dotproduct(vec2, normal) / (vec[0] * vec[0] + vec[1] * vec[1]);

  return (scale);
}

void renderparticles(void)
{
  int count;
  float vec[3];
  float alpha;
  GLuint oldtex = 0;

  for (count = 0; count < numofparticles; count++)
    if (particle[count].type == 5)
      if (particle[count].texturenum != 366)
      {
        alpha = 1.0f;
        if (particle[count].timetolive < 50)
          alpha = (float)particle[count].timetolive / 50.0f;
        // ptitSeb optimizations for Pandora
        if (oldtex != texture[particle[count].texturenum].glname)
        {
          oldtex = texture[particle[count].texturenum].glname;
          glBindTexture(GL_TEXTURE_2D, oldtex /*texture[particle[count].texturenum].glname*/);
        }
#if defined(GLES)
        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        GLfloat quad[] = {particle[count].position[0] - particle[count].rendersize * 0.5f,
                          particle[count].position[1] + particle[count].rendersize * 0.5f,
                          particle[count].position[0] + particle[count].rendersize * 0.5f,
                          particle[count].position[1] + particle[count].rendersize * 0.5f,
                          particle[count].position[0] + particle[count].rendersize * 0.5f,
                          particle[count].position[1] - particle[count].rendersize * 0.5f,
                          particle[count].position[0] - particle[count].rendersize * 0.5f,
                          particle[count].position[1] - particle[count].rendersize * 0.5f};
        GLfloat tx[]   = {0, 0, 1, 0, 1, 1, 0, 1};
        glVertexPointer(2, GL_FLOAT, 0, quad);
        glTexCoordPointer(2, GL_FLOAT, 0, tx);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
#else
        glBegin(GL_QUADS);

        glColor4f(1.0f, 1.0f, 1.0f, alpha);

        glTexCoord2f(0.0f, 0.0f);
        vec[0] = particle[count].position[0] - particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] + particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(1.0f, 0.0f);
        vec[0] = particle[count].position[0] + particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] + particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(1.0f, 1.0f);
        vec[0] = particle[count].position[0] + particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] - particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(0.0f, 1.0f);
        vec[0] = particle[count].position[0] - particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] - particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glEnd();
#endif
      }
}

void renderparticles2(void)
{
  int count;
  float vec[3];
  float alpha;
  GLuint oldtex = 0;

  for (count = 0; count < numofparticles; count++)
    if (particle[count].type == 5)
      if (particle[count].texturenum == 366)
      {
        alpha = 1.0f;
        if (particle[count].timetolive < 50)
          alpha = (float)particle[count].timetolive / 50.0f;
        // ptitSeb optimizations for Pandora
        if (oldtex != texture[particle[count].texturenum].glname)
        {
          oldtex = texture[particle[count].texturenum].glname;
          glBindTexture(GL_TEXTURE_2D, oldtex /*texture[particle[count].texturenum].glname*/);
        }
#if defined(GLES)
        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        GLfloat quad[] = {particle[count].position[0] - particle[count].rendersize * 0.5f,
                          particle[count].position[1] + particle[count].rendersize * 0.5f,
                          particle[count].position[0] + particle[count].rendersize * 0.5f,
                          particle[count].position[1] + particle[count].rendersize * 0.5f,
                          particle[count].position[0] + particle[count].rendersize * 0.5f,
                          particle[count].position[1] - particle[count].rendersize * 0.5f,
                          particle[count].position[0] - particle[count].rendersize * 0.5f,
                          particle[count].position[1] - particle[count].rendersize * 0.5f};
        GLfloat tx[]   = {0, 0, 1, 0, 1, 1, 0, 1};
        glVertexPointer(2, GL_FLOAT, 0, quad);
        glTexCoordPointer(2, GL_FLOAT, 0, tx);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
#else
        glBegin(GL_QUADS);

        glColor4f(1.0f, 1.0f, 1.0f, alpha);

        glTexCoord2f(0.0f, 0.0f);
        vec[0] = particle[count].position[0] - particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] + particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(1.0f, 0.0f);
        vec[0] = particle[count].position[0] + particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] + particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(1.0f, 1.0f);
        vec[0] = particle[count].position[0] + particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] - particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glTexCoord2f(0.0f, 1.0f);
        vec[0] = particle[count].position[0] - particle[count].rendersize * 0.5f;
        vec[1] = particle[count].position[1] - particle[count].rendersize * 0.5f;
        glVertex3f(vec[0], vec[1], 0.0f);

        glEnd();
#endif
      }
}

void renderobjects(void)
{
#if defined(GLES)
  int index        = -1;
  int index_vtxA   = -1;
  int index_vtxB   = -1;
  int index_colorB = -1;
  int index_vtxC   = -1;
  int index_texC   = -1;
  int index_vtxD   = -1;
  int index_texD   = -1;
  int index_lightD = -1;
  int index_vtxE   = -1;
  int index_texE   = -1;
  int index_colorE = -1;
#endif
  int count, count2;
  int lightcount;
  float vec[3], vec2[3];
  float normal[3];
  float color[4];
  float size;

#if defined(GLES)
  /* the light gradient texture is the same for every object and every light:
     bind it once here instead of once per object per light */
  glActiveTextureARB(GL_TEXTURE1_ARB);
  glBindTexture(GL_TEXTURE_2D, texture[332].glname);
  glActiveTextureARB(GL_TEXTURE0_ARB);
#endif

  for (count = 0; count < numofobjectrenders; count++)
  {
    if (objectrender[count].type == 1)
    {
      if (game.godmode && game.oldschool == 0)
        renderobjectinvisible(count);
      else
      {
        glDisable(GL_TEXTURE_2D);

#if defined(GLES)
        index = -1;
        GLfloat vtx[32 * 9];
        glEnableClientState(GL_VERTEX_ARRAY);
#else
        glBegin(GL_TRIANGLES);
#endif

        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        if (objectrender[count].objectnum == 1)
          glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
        if (objectrender[count].objectnum == 2)
          glColor4f(0.355f, 0.31f, 0.15f, 1.0f);
        if (objectrender[count].objectnum == 3)
          glColor4f(0.453f, 0.3f, 0.28f, 1.0f);

        if (object[objectrender[count].objectnum].hitpoints < object[objectrender[count].objectnum].prevhitpoints)
          glColor4f(0.25f, 0.0f, 0.0f, 1.0f);

        for (count2 = 0; count2 < 32; count2++)
        {
#if defined(GLES)
          vtx[++index] = objectrender[count].vertex[count2][0];
          vtx[++index] = objectrender[count].vertex[count2][1];
          vtx[++index] = objectrender[count].vertex[count2][2];
          vtx[++index] = objectrender[count].vertex[((count2 + 1) & 31)][0];
          vtx[++index] = objectrender[count].vertex[((count2 + 1) & 31)][1];
          vtx[++index] = objectrender[count].vertex[((count2 + 1) & 31)][2];
          vtx[++index] = objectrender[count].vertex[32][0];
          vtx[++index] = objectrender[count].vertex[32][1];
          vtx[++index] = objectrender[count].vertex[32][2];
#else
          glVertex3fv(objectrender[count].vertex[count2]);
          glVertex3fv(objectrender[count].vertex[((count2 + 1) & 31)]);
          glVertex3fv(objectrender[count].vertex[32]);
#endif
        }

#if defined(GLES)
        glVertexPointer(3, GL_FLOAT, 0, vtx);
        glDrawArrays(GL_TRIANGLES, 0, 32 * 3);
        glDisableClientState(GL_VERTEX_ARRAY);
#else
        glEnd();
#endif

        glEnable(GL_TEXTURE_2D);
      }

      renderobjectspecular(count);

      size = 0.75f;

      glBindTexture(GL_TEXTURE_2D, texture[objectrender[count].texturenum].glname);

#if defined(GLES)
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      glEnableClientState(GL_VERTEX_ARRAY);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      GLfloat quad[] = {objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + pi / 4.0f),
                        objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + pi / 4.0f),
                        objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 3.0f * pi / 4.0f),
                        objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 3.0f * pi / 4.0f),
                        objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 5.0f * pi / 4.0f),
                        objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 5.0f * pi / 4.0f),
                        objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 7.0f * pi / 4.0f),
                        objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 7.0f * pi / 4.0f)};
      GLfloat txA[]  = {0, 0, 1, 0, 1, 1, 0, 1};
      GLfloat txB[]  = {1, 0, 0, 0, 0, 1, 1, 1};
      glVertexPointer(2, GL_FLOAT, 0, quad);
      if (object[objectrender[count].objectnum].direction == 0)
        glTexCoordPointer(2, GL_FLOAT, 0, txA);
      else
        glTexCoordPointer(2, GL_FLOAT, 0, txB);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glDisableClientState(GL_VERTEX_ARRAY);
#else
      glBegin(GL_QUADS);

      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

      vec[0] = objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + pi / 4.0f);
      vec[1] = objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + pi / 4.0f);
      vec[2] = 0.0f;
      if (object[objectrender[count].objectnum].direction == 0)
        glTexCoord2f(0.0f, 0.0f);
      else
        glTexCoord2f(1.0f, 0.0f);
      glVertex3fv(vec);

      vec[0] = objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 3.0f * pi / 4.0f);
      vec[1] = objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 3.0f * pi / 4.0f);
      vec[2] = 0.0f;
      if (object[objectrender[count].objectnum].direction == 0)
        glTexCoord2f(1.0f, 0.0f);
      else
        glTexCoord2f(0.0f, 0.0f);
      glVertex3fv(vec);

      vec[0] = objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 5.0f * pi / 4.0f);
      vec[1] = objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 5.0f * pi / 4.0f);
      vec[2] = 0.0f;
      if (object[objectrender[count].objectnum].direction == 0)
        glTexCoord2f(1.0f, 1.0f);
      else
        glTexCoord2f(0.0f, 1.0f);
      glVertex3fv(vec);

      vec[0] = objectrender[count].vertex[32][0] + size * cos(objectrender[count].angle + 7.0f * pi / 4.0f);
      vec[1] = objectrender[count].vertex[32][1] + size * sin(objectrender[count].angle + 7.0f * pi / 4.0f);
      vec[2] = 0.0f;
      if (object[objectrender[count].objectnum].direction == 0)
        glTexCoord2f(0.0f, 1.0f);
      else
        glTexCoord2f(1.0f, 1.0f);
      glVertex3fv(vec);

      glEnd();
#endif
      if (!object[objectrender[count].objectnum].idata[0])
      {
        if (game.oldschool == 0)
          glEnable(GL_LINE_SMOOTH);

        glDisable(GL_TEXTURE_2D);

        for (count2 = 0; count2 < 32; count2++)
        {
#if defined(GLES)
          glEnableClientState(GL_VERTEX_ARRAY);
          GLfloat vtx[] = {objectrender[count].vertex[count2][0],
                           objectrender[count].vertex[count2][1],
                           objectrender[count].vertex[count2][2],
                           objectrender[count].vertex[((count2 + 1) & 31)][0],
                           objectrender[count].vertex[((count2 + 1) & 31)][1],
                           objectrender[count].vertex[((count2 + 1) & 31)][2]};
#else
          glBegin(GL_LINES);
#endif

          if (objectrender[count].objectnum == 0)
            glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
          else
            glColor4f(0.25f, 0.25f, 0.25f, 0.5f);

#if defined(GLES)
          glVertexPointer(3, GL_FLOAT, 0, vtx);
          glDrawArrays(GL_LINES, 0, 2);
          glDisableClientState(GL_VERTEX_ARRAY);
#else
          glVertex3fv(objectrender[count].vertex[count2]);
          glVertex3fv(objectrender[count].vertex[((count2 + 1) & 31)]);

          glEnd();
#endif
        }
        if ((object[objectrender[count].objectnum].button & 4) == 4)
          for (count2 = 0; count2 < 32; count2++)
          {
#if defined(GLES)
            index_vtxA = -1;
            GLfloat vtxA[6];
            glEnableClientState(GL_VERTEX_ARRAY);
#else
            glBegin(GL_LINES);
#endif
            if (objectrender[count].objectnum == 0)
              glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
            else
              glColor4f(0.25f, 0.25f, 0.25f, 0.5f);

            normal[0] = objectrender[count].vertex[count2][1] - objectrender[count].vertex[((count2 + 1) & 31)][1];
            normal[1] = objectrender[count].vertex[((count2 + 1) & 31)][0] - objectrender[count].vertex[count2][0];
            normal[2] = 0.0f;

            copyvector(vec, objectrender[count].vertex[count2]);
            scaleaddvectors(vec, vec, normal, 0.1f);
#if defined(GLES)
            vtxA[++index_vtxA] = vec[0];
            vtxA[++index_vtxA] = vec[1];
            vtxA[++index_vtxA] = vec[2];
#else
            glVertex3fv(vec);
#endif

            normal[0] = objectrender[count].vertex[((count2 + 1) & 31)][1]
                        - objectrender[count].vertex[((count2 + 2) & 31)][1];
            normal[1] = objectrender[count].vertex[((count2 + 2) & 31)][0]
                        - objectrender[count].vertex[((count2 + 1) & 31)][0];
            normal[2] = 0.0f;

            copyvector(vec, objectrender[count].vertex[((count2 + 1) & 31)]);
            scaleaddvectors(vec, vec, normal, 0.1f);
#if defined(GLES)
            vtxA[++index_vtxA] = vec[0];
            vtxA[++index_vtxA] = vec[1];
            vtxA[++index_vtxA] = vec[2];
#else
            glVertex3fv(vec);
#endif

#if defined(GLES)
            glVertexPointer(3, GL_FLOAT, 0, vtxA);
            glDrawArrays(GL_LINES, 0, 2);
            glDisableClientState(GL_VERTEX_ARRAY);
#else
            glEnd();
#endif
          }
        if ((object[objectrender[count].objectnum].button & 1) == 1)
          for (count2 = 0; count2 < 32; count2++)
          {
#if defined(GLES)
            index_vtxB   = -1;
            index_colorB = -1;
            GLfloat vtxB[12];
            GLfloat colorB[16];
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
#else
            glBegin(GL_LINES);
#endif
            if (objectrender[count].objectnum == 0)
            {
              color[0] = 0.5f;
              color[1] = 0.5f;
              color[2] = 0.5f;
            }
            else
            {
              color[0] = 0.25f;
              color[1] = 0.25f;
              color[2] = 0.25f;
            }

            normal[0] = objectrender[count].vertex[count2][1] - objectrender[count].vertex[((count2 + 1) & 31)][1];
            normal[1] = objectrender[count].vertex[((count2 + 1) & 31)][0] - objectrender[count].vertex[count2][0];
            normal[2] = 0.0f;
            scalevector(normal, normal, 0.8f);

            copyvector(vec, objectrender[count].vertex[count2]);
            color[3] = 1.0f;
#if defined(GLES)
            vtxB[++index_vtxB]     = vec[0];
            vtxB[++index_vtxB]     = vec[1];
            vtxB[++index_vtxB]     = vec[2];
            colorB[++index_colorB] = color[0];
            colorB[++index_colorB] = color[1];
            colorB[++index_colorB] = color[2];
            colorB[++index_colorB] = color[3];
#else
            glColor4fv(color);
            glVertex3fv(vec);
#endif
            addvectors(vec, vec, normal);
            color[3] = 0.0f;
#if defined(GLES)
            vtxB[++index_vtxB]     = vec[0];
            vtxB[++index_vtxB]     = vec[1];
            vtxB[++index_vtxB]     = vec[2];
            colorB[++index_colorB] = color[0];
            colorB[++index_colorB] = color[1];
            colorB[++index_colorB] = color[2];
            colorB[++index_colorB] = color[3];
#else
            glColor4fv(color);
            glVertex3fv(vec);
#endif
            scalevector(vec, objectrender[count].vertex[count2], 0.5f);
            scaleaddvectors(vec, vec, objectrender[count].vertex[((count2 + 1) & 31)], 0.5f);
            color[3] = 1.0f;
#if defined(GLES)
            vtxB[++index_vtxB]     = vec[0];
            vtxB[++index_vtxB]     = vec[1];
            vtxB[++index_vtxB]     = vec[2];
            colorB[++index_colorB] = color[0];
            colorB[++index_colorB] = color[1];
            colorB[++index_colorB] = color[2];
            colorB[++index_colorB] = color[3];
#else
            glColor4fv(color);
            glVertex3fv(vec);
#endif
            addvectors(vec, vec, normal);
            color[3] = 0.0f;
#if defined(GLES)
            vtxB[++index_vtxB]     = vec[0];
            vtxB[++index_vtxB]     = vec[1];
            vtxB[++index_vtxB]     = vec[2];
            colorB[++index_colorB] = color[0];
            colorB[++index_colorB] = color[1];
            colorB[++index_colorB] = color[2];
            colorB[++index_colorB] = color[3];
#else
            glColor4fv(color);
            glVertex3fv(vec);
#endif
#if defined(GLES)
            glVertexPointer(3, GL_FLOAT, 0, vtxB);
            glColorPointer(4, GL_FLOAT, 0, colorB);
            glDrawArrays(GL_LINES, 0, 4);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
#else
            glEnd();
#endif
          }

        glEnable(GL_TEXTURE_2D);

        glDisable(GL_LINE_SMOOTH);
      }
    }
    else
    {
      glBindTexture(GL_TEXTURE_2D, texture[objectrender[count].texturenum].glname);

#if defined(GLES)
      index_vtxC = -1;
      index_texC = -1;
      GLfloat vtxC[objectrender[count].numoftris * 9];
      GLfloat texC[objectrender[count].numoftris * 6];
      glEnableClientState(GL_VERTEX_ARRAY);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#else
      glBegin(GL_TRIANGLES);
#endif
      glColor4f(level.ambient[1][0], level.ambient[1][1], level.ambient[1][2], objectrender[count].alpha);

      if (objectrender[count].type == 8)
        if (object[objectrender[count].objectnum].lighttype == 1)
        {
#if defined(GLES)
          glColor4f(object[objectrender[count].objectnum].lightcolor[0],
                    object[objectrender[count].objectnum].lightcolor[1],
                    object[objectrender[count].objectnum].lightcolor[2], 1.0f);
#else
          glColor3fv(object[objectrender[count].objectnum].lightcolor);
#endif
        }

      for (count2 = 0; count2 < objectrender[count].numoftris; count2++)
      {
#if defined(GLES)
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][0]][0];
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][0]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][0]][0];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][0]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][0]][2];
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][1]][0];
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][1]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][1]][0];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][1]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][1]][2];
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][2]][0];
        texC[++index_texC] = objectrender[count].texcoord[objectrender[count].tri[count2][2]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][2]][0];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][2]][1];
        vtxC[++index_vtxC] = objectrender[count].vertex[objectrender[count].tri[count2][2]][2];
#else
        glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][0]]);
        glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][0]]);
        glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][1]]);
        glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][1]]);
        glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][2]]);
        glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][2]]);
#endif
      }
#if defined(GLES)
      glVertexPointer(3, GL_FLOAT, 0, vtxC);
      glTexCoordPointer(2, GL_FLOAT, 0, texC);
      glDrawArrays(GL_TRIANGLES, 0, objectrender[count].numoftris * 3);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glDisableClientState(GL_VERTEX_ARRAY);
#else
      glEnd();
#endif

      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      glEnable(GL_STENCIL_TEST);

      /* Unit 1 carries the light gradient: one texture for every light, bound
         once per frame in renderobjects(), so it only has to be enabled once
         here instead of once per light. */
      glActiveTextureARB(GL_TEXTURE1_ARB);
      glEnable(GL_TEXTURE_2D);
      glActiveTextureARB(GL_TEXTURE0_ARB);

      /* chunks (type 2/10) keep their edge-quad path, everything else uses the
         triangle body path */
      int bodyD = (objectrender[count].type != 2 && objectrender[count].type != 10);

#if defined(GLES)
      /* Body geometry and its unit 0 texcoords do not depend on which light is
         being applied, so build them once per object instead of once per light.
         Only the unit 1 gradient changes per light - and object bodies never
         had one at all: it used to be left to setuplighttexcoord(), which only
         writes immediate-mode state and is ignored on GLES. */
      int trisD = objectrender[count].numoftris;
      int v;

      if (trisD < 1)
        trisD = 1;
      GLfloat vtxD[trisD * 9];
      GLfloat texD[trisD * 6];
      GLfloat lightD[trisD * 6];

      if (bodyD)
      {
        index_vtxD = -1;
        index_texD = -1;
        for (count2 = 0; count2 < objectrender[count].numoftris; count2++)
          for (v = 0; v < 3; v++)
          {
            int vi = objectrender[count].tri[count2][v];

            texD[++index_texD] = objectrender[count].texcoord[vi][0];
            texD[++index_texD] = objectrender[count].texcoord[vi][1];
            vtxD[++index_vtxD] = objectrender[count].vertex[vi][0];
            vtxD[++index_vtxD] = objectrender[count].vertex[vi][1];
            vtxD[++index_vtxD] = objectrender[count].vertex[vi][2];
          }
      }
#endif

#if defined(GLES)
      /* The client arrays are enabled once per object and only re-pointed per
         light: an enable/disable pair costs eight driver calls per light, and a
         busy scene (80 debris objects, 2 lights) issues ~160 light draws.  The
         colour array is only used by the edge path. */
      glEnableClientState(GL_VERTEX_ARRAY);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glClientActiveTexture(GL_TEXTURE1);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glClientActiveTexture(GL_TEXTURE0);
      if (!bodyD)
        glEnableClientState(GL_COLOR_ARRAY);
#endif

      for (lightcount = 0; lightcount < frame.numoflights; lightcount++)
        if (((objectrender[count].lightflags >> lightcount) & 1) == 1)
        {
          glStencilMask((1 << lightcount));
          glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
          glStencilFunc(GL_NOTEQUAL, (1 << lightcount), (1 << lightcount));

          if (bodyD)
          {
#if defined(GLES)
            float lscaleD = (frame.light[lightcount].intensity != 0.0f)
                                ? 1.0f / frame.light[lightcount].intensity
                                : 0.0f;

            index_lightD = -1;
            for (v = 0; v <= index_vtxD; v += 3)
            {
              lightD[++index_lightD]
                  = (vtxD[v + 0] - frame.light[lightcount].position[0]) * lscaleD + 0.5f;
              lightD[++index_lightD]
                  = (vtxD[v + 1] - frame.light[lightcount].position[1]) * lscaleD + 0.5f;
            }

            glColor4f(level.ambient[1][0], level.ambient[1][1], level.ambient[1][2], objectrender[count].alpha);

            glClientActiveTexture(GL_TEXTURE1);
            glTexCoordPointer(2, GL_FLOAT, 0, lightD);
            glClientActiveTexture(GL_TEXTURE0);
            glVertexPointer(3, GL_FLOAT, 0, vtxD);
            glTexCoordPointer(2, GL_FLOAT, 0, texD);
            glDrawArrays(GL_TRIANGLES, 0, objectrender[count].numoftris * 3);
#else
            glBegin(GL_TRIANGLES);
            glColor4f(level.ambient[1][0], level.ambient[1][1], level.ambient[1][2], objectrender[count].alpha);

            for (count2 = 0; count2 < objectrender[count].numoftris; count2++)
            {
              glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][0]]);
              setuplighttexcoord(lightcount, objectrender[count].vertex[objectrender[count].tri[count2][0]]);
              glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][0]]);

              glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][1]]);
              setuplighttexcoord(lightcount, objectrender[count].vertex[objectrender[count].tri[count2][1]]);
              glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][1]]);

              glTexCoord2fv(objectrender[count].texcoord[objectrender[count].tri[count2][2]]);
              setuplighttexcoord(lightcount, objectrender[count].vertex[objectrender[count].tri[count2][2]]);
              glVertex3fv(objectrender[count].vertex[objectrender[count].tri[count2][2]]);
            }

            glEnd();
#endif
          }
          else
          {
#if defined(GLES)
            index_vtxE   = -1;
            index_texE   = -1;
            index_colorE = -1;
            /* sized from numofedges, not numoftris: the loop below writes four
               vertices per edge, so the old numoftris-based sizes overflowed
               the stack (2 tris = 8 vertices' worth of space, 16 written) */
            GLfloat vtxE[objectrender[count].numofedges * 12];
            GLfloat texE[objectrender[count].numofedges * 8];
            GLfloat colorE[objectrender[count].numofedges * 16];
#else
            glBegin(GL_QUADS);
#endif
            for (count2 = 0; count2 < objectrender[count].numofedges; count2++)
            {
              subtractvectors(vec2, frame.light[lightcount].position, objectrender[count].edgevertex[count2][0]);
              scalevector(vec, frame.light[lightcount].color, dotproduct(vec2, objectrender[count].edgenormal[count2]));
#if defined(GLES)
              colorE[++index_colorE] = vec[0];
              colorE[++index_colorE] = vec[1];
              colorE[++index_colorE] = vec[2];
              colorE[++index_colorE] = objectrender[count].alpha;
#else
              glColor4f(vec[0], vec[1], vec[2], objectrender[count].alpha);
#endif

              setuplighttexcoord(lightcount, objectrender[count].edgevertex[count2][0]);
#if defined(GLES)
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][0][0];
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][0][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][0][0];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][0][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][0][2];
#else
              glTexCoord2fv(objectrender[count].edgetexcoord[count2][0]);
              glVertex3fv(objectrender[count].edgevertex[count2][0]);
#endif

              subtractvectors(vec2, frame.light[lightcount].position, objectrender[count].edgevertex[count2][1]);
              scalevector(vec, frame.light[lightcount].color, dotproduct(vec2, objectrender[count].edgenormal[count2]));
#if defined(GLES)
              colorE[++index_colorE] = vec[0];
              colorE[++index_colorE] = vec[1];
              colorE[++index_colorE] = vec[2];
              colorE[++index_colorE] = objectrender[count].alpha;
#else
              glColor4f(vec[0], vec[1], vec[2], objectrender[count].alpha);
#endif

              setuplighttexcoord(lightcount, objectrender[count].edgevertex[count2][1]);
#if defined(GLES)
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][1][0];
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][1][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][1][0];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][1][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][1][2];
#else
              glTexCoord2fv(objectrender[count].edgetexcoord[count2][1]);
              glVertex3fv(objectrender[count].edgevertex[count2][1]);
#endif
              zerovector(vec);
#if defined(GLES)
              colorE[++index_colorE] = vec[0];
              colorE[++index_colorE] = vec[1];
              colorE[++index_colorE] = vec[2];
              colorE[++index_colorE] = objectrender[count].alpha;
#else
              glColor4f(vec[0], vec[1], vec[2], objectrender[count].alpha);
#endif

              setuplighttexcoord(lightcount, objectrender[count].edgevertex[count2][2]);
#if defined(GLES)
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][2][0];
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][2][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][2][0];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][2][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][2][2];
#else
              glTexCoord2fv(objectrender[count].edgetexcoord[count2][2]);
              glVertex3fv(objectrender[count].edgevertex[count2][2]);
#endif
              zerovector(vec);
#if defined(GLES)
              colorE[++index_colorE] = vec[0];
              colorE[++index_colorE] = vec[1];
              colorE[++index_colorE] = vec[2];
              colorE[++index_colorE] = objectrender[count].alpha;
#else
              glColor4f(vec[0], vec[1], vec[2], objectrender[count].alpha);
#endif

              setuplighttexcoord(lightcount, objectrender[count].edgevertex[count2][3]);
#if defined(GLES)
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][3][0];
              texE[++index_texE] = objectrender[count].edgetexcoord[count2][3][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][3][0];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][3][1];
              vtxE[++index_vtxE] = objectrender[count].edgevertex[count2][3][2];
#else
              glTexCoord2fv(objectrender[count].edgetexcoord[count2][3]);
              glVertex3fv(objectrender[count].edgevertex[count2][3]);
#endif
            }
#if defined(GLES)
            {
              /* These objects carry four SEPARATE quads, but the old code drew
                 all of them as one GL_TRIANGLE_FAN - a fan joins vertex 0 to
                 every later vertex, which threw long diagonal light streaks
                 across the object and everything behind it.  Expand each quad
                 to two triangles instead.

                 The light gradient (texture unit 1) also never got texcoords
                 here: setuplighttexcoord() only sets immediate-mode state,
                 which GLES ignores, so it is filled in below the same way. */
              int   edges = objectrender[count].numofedges;
              int   edge, k;
              float lscale;
              const int quadindex[6] = {0, 1, 2, 0, 2, 3};
              GLfloat vtxT[edges * 18];
              GLfloat texT[edges * 12];
              GLfloat lightT[edges * 12];
              GLfloat colorT[edges * 24];

              if (edges < 1)
                edges = 1;
              lscale = (frame.light[lightcount].intensity != 0.0f)
                           ? 1.0f / frame.light[lightcount].intensity
                           : 0.0f;

              for (edge = 0; edge < objectrender[count].numofedges; edge++)
                for (k = 0; k < 6; k++)
                {
                  int src = edge * 4 + quadindex[k];
                  int dst = edge * 6 + k;

                  vtxT[dst * 3 + 0]   = vtxE[src * 3 + 0];
                  vtxT[dst * 3 + 1]   = vtxE[src * 3 + 1];
                  vtxT[dst * 3 + 2]   = vtxE[src * 3 + 2];
                  texT[dst * 2 + 0]   = texE[src * 2 + 0];
                  texT[dst * 2 + 1]   = texE[src * 2 + 1];
                  colorT[dst * 4 + 0] = colorE[src * 4 + 0];
                  colorT[dst * 4 + 1] = colorE[src * 4 + 1];
                  colorT[dst * 4 + 2] = colorE[src * 4 + 2];
                  colorT[dst * 4 + 3] = colorE[src * 4 + 3];
                  lightT[dst * 2 + 0]
                      = (vtxE[src * 3 + 0] - frame.light[lightcount].position[0]) * lscale + 0.5f;
                  lightT[dst * 2 + 1]
                      = (vtxE[src * 3 + 1] - frame.light[lightcount].position[1]) * lscale + 0.5f;
                }

              glClientActiveTexture(GL_TEXTURE1);
              glTexCoordPointer(2, GL_FLOAT, 0, lightT);
              glClientActiveTexture(GL_TEXTURE0);
              glTexCoordPointer(2, GL_FLOAT, 0, texT);
              glColorPointer(4, GL_FLOAT, 0, colorT);
              glVertexPointer(3, GL_FLOAT, 0, vtxT);
              glDrawArrays(GL_TRIANGLES, 0, objectrender[count].numofedges * 6);
            }
#else
            glEnd();
#endif
          }

        }

#if defined(GLES)
      /* client arrays back off for the passes that follow */
      glDisableClientState(GL_VERTEX_ARRAY);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glClientActiveTexture(GL_TEXTURE1);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glClientActiveTexture(GL_TEXTURE0);
      if (!bodyD)
        glDisableClientState(GL_COLOR_ARRAY);
#endif

      /* unit 1 off again for the normally blended passes that follow */
      glActiveTextureARB(GL_TEXTURE1_ARB);
      glDisable(GL_TEXTURE_2D);
      glActiveTextureARB(GL_TEXTURE0_ARB);

      glDisable(GL_STENCIL_TEST);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      if (game.oldschool == 2)
        if (objectrender[count].type == 32)
        {
          glDisable(GL_TEXTURE_2D);
#if defined(GLES)
          GLfloat vtxF[] = {particle[rope[objectrender[count].objectnum].part1].position[0],
                            particle[rope[objectrender[count].objectnum].part1].position[1],
                            particle[rope[objectrender[count].objectnum].part1].position[2],
                            particle[rope[objectrender[count].objectnum].part2].position[0],
                            particle[rope[objectrender[count].objectnum].part2].position[1],
                            particle[rope[objectrender[count].objectnum].part2].position[2]};

          glEnableClientState(GL_VERTEX_ARRAY);
          glVertexPointer(3, GL_FLOAT, 0, vtxF);
          glDrawArrays(GL_LINES, 0, 2);
          glDisableClientState(GL_VERTEX_ARRAY);
#else
          glBegin(GL_LINES);

          glColor4f(0.0f, 0.0f, 0.0f, 1.0f);

          glVertex3fv(particle[rope[objectrender[count].objectnum].part1].position);
          glVertex3fv(particle[rope[objectrender[count].objectnum].part2].position);

          glEnd();
#endif
          glEnable(GL_TEXTURE_2D);
        }
    }
  }
}

void rendersprites(void)
{
  int count;
  int x, y;
  float vec[3];

  for (count = 0; count < numofsprites; count++)
  {
    subtractvectors(vec, sprite[count].position, view.position);
    vec[0] /= view.zoom;
    vec[1] /= view.zoom;
    vec[0] *= (float)(640 >> 1);
    vec[0] += (float)(640 >> 1);
    vec[1] *= (float)(640 >> 1);
    vec[1] = (float)(480 >> 1) - vec[1];
    x      = vec[0];
    y      = vec[1];

    drawtext(sprite[count].text, (x | TEXT_CENTER), (y | TEXT_CENTER), sprite[count].size, sprite[count].red,
             sprite[count].green, sprite[count].blue, sprite[count].alpha);
  }
}
