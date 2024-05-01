//
//  Copyright (C) 1999 by
//  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
// DESCRIPTION:
//      Rendering main loop and setup functions,
//       utility functions (BSP, geometry, trigonometry).
//      See tables.c, too.
//
//-----------------------------------------------------------------------------

#define _USE_MATH_DEFINES
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "d_loop.h"
#include "d_player.h"
#include "doomdata.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_video.h"
#include "p_mobj.h"
#include "p_setup.h" // P_SegLengths
#include "r_bsp.h"
#include "r_data.h"
#include "r_defs.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_plane.h"
#include "r_sky.h"
#include "r_state.h"
#include "r_things.h"
#include "r_voxel.h"
#include "st_stuff.h"
#include "v_flextran.h"
#include "v_video.h"
#include "z_zone.h"

// [Nugget]
#include "m_nughud.h"
#include "m_random.h"
#include "p_map.h"
#include "wi_stuff.h"

// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW 2048

// killough: viewangleoffset is a legacy from the pre-v1.2 days, when Doom
// had Left/Mid/Right viewing. +/-ANG90 offsets were placed here on each
// node, by d_net.c, to set up a L/M/R session.

int viewangleoffset;
int validcount = 1;         // increment every time a check is made
lighttable_t *fixedcolormap;
int      centerx, centery;
fixed_t  centerxfrac, centeryfrac;
fixed_t  projection;
fixed_t  skyiscale;
fixed_t  viewx, viewy, viewz;
angle_t  viewangle;
localview_t localview;
double deltatics;
boolean raw_input;
fixed_t  viewcos, viewsin;
player_t *viewplayer;
extern lighttable_t **walllights;
fixed_t  viewheightfrac; // [FG] sprite clipping optimizations

static fixed_t focallength, lightfocallength;

// [Nugget] Chasecam /--------------------------------------------------------

chasecam_t chasecam;

boolean chasecam_on;

// For Automap
fixed_t  chasexofs, chaseyofs;
angle_t  chaseaofs;

// [Nugget] -----------------------------------------------------------------/

//
// precalculated math tables
//

angle_t clipangle;
angle_t vx_clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X.

int viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.

angle_t *xtoviewangle = NULL;   // killough 2/8/98

// [FG] linear horizontal sky scrolling
angle_t *linearskyangle = NULL;

int LIGHTLEVELS;
int LIGHTSEGSHIFT;
int LIGHTBRIGHT;
int MAXLIGHTSCALE;
int LIGHTSCALESHIFT;
int MAXLIGHTZ;
int LIGHTZSHIFT;

// killough 3/20/98: Support dynamic colormaps, e.g. deep water
// killough 4/4/98: support dynamic number of them as well

int numcolormaps;
lighttable_t ***(*c_scalelight) = NULL;
lighttable_t ***(*c_zlight) = NULL;
lighttable_t **(*scalelight) = NULL;
lighttable_t **scalelightfixed = NULL;
lighttable_t **(*zlight) = NULL;
lighttable_t *fullcolormap;
lighttable_t **colormaps;

// killough 3/20/98, 4/4/98: end dynamic colormaps

int extralight;                           // bumped light from gun blasts
int extra_level_brightness;               // level brightness feature

// [Nugget] FOV effects /-----------------------------------------------------

static int r_fov; // Rendered (currently-applied) FOV, with effects added to it

static fovfx_t fovfx[NUMFOVFX]; // FOV effects (recoil, teleport)
static int     zoomed = 0;      // Current zoom state

void R_ClearFOVFX(void)
{
  R_SetZoom(ZOOM_RESET);

  for (int i = FOVFX_ZOOM+1;  i < NUMFOVFX;  i++)
  {
    // Note: the `R_SetZoom()` call above sets `setsizeneeded = true` already,
    // but we'll do it here anyways for future-proofing
    if (fovfx[i].current != 0) { setsizeneeded = true; }

    fovfx[i] = (fovfx_t) { .target = 0, .current = 0, .old = 0 };
  }
}

int R_GetFOVFX(const int fx)
{
  return fovfx[fx].current;
}

void R_SetFOVFX(const int fx)
{
  if (strictmode) { return; }

  switch (fx) {
    case FOVFX_ZOOM:
      // Handled by R_Get/SetZoom
      break;

    case FOVFX_TELEPORT:
      if (!teleporter_zoom) { break; }
      R_SetZoom(ZOOM_RESET);
      fovfx[FOVFX_TELEPORT].target = 50;
      setsizeneeded = true;
      break;
  }
}

int R_GetZoom(void)
{
  return zoomed;
}

void R_SetZoom(const int state)
{
  if (state == ZOOM_RESET || zoomed == ZOOM_RESET)
  {
    zoomed = ZOOM_RESET;
    setsizeneeded = true;
    return;
  }
  else if (zoomed != state) { setsizeneeded = true; }

  if (STRICTMODE(zoom_fov - custom_fov))
  { zoomed = state; }
  else
  { zoomed = ZOOM_OFF; }
}

// [Nugget] -----------------------------------------------------------------/

// [Nugget] Explosion shake effect /------------------------------------------

static fixed_t shake;
#define MAXSHAKE 50

void R_SetShake(int value)
{
  if (strictmode || !explosion_shake || value == -1)
  {
    shake = 0;
    return;
  }

  shake = MIN(shake + value, MAXSHAKE);
}

void R_ExplosionShake(fixed_t bombx, fixed_t bomby, int force, int range)
{
  #define SHAKERANGEMULT 5

  const mobj_t *const player = players[displayplayer].mo;
  fixed_t dx, dy, dist;

  if (strictmode || !explosion_shake) { return; }

  range *= SHAKERANGEMULT;
  force *= SHAKERANGEMULT;

  dx = abs(player->x - bombx);
  dy = abs(player->y - bomby);

  dist = MAX(dx, dy);
  dist = (dist - player->radius) >> FRACBITS;
  dist = MAX(0, dist);

  if (dist >= range) { return; }

  R_SetShake((force * (range - dist) / range) / ((128 / MAXSHAKE) * SHAKERANGEMULT));

  #undef SHAKERANGEMULT
}

// [Nugget] -----------------------------------------------------------------/

void (*colfunc)(void) = R_DrawColumn;     // current column draw function

//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
// killough 5/2/98: reformatted
//

// Workaround for optimization bug in clang
// fixes desync in competn/doom/fp2-3655.lmp and in dmnsns.wad dmn01m909.lmp
#if defined(__clang__)
int R_PointOnSide(volatile fixed_t x, volatile fixed_t y, node_t *node)
#else
int R_PointOnSide(fixed_t x, fixed_t y, node_t *node)
#endif
{
  if (!node->dx)
    return x <= node->x ? node->dy > 0 : node->dy < 0;

  if (!node->dy)
    return y <= node->y ? node->dx < 0 : node->dx > 0;

  x -= node->x;
  y -= node->y;

  // Try to quickly decide by looking at sign bits.
  if ((node->dy ^ node->dx ^ x ^ y) < 0)
    return (node->dy ^ x) < 0;  // (left is negative)
  return FixedMul(y, node->dx>>FRACBITS) >= FixedMul(node->dy>>FRACBITS, x);
}

// killough 5/2/98: reformatted

int R_PointOnSegSide(fixed_t x, fixed_t y, seg_t *line)
{
  fixed_t lx = line->v1->x;
  fixed_t ly = line->v1->y;
  fixed_t ldx = line->v2->x - lx;
  fixed_t ldy = line->v2->y - ly;

  if (!ldx)
    return x <= lx ? ldy > 0 : ldy < 0;

  if (!ldy)
    return y <= ly ? ldx < 0 : ldx > 0;

  x -= lx;
  y -= ly;

  // Try to quickly decide by looking at sign bits.
  if ((ldy ^ ldx ^ x ^ y) < 0)
    return (ldy ^ x) < 0;          // (left is negative)
  return FixedMul(y, ldx>>FRACBITS) >= FixedMul(ldy>>FRACBITS, x);
}

//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table. The +1 size of tantoangle[]
//  is to handle the case when x==y without additional
//  checking.
//
// killough 5/2/98: reformatted, cleaned up

angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
  return (y -= viewy, (x -= viewx) || y) ?
    x >= 0 ?
      y >= 0 ?
        (x > y) ? tantoangle[SlopeDiv(y,x)] :                      // octant 0
                ANG90-1-tantoangle[SlopeDiv(x,y)] :                // octant 1
        x > (y = -y) ? 0-tantoangle[SlopeDiv(y,x)] :               // octant 8
                       ANG270+tantoangle[SlopeDiv(x,y)] :          // octant 7
      y >= 0 ? (x = -x) > y ? ANG180-1-tantoangle[SlopeDiv(y,x)] : // octant 3
                            ANG90 + tantoangle[SlopeDiv(x,y)] :    // octant 2
        (x = -x) > (y = -y) ? ANG180+tantoangle[ SlopeDiv(y,x)] :  // octant 4
                              ANG270-1-tantoangle[SlopeDiv(x,y)] : // octant 5
    0;
}

angle_t R_PointToAngle2(fixed_t viewx, fixed_t viewy, fixed_t x, fixed_t y)
{
  return (y -= viewy, (x -= viewx) || y) ?
    x >= 0 ?
      y >= 0 ?
        (x > y) ? tantoangle[SlopeDiv(y,x)] :                      // octant 0
                ANG90-1-tantoangle[SlopeDiv(x,y)] :                // octant 1
        x > (y = -y) ? 0-tantoangle[SlopeDiv(y,x)] :               // octant 8
                       ANG270+tantoangle[SlopeDiv(x,y)] :          // octant 7
      y >= 0 ? (x = -x) > y ? ANG180-1-tantoangle[SlopeDiv(y,x)] : // octant 3
                            ANG90 + tantoangle[SlopeDiv(x,y)] :    // octant 2
        (x = -x) > (y = -y) ? ANG180+tantoangle[ SlopeDiv(y,x)] :  // octant 4
                              ANG270-1-tantoangle[SlopeDiv(x,y)] : // octant 5
    0;
}

// [FG] overflow-safe R_PointToAngle() flavor,
// only used in R_CheckBBox(), R_AddLine() and P_SegLengths()

angle_t R_PointToAngleCrispy(fixed_t x, fixed_t y)
{
  // [FG] fix overflows for very long distances
  int64_t y_viewy = (int64_t)y - viewy;
  int64_t x_viewx = (int64_t)x - viewx;

  // [FG] the worst that could happen is e.g. INT_MIN-INT_MAX = 2*INT_MIN
  if (x_viewx < INT_MIN || x_viewx > INT_MAX || y_viewy < INT_MIN || y_viewy > INT_MAX)
  {
    // [FG] preserving the angle by halfing the distance in both directions
    x = (int)(x_viewx / 2 + viewx);
    y = (int)(y_viewy / 2 + viewy);
  }

  return (y -= viewy, (x -= viewx) || y) ?
    x >= 0 ?
      y >= 0 ?
        (x > y) ? tantoangle[SlopeDivCrispy(y,x)] :                      // octant 0
                ANG90-1-tantoangle[SlopeDivCrispy(x,y)] :                // octant 1
        x > (y = -y) ? 0-tantoangle[SlopeDivCrispy(y,x)] :               // octant 8
                       ANG270+tantoangle[SlopeDivCrispy(x,y)] :          // octant 7
      y >= 0 ? (x = -x) > y ? ANG180-1-tantoangle[SlopeDivCrispy(y,x)] : // octant 3
                            ANG90 + tantoangle[SlopeDivCrispy(x,y)] :    // octant 2
        (x = -x) > (y = -y) ? ANG180+tantoangle[SlopeDivCrispy(y,x)] :  // octant 4
                              ANG270-1-tantoangle[SlopeDivCrispy(x,y)] : // octant 5
    0;
}

// WiggleFix: move R_ScaleFromGlobalAngle to r_segs.c,
// above R_StoreWallRange


// [crispy] in widescreen mode, make sure the same number of horizontal
// pixels shows the same part of the game scene as in regular rendering mode
static int scaledviewwidth_nonwide, viewwidth_nonwide;
static fixed_t centerxfrac_nonwide;

//
// R_InitTextureMapping
//
// killough 5/2/98: reformatted

static void R_InitTextureMapping(void)
{
  register int i,x;
  fixed_t slopefrac;
  angle_t fov;
  double linearskyfactor;

  // Use tangent table to generate viewangletox:
  //  viewangletox will give the next greatest x
  //  after the view angle.
  //
  // Calc focallength
  //  so FIELDOFVIEW angles covers SCREENWIDTH.

  // [Nugget] Use `r_fov` instead of `custom_fov`

  if (r_fov == FOV_DEFAULT && centerxfrac == centerxfrac_nonwide)
  {
    fov = FIELDOFVIEW;
    slopefrac = finetangent[FINEANGLES / 4 + fov / 2];
    focallength = FixedDiv(centerxfrac_nonwide, slopefrac);
    lightfocallength = centerxfrac_nonwide;
    projection = centerxfrac_nonwide;
  }
  else
  {
    const double slope = (tan(r_fov * M_PI / 360.0) *
                          centerxfrac / centerxfrac_nonwide);

    // For correct light across FOV range. Calculated like R_InitTables().
    const double lightangle = atan(slope) + M_PI / FINEANGLES;
    const double lightslopefrac = tan(lightangle) * FRACUNIT;
    lightfocallength = FixedDiv(centerxfrac, lightslopefrac);

    fov = atan(slope) * FINEANGLES / M_PI;
    slopefrac = finetangent[FINEANGLES / 4 + fov / 2];
    focallength = FixedDiv(centerxfrac, slopefrac);
    projection = centerxfrac / slope;
  }

  for (i=0 ; i<FINEANGLES/2 ; i++)
    {
      int t;
      if (finetangent[i] > slopefrac)
        t = -1;
      else
        if (finetangent[i] < -slopefrac)
          t = viewwidth+1;
      else
        {
          t = FixedMul(finetangent[i], focallength);
          t = (centerxfrac - t + FRACUNIT-1) >> FRACBITS;
          if (t < -1)
            t = -1;
          else
            if (t > viewwidth+1)
              t = viewwidth+1;
        }
      viewangletox[i] = t;
    }
    
  // Scan viewangletox[] to generate xtoviewangle[]:
  //  xtoviewangle will give the smallest view angle
  //  that maps to x.

  linearskyfactor = FIXED2DOUBLE(slopefrac) * ANG90;

  for (x=0; x<=viewwidth; x++)
    {
      for (i=0; viewangletox[i] > x; i++)
        ;
      xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
      // [FG] linear horizontal sky scrolling
      linearskyangle[x] = (0.5 - x / (double)viewwidth) * linearskyfactor;
    }
    
  // Take out the fencepost cases from viewangletox.
  for (i=0; i<FINEANGLES/2; i++)
    if (viewangletox[i] == -1)
      viewangletox[i] = 0;
    else 
      if (viewangletox[i] == viewwidth+1)
        viewangletox[i] = viewwidth;
        
  clipangle = xtoviewangle[0];

  vx_clipangle = clipangle - ((fov << ANGLETOFINESHIFT) - ANG90);
}

//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//

#define DISTMAP 2

boolean smoothlight;

void R_InitLightTables (void)
{
  int i, cm;

  if (c_scalelight)
  {
    for (cm = 0; cm < numcolormaps; ++cm)
    {
      for (i = 0; i < LIGHTLEVELS; ++i)
        Z_Free(c_scalelight[cm][i]);

      Z_Free(c_scalelight[cm]);
    }
    Z_Free(c_scalelight);
  }

  if (scalelightfixed)
  {
    Z_Free(scalelightfixed);
  }

  if (c_zlight)
  {
    for (cm = 0; cm < numcolormaps; ++cm)
    {
      for (i = 0; i < LIGHTLEVELS; ++i)
        Z_Free(c_zlight[cm][i]);

      Z_Free(c_zlight[cm]);
    }
    Z_Free(c_zlight);
  }

  if (smoothlight)
  {
      LIGHTLEVELS = 32;
      LIGHTSEGSHIFT = 3;
      LIGHTBRIGHT = 2;
      MAXLIGHTSCALE = 48;
      LIGHTSCALESHIFT = 12;
      MAXLIGHTZ = 1024;
      LIGHTZSHIFT = 17;
  }
  else
  {
      LIGHTLEVELS = 16;
      LIGHTSEGSHIFT = 4;
      LIGHTBRIGHT = 1;
      MAXLIGHTSCALE = 48;
      LIGHTSCALESHIFT = 12;
      MAXLIGHTZ = 128;
      LIGHTZSHIFT = 20;
  }

  scalelightfixed = Z_Malloc(MAXLIGHTSCALE * sizeof(*scalelightfixed), PU_STATIC, 0);

  // killough 4/4/98: dynamic colormaps
  c_zlight = Z_Malloc(sizeof(*c_zlight) * numcolormaps, PU_STATIC, 0);
  c_scalelight = Z_Malloc(sizeof(*c_scalelight) * numcolormaps, PU_STATIC, 0);

  for (cm = 0; cm < numcolormaps; ++cm)
  {
    c_zlight[cm] = Z_Malloc(LIGHTLEVELS * sizeof(**c_zlight), PU_STATIC, 0);
    c_scalelight[cm] = Z_Malloc(LIGHTLEVELS * sizeof(**c_scalelight), PU_STATIC, 0);
  }

  // Calculate the light levels to use
  //  for each level / distance combination.
  for (i=0; i< LIGHTLEVELS; i++)
    {
      int j, startmap = ((LIGHTLEVELS-LIGHTBRIGHT-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;

      for (cm = 0; cm < numcolormaps; ++cm)
      {
        c_scalelight[cm][i] = Z_Malloc(MAXLIGHTSCALE * sizeof(***c_scalelight), PU_STATIC, 0);
        c_zlight[cm][i] = Z_Malloc(MAXLIGHTZ * sizeof(***c_zlight), PU_STATIC, 0);
      }

      for (j=0; j<MAXLIGHTZ; j++)
        {
          int scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
          int t, level = startmap - (scale >>= LIGHTSCALESHIFT)/DISTMAP;

          if (level < 0)
            level = 0;
          else
            if (level >= NUMCOLORMAPS)
              level = NUMCOLORMAPS-1;

          // killough 3/20/98: Initialize multiple colormaps
          level *= 256;
          for (t=0; t<numcolormaps; t++)         // killough 4/4/98
            c_zlight[t][i][j] = colormaps[t] + level;
        }
    }
}

boolean setsmoothlight;

void R_SmoothLight(void)
{
  setsmoothlight = false;
  // [crispy] re-calculate the zlight[][] array
  R_InitLightTables();
  // [crispy] re-calculate the scalelight[][] array
  // R_ExecuteSetViewSize();
  // [crispy] re-calculate fake contrast
  P_SegLengths(true);
}

int R_GetLightIndex(fixed_t scale)
{
  const int index = ((int64_t)scale * (160 << FRACBITS) / lightfocallength) >> LIGHTSCALESHIFT;
  return BETWEEN(0, MAXLIGHTSCALE - 1, index);
}

static fixed_t viewpitch;

static void R_SetupFreelook(void)
{
  fixed_t dy;
  int i;

  if (viewpitch)
  {
    dy = FixedMul(projection, -finetangent[(ANG90 - viewpitch) >> ANGLETOFINESHIFT]);
    dy = (fixed_t)((int64_t)dy * SCREENHEIGHT / ACTUALHEIGHT);
  }
  else
  {
    dy = 0;
  }

  centery = viewheight / 2 + (dy >> FRACBITS);
  centeryfrac = centery << FRACBITS;

  for (i = 0; i < viewheight; i++)
  {
    dy = abs(((i - centery) << FRACBITS) + FRACUNIT / 2);
    yslope[i] = FixedDiv(projection, dy);
  }
}


//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//

boolean setsizeneeded;
int     setblocks;

void R_SetViewSize(int blocks)
{
  setsizeneeded = true;
  setblocks = blocks;
}

//
// R_ExecuteSetViewSize
//

void R_ExecuteSetViewSize (void)
{
  int i;
  vrect_t view;

  setsizeneeded = false;

  // [Nugget] Alt. intermission background
  if (WI_UsingAltInterpic() && (gamestate == GS_INTERMISSION))
  { setblocks = 11; }

  if (setblocks == 11)
    {
      scaledviewwidth_nonwide = NONWIDEWIDTH;
      scaledviewwidth = video.unscaledw;
      scaledviewheight = SCREENHEIGHT;                    // killough 11/98
    }
  // [crispy] hard-code to SCREENWIDTH and SCREENHEIGHT minus status bar height
  else if (setblocks == 10)
    {
      scaledviewwidth_nonwide = NONWIDEWIDTH;
      scaledviewwidth = video.unscaledw;
      scaledviewheight = SCREENHEIGHT - ST_HEIGHT;
    }
  else
    {
      const int st_screen = SCREENHEIGHT - ST_HEIGHT;

      scaledviewwidth_nonwide = setblocks * 32;
      scaledviewheight = (setblocks * st_screen / 10) & ~7; // killough 11/98

      if (widescreen)
        scaledviewwidth = (scaledviewheight * video.unscaledw / st_screen) & ~7;
      else
        scaledviewwidth = scaledviewwidth_nonwide;
    }

  scaledviewx = (video.unscaledw - scaledviewwidth) / 2;

  if (scaledviewwidth == video.unscaledw)
    scaledviewy = 0;
  else
    scaledviewy = (SCREENHEIGHT - ST_HEIGHT - scaledviewheight) / 2;

  view.x = scaledviewx;
  view.y = scaledviewy;
  view.w = scaledviewwidth;
  view.h = scaledviewheight;

  V_ScaleRect(&view);

  viewwindowx = view.sx;
  viewwindowy = view.sy;
  viewwidth   = view.sw;
  viewheight  = view.sh;

  viewwidth_nonwide = V_ScaleX(scaledviewwidth_nonwide);

  { // [Nugget] FOV changes
    static int oldtic = -1;
    int fx = 0;
    int zoomtarget;

    if (strictmode || zoomed == ZOOM_RESET) { // Force zoom reset
      zoomtarget = 0;
      fovfx[FOVFX_ZOOM] = (fovfx_t) { .target = 0, .current = 0, .old = 0 };
      zoomed = ZOOM_OFF;
    }
    else {
      zoomtarget = (zoomed ? zoom_fov - custom_fov : 0);
      // In case `custom_fov` changes while zoomed in...
      if (zoomed && abs(fovfx[FOVFX_ZOOM].target) > abs(zoomtarget))
      { fovfx[FOVFX_ZOOM] = (fovfx_t) { .target = zoomtarget, .current = zoomtarget, .old = zoomtarget }; }
    }

    if (!strictmode)
    {
      if (fovfx[FOVFX_ZOOM].target != zoomtarget)
      {
        setsizeneeded = true;
      }
      else for (i = 0;  i < NUMFOVFX;  i++)
        if (fovfx[i].target || fovfx[i].current)
        {
          setsizeneeded = true;
          break;
        }
    }

    if (setsizeneeded)
    {
      if (oldtic != gametic)
      {
        setsizeneeded = false;

        fovfx[FOVFX_ZOOM].old = fovfx[FOVFX_ZOOM].current = fovfx[FOVFX_ZOOM].target;

        if (zoomtarget || fovfx[FOVFX_ZOOM].target)
        {
          // Special handling for zoom
          int step = zoomtarget - fovfx[FOVFX_ZOOM].target;
          const int sign = ((step > 0) ? 1 : -1);
          step = BETWEEN(1, 16, round(abs(step) / 3.0));

          fovfx[FOVFX_ZOOM].target += step*sign;

          if (   (sign > 0 && fovfx[FOVFX_ZOOM].target > zoomtarget)
              || (sign < 0 && fovfx[FOVFX_ZOOM].target < zoomtarget))
          {
            fovfx[FOVFX_ZOOM].target = zoomtarget;
          }

          if (fovfx[FOVFX_ZOOM].current != fovfx[FOVFX_ZOOM].target)
          { setsizeneeded = true; }
        }

        fovfx[FOVFX_TELEPORT].old = fovfx[FOVFX_TELEPORT].current = fovfx[FOVFX_TELEPORT].target;

        if (fovfx[FOVFX_TELEPORT].target)
        {
          if ((fovfx[FOVFX_TELEPORT].target -= 5) < 0)
          { fovfx[FOVFX_TELEPORT].target = 0; }
          else
          { setsizeneeded = true; }
        }
      }
      else if (uncapped)
        for (i = 0;  i < NUMFOVFX;  i++)
        { fovfx[i].current = fovfx[i].old + ((fovfx[i].target - fovfx[i].old) * ((float) fractionaltic/FRACUNIT)); }
    }

    oldtic = gametic;

    for (i = 0;  i < NUMFOVFX;  i++)
    { fx += fovfx[i].current; }

    r_fov = (WI_UsingAltInterpic() && (gamestate == GS_INTERMISSION))
           ? MAX(140, custom_fov) : custom_fov + fx;
  }

  centerxfrac = (viewwidth << FRACBITS) / 2;
  centerx = centerxfrac >> FRACBITS;
  centerxfrac_nonwide = (viewwidth_nonwide << FRACBITS) / 2;

  viewheightfrac = viewheight << (FRACBITS + 1); // [FG] sprite clipping optimizations

  R_InitBuffer();       // killough 11/98

  R_InitTextureMapping();

  R_SetupFreelook();

  // psprite scales
  pspritescale = FixedDiv(viewwidth_nonwide, SCREENWIDTH);       // killough 11/98
  pspriteiscale = FixedDiv(SCREENWIDTH, viewwidth_nonwide);      // killough 11/98

  // [FG] make sure that the product of the weapon sprite scale factor
  //      and its reciprocal is always at least FRACUNIT to
  //      fix garbage lines at the top of weapon sprites
  while (FixedMul(pspriteiscale, pspritescale) < FRACUNIT)
    pspriteiscale++;

  if (custom_fov == FOV_DEFAULT)
  {
    skyiscale = FixedDiv(SCREENWIDTH, viewwidth_nonwide);
  }
  else
  {
    skyiscale = tan(custom_fov * M_PI / 360.0) * SCREENWIDTH / viewwidth_nonwide * FRACUNIT;
  }

  for (i=0 ; i<viewwidth ; i++)
    {
      fixed_t cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
      distscale[i] = FixedDiv(FRACUNIT,cosadj);
      // thing clipping
      screenheightarray[i] = viewheight;
    }

  // Calculate the light levels to use
  //  for each level / scale combination.
  for (i=0; i<LIGHTLEVELS; i++)
    {
      int j, startmap = ((LIGHTLEVELS-LIGHTBRIGHT-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;

      for (j=0 ; j<MAXLIGHTSCALE ; j++)
        {                                       // killough 11/98:
          int t, level = startmap - j / DISTMAP;

          if (level < 0)
            level = 0;

          if (level >= NUMCOLORMAPS)
            level = NUMCOLORMAPS-1;

          // killough 3/20/98: initialize multiple colormaps
          level *= 256;

          for (t=0; t<numcolormaps; t++)     // killough 4/4/98
            c_scalelight[t][i][j] = colormaps[t] + level;
        }
    }

  // [crispy] forcefully initialize the status bar backing screen
  ST_refreshBackground();

  pspr_interp = false;
}

//
// R_Init
//

void R_Init (void)
{
  R_InitData();
  R_SetViewSize(screenblocks);
  R_InitPlanes();
  R_InitLightTables();
  R_InitSkyMap();
  R_InitTranslationTables();
  V_InitFlexTranTable();

  // [FG] spectre drawing mode
  R_SetFuzzColumnMode();
}

//
// R_PointInSubsector
//
// killough 5/2/98: reformatted, cleaned up

subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
  int nodenum = numnodes-1;

  // [FG] fix crash when loading trivial single subsector maps
  if (!numnodes)
  {
    return subsectors;
  }

  while (!(nodenum & NF_SUBSECTOR))
    nodenum = nodes[nodenum].children[R_PointOnSide(x, y, nodes+nodenum)];
  return &subsectors[nodenum & ~NF_SUBSECTOR];
}

static inline boolean CheckLocalView(const player_t *player)
{
  return (
    // Don't use localview when interpolation is preferred.
    raw_input &&
    // Don't use localview if the player is spying.
    player == &players[consoleplayer] &&
    // Don't use localview if the player is dead.
    player->playerstate != PST_DEAD &&
    // Don't use localview if the player just teleported.
    !player->mo->reactiontime &&
    // Don't use localview if a demo is playing.
    !demoplayback &&
    // Don't use localview during a netgame (single-player or solo-net only).
    (!netgame || solonet)
  );
}

//
// R_SetupFrame
//

void R_SetupFrame (player_t *player)
{
  int i, cm;
  fixed_t pitch;

  // [Nugget]
  fixed_t playerz, basepitch;
  static angle_t old_interangle, target_interangle;
  static fixed_t chasecamheight;

  viewplayer = player;
  // [AM] Interpolate the player camera if the feature is enabled.
  if (uncapped &&
      // Don't interpolate on the first tic of a level,
      // otherwise oldviewz might be garbage.
      leveltime > 1 &&
      // Don't interpolate if the player did something
      // that would necessitate turning it off for a tic.
      player->mo->interp == true &&
      // Don't interpolate during a paused state
      leveltime > oldleveltime)
  {
    // Use localview unless the player or game is in an invalid state, in which
    // case fall back to interpolation.
    const boolean use_localview = CheckLocalView(player);

    // Interpolate player camera from their old position to their current one.
    viewx = LerpFixed(player->mo->oldx, player->mo->x);
    viewy = LerpFixed(player->mo->oldy, player->mo->y);
    viewz = LerpFixed(player->oldviewz, player->viewz);

    playerz = LerpFixed(player->mo->oldz, player->mo->z); // [Nugget]

    if (use_localview)
    {
      viewangle = (player->mo->angle + localview.angle - localview.ticangle +
                   LerpAngle(localview.oldticangle, localview.ticangle));
    }
    else
    {
      viewangle = LerpAngle(player->mo->oldangle, player->mo->angle);
    }

    if (use_localview && !player->centering)
    {
      basepitch = player->pitch + localview.pitch;
      basepitch = BETWEEN(-MAX_PITCH_ANGLE, MAX_PITCH_ANGLE, basepitch);
    }
    else
    {
      basepitch = LerpFixed(player->oldpitch, player->pitch);
    }

    pitch = basepitch;

    // [crispy] pitch is actual lookdir and weapon pitch
    pitch += LerpFixed(player->oldrecoilpitch, player->recoilpitch);

    // [Nugget] Flinching
    pitch += LerpFixed(player->oldflinch, player->flinch);
  }
  else
  {
    viewx = player->mo->x;
    viewy = player->mo->y;
    viewz = player->viewz; // [FG] moved here
    viewangle = player->mo->angle;
    // [crispy] pitch is actual lookdir and weapon pitch
    basepitch = player->pitch;
    pitch = basepitch + player->recoilpitch;

    // [Nugget]
    playerz = player->mo->z;
    pitch += player->flinch; // Flinching
  }

  // [Nugget] /---------------------------------------------------------------

  // Alt. intermission background
  if (WI_UsingAltInterpic() && (gamestate == GS_INTERMISSION))
  {
    static int oldtic = -1;

    if (oldtic != gametic) {
      old_interangle = viewangle = target_interangle;
      target_interangle += ANG1;
    }
    else if (uncapped)
    { viewangle = LerpAngle(old_interangle, target_interangle); }

    oldtic = gametic;

    basepitch = pitch = 0;
  }
  else {
    target_interangle = viewangle;

    // NUGHUD
    if (STRICTMODE(st_crispyhud)) {
      angle_t viewoffset = nughud.viewoffset * ANG1/2;
      basepitch += viewoffset;
          pitch += viewoffset;
    }
  }

  // Explosion shake effect
  chasecamheight = chasecam_height * FRACUNIT;
  if (shake > 0)
  {
    static fixed_t xofs=0, yofs=0, zofs=0;

    if (!((menuactive && !demoplayback && !netgame) || paused))
    {
      static int oldtime = -1;

      #define CALCSHAKE (((Woof_Random() - 128) % 3) * FRACUNIT) * shake / MAXSHAKE
      xofs = CALCSHAKE;
      yofs = CALCSHAKE;
      zofs = CALCSHAKE;
      #undef CALCSHAKE

      if (oldtime != leveltime) { shake--; }

      oldtime = leveltime;
    }

    viewx += xofs;
    viewy += yofs;
    viewz += zofs;
    chasecamheight += zofs;
  }

  // Chasecam

  chasecam_on = STRICTMODE(chasecam_mode || (death_camera && player->mo->health <= 0 && player->playerstate == PST_DEAD))
                && !(WI_UsingAltInterpic() && (gamestate == GS_INTERMISSION));

  if (chasecam_on)
  {
    fixed_t slope = basepitch ? (fixed_t) ((int64_t) finetangent[(ANG90 - basepitch) >> ANGLETOFINESHIFT] * SCREENHEIGHT / ACTUALHEIGHT) : 0;

    static fixed_t oldextradist = 0, extradist = 0;

    const fixed_t z = MIN(playerz + ((player->mo->health <= 0 && player->playerstate == PST_DEAD) ? 6*FRACUNIT : chasecamheight),
                          player->mo->ceilingz - (2*FRACUNIT));

    fixed_t dist = chasecam_distance * FRACUNIT;

    const fixed_t oldviewx = viewx,
                  oldviewy = viewy;

    const angle_t oldviewangle = viewangle;

    if (chasecam_mode == CHASECAMMODE_FRONT) {
      viewangle += ANG180;
      slope      = -slope;
      basepitch  = -basepitch;
      pitch     += basepitch * 2;
    }

    {
      static int oldtic = -1;

      if (oldtic != gametic) {
        oldextradist = extradist;
        extradist = FixedMul(player->mo->momx, finecosine[viewangle >> ANGLETOFINESHIFT])
                  + FixedMul(player->mo->momy,   finesine[viewangle >> ANGLETOFINESHIFT]);
      }

      oldtic = gametic;
    }

    if (uncapped && leveltime > 1 && player->mo->interp == true && leveltime > oldleveltime)
    {
      dist += LerpFixed(oldextradist, extradist);
    }
    else { dist += extradist; }

    P_PositionChasecam(z, dist, slope);

    if (chasecam.hit) {
      viewx = chasecam.x;
      viewy = chasecam.y;
      viewz = chasecam.z;
    }
    else {
      const fixed_t dx = FixedMul(dist, finecosine[viewangle >> ANGLETOFINESHIFT]),
                    dy = FixedMul(dist,   finesine[viewangle >> ANGLETOFINESHIFT]);

      const sector_t *const sec = R_PointInSubsector(viewx-dx, viewy-dy)->sector;

      viewz = z + FixedMul(slope, dist);

      if (viewz < sec->floorheight+FRACUNIT || sec->ceilingheight-FRACUNIT < viewz)
      {
        fixed_t frac;

        viewz  = BETWEEN(sec->floorheight+FRACUNIT, sec->ceilingheight-FRACUNIT, viewz);
        frac   = FixedDiv(viewz - z, FixedMul(slope, dist));
        viewx -= FixedMul(dx, frac);
        viewy -= FixedMul(dy, frac);
      }
      else {
        viewx -= dx;
        viewy -= dy;
      }
    }

    chasexofs = viewx - oldviewx;
    chaseyofs = viewy - oldviewy;
    chaseaofs = viewangle - oldviewangle;
  }
  else { chasexofs = chaseyofs = chaseaofs = 0; }

  // [Nugget] ---------------------------------------------------------------/

  if (pitch != viewpitch)
  {
    viewpitch = pitch;
    R_SetupFreelook();
  }

  // 3-screen display mode.
  viewangle += viewangleoffset;

  // [Nugget]: [crispy] A11Y
  if (!(strictmode || a11y_weapon_flash))
    extralight = 0;
  else
    extralight = player->extralight;

  extralight += STRICTMODE(LIGHTBRIGHT * extra_level_brightness);

  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

  // killough 3/20/98, 4/4/98: select colormap based on player status

  if (player->mo->subsector->sector->heightsec != -1)
    {
      const sector_t *s = player->mo->subsector->sector->heightsec + sectors;
      cm = viewz < s->interpfloorheight ? s->bottommap : viewz > s->interpceilingheight ?
        s->topmap : s->midmap;
      if (cm < 0 || cm > numcolormaps)
        cm = 0;
    }
  else
    cm = 0;

  fullcolormap = colormaps[cm];
  zlight = c_zlight[cm];
  scalelight = c_scalelight[cm];

  if (player->fixedcolormap)
    {
      fixedcolormap = fullcolormap   // killough 3/20/98: use fullcolormap
        + player->fixedcolormap*256*sizeof(lighttable_t);

      walllights = scalelightfixed;

      for (i=0 ; i<MAXLIGHTSCALE ; i++)
        scalelightfixed[i] = fixedcolormap;
    }
  else
    fixedcolormap = 0;

  validcount++;
}

//
// R_ShowStats
//

int rendered_visplanes, rendered_segs, rendered_vissprites, rendered_voxels;

static void R_ClearStats(void)
{
  rendered_visplanes = 0;
  rendered_segs = 0;
  rendered_vissprites = 0;
  rendered_voxels = 0;
}

int autodetect_hom = 0;       // killough 2/7/98: HOM autodetection flag

//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{
  R_ClearStats();

  R_SetupFrame (player);

  // Clear buffers.
  R_ClearClipSegs ();
  R_ClearDrawSegs ();
  R_ClearPlanes ();
  R_ClearSprites ();
  VX_ClearVoxels ();

  if (autodetect_hom)
    { // killough 2/10/98: add flashing red HOM indicators
      pixel_t c[47*47];
      extern int lastshottic;
      int i , color = !flashing_hom || (gametic % 20) < 9 ? 0xb0 : 0;
      V_FillRect(scaledviewx, scaledviewy, scaledviewwidth, scaledviewheight, color);
      for (i=0;i<47*47;i++)
        {
          char t =
"/////////////////////////////////////////////////////////////////////////////"
"/////////////////////////////////////////////////////////////////////////////"
"///////jkkkkklk////////////////////////////////////hkllklklkklkj/////////////"
"///////////////////jkkkkklklklkkkll//////////////////////////////kkkkkklklklk"
"lkkkklk//////////////////////////jllkkkkklklklklklkkklk//////////////////////"
"//klkkllklklklkllklklkkklh//////////////////////kkkkkjkjjkkj\3\205\214\3lllkk"
"lkllh////////////////////kllkige\211\210\207\206\205\204\203\203\203\205`\206"
"\234\234\234\234kkllg//////////////////klkkjhfe\210\206\203\203\203\202\202"
"\202\202\202\202\203\205`\207\211eikkk//////////////////kkkk\3g\211\207\206"
"\204\203\202\201\201\200\200\200\200\200\201\201\202\204b\210\211\3lkh///////"
"//////////lklki\213\210b\206\203\201\201\200\200\200\200\200Z\200\200\200\202"
"\203\204\205\210\211jll/////////////////lkkk\3\212\210b\205\202\201\200\200"
"\200XW\200\200\200\200\200\200\202\203\204\206\207eklj////////////////lkkjg"
"\211b\206\204\202\200\200\200YWWX\200Z\200\200\200\202\203\203\205bdjkk//////"
"//////////llkig\211a\205\203\202\200\200\200YXWX\200\200\200\200\200\201\202"
"\203\203\206\207ekk////////////////lkki\3\211\206\204\202\201\200\200XXWWWXX"
"\200\200\200\200\202\202\204\206\207ekk////////////////lkkj\3e\206\206\204\\"
"\200\200XWVVWWWXX\200\200\200\\\203\205\207\231kk////////////////lkkjjgcccfd"
"\207\203WVUVW\200\200\202\202\204\204\205\204\206\210gkk////////////////kkkkj"
"e``\210hjjgb\200W\200\205\206fhghcbdcdfkk////////////////jkkj\3\207ab\211e"
"\213j\3g\204XX\207\213jii\212\207\203\204\210gfkj///////////////j\211lkjf\210"
"\214\3\3kj\213\213\211\205X\200\205\212\210\213\213\213\211\210\203\205gelj//"
"////////////hf\211\213kh\212\212i\212gkh\202\203\210\210\202\201\206\207\206"
"\\kkhf\210aabkk//////////////je\210\210\3g\210\207\210e\210c\205\204\202\210"
"\207\203\202\210\205\203\203fjbe\213\210bbieW/////////////ke\207\206ie\206"
"\203\203\203\205\205\204\203\210\211\207\202\202\206\210\203\204\206\207\210"
"\211\231\206\206`\206\206]/////////////kf\\\202ig\204\203\202\201\\\202\202"
"\205\207\210\207\203\202\206\206\206\205\203\203\203\202\202\203\204b\206\204"
"Z/////////////i\3\\\204j\212\204\202\201\200\202\202\202\203\206\211\210\203"
"\203c\205\202\201\201\201\200\200\201\202\204a\204\201W/////////////j\3\207"
"\210jh\206\202\200\200\200\200\200\202\206\211\205\202\202bb\201\200\200\200"
"\200\200\200\202\203b\\WW/////////////jke\206jic\203\201\200\200\200\200\202"
"\211\211\201\200\200\204\210\201\200\200W\200\200\200\201\204c\\\200]////////"
"//////kd\210\3\3e\205\202\200\200W\200\202\211\210\210\201\202\207\210\203"
"\200WWW\200\200\202\205d\\\202///////////////kkdhigb\203\201\200\200\200\202"
"\206\210\210\205\210\211\206\203\200WWW\200\201\203ce\203\205////////////////"
"ijkig\211\203\201\200\200\202\206\207\207\205\206\207\210\206\203\200\200WW"
"\200\203\206ce\202_//////////////////jig\210\203\202\200\201\206\210\210\205"
"\204\204\205\206\206\204\202\200\200\200\200\203bcd////////////////////hjgc"
"\205\202\201\203\206\210\206\204\204\202\202\204\205\206\204\200\200\200\201"
"\206\207c//////////////////////j\3\207\204\203\202\202\211c\204\201W\200\200"
"\203\205\206\203\200\200\200\203\206b///////////////////////ihd\204\203\202"
"\201\207f\205VTVTW\202\210\206Z\200\200\203aa////////////////////////jg\204"
"\204\203\201\202\210\211\211c\206\205\210d\210\200\200\200\202\204ac/////////"
"///////////////j\3b\203\203\202\202\205\207\206\205\207\207\206\206\202\200"
"\201\202\203ac/////////////////////////iid\206\204\203\202\204\205\377\205"
"\204\205\204\203\201\200\202\203\203bc//////////////////////////ej\207\205"
"\203\201\202\202\203\207\204\203\202\202\201\201\203\203bd///////////////////"
"////////ee\3a\204\201\200\201\202\205\203\201\200\200\201\202\204\205cc//////"
"//////////////////////c\3ec\203\201\200\200\201\202\201\200\200\202\203\206cc"
"//////////////////////////////c\3f\206\203\201\200\200\200\200\200\201\203bdc"
"////////////////////////////////g\3\211\206\202\\\201\200\201\202\203dde/////"
"/////////////////////////////\234\3db\203\203\203\203adec////////////////////"
"/////////////////hffed\211de////////////////////"[i];
          c[i] = t=='/' ? color : t;
        }
      if (gametic-lastshottic < TICRATE*2 && gametic-lastshottic > TICRATE/8
          && !no_killough_face) // [Nugget]
        V_DrawBlock(scaledviewx +  scaledviewwidth/2 - 24,
                    scaledviewy + scaledviewheight/2 - 24, 47, 47, c);
      R_DrawViewBorder();
    }

  // check for new console commands.
  NetUpdate ();

  // The head node is the last node output.
  R_RenderBSPNode (numnodes-1);

  VX_NearbySprites ();

  // [FG] update automap while playing
  if (automap_on)
    return;

  // Check for new console commands.
  NetUpdate ();

  R_DrawPlanes ();

  // Check for new console commands.
  NetUpdate ();

  // [crispy] draw fuzz effect independent of rendering frame rate
  R_SetFuzzPosDraw();
  R_DrawMasked ();

  // Check for new console commands.
  NetUpdate ();
}

void R_InitAnyRes(void)
{
  R_InitSpritesRes();
  R_InitBufferRes();
  R_InitPlanesRes();
}

//----------------------------------------------------------------------------
//
// $Log: r_main.c,v $
// Revision 1.13  1998/05/07  00:47:52  killough
// beautification
//
// Revision 1.12  1998/05/03  23:00:14  killough
// beautification, fix #includes and declarations
//
// Revision 1.11  1998/04/07  15:24:15  killough
// Remove obsolete HOM detector
//
// Revision 1.10  1998/04/06  04:47:46  killough
// Support dynamic colormaps
//
// Revision 1.9  1998/03/23  03:37:14  killough
// Add support for arbitrary number of colormaps
//
// Revision 1.8  1998/03/16  12:44:12  killough
// Optimize away some function pointers
//
// Revision 1.7  1998/03/09  07:27:19  killough
// Avoid using FP for point/line queries
//
// Revision 1.6  1998/02/17  06:22:45  killough
// Comment out audible HOM alarm for now
//
// Revision 1.5  1998/02/10  06:48:17  killough
// Add flashing red HOM indicator for TNTHOM cheat
//
// Revision 1.4  1998/02/09  03:22:17  killough
// Make TNTHOM control HOM detector, change array decl to MAX_*
//
// Revision 1.3  1998/02/02  13:29:41  killough
// comment out dead code, add HOM detector
//
// Revision 1.2  1998/01/26  19:24:42  phares
// First rev with no ^Ms
//
// Revision 1.1.1.1  1998/01/19  14:03:02  rand
// Lee's Jan 19 sources
//
//
//----------------------------------------------------------------------------
