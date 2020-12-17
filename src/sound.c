/*
 * See Licensing and Copyright notice in naev.h
 */

/**
 * @file sound.c
 *
 * @brief Handles all the sound details.
 */


#include "sound.h"

#include "naev.h"

#include <math.h>
#include <sys/stat.h>

#include "SDL.h"
#include "SDL_endian.h"
#include "SDL_thread.h"
#include "SDL_mutex.h"

#include "camera.h"
#include "conf.h"
#include "log.h"
#include "music.h"
#include "ncompat.h"
#include "ndata.h"
#include "nopenal.h"
#include "nstring.h"
#include "physics.h"
#include "player.h"
#include "sound.h"


/*
 * SOUND OVERVIEW
 *
 *
 * We use a priority virtual voice system with pre-allocated buffers.
 *
 * Naming:
 *    buffer - sound sample
 *    source - openal object that plays sound
 *    voice - virtual object that wants to play sound
 *
 *
 * 1) First we allocate all the buffers based on what we find inside the
 * datafile.
 * 2) Then we allocate all the possible sources (giving the music system
 * what it needs).
 * 3) Now we allow the user to dynamically create voices, these voices will
 * always try to grab a source from the source pool.  If they can't they
 * will pretend to play the buffer.
 * 4) Every so often we'll check to see if the important voices are being
 * played and take away the sources from the lesser ones.
 *
 *
 * EFX
 *
 * We use multiple effects, namely:
 *
 * - Air absorption factor
 * - Reverb
 */


#define SOUND_FADEOUT         100
#define SOUND_SUFFIX_WAV   ".wav" /**< Suffix of sounds. */
#define SOUND_SUFFIX_OGG   ".ogg" /**< Suffix of sounds. */


/**
 * @typedef voice_state_t
 * @brief The state of a voice.
 * @sa alVoice
 */
typedef enum voice_state_ {
   VOICE_STOPPED, /**< Voice is stopped. */
   VOICE_PLAYING, /**< Voice is playing. */
   VOICE_FADEOUT, /**< Voice is fading out. */
   VOICE_DESTROY  /**< Voice should get destroyed asap. */
} voice_state_t;


typedef struct alGroup_s {
   int id; /**< Group ID. */

   /* Sources. */
   ALuint *sources; /**< Sources in the group. */
   int nsources; /**< Number of sources in the group. */

   voice_state_t state; /**< Currently global group state. */
   int fade_timer; /**< Fadeout timer. */
   int speed; /**< Whether or not pitch affects. */
   double volume; /**< Volume of the group. */
} alGroup_t;
static alGroup_t *al_groups = NULL; /**< Created groups. */
static int al_ngroups       = 0; /**< Number of created groups. */
static int al_groupidgen    = 0; /**< Used to create group IDs. */


/**
 * @struct alSound
 *
 * @brief Contains a sound buffer.
 */
typedef struct alSound_ {
   char *name; /**< Buffer's name. */
   double length; /**< Length of the buffer. */
   ALuint buf; /**< Buffer data. */
} alSound;


/**
 * @struct alVoice
 *
 * @brief Represents a voice in the game.
 *
 * A voice would be any object that is creating sound.
 */
typedef struct alVoice_ {
   struct alVoice_ *prev; /**< Linked list previous member. */
   struct alVoice_ *next; /**< Linked list next member. */

   int id; /**< Identifier of the voice. */

   voice_state_t state; /**< Current state of the sound. */
   unsigned int flags; /**< Voice flags. */

   ALfloat pos[3]; /**< Position of the voice. */
   ALfloat vel[3]; /**< Velocity of the voice. */
   ALuint source; /**< Source current in use. */
   ALuint buffer; /**< Buffer attached to the voice. */
} alVoice;


/*
 * Global sound lock.
 */
SDL_mutex *sound_lock = NULL; /**< Global sound lock, always lock this before
                                   using any OpenAL functions. */

#define voiceLock()        SDL_LockMutex(voice_mutex)
#define voiceUnlock()      SDL_UnlockMutex(voice_mutex)


/*
 * Global sound properties.
 */
int sound_disabled            = 0; /**< Whether sound is disabled. */
static int sound_initialized  = 0; /**< Whether or not sound is initialized. */


/*
 * Sound list.
 */
static alSound *sound_list    = NULL; /**< List of available sounds. */
static int sound_nlist        = 0; /**< Number of available sounds. */


/*
 * Voices.
 */
static int voice_genid        = 0; /**< Voice identifier generator. */
alVoice *voice_active         = NULL; /**< Active voices. */
static alVoice *voice_pool    = NULL; /**< Pool of free voices. */
static SDL_mutex *voice_mutex = NULL; /**< Lock for voices. */


/*
 * Internally used sounds.
 */
static int snd_compression    = -1; /**< Compression sound. */
static int snd_compressionG   = -1; /**< Compression sound group. */
static double snd_compression_gain = 0.; /**< Current compression gain. */


/*
 * Global device and context.
 */
static ALCcontext *al_context = NULL; /**< OpenAL context. */
static ALCdevice *al_device   = NULL; /**< OpenAL device. */
static ALfloat svolume        = 1.; /**< Sound global volume (logarithmic). */
static ALfloat svolume_lin    = 1.; /**< Sound global volume (linear). */
static ALfloat svolume_speed  = 1.; /**< Sound global volume modulator for speed. */


/*
 * struct to hold all the sources and currently attached voice
 */
static ALuint *source_stack   = NULL; /**< Free source pool. */
static ALuint *source_total   = NULL; /**< Total source pool. */
static ALuint *source_all     = NULL; /**< All the sources. */
static int source_nstack      = 0; /**< Number of free sources in the pool. */
static int source_ntotal      = 0; /**< Number of general use sources. */
static int source_nall        = 0; /**< Total number of sources. */
static int source_mstack      = 0; /**< Memory allocated for sources in the pool. */


/*
 * EFX stuff.
 */
static ALuint efx_directSlot  = 0; /**< Direct 3d source slot. */
static ALuint efx_reverb      = 0; /**< Reverb effect. */
static ALuint efx_echo        = 0; /**< Echo effect. */


/*
 * Sound speed.
 */
static double sound_speed     = 1.; /**< Sound speed. */

/*
 * Context info.
 */
typedef struct alInfo_s {
   ALint efx; /**< Whether or not context has efx extension. */
   ALint efx_major; /**< EFX major version. */
   ALint efx_minor; /**< EFX minor version. */
   ALint efx_auxSends; /**< Number of auxiliary sends of the context. */
   /* Effect types. */
   ALint efx_reverb; /**< Reverb effect supported. */
   ALint efx_echo; /**< Echo effect supported. */
} alInfo_t;
static alInfo_t al_info; /**< OpenAL context info. */
/*
 * prototypes
 */
/* General. */
static int sound_makeList (void);
static int sound_load( alSound *snd, const char *filename );
static void sound_free( alSound *snd );
static ALuint sound_al_getSource (void);
static int al_playVoice( alVoice *v, alSound *s, ALfloat px, ALfloat py, ALfloat vx, ALfloat vy, ALint relative );
static int sound_al_loadWav( alSound *snd, SDL_RWops *rw );
static int sound_al_loadOgg( alSound *snd, OggVorbis_File *vf );
/* Voices. */
static alVoice* voice_new (void);
static int voice_add( alVoice* v );
static alVoice* voice_get( int id );
/* Creation.  */
static int sound_al_init (void);
static int al_enableEFX (void);
static void sound_al_exit (void);
/* Sound creation.  */
static int sound_al_load( alSound *snd, const char *filename );
static const char* vorbis_getErr( int err );
/* Sound playing.  */
static void sound_al_updateVoice( alVoice *v );
/* Sound management.  */
static void sound_al_update (void);
static void sound_al_stop( alVoice *v );
static void sound_al_setSpeed( double s );
static void sound_al_volumeUpdate (void);
static void al_pausev( ALint n, ALuint *s );
static void al_resumev( ALint n, ALuint *s );
/* Groups.  */
static alGroup_t *sound_al_getGroup( int group );


/**
 * @brief Initializes the sound subsystem.
 *
 *    @return 0 on success.
 */
int sound_init (void)
{
   int ret;

   /* See if sound is disabled. */
   if (conf.nosound) {
      sound_disabled = 1;
      music_disabled = 1;
   }

   /* Parse conf. */
   if (sound_disabled && music_disabled)
      return 0;

   /* Initialize sound backend. */
   ret = sound_al_init();
   if (ret != 0) {
      sound_disabled = 1;
      music_disabled = 1;
      WARN(_("Sound disabled."));
      return ret;
   }

   /* Create voice lock. */
   voice_mutex = SDL_CreateMutex();
   if (voice_mutex == NULL)
      WARN(_("Unable to create voice mutex."));

   /* Load available sounds. */
   ret = sound_makeList();
   if (ret != 0)
      return ret;

   /* Initialize music. */
   ret = music_init();
   if (ret != 0) {
      music_disabled = 1;
      WARN(_("Music disabled."));
   }

   /* Set volume. */
   if ((conf.sound > 1.) || (conf.sound < 0.))
      WARN(_("Sound has invalid value, clamping to [0:1]."));
   sound_volume(conf.sound);

   /* Initialized. */
   sound_initialized = 1;

   /* Load compression noise. */
   snd_compression = sound_get( "compression" );
   if (snd_compression >= 0) {
      snd_compressionG = sound_createGroup( 1 );
      sound_speedGroup( snd_compressionG, 0 );
   }

   return 0;
}


/**
 * @brief Initializes the sound subsystem.
 *
 *    @return 0 on success.
 */
int sound_al_init (void)
{
   int ret;
   ALuint s;
   ALint freq;
   ALint attribs[4] = { 0, 0, 0, 0 };

   /* Default values. */
   ret = 0;

   /* we'll need a mutex */
   sound_lock = SDL_CreateMutex();
   soundLock();

   /* opening the default device */
   al_device = alcOpenDevice(NULL);
   if (al_device == NULL) {
      WARN(_("Unable to open default sound device"));
      ret = -1;
      goto snderr_dev;
   }

   /* Query EFX extension. */
   if (conf.al_efx) {
      al_info.efx = alcIsExtensionPresent( al_device, "ALC_EXT_EFX" );
      if (al_info.efx == AL_TRUE) {
         attribs[0] = ALC_MAX_AUXILIARY_SENDS;
         attribs[1] = 4;
      }
   }
   else
      al_info.efx = AL_FALSE;

   /* Create the OpenAL context */
   al_context = alcCreateContext( al_device, attribs );
   if (al_context == NULL) {
      WARN(_("Unable to create OpenAL context"));
      ret = -2;
      goto snderr_ctx;
   }

   /* Clear the errors */
   alGetError();

   /* Set active context */
   if (alcMakeContextCurrent( al_context )==AL_FALSE) {
      WARN(_("Failure to set default context"));
      ret = -4;
      goto snderr_act;
   }

   /* Get context information. */
   alcGetIntegerv( al_device, ALC_FREQUENCY, sizeof(freq), &freq );

   /* Try to enable EFX. */
   if (al_info.efx == AL_TRUE) {
      al_enableEFX();
   }
   else {
      al_info.efx_reverb = AL_FALSE;
      al_info.efx_echo   = AL_FALSE;
   }

   /* Allocate source for music. */
   alGenSources( 1, &music_source );

   /* Check for errors. */
   al_checkErr();

   /* Start allocating the sources - music has already taken theirs */
   source_nstack  = 0;
   source_mstack  = conf.snd_voices;
   source_stack   = malloc( sizeof( ALuint ) * source_mstack );
   while (source_nstack < conf.snd_voices) {
      alGenSources( 1, &s );
      source_stack[source_nstack] = s;

      /* How OpenAL distance model works:
       *
       * Clamped:
       *  gain = distance_function( CLAMP( AL_REFERENCE_DISTANCE, AL_MAX_DISTANCE, distance ) );
       *
       * Distance functions:
       *                                       AL_REFERENCE_DISTANCE
       *  * Inverse = ------------------------------------------------------------------------------
       *              AL_REFERENCE_DISTANCE + AL_ROLLOFF_FACTOR ( distance - AL_REFERENCE_DISTANCE )
       *
       *             1 - AL_ROLLOFF_FACTOR ( distance - AL_REFERENCE_DISTANCE )
       *  * Linear = ----------------------------------------------------------
       *                      AL_MAX_DISTANCE - AL_REFERENCE_DISTANCE
       *
       *                  /       distance        \ -AL_ROLLOFF_FACTOR
       *  * Exponential = | --------------------- |
       *                  \ AL_REFERENCE_DISTANCE /
       *
       *
       * Some values:
       *
       *  model    falloff  reference   100     1000    5000   10000
       *  linear     1        500      1.000   0.947   0.526   0.000
       *  inverse    1        500      1.000   0.500   0.100   0.050
       *  exponent   1        500      1.000   0.500   0.100   0.050
       *  inverse   0.5       500      1.000   0.667   0.182   0.095
       *  exponent  0.5       500      1.000   0.707   0.316   0.223
       *  inverse    2        500      1.000   0.333   0.052   0.026
       *  exponent   2        500      1.000   0.250   0.010   0.003
       */
      alSourcef( s, AL_REFERENCE_DISTANCE, 500. ); /* Close distance to clamp at (doesn't get louder). */
      alSourcef( s, AL_MAX_DISTANCE,       25000. ); /* Max distance to clamp at (doesn't get quieter). */
      alSourcef( s, AL_ROLLOFF_FACTOR,     1. ); /* Determines how it drops off. */

      /* Set the filter. */
      if (al_info.efx == AL_TRUE)
         alSource3i( s, AL_AUXILIARY_SEND_FILTER, efx_directSlot, 0, AL_FILTER_NULL );

      /* Check for error. */
      if (alGetError() == AL_NO_ERROR)
         source_nstack++;
      else
         break;
   }

   if ( source_nstack == 0 ) {
      WARN( _( "OpenAL failed to initialize sources" ) );
      source_mstack = 0;
      free( source_stack );
      source_stack = NULL;
   }
   else {
      /* Reduce ram usage. */
      source_mstack = source_nstack;
      source_stack  = realloc( source_stack, sizeof( ALuint ) * source_mstack );
      /* Copy allocated sources to total stack. */
      source_ntotal = source_mstack;
      source_total  = malloc( sizeof( ALuint ) * source_mstack );
      memcpy( source_total, source_stack, sizeof( ALuint ) * source_mstack );

      /* Copy allocated sources to all stack. */
      source_nall = source_mstack;
      source_all  = malloc( sizeof( ALuint ) * source_mstack );
      memcpy( source_all, source_stack, sizeof( ALuint ) * source_mstack );
   }

   /* Set up how sound works. */
   alDistanceModel( AL_INVERSE_DISTANCE_CLAMPED ); /* Clamping is fundamental so it doesn't sound like crap. */
   alDopplerFactor( 1. );
   sound_env( SOUND_ENV_NORMAL, 0. );

   /* Check for errors. */
   al_checkErr();

   /* we can unlock now */
   soundUnlock();

   /* debug magic */
   DEBUG(_("OpenAL started: %d Hz"), freq);
   DEBUG(_("Renderer: %s"), alGetString(AL_RENDERER));
   if (al_info.efx)
      DEBUG(_("Version: %s with EFX %d.%d"), alGetString(AL_VERSION),
            al_info.efx_major, al_info.efx_minor);
   else
      DEBUG(_("Version: %s without EFX"), alGetString(AL_VERSION));
   DEBUG_BLANK();

   return ret;

   /*
    * error handling
    */
snderr_act:
   alcDestroyContext( al_context );
snderr_ctx:
   al_context = NULL;
   alcCloseDevice( al_device );
snderr_dev:
   al_device = NULL;
   soundUnlock();
   SDL_DestroyMutex( sound_lock );
   sound_lock = NULL;
   return ret;
}


/**
 * @brief Enables the OpenAL EFX extension.
 *
 *    @return 0 on success.
 */
static int al_enableEFX (void)
{
   /* Issues with ALSOFT 1.19.1 crashes so we work around it.
    * TODO: Disable someday. */
   if (strcmp(alGetString(AL_VERSION), "1.1 ALSOFT 1.19.1")==0) {
      DEBUG(_("Crashing ALSOFT version detected, disabling EFX"));
      al_info.efx = AL_FALSE;
      return -1;
   }

   /* Get general information. */
   alcGetIntegerv( al_device, ALC_MAX_AUXILIARY_SENDS, 1, &al_info.efx_auxSends );
   alcGetIntegerv( al_device, ALC_EFX_MAJOR_VERSION, 1, &al_info.efx_major );
   alcGetIntegerv( al_device, ALC_EFX_MINOR_VERSION, 1, &al_info.efx_minor );

   /* Get function pointers. */
   nalGenAuxiliaryEffectSlots  = alGetProcAddress( "alGenAuxiliaryEffectSlots" );
   nalDeleteAuxiliaryEffectSlots = alGetProcAddress( "alDeleteAuxiliaryEffectSlots" );
   nalIsAuxiliaryEffectSlot    = alGetProcAddress( "alIsAuxiliaryEffectSlot" );
   nalAuxiliaryEffectSloti     = alGetProcAddress( "alAuxiliaryEffectSloti" );
   nalAuxiliaryEffectSlotiv    = alGetProcAddress( "alAuxiliaryEffectSlotiv" );
   nalAuxiliaryEffectSlotf     = alGetProcAddress( "alAuxiliaryEffectSlotf" );
   nalAuxiliaryEffectSlotfv    = alGetProcAddress( "alAuxiliaryEffectSlotfv" );
   nalGetAuxiliaryEffectSloti  = alGetProcAddress( "alGetAuxiliaryEffectSloti" );
   nalGetAuxiliaryEffectSlotiv = alGetProcAddress( "alGetAuxiliaryEffectSlotiv" );
   nalGetAuxiliaryEffectSlotf  = alGetProcAddress( "alGetAuxiliaryEffectSlotf" );
   nalGetAuxiliaryEffectSlotfv = alGetProcAddress( "alGetAuxiliaryEffectSlotfv" );
   nalGenFilters               = alGetProcAddress( "alGenFilters" );
   nalDeleteFilters            = alGetProcAddress( "alDeleteFilters" );
   nalFilteri                  = alGetProcAddress( "alFilteri" );
   nalFilteriv                 = alGetProcAddress( "alFilteriv" );
   nalFilterf                  = alGetProcAddress( "alFilterf" );
   nalFilterfv                 = alGetProcAddress( "alFilterfv" );
   nalGenEffects               = alGetProcAddress( "alGenEffects" );
   nalDeleteEffects            = alGetProcAddress( "alDeleteEffects" );
   nalEffecti                  = alGetProcAddress( "alEffecti" );
   nalEffectiv                 = alGetProcAddress( "alEffectiv" );
   nalEffectf                  = alGetProcAddress( "alEffectf" );
   nalEffectfv                 = alGetProcAddress( "alEffectfv" );
   if (!nalGenAuxiliaryEffectSlots || !nalDeleteAuxiliaryEffectSlots ||
         !nalIsAuxiliaryEffectSlot ||
         !nalAuxiliaryEffectSloti || !nalAuxiliaryEffectSlotiv ||
         !nalAuxiliaryEffectSlotf || !nalAuxiliaryEffectSlotfv ||
         !nalGetAuxiliaryEffectSloti || !nalGetAuxiliaryEffectSlotiv ||
         !nalGetAuxiliaryEffectSlotf || !nalGetAuxiliaryEffectSlotfv ||
         !nalGenFilters || !nalDeleteFilters ||
         !nalFilteri || !nalFilteriv || !nalFilterf || !nalFilterfv ||
         !nalGenEffects || !nalDeleteEffects ||
         !nalEffecti || !nalEffectiv || !nalEffectf || !nalEffectfv) {
      DEBUG(_("OpenAL EFX functions not found, disabling EFX."));
      al_info.efx = AL_FALSE;
      return -1;
   }

   /* Create auxiliary slot. */
   nalGenAuxiliaryEffectSlots( 1, &efx_directSlot );

   /* Create reverb effect. */
   nalGenEffects( 1, &efx_reverb );
   nalEffecti( efx_reverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB );
   if (alGetError() != AL_NO_ERROR) {
      DEBUG(_("OpenAL Reverb not found, disabling."));
      al_info.efx_reverb = AL_FALSE;
      nalDeleteEffects( 1, &efx_reverb );
   }
   else {
      al_info.efx_reverb = AL_TRUE;

      /* Set Reverb parameters. */
      /*nalEffectf( efx_reverb, AL_REVERB_DECAY_TIME, 15. );*/
   }

   /* Create echo effect. */
   nalGenEffects( 1, &efx_echo );
   nalEffecti( efx_echo, AL_EFFECT_TYPE, AL_EFFECT_ECHO );
   if (alGetError() != AL_NO_ERROR) {
      DEBUG(_("OpenAL Echo not found, disabling."));
      al_info.efx_echo = AL_FALSE;
      nalDeleteEffects( 1, &efx_echo );
   }
   else {
      al_info.efx_echo = AL_TRUE;

      /* Set Echo parameters. */
      nalEffectf( efx_echo, AL_ECHO_DELAY, 0.207 );
   }

   /* Set up the listener. */
   alListenerf( AL_METERS_PER_UNIT, 5. );

   /* Check for errors. */
   al_checkErr();

   return 0;
}


/**
 * @brief Cleans up after the sound subsytem.
 */
void sound_exit (void)
{
   int i;
   alVoice *v;

   /* Nothing to disable. */
   if (sound_disabled || !sound_initialized)
      return;

   /* Exit music subsystem. */
   music_exit();

   if (voice_mutex != NULL) {
      voiceLock();
      /* free the voices. */
      while (voice_active != NULL) {
         v = voice_active;
         voice_active = v->next;
         free(v);
      }
      while (voice_pool != NULL) {
         v = voice_pool;
         voice_pool = v->next;
         free(v);
      }
      voiceUnlock();

      /* Destroy voice lock. */
      SDL_DestroyMutex(voice_mutex);
      voice_mutex = NULL;
   }

   /* free the sounds */
   for (i=0; i<sound_nlist; i++)
      sound_free( &sound_list[i] );
   free( sound_list );
   sound_list = NULL;
   sound_nlist = 0;

   /* Exit sound subsystem. */
   sound_al_exit();

   /* Sound is done. */
   sound_initialized = 0;
}


/**
 * @brief Cleans up after the sound subsytem.
 */
void sound_al_exit (void)
{
   int i;

   soundLock();

   /* Free groups. */
   for (i=0; i<al_ngroups; i++) {
      free(al_groups[i].sources);
      al_groups[i].sources  = NULL;
      al_groups[i].nsources = 0;
   }
   free(al_groups);
   al_groups  = NULL;
   al_ngroups = 0;

   /* Free stacks. */
   if (source_all != NULL) {
      alSourceStopv(   source_nall, source_all );
      alDeleteSources( source_nall, source_all );
      free(source_all);
   }
   source_all        = NULL;
   source_nall       = 0;
   free(source_total);
   source_total      = NULL;
   source_ntotal     = 0;
   free(source_stack);
   source_stack      = NULL;
   source_nstack     = 0;
   source_mstack     = 0;

   /* Clean up EFX stuff. */
   if (al_info.efx == AL_TRUE) {
      nalDeleteAuxiliaryEffectSlots( 1, &efx_directSlot );
      if (al_info.efx_reverb == AL_TRUE)
         nalDeleteEffects( 1, &efx_reverb );
      if (al_info.efx_echo == AL_TRUE)
         nalDeleteEffects( 1, &efx_echo );
   }

   /* Clean up global stuff. */
   if (al_context) {
      alcMakeContextCurrent(NULL);
      alcDestroyContext( al_context );
   }
   if (al_device)
      alcCloseDevice( al_device );

   soundUnlock();
   SDL_DestroyMutex( sound_lock );
}


/**
 * @brief Gets the buffer to sound of name.
 *
 *    @param name Name of the sound to get the id of.
 *    @return ID of the sound matching name.
 */
int sound_get( char* name )
{
   int i;

   if (sound_disabled)
      return 0;

   for (i=0; i<sound_nlist; i++)
      if (strcmp(name, sound_list[i].name)==0)
         return i;

   WARN(_("Sound '%s' not found in sound list"), name);
   return -1;
}


/**
 * @brief Gets the length of the sound buffer.
 *
 *    @param sound ID of the buffer to get the length of..
 *    @return The length of the buffer.
 */
double sound_length( int sound )
{
   if (sound_disabled)
      return 0.;

   return sound_list[sound].length;
}


/**
 * @brief Plays the sound in the first available channel.
 *
 *    @param sound Sound to play.
 *    @return Voice identifier on success.
 */
int sound_play( int sound )
{
   alVoice *v;
   alSound *s;

   if (sound_disabled)
      return 0;

   if ((sound < 0) || (sound >= sound_nlist))
      return -1;

   /* Gets a new voice. */
   v = voice_new();

   /* Get the sound. */
   s = &sound_list[sound];

   /* Try to play the sound. */
   if (al_playVoice( v, s, 0., 0., 0., 0., AL_TRUE ))
      return -1;

   /* Set state and add to list. */
   v->state = VOICE_PLAYING;
   v->id = ++voice_genid;
   voice_add(v);

   return v->id;
}


/**
 * @brief Plays a sound based on position.
 *
 *    @param sound Sound to play.
 *    @param px X position of the sound.
 *    @param py Y position of the sound.
 *    @param vx X velocity of the sound.
 *    @param vy Y velocity of the sound.
 *    @return Voice identifier on success.
 */
int sound_playPos( int sound, double px, double py, double vx, double vy )
{
   alVoice *v;
   alSound *s;
   Pilot *p;
   double cx, cy, dist;
   int target;

   if (sound_disabled)
      return 0;

   if ((sound < 0) || (sound >= sound_nlist))
      return -1;

   target = cam_getTarget();

   /* Following a pilot. */
   p = pilot_get(target);
   if (target && (p != NULL)) {
      if (!pilot_inRange( p, px, py ))
         return 0;
   }
   /* Set to a position. */
   else {
      cam_getPos(&cx, &cy);
      dist = pow2(px - cx) + pow2(py - cy);
      if (dist > pilot_sensorRange())
         return 0;
   }

   /* Gets a new voice. */
   v = voice_new();

   /* Get the sound. */
   s = &sound_list[sound];

   /* Try to play the sound. */
   if (al_playVoice( v, s, px, py, vx, vy, AL_FALSE ))
      return -1;

   /* Actually add the voice to the list. */
   v->state = VOICE_PLAYING;
   v->id = ++voice_genid;
   voice_add(v);

   return v->id;
}


/**
 * @brief Updates the position of a voice.
 *
 *    @param voice Identifier of the voice to update.
 *    @param px New x position to update to.
 *    @param py New y position to update to.
 *    @param vx New x velocity of the sound.
 *    @param vy New y velocity of the sound.
 */
int sound_updatePos( int voice, double px, double py, double vx, double vy )
{
   alVoice *v;

   if (sound_disabled)
      return 0;

   v = voice_get(voice);
   if (v != NULL) {

      /* Update the voice. */
      v->pos[0] = px;
      v->pos[1] = py;
      v->vel[0] = vx;
      v->vel[1] = vy;
   }

   return 0;
}


/**
 * @brief Updates the sounds removing obsolete ones and such.
 *
 *    @return 0 on success.
 */
int sound_update( double dt )
{
   alVoice *v, *tv;

   /* Update music if needed. */
   music_update(dt);

   if (sound_disabled)
      return 0;

   /* System update. */
   sound_al_update();

   if (voice_active == NULL)
      return 0;

   voiceLock();

   /* The actual control loop. */
   for (v=voice_active; v!=NULL; v=v->next) {
      /* Run first to clear in same iteration. */
      sound_al_updateVoice( v );

      /* Destroy and toss into pool. */
      if ((v->state == VOICE_STOPPED) || (v->state == VOICE_DESTROY)) {

         /* Remove from active list. */
         tv = v->prev;
         if (tv == NULL) {
            voice_active = v->next;
            if (voice_active != NULL)
               voice_active->prev = NULL;
         }
         else {
            tv->next = v->next;
            if (tv->next != NULL)
               tv->next->prev = tv;
         }

         /* Add to free pool. */
         v->next = voice_pool;
         v->prev = NULL;
         voice_pool = v;
         if (v->next != NULL)
            v->next->prev = v;

         /* Avoid loop blockage. */
         v = (tv != NULL) ? tv->next : voice_active;
         if (v == NULL)
            break;
      }
   }

   voiceUnlock();

   return 0;
}


/**
 * @brief Updates the group sounds.
 */
void sound_al_update (void)
{
   int i, j;
   alGroup_t *g;
   ALfloat d, v;
   unsigned int t, f;

   t = SDL_GetTicks();

   for (i=0; i<al_ngroups; i++) {
      g = &al_groups[i];
      /* Handle fadeout. */
      if (g->state != VOICE_FADEOUT)
         continue;

      /* Calculate fadeout. */
      f = t - g->fade_timer;
      if (f < SOUND_FADEOUT) {
         d = 1. - (ALfloat) f / (ALfloat) SOUND_FADEOUT;
         v = d * svolume * g->volume;
         if (g->speed)
            v *= svolume_speed;
         soundLock();
         for (j=0; j<g->nsources; j++)
            alSourcef( g->sources[j], AL_GAIN, v );
         /* Check for errors. */
         al_checkErr();
         soundUnlock();
      }
      /* Fadeout done. */
      else {
         soundLock();
         v = svolume * g->volume;
         if (g->speed)
            v *= svolume_speed;
         for (j=0; j<g->nsources; j++) {
            alSourceStop( g->sources[j] );
            alSourcei( g->sources[j], AL_BUFFER, AL_NONE );
            alSourcef( g->sources[j], AL_GAIN, v );
         }
         /* Check for errors. */
         al_checkErr();
         soundUnlock();

         /* Mark as done. */
         g->state = VOICE_PLAYING;
      }
   }
}


/**
 * @brief Updates the voice.
 *
 *    @param v Voice to update.
 */
void sound_al_updateVoice( alVoice *v )
{
   ALint state;

   /* Invalid source, mark to delete. */
   if (v->source == 0) {
      v->state = VOICE_DESTROY;
      return;
   }

   soundLock();

   /* Get status. */
   alGetSourcei( v->source, AL_SOURCE_STATE, &state );
   if (state == AL_STOPPED) {

      /* Remove buffer so it doesn't start up again if resume is called. */
      alSourcei( v->source, AL_BUFFER, AL_NONE );

      /* Check for errors. */
      al_checkErr();

      soundUnlock();

      /* Put source back on the list. */
      source_stack[source_nstack] = v->source;
      source_nstack++;
      v->source = 0;

      /* Mark as stopped - erased next iteration. */
      v->state = VOICE_STOPPED;
      return;
   }

   /* Set up properties. */
   alSourcef(  v->source, AL_GAIN, svolume*svolume_speed );
   alSourcefv( v->source, AL_POSITION, v->pos );
   alSourcefv( v->source, AL_VELOCITY, v->vel );

   /* Check for errors. */
   al_checkErr();

   soundUnlock();
}


/**
 * @brief Pauses all the sounds.
 */
void sound_pause (void)
{
   if (sound_disabled)
      return;

   soundLock();
   al_pausev( source_ntotal, source_total );
   /* Check for errors. */
   al_checkErr();
   soundUnlock();

   if (snd_compression >= 0)
      sound_pauseGroup( snd_compressionG );
}


/**
 * @brief Resumes all the sounds.
 */
void sound_resume (void)
{
   if (sound_disabled)
      return;

   soundLock();
   al_resumev( source_ntotal, source_total );
   /* Check for errors. */
   al_checkErr();
   soundUnlock();

   if (snd_compression >= 0)
      sound_resumeGroup( snd_compressionG );
}


/**
 * @brief Stops all the playing voices.
 */
void sound_stopAll (void)
{
   alVoice *v;

   if (sound_disabled)
      return;

   /* Make sure there are voices. */
   if (voice_active==NULL)
      return;

   voiceLock();
   for (v=voice_active; v!=NULL; v=v->next) {
      sound_al_stop( v );
      v->state = VOICE_STOPPED;
   }
   voiceUnlock();
}


/**
 * @brief Stops a voice from playing.
 *
 *    @param voice Identifier of the voice to stop.
 */
void sound_stop( int voice )
{
   alVoice *v;

   if (sound_disabled)
      return;

   v = voice_get(voice);
   if (v != NULL) {
      sound_al_stop( v );
      v->state = VOICE_STOPPED;
   }
}


/**
 * @brief Stops playing sound.
 */
void sound_al_stop( alVoice* voice )
{
   soundLock();

   if (voice->source != 0)
      alSourceStop( voice->source );

   /* Check for errors. */
   al_checkErr();

   soundUnlock();
}


/**
 * @brief Updates the sound listener.
 *
 *    @param dir Direction listener is facing.
 *    @param px X position of the listener.
 *    @param py Y position of the listener.
 *    @param vx X velocity of the listener.
 *    @param vy Y velocity of the listener.
 *    @return 0 on success.
 *
 * @sa sound_playPos
 */
int sound_updateListener( double dir, double px, double py,
      double vx, double vy )
{
   double c, s;
   ALfloat ori[6], pos[3], vel[3];

   if (sound_disabled)
      return 0;

   c = cos(dir);
   s = sin(dir);

   soundLock();

   ori[0] = c;
   ori[1] = s;
   ori[2] = 0.;
   ori[3] = 0.;
   ori[4] = 0.;
   ori[5] = 1.;
   alListenerfv( AL_ORIENTATION, ori );
   pos[0] = px;
   pos[1] = py;
   pos[2] = 0.;
   alListenerfv( AL_POSITION, pos );
   vel[0] = vx;
   vel[1] = vy;
   vel[2] = 0.;
   alListenerfv( AL_VELOCITY, vel );

   /* Check for errors. */
   al_checkErr();

   soundUnlock();

   return 0;
}


/**
 * @brief Sets the speed to play the sound at.
 *
 *    @param s Speed to play sound at.
 */
void sound_setSpeed( double s )
{
   double v;
   int playing;

   if (sound_disabled)
      return;

   /* We implement the brown noise here. */
   playing = (snd_compression_gain > 0.);
   if (player.tc_max > 2.)
      v = CLAMP( 0, 1., MAX( (s-2)/10., (s-2) / (player.tc_max-2) ) );
   else
      v = CLAMP( 0, 1., (s-2)/10. );

   if (v > 0.) {
      if (snd_compression >= 0) {
         if (!playing)
            sound_playGroup( snd_compressionG, snd_compression, 0 ); /* Start playing only if it's not playing. */
         sound_volumeGroup( snd_compressionG, v );
      }
      svolume_speed =  1.-v;
   }
   else if (playing) {
      if (snd_compression >= 0)
         sound_stopGroup( snd_compressionG ); /* Stop compression sound. */
      svolume_speed =  1.; /* Restore volume. */
   }
   snd_compression_gain = v;
   sound_al_volumeUpdate();

   return sound_al_setSpeed( s );
}


/**
 * @brief Internal volume update function.
 */
static void sound_al_volumeUpdate (void)
{
   int i, j;
   alGroup_t *g;
   double v;

   soundLock();
   /* Do generic ones. */
   for (i=0; i<source_ntotal; i++)
      alSourcef( source_total[i], AL_GAIN, svolume*svolume_speed );
   /* Do specific groups. */
   for (i=0; i<al_ngroups; i++) {
      g = &al_groups[i];
      v = svolume * g->volume;
      if (g->speed)
         v *= svolume_speed;
      for (j=0; j<g->nsources; j++)
         alSourcef( g->sources[j], AL_GAIN, v );
   }
   soundUnlock();
}


/**
 * @brief Set the playing speed.
 */
void sound_al_setSpeed( double s )
{
   int i, j;
   alGroup_t *g;

   soundLock();
   sound_speed = s; /* Set the speed. */
   /* Do all the groupless. */
   for (i=0; i<source_ntotal; i++)
      alSourcef( source_total[i], AL_PITCH, s );
   /* Do specific groups. */
   for (i=0; i<al_ngroups; i++) {
      g = &al_groups[i];
      if (!g->speed)
         continue;
      for (j=0; j<g->nsources; j++)
         alSourcef( g->sources[j], AL_PITCH, s );
   }
   /* Check for errors. */
   al_checkErr();
   soundUnlock();
}


/**
 * @brief Makes the list of available sounds.
 */
static int sound_makeList (void)
{
   char** files;
   size_t nfiles,i;
   char path[PATH_MAX];
   char tmp[64];
   int len, suflen, flen;
   int mem;

   if (sound_disabled)
      return 0;

   /* get the file list */
   files = ndata_list( SOUND_PATH, &nfiles );

   /* load the profiles */
   mem = 0;
   suflen = strlen(SOUND_SUFFIX_WAV);
   for (i=0; i<nfiles; i++) {
      flen = strlen(files[i]);

      /* Must be longer than suffix. */
      if (flen < suflen) {
         free(files[i]);
         continue;
      }

      /* Make sure is wav or ogg. */
      if ((strncmp( &files[i][flen - suflen], SOUND_SUFFIX_WAV, suflen)!=0) &&
            (strncmp( &files[i][flen - suflen], SOUND_SUFFIX_OGG, suflen)!=0)) {
         free(files[i]);
         continue;
      }

      /* grow the selection size */
      sound_nlist++;
      if (sound_nlist > mem) { /* we must grow */
         mem += 32; /* we'll overallocate most likely */
         sound_list = realloc( sound_list, mem*sizeof(alSound));
      }

      /* remove the suffix */
      len = flen - suflen;
      strncpy( tmp, files[i], len );
      tmp[len] = '\0';

      /* Load the sound. */
      sound_list[sound_nlist-1].name = strdup(tmp);
      nsnprintf( path, PATH_MAX, SOUND_PATH"%s", files[i] );
      if (sound_load( &sound_list[sound_nlist-1], path ))
         sound_nlist--; /* Song not actually added. */

      /* Clean up. */
      free(files[i]);
   }
   /* shrink to minimum ram usage */
   sound_list = realloc( sound_list, sound_nlist*sizeof(alSound));

   DEBUG( ngettext("Loaded %d Sound", "Loaded %d Sounds", sound_nlist), sound_nlist );

   /* More clean up. */
   free(files);

   return 0;
}


/**
 * @brief Sets the volume.
 *
 *    @param vol Volume to set to.
 *    @return 0 on success.
 */
int sound_volume( const double vol )
{
   if (sound_disabled)
      return 0;

   /* Calculate volume level. */
   svolume_lin = vol;
   if (vol > 0.) /* Floor of -48 dB (0.00390625 amplitude) */
      svolume = (ALfloat) 1 / pow(2, (1 - vol) * 8);
   else
      svolume     = 0.;

   /* Update volume. */
   sound_al_volumeUpdate();

   return 0;
}


/**
 * @brief Gets the current sound volume (linear).
 *
 *    @return The current sound volume level.
 */
double sound_getVolume (void)
{
   if (sound_disabled)
      return 0.;

   return svolume_lin;
}


/**
 * @brief Gets the current sound volume (logarithmic).
 *
 *    @return The current sound volume level.
 */
double sound_getVolumeLog(void)
{
   if (sound_disabled)
      return 0.;

   return svolume;
}


/**
 * @brief Loads a sound into the sound_list.
 *
 *    @param snd Sound to load into.
 *    @param filename Name fo the file to load.
 *    @return 0 on success.
 *
 * @sa sound_makeList
 */
static int sound_load( alSound *snd, const char *filename )
{
   if (sound_disabled)
      return -1;

   return sound_al_load( snd, filename );
}


/**
 * @brief Frees the sound.
 *
 *    @param snd Sound to free.
 */
static void sound_free( alSound *snd )
{
   /* Free general stuff. */
   free(snd->name);
   snd->name = NULL;

   /* Free internals. */
   soundLock();

   /* free the stuff */
   alDeleteBuffers( 1, &snd->buf );

   soundUnlock();
}


/**
 * @brief Creates a sound group.
 *
 *    @param size Size of the group.
 *    @return ID of the group created on success, 0 on error.
 */
int sound_createGroup( int size )
{
   int i, j, id;
   alGroup_t *g;

   if (sound_disabled)
      return 0;

   /* Get new ID. */
   id = ++al_groupidgen;

   /* Grow group list. */
   al_ngroups++;
   al_groups = realloc( al_groups, sizeof(alGroup_t) * al_ngroups );
   g        = &al_groups[ al_ngroups-1 ];
   memset( g, 0, sizeof(alGroup_t) );
   g->id    = id;
   g->state = VOICE_PLAYING;
   g->speed = 1;
   g->volume = 1.;

   /* Allocate sources. */
   g->sources  = calloc( size, sizeof(ALuint) );
   g->nsources = size;

   /* Add some sources. */
   for (i=0; i<size; i++) {
      /* Make sure there's enough. */
      if (source_nstack <= 0)
         goto group_err;

      /* Pull one off the stack. */
      source_nstack--;
      g->sources[i] = source_stack[source_nstack];

      /* Disable EFX, they don't affect groups. */
      if (al_info.efx_reverb == AL_TRUE) {
         alSourcef(  g->sources[i], AL_AIR_ABSORPTION_FACTOR, 0. );
         alSource3i( g->sources[i], AL_AUXILIARY_SEND_FILTER,
               AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL );
      }

      /* Remove from total too. */
      for (j=0; j<source_ntotal; j++) {
         if (g->sources[i] == source_total[j]) {
            source_ntotal--;
            memmove( &source_total[j], &source_total[j+1],
                  sizeof(ALuint) * (source_ntotal - j) );
            break;
         }
      }
   }

   return id;

group_err:
   free(g->sources);
   al_ngroups--;
   return 0;
}


/**
 * @brief Plays a sound in a group.
 *
 *    @param group Group to play sound in.
 *    @param sound Sound to play.
 *    @param once Whether to play only once.
 *    @return 0 on success.
 */
int sound_playGroup( int group, int sound, int once )
{
   int i, j;
   alGroup_t *g;
   ALint state;
   double v;

   if (sound_disabled)
      return 0;

   if ((sound < 0) || (sound >= sound_nlist))
      return -1;

   for (i=0; i<al_ngroups; i++) {
      /* Find group. */
      if (al_groups[i].id != group)
         continue;

      g = &al_groups[i];
      g->state = VOICE_PLAYING;
      soundLock();
      for (j=0; j<g->nsources; j++) {
         alGetSourcei( g->sources[j], AL_SOURCE_STATE, &state );

         /* No free ones, just smash the last one. */
         if (j == g->nsources-1) {
            if (state != AL_STOPPED)
               alSourceStop( g->sources[j] );
         }
         /* Ignore playing/paused. */
         else if ((state == AL_PLAYING) || (state == AL_PAUSED))
            continue;

         /* Attach buffer. */
         alSourcei( g->sources[j], AL_BUFFER, sound_list[sound].buf );

         /* Do not do positional sound. */
         alSourcei( g->sources[j], AL_SOURCE_RELATIVE, AL_TRUE );

         /* See if should loop. */
         alSourcei( g->sources[j], AL_LOOPING, (once) ? AL_FALSE : AL_TRUE );

         /* Set volume. */
         v = svolume * g->volume;
         if (g->speed)
            v *= svolume_speed;
         alSourcef( g->sources[j], AL_GAIN, v );

         /* Start playing. */
         alSourcePlay( g->sources[j] );

         /* Check for errors. */
         al_checkErr();

         soundUnlock();
         return 0;
      }
      soundUnlock();

      WARN(_("Group '%d' has no free sounds."), group );

      /* Group matched but not found. */
      break;
   }

   if (i>=al_ngroups)
      WARN(_("Group '%d' not found."), group);

   return -1;
}


/**
 * @brief Stops all the sounds in a group.
 *
 *    @param group Group to stop all its sounds.
 */
void sound_stopGroup( int group )
{
   alGroup_t *g;

   if (sound_disabled)
      return;

   g = sound_al_getGroup( group );
   if (g == NULL)
      return;

   g->state      = VOICE_FADEOUT;
   g->fade_timer = SDL_GetTicks();
}


/**
 * @brief Pauses all the sounds in a group.
 *
 *    @param group Group to pause sounds.
 */
void sound_pauseGroup( int group )
{
   alGroup_t *g;

   if (sound_disabled)
      return;

   g = sound_al_getGroup( group );
   if (g == NULL)
      return;

   soundLock();
   al_pausev( g->nsources, g->sources );
   soundUnlock();
}


/**
 * @brief Resumes all the sounds in a group.
 *
 *    @param group Group to resume sounds.
 */
void sound_resumeGroup( int group )
{
   alGroup_t *g;

   if (sound_disabled)
      return;

   g = sound_al_getGroup( group );
   if (g == NULL)
      return;

   soundLock();
   al_resumev( g->nsources, g->sources );
   soundUnlock();
}


/**
 * @brief Sets whether or not the speed affects a group.
 *
 *    @param group Group to set if speed affects it.
 *    @param enable Whether or not speed affects the group.
 */
void sound_speedGroup( int group, int enable )
{
   alGroup_t *g;

   if (sound_disabled)
      return;

   g = sound_al_getGroup( group );
   if (g == NULL)
      return;

   g->speed = enable;
}


/**
 * @brief Sets the volume of a group.
 *
 *    @param group Group to set the volume of.
 *    @param volume Volume to set to in the [0-1] range.
 */
void sound_volumeGroup( int group, double volume )
{
   alGroup_t *g;

   if (sound_disabled)
      return;

   g = sound_al_getGroup( group );
   if (g == NULL)
      return;

   g->volume = volume;
}


/**
 * @brief Gets a group by ID.
 */
static alGroup_t *sound_al_getGroup( int group )
{
   int i;
   for (i=0; i<al_ngroups; i++) {
      if (al_groups[i].id != group)
         continue;
      return &al_groups[i];
   }
   WARN(_("Group '%d' not found."), group);
   return NULL;
}


/**
 * @brief Sets up the sound environment.
 *
 *    @param env Type of environment to set up.
 *    @param param Environment parameter.
 *    @return 0 on success.
 */
int sound_env( SoundEnv_t env, double param )
{
   int i;
   ALuint s;
   ALfloat f;

   if (sound_disabled)
      return 0;

   soundLock();
   switch (env) {
      case SOUND_ENV_NORMAL:
         /* Set global parameters. */
         alSpeedOfSound( 3433. );

         if (al_info.efx == AL_TRUE) {
            /* Disconnect the effect. */
            nalAuxiliaryEffectSloti( efx_directSlot,
                  AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL );

            /* Set per-source parameters. */
            for (i=0; i<source_ntotal; i++) {
               s = source_total[i];
               alSourcef( s, AL_AIR_ABSORPTION_FACTOR, 0. );
            }
         }
         break;

      case SOUND_ENV_NEBULA:
         f = param / 1000.;

         /* Set global parameters. */
         alSpeedOfSound( 3433./(1. + f*2.) );

         if (al_info.efx == AL_TRUE) {

            if (al_info.efx_reverb == AL_TRUE) {
               /* Tweak the reverb. */
               nalEffectf( efx_reverb, AL_REVERB_DECAY_TIME,    10. );
               nalEffectf( efx_reverb, AL_REVERB_DECAY_HFRATIO, 0.5 );

               /* Connect the effect. */
               nalAuxiliaryEffectSloti( efx_directSlot,
                     AL_EFFECTSLOT_EFFECT, efx_reverb );
            }

            /* Set per-source parameters. */
            for (i=0; i<source_ntotal; i++) {
               s = source_total[i];
               /* Value is from 0. (normal) to 10..
                * It represents the attenuation per meter. In this case it decreases by
                * 0.05*AB_FACTOR dB/meter where AB_FACTOR is the air absoprtion factor.
                * In our case each pixel represents 5 meters.
                */
               alSourcef( s, AL_AIR_ABSORPTION_FACTOR, 3.*f );
            }
         }
         break;
   }

   /* Check for errors. */
   al_checkErr();

   soundUnlock();

   return 0;
}


/**
 * @brief Gets a new voice ready to be used.
 *
 *    @return New voice ready to use.
 */
alVoice* voice_new (void)
{
   alVoice *v;

   /* No free voices, allocate a new one. */
   if (voice_pool == NULL) {
      v = calloc( 1, sizeof(alVoice) );
      voice_pool = v;
      return v;
   }

   /* First free voice. */
   v = voice_pool; /* We do not touch the next nor prev, it's still in the pool. */
   return v;
}


/**
 * @brief Adds a voice to the active voice stack.
 *
 *    @param v Voice to add to the active voice stack.
 *    @return 0 on success.
 */
int voice_add( alVoice* v )
{
   alVoice *tv;

   /* Remove from pool. */
   if (v->prev != NULL) {
      tv = v->prev;
      tv->next = v->next;
      if (tv->next != NULL)
         tv->next->prev = tv;
   }
   else { /* Set pool to be the next. */
      voice_pool = v->next;
      if (voice_pool != NULL)
         voice_pool->prev = NULL;
   }

   /* Insert to the front of active voices. */
   voiceLock();
   tv = voice_active;
   v->next = tv;
   v->prev = NULL;
   voice_active = v;
   if (tv != NULL)
      tv->prev = v;
   voiceUnlock();
   return 0;
}


/**
 * @brief Gets a voice by identifier.
 *
 *    @param id Identifier to look for.
 *    @return Voice matching identifier or NULL if not found.
 */
alVoice* voice_get( int id )
{
   alVoice *v;

   /* Make sure there are voices. */
   if (voice_active==NULL)
      return NULL;

   voiceLock();
   for (v=voice_active; v!=NULL; v=v->next)
      if (v->id == id)
         break;
   voiceUnlock();

   return v;
}


/*
 * Vorbis stuff.
 */
static size_t ovpack_read( void *ptr, size_t size, size_t nmemb, void *datasource )
{
   SDL_RWops *rw = datasource;
   return (size_t) SDL_RWread( rw, ptr, size, nmemb );
}
static int ovpack_seek( void *datasource, ogg_int64_t offset, int whence )
{
   SDL_RWops *rw = datasource;
   return SDL_RWseek( rw, offset, whence );
}
static int ovpack_close( void *datasource )
{
   SDL_RWops *rw = datasource;
   return SDL_RWclose( rw );
}
static int ovpack_closeFake( void *datasource )
{
   (void) datasource;
   return 0;
}
static long ovpack_tell( void *datasource )
{
   SDL_RWops *rw = datasource;
   return SDL_RWseek( rw, 0, SEEK_CUR );
}
ov_callbacks sound_al_ovcall = {
   .read_func  = ovpack_read,
   .seek_func  = ovpack_seek,
   .close_func = ovpack_close,
   .tell_func  = ovpack_tell
}; /**< Vorbis call structure to handle rwops. */
ov_callbacks sound_al_ovcall_noclose = {
   .read_func  = ovpack_read,
   .seek_func  = ovpack_seek,
   .close_func = ovpack_closeFake,
   .tell_func  = ovpack_tell
}; /**< Vorbis call structure to handle rwops without closing. */


/**
 * @brief Loads a wav file from the rw if possible.
 *
 * @note Closes the rw.
 *
 *    @param snd Sound to load wav into.
 *    @param rw Data for the wave.
 */
static int sound_al_loadWav( alSound *snd, SDL_RWops *rw )
{
   SDL_AudioSpec wav_spec;
   Uint32 wav_length;
   Uint8 *wav_buffer;
   ALenum format;

   SDL_RWseek( rw, 0, SEEK_SET );

   /* Load WAV. */
   if (SDL_LoadWAV_RW( rw, 0, &wav_spec, &wav_buffer, &wav_length) == NULL) {
      WARN(_("SDL_LoadWav_RW failed: %s"), SDL_GetError());
      return -1;
   }

   /* Handle format. */
   switch (wav_spec.format) {
      case AUDIO_U8:
      case AUDIO_S8:
         format = (wav_spec.channels==1) ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8;
         break;
      case AUDIO_U16LSB:
      case AUDIO_S16LSB:
         format = (wav_spec.channels==1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
         break;
      case AUDIO_U16MSB:
      case AUDIO_S16MSB:
         WARN( _("Big endian WAVs unsupported!") );
         return -1;
      default:
         WARN( _("Invalid WAV format!") );
         return -1;
   }

   /* Load into openal. */
   soundLock();
   /* Create new buffer. */
   alGenBuffers( 1, &snd->buf );
   /* Put into the buffer. */
   alBufferData( snd->buf, format, wav_buffer, wav_length, wav_spec.freq );
   soundUnlock();

   /* Clean up. */
   free( wav_buffer );
   return 0;
}


/**
 * @brief Gets the vorbisfile error in human readable form..
 */
static const char* vorbis_getErr( int err )
{
   switch (err) {
      case OV_EREAD:       return _("A read from media returned an error.");
      case OV_EFAULT:      return _("Internal logic fault; indicates a bug or heap/stack corruption.");
      case OV_EIMPL:       return _("Feature not implemented.");
      case OV_EINVAL:      return _("Either an invalid argument, or incompletely initialized argument passed to libvorbisfile call");
      case OV_ENOTVORBIS:  return _("Bitstream is not Vorbis data.");
      case OV_EBADHEADER:  return _("Invalid Vorbis bitstream header.");
      case OV_EVERSION:    return _("Vorbis version mismatch.");
      case OV_EBADLINK:    return _("The given link exists in the Vorbis data stream, but is not decipherable due to garbage or corruption.");
      case OV_ENOSEEK:     return _("The given stream is not seekable.");

      default: return _("Unknown vorbisfile error.");
   }
}


/**
 * @brief Loads an ogg file from a tested format if possible.
 *
 *    @param snd Sound to load ogg into.
 *    @param vf Vorbisfile containing the song.
 */
static int sound_al_loadOgg( alSound *snd, OggVorbis_File *vf )
{
   int ret;
   long i;
   int section;
   vorbis_info *info;
   ALenum format;
   ogg_int64_t len;
   char *buf;

   /* Finish opening the file. */
   ret = ov_test_open(vf);
   if (ret) {
      WARN(_("Failed to finish loading Ogg file: %s"), vorbis_getErr(ret) );
      return -1;
   }

   /* Get file information. */
   info   = ov_info( vf, -1 );
   format = (info->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
   len    = ov_pcm_total( vf, -1 ) * info->channels * 2;

   /* Allocate memory. */
   buf = malloc( len );

   /* Fill buffer. */
   i = 0;
   while (i < len) {
      /* Fill buffer with data in the 16 bit signed samples format. */
      i += ov_read( vf, &buf[i], len-i, HAS_BIGENDIAN, 2, 1, &section );
   }

   soundLock();
   /* Create new buffer. */
   alGenBuffers( 1, &snd->buf );
   /* Put into buffer. */
   alBufferData( snd->buf, format, buf, len, info->rate );
   soundUnlock();

   /* Clean up. */
   free(buf);
   ov_clear(vf);

   return 0;
}


/**
 * @brief Loads the sound.
 *
 *    @param snd Sound to load.
 *    @param filename Name of the file to load into sound.
 */
int sound_al_load( alSound *snd, const char *filename )
{
   int ret;
   SDL_RWops *rw;
   OggVorbis_File vf;
   ALint freq, bits, channels, size;

   /* get the file data buffer from packfile */
   rw = ndata_rwops( filename );

   /* Check to see if it's an Ogg. */
   if (ov_test_callbacks( rw, &vf, NULL, 0, sound_al_ovcall_noclose )==0)
      ret = sound_al_loadOgg( snd, &vf );

   /* Otherwise try WAV. */
   else {
      /* Destroy the partially loaded vorbisfile. */
      ov_clear(&vf);

      /* Try to load Wav. */
      ret = sound_al_loadWav( snd, rw );
   }

   /* Close RWops. */
   SDL_RWclose(rw);

   /* Failed to load. */
   if (ret != 0) {
      WARN(_("Failed to load sound file '%s'."), filename);
      return ret;
   }

   soundLock();

   /* Get the length of the sound. */
   alGetBufferi( snd->buf, AL_FREQUENCY, &freq );
   alGetBufferi( snd->buf, AL_BITS, &bits );
   alGetBufferi( snd->buf, AL_CHANNELS, &channels );
   alGetBufferi( snd->buf, AL_SIZE, &size );
   if ((freq==0) || (bits==0) || (channels==0)) {
      WARN(_("Something went wrong when loading sound file '%s'."), filename);
      snd->length = 0;
   }
   else
      snd->length = (double)size / (double)(freq * (bits/8) * channels);

   /* Check for errors. */
   al_checkErr();

   soundUnlock();

   return 0;
}


/**
 * @brief Gets a free OpenAL source.
 */
static ALuint sound_al_getSource (void)
{
   ALuint source;

   /* Make sure there's enough. */
   if (source_nstack <= 0)
      return 0;

   /* Pull one off the stack. */
   source_nstack--;
   source = source_stack[source_nstack];

   return source;
}


/**
 * @brief Plays a voice.
 */
static int al_playVoice( alVoice *v, alSound *s,
      ALfloat px, ALfloat py, ALfloat vx, ALfloat vy, ALint relative )
{
   /* Set up the source and buffer. */
   v->source = sound_al_getSource();
   if (v->source == 0)
      return -1;
   v->buffer = s->buf;

   soundLock();

   /* Attach buffer. */
   alSourcei( v->source, AL_BUFFER, v->buffer );

   /* Enable positional sound. */
   alSourcei( v->source, AL_SOURCE_RELATIVE, relative );

   /* Update position. */
   v->pos[0] = px;
   v->pos[1] = py;
   v->pos[2] = 0.;
   v->vel[0] = vx;
   v->vel[1] = vy;
   v->vel[2] = 0.;

   /* Set up properties. */
   alSourcef(  v->source, AL_GAIN, svolume*svolume_speed );
   alSourcefv( v->source, AL_POSITION, v->pos );
   alSourcefv( v->source, AL_VELOCITY, v->vel );

   /* Defaults just in case. */
   alSourcei( v->source, AL_LOOPING, AL_FALSE );

   /* Start playing. */
   alSourcePlay( v->source );

   /* Check for errors. */
   al_checkErr();

   soundUnlock();

   return 0;
}


/**
 * @brief Acts like alSourcePausev but with proper checks.
 */
static void al_pausev( ALint n, ALuint *s )
{
   int i;
   ALint state;

   for (i=0; i<n; i++) {
      alGetSourcei( s[i], AL_SOURCE_STATE, &state );
      if (state == AL_PLAYING)
         alSourcePause( s[i] );
   }
}


/**
 * @brief Acts like alSourcePlayv but with proper checks to just resume.
 */
static void al_resumev( ALint n, ALuint *s )
{
   int i;
   ALint state;

   for (i=0; i<n; i++) {
      alGetSourcei( s[i], AL_SOURCE_STATE, &state );
      if (state == AL_PAUSED)
         alSourcePlay( s[i] );
   }
}
