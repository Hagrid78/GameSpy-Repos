/*
Copyright (C) 1997-2001 Id Software, Inc.

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
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/
// snd_mem.c: sound caching

#include "client.h"
#include "snd_loc.h"
#include "snd_codec.h"

/*
================
ResampleSfx
================
*/
void ResampleSfx (sfx_t *sfx, int inrate, int inwidth, byte *data)
{
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;
	sfxcache_t	*sc;

	sc = sfx->cache;

	if (!sc)
		return;

	stepscale = (float) inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = sc->length / stepscale;
	sc->length = outcount;

	if (sc->loopstart != -1)
		sc->loopstart = sc->loopstart / stepscale;

	sc->speed = dma.speed;

	if (s_loadas8bit->value)
		sc->width = 1;
	else
		sc->width = inwidth;

	sc->stereo = 0;

	// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
		// fast special case
		for (i = 0; i < outcount; i++)
			((signed char *) sc->data) [i] = (int) ((unsigned char) (data[i]) - 128);
	}
	else
	{
		// general case
		samplefrac = 0;
		fracstep = stepscale * 256;

		for (i = 0; i < outcount; i++)
		{
			srcsample = samplefrac >> 8;
			samplefrac += fracstep;

			if (inwidth == 2)
				sample = LittleShort (((short *) data) [srcsample]);
			else
				sample = (int) ((unsigned char) (data[srcsample]) - 128) << 8;

			if (sc->width == 2)
				((short *) sc->data) [i] = sample;
			else
				((signed char *) sc->data) [i] = sample >> 8;
		}
	}
}

//=============================================================================

/*
==============
S_LoadSound
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
	char	namebuffer[MAX_QPATH];
	byte	*data;
	snd_info_t info;
	int		len;
	float	stepscale;
	sfxcache_t	*sc;
	char	*name;

	if (s->name[0] == '*')
		return NULL;

	// see if still in memory
	sc = s->cache;

	if (sc)
		return sc;

	// load it in
	if (s->truename)
		name = s->truename;
	else
		name = s->name;

	if (name[0] == '#')
		strcpy (namebuffer, &name[1]);
	else
		Com_sprintf (namebuffer, sizeof (namebuffer), "sound/%s", name);
	
	data = S_CodecLoad (namebuffer, &info);
	if (!data)
	{
		return NULL;
	}
	
	stepscale = (float) info.rate / dma.speed;
	len = info.samples / stepscale;

	len = len * info.width * info.channels;

	sc = s->cache = Z_Malloc (len + sizeof (sfxcache_t));

	if (!sc)
	{
		Z_Free(data);
		return NULL;
	}

	sc->length = info.samples;
	sc->loopstart = info.loopstart;
	sc->speed = info.rate;
	sc->width = info.width;
	sc->stereo = info.channels;

	ResampleSfx (s, sc->speed, sc->width, data + info.dataofs);

	Z_Free(data);

	return sc;
}
