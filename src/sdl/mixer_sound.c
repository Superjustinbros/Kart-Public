// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2014-2018 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief SDL Mixer interface for sound

#include "../doomdef.h"

#if defined(HAVE_SDL) && defined(HAVE_MIXER) && SOUND==SOUND_MIXER

#include "../sounds.h"
#include "../s_sound.h"
#include "../i_sound.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../byteptr.h"

#ifdef _MSC_VER
#pragma warning(disable : 4214 4244)
#endif
#include "SDL.h"
#ifdef _MSC_VER
#pragma warning(default : 4214 4244)
#endif

#include "SDL_mixer.h"

/* This is the version number macro for the current SDL_mixer version: */
#ifndef SDL_MIXER_COMPILEDVERSION
#define SDL_MIXER_COMPILEDVERSION \
	SDL_VERSIONNUM(MIX_MAJOR_VERSION, MIX_MINOR_VERSION, MIX_PATCHLEVEL)
#endif

/* This macro will evaluate to true if compiled with SDL_mixer at least X.Y.Z */
#ifndef SDL_MIXER_VERSION_ATLEAST
#define SDL_MIXER_VERSION_ATLEAST(X, Y, Z) \
	(SDL_MIXER_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))
#endif

// thanks alam for making the buildbots happy!
#if SDL_MIXER_VERSION_ATLEAST(2,0,2)
#define MUS_MP3_MAD MUS_MP3_MAD_UNUSED
#define MUS_MODPLUG MUS_MODPLUG_UNUSED
#endif

#ifdef HAVE_LIBGME
#include "gme/gme.h"
#define GME_TREBLE 5.0f
#define GME_BASS 1.0f

#ifdef HAVE_ZLIB
#ifndef _MSC_VER
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#endif

#ifndef _LFS64_LARGEFILE
#define _LFS64_LARGEFILE
#endif

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 0
#endif

#include "zlib.h"
#endif // HAVE_ZLIB
#endif // HAVE_LIBGME

UINT8 sound_started = false;

static Mix_Music *music;
static UINT8 music_volume, sfx_volume;
static float loop_point;
static boolean songpaused;

#ifdef HAVE_LIBGME
static Music_Emu *gme;
static INT32 current_track;
#endif

/// ------------------------
/// Audio System
/// ------------------------

void I_StartupSound(void)
{
	I_Assert(!sound_started);

#ifdef _WIN32
	// Force DirectSound instead of WASAPI
	// SDL 2.0.6+ defaults to the latter and it screws up our sound effects
	SDL_setenv("SDL_AUDIODRIVER", "directsound", 1);
#endif

	// EE inits audio first so we're following along.
	if (SDL_WasInit(SDL_INIT_AUDIO) == SDL_INIT_AUDIO)
	{
		CONS_Debug(DBG_DETAILED, "SDL Audio already started\n");
		return;
	}
	else if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		CONS_Alert(CONS_ERROR, "Error initializing SDL Audio: %s\n", SDL_GetError());
		// call to start audio failed -- we do not have it
		return;
	}

	music = NULL;
	music_volume = sfx_volume = 0;

#if SDL_MIXER_VERSION_ATLEAST(1,2,11)
	Mix_Init(MIX_INIT_FLAC|MIX_INIT_MOD|MIX_INIT_MP3|MIX_INIT_OGG);
#endif

	if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 2048) < 0)
	{
		CONS_Alert(CONS_ERROR, "Error starting SDL_Mixer: %s\n", Mix_GetError());
		// call to start audio failed -- we do not have it
		return;
	}

	sound_started = true;
	songpaused = false;
	Mix_AllocateChannels(256);
}

void I_ShutdownSound(void)
{
	if (!sound_started)
		return; // not an error condition
	sound_started = false;

	Mix_CloseAudio();
#if SDL_MIXER_VERSION_ATLEAST(1,2,11)
	Mix_Quit();
#endif

	SDL_QuitSubSystem(SDL_INIT_AUDIO);

#ifdef HAVE_LIBGME
	if (gme)
		gme_delete(gme);
#endif
}

void I_UpdateSound(void)
{
}

/// ------------------------
/// SFX
/// ------------------------

// this is as fast as I can possibly make it.
// sorry. more asm needed.
static Mix_Chunk *ds2chunk(void *stream)
{
	UINT16 ver,freq;
	UINT32 samples, i, newsamples;
	UINT8 *sound;

	SINT8 *s;
	INT16 *d;
	INT16 o;
	fixed_t step, frac;

	// lump header
	ver = READUINT16(stream); // sound version format?
	if (ver != 3) // It should be 3 if it's a doomsound...
		return NULL; // onos! it's not a doomsound!
	freq = READUINT16(stream);
	samples = READUINT32(stream);

	// convert from signed 8bit ???hz to signed 16bit 44100hz.
	switch(freq)
	{
	case 44100:
		if (samples >= UINT32_MAX>>2)
			return NULL; // would wrap, can't store.
		newsamples = samples;
		break;
	case 22050:
		if (samples >= UINT32_MAX>>3)
			return NULL; // would wrap, can't store.
		newsamples = samples<<1;
		break;
	case 11025:
		if (samples >= UINT32_MAX>>4)
			return NULL; // would wrap, can't store.
		newsamples = samples<<2;
		break;
	default:
		frac = (44100 << FRACBITS) / (UINT32)freq;
		if (!(frac & 0xFFFF)) // other solid multiples (change if FRACBITS != 16)
			newsamples = samples * (frac >> FRACBITS);
		else // strange and unusual fractional frequency steps, plus anything higher than 44100hz.
			newsamples = FixedMul(FixedDiv(samples, freq), 44100) + 1; // add 1 to counter truncation.
		if (newsamples >= UINT32_MAX>>2)
			return NULL; // would and/or did wrap, can't store.
		break;
	}
	sound = Z_Malloc(newsamples<<2, PU_SOUND, NULL); // samples * frequency shift * bytes per sample * channels

	s = (SINT8 *)stream;
	d = (INT16 *)sound;

	i = 0;
	switch(freq)
	{
	case 44100: // already at the same rate? well that makes it simple.
		while(i++ < samples)
		{
			o = ((INT16)(*s++)+0x80)<<8; // changed signedness and shift up to 16 bits
			*d++ = o; // left channel
			*d++ = o; // right channel
		}
		break;
	case 22050: // unwrap 2x
		while(i++ < samples)
		{
			o = ((INT16)(*s++)+0x80)<<8; // changed signedness and shift up to 16 bits
			*d++ = o; // left channel
			*d++ = o; // right channel
			*d++ = o; // left channel
			*d++ = o; // right channel
		}
		break;
	case 11025: // unwrap 4x
		while(i++ < samples)
		{
			o = ((INT16)(*s++)+0x80)<<8; // changed signedness and shift up to 16 bits
			*d++ = o; // left channel
			*d++ = o; // right channel
			*d++ = o; // left channel
			*d++ = o; // right channel
			*d++ = o; // left channel
			*d++ = o; // right channel
			*d++ = o; // left channel
			*d++ = o; // right channel
		}
		break;
	default: // convert arbitrary hz to 44100.
		step = 0;
		frac = ((UINT32)freq << FRACBITS) / 44100 + 1; //Add 1 to counter truncation.
		while (i < samples)
		{
			o = (INT16)(*s+0x80)<<8; // changed signedness and shift up to 16 bits
			while (step < FRACUNIT) // this is as fast as I can make it.
			{
				*d++ = o; // left channel
				*d++ = o; // right channel
				step += frac;
			}
			do {
				i++; s++;
				step -= FRACUNIT;
			} while (step >= FRACUNIT);
		}
		break;
	}

	// return Mixer Chunk.
	return Mix_QuickLoad_RAW(sound, (Uint32)((UINT8*)d-sound));
}

void *I_GetSfx(sfxinfo_t *sfx)
{
	void *lump;
	Mix_Chunk *chunk;
	SDL_RWops *rw;
#ifdef HAVE_LIBGME
	Music_Emu *emu;
	gme_info_t *info;
#endif

	if (sfx->lumpnum == LUMPERROR)
		sfx->lumpnum = S_GetSfxLumpNum(sfx);
	sfx->length = W_LumpLength(sfx->lumpnum);

	lump = W_CacheLumpNum(sfx->lumpnum, PU_SOUND);

	// convert from standard DoomSound format.
	chunk = ds2chunk(lump);
	if (chunk)
	{
		Z_Free(lump);
		return chunk;
	}

	// Not a doom sound? Try something else.
#ifdef HAVE_LIBGME
	// VGZ format
	if (((UINT8 *)lump)[0] == 0x1F
		&& ((UINT8 *)lump)[1] == 0x8B)
	{
#ifdef HAVE_ZLIB
		UINT8 *inflatedData;
		size_t inflatedLen;
		z_stream stream;
		int zErr; // Somewhere to handle any error messages zlib tosses out

		memset(&stream, 0x00, sizeof (z_stream)); // Init zlib stream
		// Begin the inflation process
		inflatedLen = *(UINT32 *)lump + (sfx->length-4); // Last 4 bytes are the decompressed size, typically
		inflatedData = (UINT8 *)Z_Malloc(inflatedLen, PU_SOUND, NULL); // Make room for the decompressed data
		stream.total_in = stream.avail_in = sfx->length;
		stream.total_out = stream.avail_out = inflatedLen;
		stream.next_in = (UINT8 *)lump;
		stream.next_out = inflatedData;

		zErr = inflateInit2(&stream, 32 + MAX_WBITS);
		if (zErr == Z_OK) // We're good to go
		{
			zErr = inflate(&stream, Z_FINISH);
			if (zErr == Z_STREAM_END) {
				// Run GME on new data
				if (!gme_open_data(inflatedData, inflatedLen, &emu, 44100))
				{
					short *mem;
					UINT32 len;
					gme_equalizer_t eq = {GME_TREBLE, GME_BASS, 0,0,0,0,0,0,0,0};

					Z_Free(inflatedData); // GME supposedly makes a copy for itself, so we don't need this lying around
					Z_Free(lump); // We're done with the uninflated lump now, too.

					gme_start_track(emu, 0);
					gme_set_equalizer(emu, &eq);
					gme_track_info(emu, &info, 0);

					len = (info->play_length * 441 / 10) << 2;
					mem = malloc(len);
					gme_play(emu, len >> 1, mem);
					gme_delete(emu);

					return Mix_QuickLoad_RAW((Uint8 *)mem, len);
				}
			}
			else
			{
				const char *errorType;
				switch (zErr)
				{
					case Z_ERRNO:
						errorType = "Z_ERRNO"; break;
					case Z_STREAM_ERROR:
						errorType = "Z_STREAM_ERROR"; break;
					case Z_DATA_ERROR:
						errorType = "Z_DATA_ERROR"; break;
					case Z_MEM_ERROR:
						errorType = "Z_MEM_ERROR"; break;
					case Z_BUF_ERROR:
						errorType = "Z_BUF_ERROR"; break;
					case Z_VERSION_ERROR:
						errorType = "Z_VERSION_ERROR"; break;
					default:
						errorType = "unknown error";
				}
				CONS_Alert(CONS_ERROR,"Encountered %s when running inflate: %s\n", errorType, stream.msg);
			}
			(void)inflateEnd(&stream);
		}
		else // Hold up, zlib's got a problem
		{
			const char *errorType;
			switch (zErr)
			{
				case Z_ERRNO:
					errorType = "Z_ERRNO"; break;
				case Z_STREAM_ERROR:
					errorType = "Z_STREAM_ERROR"; break;
				case Z_DATA_ERROR:
					errorType = "Z_DATA_ERROR"; break;
				case Z_MEM_ERROR:
					errorType = "Z_MEM_ERROR"; break;
				case Z_BUF_ERROR:
					errorType = "Z_BUF_ERROR"; break;
				case Z_VERSION_ERROR:
					errorType = "Z_VERSION_ERROR"; break;
				default:
					errorType = "unknown error";
			}
			CONS_Alert(CONS_ERROR,"Encountered %s when running inflateInit: %s\n", errorType, stream.msg);
		}
		Z_Free(inflatedData); // GME didn't open jack, but don't let that stop us from freeing this up
#else
		return NULL; // No zlib support
#endif
	}
	// Try to read it as a GME sound
	else if (!gme_open_data(lump, sfx->length, &emu, 44100))
	{
		short *mem;
		UINT32 len;
		gme_equalizer_t eq = {GME_TREBLE, GME_BASS, 0,0,0,0,0,0,0,0};

		Z_Free(lump);

		gme_start_track(emu, 0);
		gme_set_equalizer(emu, &eq);
		gme_track_info(emu, &info, 0);

		len = (info->play_length * 441 / 10) << 2;
		mem = malloc(len);
		gme_play(emu, len >> 1, mem);
		gme_delete(emu);

		return Mix_QuickLoad_RAW((Uint8 *)mem, len);
	}
#endif

	// Try to load it as a WAVE or OGG using Mixer.
	rw = SDL_RWFromMem(lump, sfx->length);
	if (rw != NULL)
	{
		chunk = Mix_LoadWAV_RW(rw, 1);
		return chunk;
	}

	return NULL; // haven't been able to get anything
}

void I_FreeSfx(sfxinfo_t *sfx)
{
	if (sfx->data)
	{
		Mix_Chunk *chunk = (Mix_Chunk*)sfx->data;
		UINT8 *abufdata = NULL;
		if (chunk->allocated == 0)
		{
			// We allocated the data in this chunk, so get the abuf from mixer, then let it free the chunk, THEN we free the data
			// I believe this should ensure the sound is not playing when we free it
			abufdata = chunk->abuf;
		}
		Mix_FreeChunk(sfx->data);
		if (abufdata)
		{
			// I'm going to assume we used Z_Malloc to allocate this data.
			Z_Free(abufdata);
		}
	}
	sfx->data = NULL;
	sfx->lumpnum = LUMPERROR;
}

INT32 I_StartSound(sfxenum_t id, UINT8 vol, UINT8 sep, UINT8 pitch, UINT8 priority, INT32 channel)
{
	UINT8 volume = (((UINT16)vol + 1) * (UINT16)sfx_volume) / 62; // (256 * 31) / 62 == 127
	INT32 handle = Mix_PlayChannel(channel, S_sfx[id].data, 0);
	Mix_Volume(handle, volume);
	Mix_SetPanning(handle, min((UINT16)(0xff-sep)<<1, 0xff), min((UINT16)(sep)<<1, 0xff));
	(void)pitch; // Mixer can't handle pitch
	(void)priority; // priority and channel management is handled by SRB2...
	return handle;
}

void I_StopSound(INT32 handle)
{
	Mix_HaltChannel(handle);
}

boolean I_SoundIsPlaying(INT32 handle)
{
	return Mix_Playing(handle);
}

void I_UpdateSoundParams(INT32 handle, UINT8 vol, UINT8 sep, UINT8 pitch)
{
	UINT8 volume = (((UINT16)vol + 1) * (UINT16)sfx_volume) / 62; // (256 * 31) / 62 == 127
	Mix_Volume(handle, volume);
	Mix_SetPanning(handle, min((UINT16)(0xff-sep)<<1, 0xff), min((UINT16)(sep)<<1, 0xff));
	(void)pitch;
}

void I_SetSfxVolume(UINT8 volume)
{
	sfx_volume = volume;
}

/// ------------------------
/// Music Hooks
/// ------------------------

static void music_loop(void)
{
	Mix_PlayMusic(music, 0);
	Mix_SetMusicPosition(loop_point);
}

#ifdef HAVE_LIBGME
static void mix_gme(void *udata, Uint8 *stream, int len)
{
	int i;
	short *p;

	(void)udata;

	// no gme? no music.
	if (!gme || gme_track_ended(gme) || songpaused)
		return;

	// play gme into stream
	gme_play(gme, len/2, (short *)stream);

	// apply volume to stream
	for (i = 0, p = (short *)stream; i < len/2; i++, p++)
		*p = ((INT32)*p) * music_volume*2 / 42;
}
#endif

/// ------------------------
/// Music System
/// ------------------------

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
	I_UnloadSong();
}

/// ------------------------
/// Music Properties
/// ------------------------

musictype_t I_SongType(void)
{
#ifdef HAVE_LIBGME
	if (gme)
		return MU_GME;
	else
#endif
	if (!music)
		return MU_NONE;
	else if (Mix_GetMusicType(music) == MUS_MID)
		return MU_MID;
	else if (Mix_GetMusicType(music) == MUS_MOD || Mix_GetMusicType(music) == MUS_MODPLUG)
		return MU_MOD;
	else if (Mix_GetMusicType(music) == MUS_MP3 || Mix_GetMusicType(music) == MUS_MP3_MAD)
		return MU_MP3;
	else
		return (musictype_t)Mix_GetMusicType(music);
}

boolean I_SongPlaying(void)
{
	return (
#ifdef HAVE_LIBGME
		(I_SongType() == MU_GME && gme) ||
#endif
		music != NULL
	);
}

boolean I_SongPaused(void)
{
	return songpaused;
}

/// ------------------------
/// Music Effects
/// ------------------------

boolean I_SetSongSpeed(float speed)
{
	if (speed > 250.0f)
		speed = 250.0f; //limit speed up to 250x
#ifdef HAVE_LIBGME
	if (gme)
	{
		SDL_LockAudio();
		gme_set_tempo(gme, speed);
		SDL_UnlockAudio();
		return true;
	}
#else
	(void)speed;
#endif
	return false;
}

/// ------------------------
/// Music Playback
/// ------------------------

boolean I_LoadSong(char *data, size_t len)
{
	const char *key1 = "LOOP";
	const char *key2 = "POINT=";
	const char *key3 = "MS=";
	const size_t key1len = strlen(key1);
	const size_t key2len = strlen(key2);
	const size_t key3len = strlen(key3);
	char *p = data;
	SDL_RWops *rw;

	if (music
#ifdef HAVE_LIBGME
		|| gme
#endif
	)
		I_UnloadSong();

#ifdef HAVE_LIBGME
	if ((UINT8)data[0] == 0x1F
		&& (UINT8)data[1] == 0x8B)
	{
#ifdef HAVE_ZLIB
		UINT8 *inflatedData;
		size_t inflatedLen;
		z_stream stream;
		int zErr; // Somewhere to handle any error messages zlib tosses out

		memset(&stream, 0x00, sizeof (z_stream)); // Init zlib stream
		// Begin the inflation process
		inflatedLen = *(UINT32 *)(data + (len-4)); // Last 4 bytes are the decompressed size, typically
		inflatedData = (UINT8 *)Z_Calloc(inflatedLen, PU_MUSIC, NULL); // Make room for the decompressed data
		stream.total_in = stream.avail_in = len;
		stream.total_out = stream.avail_out = inflatedLen;
		stream.next_in = (UINT8 *)data;
		stream.next_out = inflatedData;

		zErr = inflateInit2(&stream, 32 + MAX_WBITS);
		if (zErr == Z_OK) // We're good to go
		{
			zErr = inflate(&stream, Z_FINISH);
			if (zErr == Z_STREAM_END) {
				// Run GME on new data
				if (!gme_open_data(inflatedData, inflatedLen, &gme, 44100))
				{
					gme_equalizer_t eq = {GME_TREBLE, GME_BASS, 0,0,0,0,0,0,0,0};
					gme_start_track(gme, 0);
					current_track = 0;
					gme_set_equalizer(gme, &eq);
					Mix_HookMusic(mix_gme, gme);
					Z_Free(inflatedData); // GME supposedly makes a copy for itself, so we don't need this lying around
					return true;
				}
			}
			else
			{
				const char *errorType;
				switch (zErr)
				{
					case Z_ERRNO:
						errorType = "Z_ERRNO"; break;
					case Z_STREAM_ERROR:
						errorType = "Z_STREAM_ERROR"; break;
					case Z_DATA_ERROR:
						errorType = "Z_DATA_ERROR"; break;
					case Z_MEM_ERROR:
						errorType = "Z_MEM_ERROR"; break;
					case Z_BUF_ERROR:
						errorType = "Z_BUF_ERROR"; break;
					case Z_VERSION_ERROR:
						errorType = "Z_VERSION_ERROR"; break;
					default:
						errorType = "unknown error";
				}
				CONS_Alert(CONS_ERROR,"Encountered %s when running inflate: %s\n", errorType, stream.msg);
			}
			(void)inflateEnd(&stream);
		}
		else // Hold up, zlib's got a problem
		{
			const char *errorType;
			switch (zErr)
			{
				case Z_ERRNO:
					errorType = "Z_ERRNO"; break;
				case Z_STREAM_ERROR:
					errorType = "Z_STREAM_ERROR"; break;
				case Z_DATA_ERROR:
					errorType = "Z_DATA_ERROR"; break;
				case Z_MEM_ERROR:
					errorType = "Z_MEM_ERROR"; break;
				case Z_BUF_ERROR:
					errorType = "Z_BUF_ERROR"; break;
				case Z_VERSION_ERROR:
					errorType = "Z_VERSION_ERROR"; break;
				default:
					errorType = "unknown error";
			}
			CONS_Alert(CONS_ERROR,"Encountered %s when running inflateInit: %s\n", errorType, stream.msg);
		}
		Z_Free(inflatedData); // GME didn't open jack, but don't let that stop us from freeing this up
#else
		CONS_Alert(CONS_ERROR,"Cannot decompress VGZ; no zlib support\n");
		return true;
#endif
	}
	else if (!gme_open_data(data, len, &gme, 44100))
	{
		gme_equalizer_t eq = {GME_TREBLE, GME_BASS, 0,0,0,0,0,0,0,0};
		gme_set_equalizer(gme, &eq);
		return true;
	}
#endif

	rw = SDL_RWFromMem(data, len);
	if (rw != NULL)
	{
		music = Mix_LoadMUS_RW(rw, 1);
	}
	if (!music)
	{
		CONS_Alert(CONS_ERROR, "Mix_LoadMUS_RW: %s\n", Mix_GetError());
		return false;
	}

	// Find the OGG loop point.
	loop_point = 0.0f;

	while ((UINT32)(p - data) < len)
	{
		if (strncmp(p++, key1, key1len))
			continue;
		p += key1len-1; // skip OOP (the L was skipped in strncmp)
		if (!strncmp(p, key2, key2len)) // is it LOOPPOINT=?
		{
			p += key2len; // skip POINT=
			loop_point = (float)((44.1L+atoi(p)) / 44100.0L); // LOOPPOINT works by sample count.
			// because SDL_Mixer is USELESS and can't even tell us
			// something simple like the frequency of the streaming music,
			// we are unfortunately forced to assume that ALL MUSIC is 44100hz.
			// This means a lot of tracks that are only 22050hz for a reasonable downloadable file size will loop VERY badly.
		}
		else if (!strncmp(p, key3, key3len)) // is it LOOPMS=?
		{
			p += key3len; // skip MS=
			loop_point = (float)(atoi(p) / 1000.0L); // LOOPMS works by real time, as miliseconds.
			// Everything that uses LOOPMS will work perfectly with SDL_Mixer.
		}
		// Neither?! Continue searching.
	}

	return true;
}

void I_UnloadSong(void)
{
	I_StopSong();

#ifdef HAVE_LIBGME
	if (gme)
	{
		gme_delete(gme);
		gme = NULL;
	}
#endif
	if (music)
	{
		Mix_FreeMusic(music);
		music = NULL;
	}
}

boolean I_PlaySong(boolean looping)
{
	boolean lpz = fpclassify(loop_point) == FP_ZERO;
#ifdef HAVE_LIBGME
	if (gme)
	{
		gme_start_track(gme, 0);
		current_track = 0;
		Mix_HookMusic(mix_gme, gme);
		return true;
	}
	else
#endif
	if (!music)
		return false;


	if (Mix_PlayMusic(music, looping && lpz ? -1 : 0) == -1)
	{
		CONS_Alert(CONS_ERROR, "Mix_PlayMusic: %s\n", Mix_GetError());
		return false;
	}
	Mix_VolumeMusic((UINT32)music_volume*128/31);

	if (!lpz)
		Mix_HookMusicFinished(music_loop);
	return true;
}

void I_StopSong(void)
{
#ifdef HAVE_LIBGME
	if (gme)
	{
		Mix_HookMusic(NULL, NULL);
		current_track = -1;
	}
#endif
	if (music)
	{
		Mix_HookMusicFinished(NULL);
		Mix_HaltMusic();
	}
}

void I_PauseSong(void)
{
	Mix_PauseMusic();
	songpaused = true;
}

void I_ResumeSong(void)
{
	Mix_ResumeMusic();
	songpaused = false;
}

void I_SetMusicVolume(UINT8 volume)
{
	if (!I_SongPlaying())
		return;

#ifdef _WIN32
	if (I_SongType() == MU_MID)
		// HACK: Until we stop using native MIDI,
		// disable volume changes
		music_volume = 31;
	else
#endif
		music_volume = volume;

	Mix_VolumeMusic((UINT32)music_volume*128/31);
}

boolean I_SetSongTrack(int track)
{
#ifdef HAVE_LIBGME
	if (current_track == track)
		return false;

	// If the specified track is within the number of tracks playing, then change it
	if (gme)
	{
		SDL_LockAudio();
		if (track >= 0
			&& track < gme_track_count(gme))
		{
			gme_err_t gme_e = gme_start_track(gme, track);
			if (gme_e != NULL)
			{
				CONS_Alert(CONS_ERROR, "GME error: %s\n", gme_e);
				return false;
			}
			current_track = track;
			SDL_UnlockAudio();
			return true;
		}
		SDL_UnlockAudio();
		return false;
	}
#endif
	(void)track;
	return false;
}

#endif
