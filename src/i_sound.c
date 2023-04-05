//
//  Copyright (C) 1999 by
//  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
//  Copyright(C) 2020-2023 Fabian Greffrath
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
//      System interface for sound.
//
//-----------------------------------------------------------------------------

#include <math.h>

#include "al.h"
#include "alc.h"

#include "doomstat.h"
#include "i_sndfile.h"
#include "i_sound.h"
#include "w_wad.h"

// Music modules
extern music_module_t music_win_module;
extern music_module_t music_fl_module;
extern music_module_t music_oal_module;
extern music_module_t music_opl_module;

typedef struct
{
    music_module_t *module;
    int num_devices;
} music_modules_t;

static music_modules_t music_modules[] =
{
#if defined(_WIN32)
    { &music_win_module, 1 },
#endif
#if defined(HAVE_FLUIDSYNTH)
    { &music_fl_module, 1 },
#endif
    { &music_opl_module, 1 },
};

static music_module_t *midi_player_module = NULL;
static music_module_t *active_module = NULL;

// haleyjd: safety variables to keep changes to *_card from making
// these routines think that sound has been initialized when it hasn't
static boolean snd_init = false;

static ALuint *openal_sources;

typedef struct {
  // SFX id of the playing sound effect.
  // Used to catch duplicates (like chainsaw).
  sfxinfo_t *sfx;
  // The channel data pointer.
  ALuint *data;
  // haleyjd 06/16/08: unique id number
  int idnum;
} channel_info_t;

channel_info_t channelinfo[MAX_CHANNELS];

// Pitch to stepping lookup.
float steptable[256];

//
// StopChannel
//
// cph 
// Stops a sound, unlocks the data 
//
static void StopChannel(int channel)
{
  int cnum;

#ifdef RANGECHECK
  // haleyjd 02/18/05: bounds checking
  if (channel < 0 || channel >= MAX_CHANNELS)
    return;
#endif

  if (channelinfo[channel].data)
  {
    alSourceStop(openal_sources[channel]);

    // [FG] immediately free samples not connected to a sound SFX
    if (channelinfo[channel].sfx == NULL)
    {
      free(channelinfo[channel].data);
    }
    channelinfo[channel].data = NULL;

    if (channelinfo[channel].sfx)
    {
      // haleyjd 06/03/06: see if we can free the sound
      for (cnum = 0; cnum < MAX_CHANNELS; cnum++)
      {
        if (cnum == channel)
          continue;

        if (channelinfo[cnum].sfx &&
            channelinfo[cnum].sfx->data == channelinfo[channel].sfx->data)
        {
          return; // still being used by some channel
        }
      }
    }
  }

  channelinfo[channel].sfx = NULL;
}

#define SOUNDHDRSIZE 8

//
// CacheSound
//
// haleyjd: needs to take a sfxinfo_t ptr, not a sound id num
// haleyjd 06/03/06: changed to return boolean for failure or success
//
static boolean CacheSound(sfxinfo_t *sfx, int channel)
{
  int lumpnum, lumplen;
  byte *lumpdata = NULL, *wavdata = NULL;

#ifdef RANGECHECK
  if (channel < 0 || channel >= MAX_CHANNELS)
  {
    I_Error("CacheSound: channel out of range!\n");
  }
#endif

  // haleyjd 02/18/05: null ptr check
  if (!snd_init || !sfx)
  {
    return false;
  }

  StopChannel(channel);

  lumpnum = I_GetSfxLumpNum(sfx);

  if (lumpnum < 0)
  {
    return false;
  }

  lumplen = W_LumpLength(lumpnum);

  // haleyjd 10/08/04: do not play zero-length sound lumps
  if (lumplen <= SOUNDHDRSIZE)
  {
    return false;
  }

  // haleyjd 06/03/06: rewrote again to make sound data properly freeable
  while (sfx->data == NULL)
  {
    byte *sampledata;
    ALsizei size, freq;
    ALenum format;
    ALuint *buffer;

    // haleyjd: this should always be called (if lump is already loaded,
    // W_CacheLumpNum handles that for us).
    lumpdata = (byte *)W_CacheLumpNum(lumpnum, PU_STATIC);

    // Check the header, and ensure this is a valid sound
    if (lumpdata[0] == 0x03 && lumpdata[1] == 0x00)
    {
      freq = (lumpdata[3] <<  8) |  lumpdata[2];
      size = (lumpdata[7] << 24) | (lumpdata[6] << 16) |
             (lumpdata[5] <<  8) |  lumpdata[4];

      // don't play sounds that think they're longer than they really are
      if (size > lumplen - SOUNDHDRSIZE)
      {
        break;
      }

      sampledata = lumpdata + SOUNDHDRSIZE;

      // All Doom sounds are 8-bit
      format = AL_FORMAT_MONO8;
    }
    else
    {
      size = lumplen;

      if (I_SND_LoadFile(lumpdata, &format, &wavdata, &size, &freq) == false)
      {
        break;
      }

      sampledata = wavdata;
    }

    buffer = malloc(sizeof(*buffer));
    alGenBuffers(1, buffer);
    alBufferData(*buffer, format, sampledata, size, freq);
    if (alGetError() != AL_NO_ERROR)
    {
        fprintf(stderr, "CacheSound: Error buffering data.\n");
        break;
    }
    sfx->data = buffer;
  }

  // don't need original lump data any more
  if (lumpdata)
  {
    Z_Free(lumpdata);
  }
  if (wavdata)
  {
    free(wavdata);
  }

  if (sfx->data == NULL)
  {
    sfx->lumpnum = -2; // [FG] don't try again
    return false;
  }

  // Preserve sound SFX id
  channelinfo[channel].sfx = sfx;

  channelinfo[channel].data = sfx->data;

  return true;
}

int forceFlipPan;

//
// I_UpdateSoundParams
//
// Changes sound parameters in response to stereo panning and relative location
// change.
//
void I_UpdateSoundParams(int channel, int volume, int separation)
{
  ALfloat pan;
  ALuint source;

  if (!snd_init)
    return;

#ifdef RANGECHECK
  if (channel < 0 || channel >= MAX_CHANNELS)
    I_Error("I_UpdateSoundParams: channel out of range");
#endif

  // SoM 7/1/02: forceFlipPan accounted for here
  if (forceFlipPan)
    separation = 254 - separation;

  source = openal_sources[channel];

  alSourcef(source, AL_GAIN, (ALfloat)volume / 127.0f);

  // Create a panning effect by moving the source in an arc around the listener.
  // https://github.com/kcat/openal-soft/issues/194
  pan = (ALfloat)separation / 255.0f - 0.5f;
  alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
  alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
  alSource3f(source, AL_POSITION, pan, 0.0f, -sqrtf(1.0f - pan * pan));
}

// [FG] variable pitch bend range
int pitch_bend_range;

//
// I_SetChannels
//
// Init internal lookups (raw data, mixing buffer, channels).
// This function sets up internal lookups used during
//  the mixing process. 
//
void I_SetChannels(void)
{
  int i;
  const double base = pitch_bend_range / 100.0;

  // Okay, reset internal mixing channels to zero.
  for (i = 0; i < MAX_CHANNELS; i++)
  {
    memset(&channelinfo[i], 0, sizeof(channel_info_t));
  }

  // This table provides step widths for pitch parameters.
  for (i = 0; i < arrlen(steptable); i++)
  {
    steptable[i] = pow(base, (double)(2 * (i - NORM_PITCH)) / NORM_PITCH); // [FG] variable pitch bend range
  }
}

//
// I_SetSfxVolume
//
void I_SetSfxVolume(int volume)
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.

  snd_SfxVolume = volume;
}

// jff 1/21/98 moved music volume down into MUSIC API with the rest

//
// I_GetSfxLumpNum
//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
  if (sfx->lumpnum == -1)
  {
    char namebuf[16];

    memset(namebuf, 0, sizeof(namebuf));

    strcpy(namebuf, "DS");
    strcpy(namebuf+2, sfx->name);

    sfx->lumpnum = W_CheckNumForName(namebuf);
  }

  return sfx->lumpnum;
}

// Almost all of the sound code from this point on was
// rewritten by Lee Killough, based on Chi's rough initial
// version.

//
// I_StartSound
//
// This function adds a sound to the list of currently
// active sounds, which is maintained as a given number
// of internal channels. Returns a free channel.
//
int I_StartSound(sfxinfo_t *sound, int vol, int sep, int pitch, boolean loop)
{
  static unsigned int id = 0;
  int channel;

  if (!snd_init)
    return -1;

  // haleyjd 06/03/06: look for an unused hardware channel
  for (channel = 0; channel < MAX_CHANNELS; channel++)
  {
    if (channelinfo[channel].data == NULL)
      break;
  }

  // all used? don't play the sound. It's preferable to miss a sound
  // than it is to cut off one already playing, which sounds weird.
  if (channel == MAX_CHANNELS)
    return -1;

  if (CacheSound(sound, channel))
  {
    ALuint source = openal_sources[channel];
    ALuint buffer = *channelinfo[channel].data;

    channelinfo[channel].idnum = id++; // give the sound a unique id
    I_UpdateSoundParams(channel, vol, sep);

    alSourcei(source, AL_BUFFER, buffer);
    alSourcei(source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    if (pitch != NORM_PITCH)
    {
      alSourcef(source, AL_PITCH, steptable[pitch]);
    }

    alSourcePlay(source);
    if (alGetError() != AL_NO_ERROR)
    {
      fprintf(stderr, "I_StartSound: Error playing sfx.\n");
    }
  }
  else
    channel = -1;

  return channel;
}

//
// I_StopSound
//
// Stop the sound. Necessary to prevent runaway chainsaw,
// and to stop rocket launches when an explosion occurs.
//
void I_StopSound(int channel)
{
  if (!snd_init)
    return;

#ifdef RANGECHECK
  if (channel < 0 || channel >= MAX_CHANNELS)
    I_Error("I_StopSound: channel out of range");
#endif

  StopChannel(channel);
}

//
// I_SoundIsPlaying
//
// haleyjd: wow, this can actually do something in the Windows version :P
//
int I_SoundIsPlaying(int channel)
{
  ALint value;

  if (!snd_init)
    return false;

#ifdef RANGECHECK
  if (channel < 0 || channel >= MAX_CHANNELS)
    I_Error("I_SoundIsPlaying: channel out of range");
#endif

  alGetSourcei(openal_sources[channel], AL_SOURCE_STATE, &value);
  return (value == AL_PLAYING);
}

//
// I_SoundID
//
// haleyjd: returns the unique id number assigned to a specific instance
// of a sound playing on a given channel. This is required to make sure
// that the higher-level sound code doesn't start updating sounds that have
// been displaced without it noticing.
//
int I_SoundID(int channel)
{
  if (!snd_init)
    return 0;

#ifdef RANGECHECK
  if (channel < 0 || channel >= MAX_CHANNELS)
    I_Error("I_SoundID: channel out of range\n");
#endif

  return channelinfo[channel].idnum;
}

// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
//  allegro does this now

void I_UpdateSound(void)
{
  int i;

  // Check all channels to see if a sound has finished

  for (i = 0; i < MAX_CHANNELS; i++)
  {
    if (channelinfo[i].data && !I_SoundIsPlaying(i))
    {
      // Sound has finished playing on this channel,
      // but sound data has not been released to cache

      StopChannel(i);
    }
  }
}

//
// I_ShutdownSound
//
// atexit handler.
//
void I_ShutdownSound(void)
{
    int i;
    ALCcontext *context;
    ALCdevice *device;

    if (!snd_init)
    {
        return;
    }

    context = alcGetCurrentContext();

    if (!context)
    {
       return;
    }

    device = alcGetContextsDevice(context);

    alDeleteSources(MAX_CHANNELS, openal_sources);
    for (i = 0; i < num_sfx; ++i)
    {
        if (S_sfx[i].data)
        {
            alDeleteBuffers(1, S_sfx[i].data);
            free(S_sfx[i].data);
        }
    }
    if (alGetError() != AL_NO_ERROR)
    {
        fprintf(stderr, "I_ShutdownSound: Failed to delete object IDs.\n");
    }

    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);
    alcCloseDevice(device);

    snd_init = false;
}

// [FG] add links for likely missing sounds

struct {
  const int from, to;
} static const sfx_subst[] = {
  {sfx_secret, sfx_itmbk},
  {sfx_itmbk,  sfx_getpow},
  {sfx_getpow, sfx_itemup},
  {sfx_itemup, sfx_None},

  {sfx_splash, sfx_oof},
  {sfx_ploosh, sfx_oof},
  {sfx_lvsiz,  sfx_oof},
  {sfx_splsml, sfx_None},
  {sfx_plosml, sfx_None},
  {sfx_lavsml, sfx_None},
};

//
// I_InitSound
//

void I_InitSound(void)
{
    const ALCchar *name;
    ALCdevice *device;
    ALCcontext *context;

    if (nosfxparm)
    {
        return;
    }

    printf("I_InitSound: ");

    name = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);

    device = alcOpenDevice(name);
    if (device)
    {
        printf("Using '%s'.\n", name);
    }
    else
    {
        printf("Could not open a device.\n");
        return;
    }

    context = alcCreateContext(device, NULL);
    if (!context || alcMakeContextCurrent(context) == ALC_FALSE)
    {
        fprintf(stderr, "I_InitSound: Error making context.\n");
        return;
    }

    openal_sources = malloc(MAX_CHANNELS * sizeof(*openal_sources));
    alGenSources(MAX_CHANNELS, openal_sources);

    I_AtExit(I_ShutdownSound, true);

    snd_init = true;

    // [FG] precache all sound effects
    {
      int i;

      printf(" Precaching all sound effects... ");
      for (i = 1; i < num_sfx; i++)
      {
        // DEHEXTRA has turned S_sfx into a sparse array
        if (!S_sfx[i].name)
          continue;

        CacheSound(&S_sfx[i], 0);
      }
      StopChannel(0);
      printf("done.\n");

      // [FG] add links for likely missing sounds
      for (i = 0; i < arrlen(sfx_subst); i++)
      {
        sfxinfo_t *from = &S_sfx[sfx_subst[i].from],
                    *to = &S_sfx[sfx_subst[i].to];

        if (from->lumpnum == -1)
        {
          from->link = to;
          from->pitch = NORM_PITCH;
          from->volume = 0;
        }
      }
    }
}

int midi_player; // current music module

void I_SetMidiPlayer(int device)
{
    int i, accum;

    if (midi_player_module)
    {
        midi_player_module->I_ShutdownMusic();
        midi_player_module = NULL;
    }

    for (i = 0, accum = 0; i < arrlen(music_modules); ++i)
    {
        int num_devices = music_modules[i].num_devices;

        if (device >= accum && device < accum + num_devices)
        {
            midi_player_module = music_modules[i].module;
            midi_player = i;
            device -= accum;
            break;
        }

        accum += num_devices;
    }

    midi_player_module->I_InitMusic(device);
    active_module = midi_player_module;
}

boolean I_InitMusic(void)
{
    if (nomusicparm)
    {
        return false;
    }

    music_oal_module.I_InitMusic(0);

    I_AtExit(I_ShutdownMusic, true);

    if (midi_player < arrlen(music_modules))
    {
        midi_player_module = music_modules[midi_player].module;
        if (midi_player_module->I_InitMusic(DEFAULT_MIDI_DEVICE))
        {
            active_module = midi_player_module;
            return true;
        }
    }

    // Fall back to module 0 device 0.
    midi_player = 0;
    midi_player_module = music_modules[0].module;
    midi_player_module->I_InitMusic(0);

    active_module = midi_player_module;

    return true;
}

void I_ShutdownMusic(void)
{
    music_oal_module.I_ShutdownMusic();

    if (midi_player_module)
    {
        midi_player_module->I_ShutdownMusic();
    }
}

void I_SetMusicVolume(int volume)
{
    if (active_module)
    {
        active_module->I_SetMusicVolume(volume);
    }
}

void I_PauseSong(void *handle)
{
    active_module->I_PauseSong(handle);
}

void I_ResumeSong(void *handle)
{
    active_module->I_ResumeSong(handle);
}

boolean IsMid(byte *mem, int len)
{
    return len > 4 && !memcmp(mem, "MThd", 4);
}

boolean IsMus(byte *mem, int len)
{
    return len > 4 && !memcmp(mem, "MUS\x1a", 4);
}

void *I_RegisterSong(void *data, int size)
{
    if (IsMus(data, size) || IsMid(data, size))
    {
        if (midi_player_module)
        {
            active_module = midi_player_module;
            return active_module->I_RegisterSong(data, size);
        }
    }

    if (midi_player_module == &music_opl_module)
    {
        midi_player_module->I_ShutdownMusic();
    }

    active_module = &music_oal_module;
    return active_module->I_RegisterSong(data, size);
}

void I_PlaySong(void *handle, boolean looping)
{
    active_module->I_PlaySong(handle, looping);
}

void I_StopSong(void *handle)
{
    active_module->I_StopSong(handle);
}

void I_UnRegisterSong(void *handle)
{
    active_module->I_UnRegisterSong(handle);
}

// Get a list of devices for all music modules. Retrieve the selected device, as
// each module manages and stores its own devices independently.

int I_DeviceList(const char *devices[], int size, int *current_device)
{
    int i, accum;

    *current_device = 0;

    for (i = 0, accum = 0; i < arrlen(music_modules); ++i)
    {
        int numdev, curdev;
        music_module_t *module = music_modules[i].module;

        numdev = module->I_DeviceList(devices + accum, size - accum, &curdev);

        music_modules[i].num_devices = numdev;

        if (midi_player == i)
        {
            *current_device = accum + curdev;
        }

        accum += numdev;
    }

    return accum;
}
