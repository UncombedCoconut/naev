/*
 * See Licensing and Copyright notice in naev.h
 */

/**
 * @file music.c
 *
 * @brief Controls all the music playing.
 */


#include <math.h>
#include <vorbis/vorbisfile.h>

#include "music.h"

#include "naev.h"

#include "SDL.h"
#include "SDL_rwops.h"
#include "SDL_thread.h"

#include "conf.h"
#include "log.h"
#include "music.h"
#include "naev.h"
#include "ndata.h"
#include "nluadef.h"
#include "nlua.h"
#include "nlua_music.h"
#include "nlua_var.h"
#include "nstring.h"
#include "sound_openal.h"


#define MUSIC_SUFFIX       ".ogg" /**< Suffix of musics. */
#define CHUNK_SIZE         32 /**< Size of a chunk to allocate. */
#define RG_PREAMP_DB       0.0 /**< Default pre-amp in dB.  */


typedef enum music_cmd_e {
   MUSIC_CMD_NONE,
   MUSIC_CMD_KILL,
   MUSIC_CMD_STOP,
   MUSIC_CMD_PLAY,
   MUSIC_CMD_PAUSE,
   MUSIC_CMD_FADEIN,
   MUSIC_CMD_FADEOUT
} music_cmd_t;


typedef enum music_state_e {
   MUSIC_STATE_DEAD,
   MUSIC_STATE_STARTUP,
   MUSIC_STATE_IDLE,
   MUSIC_STATE_FADEIN,
   MUSIC_STATE_FADEOUT,
   MUSIC_STATE_PLAYING,
   MUSIC_STATE_PAUSED,
   /* Internal usage. */
   MUSIC_STATE_LOADING,
   MUSIC_STATE_STOPPING,
   MUSIC_STATE_PAUSING,
   MUSIC_STATE_RESUMING
} music_state_t;


static SDL_Thread *music_player = NULL; /**< Music player thread. */

/*
 * Playing buffers.
 */
static int music_bufSize            = 32*1024; /**< Size of music playing buffer. */
static char *music_buf              = NULL; /**< Music playing buffer. */


static music_cmd_t   music_command  = MUSIC_CMD_NONE; /**< Target music state. */
static music_state_t music_state    = MUSIC_STATE_DEAD; /**< Current music state. */
static int music_forced             = 0; /**< Whether or not music is force stopped. */


/*
 * saves the music to ram in this structure
 */
typedef struct alMusic_ {
   char name[PATH_MAX]; /**< Song name. */
   SDL_RWops *rw; /**< RWops file reading from. */
   OggVorbis_File stream; /**< Vorbis file stream. */
   vorbis_info* info; /**< Information of the stream. */
   ALenum format; /**< Stream format. */
   /* Replaygain information. */
   ALfloat rg_scale_factor; /**< Scale factor. */
   ALfloat rg_max_scale; /**< Maximum scale factor before clipping. */
} alMusic;


/*
 * song currently playing
 */
static alMusic music_vorbis; /**< Current music. */
static ALuint music_buffer[2]; /**< Front and back buffer. */


/*
 * volume
 */
static ALfloat music_vol     = 1.; /**< Current volume level (logarithmic). */
static ALfloat music_vol_lin = 1.; /**< Current volume level (linear). */


int music_disabled = 0; /**< Whether or not music is disabled. */


/*
 * Locks.
 */
static SDL_mutex *music_lock = NULL; /**< lock for music_runLua so it doesn't
                                          run twice in a row with weird
                                          results.
                                          DO NOT CALL MIX_* FUNCTIONS WHEN
                                          LOCKED!!! */
static SDL_mutex *music_vorbis_lock = NULL; /**< Lock for vorbisfile operations. */
static SDL_cond  *music_state_cond  = NULL; /**< Cond for thread to signal status updates. */
static SDL_mutex *music_state_lock  = NULL; /**< Lock for music state. */

/* Lock for all state/cond operations. */
#define musicLock()        SDL_mutexP(music_state_lock)
#define musicUnlock()      SDL_mutexV(music_state_lock)

/* Lock for all vorbisfile operations. */
#define musicVorbisLock()  SDL_mutexP(music_vorbis_lock)
#define musicVorbisUnlock() SDL_mutexV(music_vorbis_lock)


static int music_runchoose = 0; /**< Whether or not music should run the choose function. */
static char music_situation[PATH_MAX]; /**< What situation music is in. */


/*
 * global music lua
 */
static nlua_env music_env = LUA_NOREF; /**< The Lua music control env. */
/* functions */
static int music_runLua( const char *situation );


/*
 * The current music.
 */
static char *music_name       = NULL; /**< Current music name. */
static unsigned int music_start = 0; /**< Music start playing time. */
static double music_timer     = 0.; /**< Music timer. */


/*
 * prototypes
 */
/* music stuff */
static int music_al_init (void);
static int music_find (void);
static void music_free (void);
static void music_al_free (void);
static void music_al_exit (void);
static int music_al_load( const char* name, SDL_RWops *rw );
/* Lua stuff */
static int music_luaInit (void);
static void music_luaQuit (void);
/* Music thread stuff. */
#ifdef HAVE_OV_READ_FILTER
static void rg_filter( float **pcm, long channels, long samples, void *filter_param );
#endif /* HAVE_OV_READ_FILTER */
static void music_thread_kill (void);
static int music_thread( void* unused );
static int stream_loadBuffer( ALuint buffer );


/**
 * @brief Updates the music.
 */
void music_update( double dt )
{
   char buf[PATH_MAX];

   if (music_disabled)
      return;

   /* Timer stuff. */
   if (music_timer > 0.) {
      music_timer -= dt;
      if (music_timer <= 0.)
         music_runchoose = 1;
   }

   /* Lock music and see if needs to update. */
   SDL_mutexP(music_lock);
   if (music_runchoose == 0) {
      SDL_mutexV(music_lock);
      return;
   }
   music_runchoose = 0;
   strncpy(buf, music_situation, PATH_MAX);
   buf[ PATH_MAX-1 ] = '\0';
   SDL_mutexV(music_lock);
   music_runLua( buf );

   /* Make sure music is playing. */
   if (!music_isPlaying())
      music_choose("idle");
}


/**
 * @brief Runs the Lua music choose function.
 *
 *    @param situation Situation in to choose music for.
 *    @return 0 on success.
 */
static int music_runLua( const char *situation )
{
   if (music_disabled)
      return 0;

   /* Run the choose function in Lua. */
   nlua_getenv( music_env, "choose" );
   if (situation != NULL)
      lua_pushstring( naevL, situation );
   else
      lua_pushnil( naevL );
   if (nlua_pcall(music_env, 1, 0)) { /* error has occurred */
      WARN(_("Error while choosing music: %s"), lua_tostring(naevL,-1));
      lua_pop(naevL,1);
   }

   return 0;
}


/**
 * @brief Initializes the music subsystem.
 *
 *    @return 0 on success.
 */
int music_init (void)
{
   if (music_disabled)
      return 0;

   /* Start the subsystem. */
   if (music_al_init())
      return -1;

   /* Load the music. */
   if (music_find() < 0)
      return -1;

   /* Start up Lua. */
   if (music_luaInit() < 0)
      return -1;

   /* Set the volume. */
   if ((conf.music > 1.) || (conf.music < 0.))
      WARN(_("Music has invalid value, clamping to [0:1]."));
   music_volume(conf.music);

   /* Create the lock. */
   music_lock = SDL_CreateMutex();

   return 0;
}


/**
 * @brief Exits the music subsystem.
 */
void music_exit (void)
{
   if (music_disabled)
      return;

   /* Free the music. */
   music_free();

   /* Exit the subsystem. */
   music_al_exit();

   /* Destroy the lock. */
   if (music_lock != NULL) {
      SDL_DestroyMutex(music_lock);
      music_lock = NULL;
   }

   /* Clean up Lua. */
   music_luaQuit();
}


/**
 * @brief Frees the current playing music.
 */
static void music_free (void)
{
   if (music_disabled)
      return;

   free(music_name);
   music_name = NULL;
   music_start = 0;

   music_al_free();
}


/**
 * @brief Frees the music.
 */
void music_al_free (void)
{
   /* Stop music if needed. */
   musicLock();
   if (music_state != MUSIC_STATE_IDLE) {
      music_command = MUSIC_CMD_STOP;
      music_forced  = 1;
      while (1) {
         SDL_CondWait( music_state_cond, music_state_lock );
         if (music_state == MUSIC_STATE_IDLE) {
            music_forced = 0;
            break;
         }
      }
   }
   musicUnlock();

   musicVorbisLock();

   if (music_vorbis.rw != NULL) {
      ov_clear( &music_vorbis.stream );
      music_vorbis.rw = NULL; /* somewhat officially ended */
   }

   musicVorbisUnlock();
}


/**
 * @brief Frees the music.
 */
void music_al_exit (void)
{
   /* Kill the thread. */
   music_thread_kill();

   soundLock();

   /* Free the music. */
   alDeleteBuffers( 2, music_buffer );
   alDeleteSources( 1, &music_source );

   /* Check for errors. */
   al_checkErr();

   soundUnlock();

   free(music_buf);
   music_buf = NULL;

   /* Destroy the mutex. */
   SDL_DestroyMutex( music_vorbis_lock );
   SDL_DestroyMutex( music_state_lock );
   SDL_DestroyCond( music_state_cond );
}


/**
 * @brief Tells the music thread to die.
 */
static void music_thread_kill (void)
{
   int ret;
   musicLock();

   music_command = MUSIC_CMD_KILL;
   music_forced  = 1;
   while (1) {
      ret = SDL_CondWaitTimeout( music_state_cond, music_state_lock, 3000 );

      /* Timed out, just slaughter the thread. */
      if (ret == SDL_MUTEX_TIMEDOUT) {
         WARN(_("Music thread did not exit when asked, ignoring..."));
         break;
      }

      /* Ended properly, breaking. */
      if (music_state == MUSIC_STATE_DEAD)
         break;
   }

   musicUnlock();
}


/**
 * @brief Initializes the OpenAL music subsystem.
 */
int music_al_init (void)
{
   ALfloat v[] = { 0., 0., 0. };

   /* Create threading mechanisms. */
   music_state_cond  = SDL_CreateCond();
   music_state_lock  = SDL_CreateMutex();
   music_vorbis_lock = SDL_CreateMutex();
   music_vorbis.rw   = NULL; /* indication it's not loaded */

   /* Create the buffer. */
   music_bufSize     = conf.al_bufsize * 1024;
   music_buf         = malloc( music_bufSize );

   soundLock();

   /* music_source created in sound_al_init. */

   /* Generate buffers and sources. */
   alGenBuffers( 2, music_buffer );

   /* Set up OpenAL properties. */
   alSourcef(  music_source, AL_GAIN, music_vol );
   alSourcei(  music_source, AL_SOURCE_RELATIVE, AL_TRUE );
   alSourcefv( music_source, AL_POSITION, v );
   alSourcefv( music_source, AL_VELOCITY, v );

   /* Check for errors. */
   al_checkErr();

   /* Set state to none. */
   music_state = 0;

   soundUnlock();

   /*
    * Start up thread and have it inform us when it already reaches the main loop.
    */
   musicLock();
   music_state  = MUSIC_STATE_STARTUP;
   music_player = SDL_CreateThread( music_thread,
         "music_thread",
         NULL );
   SDL_CondWait( music_state_cond, music_state_lock );
   musicUnlock();

   return 0;
}


/**
 * @brief Internal music loading routines.
 *
 *    @return 0 on success.
 */
static int music_find (void)
{
   char** files;
   size_t nfiles,i;
   int suflen, flen;
   int nmusic;

   if (music_disabled)
      return 0;

   /* get the file list */
   files = ndata_list( MUSIC_PATH, &nfiles );

   /* load the profiles */
   nmusic = 0;
   suflen = strlen(MUSIC_SUFFIX);
   for (i=0; i<nfiles; i++) {
      flen = strlen(files[i]);
      if ((flen > suflen) &&
            strncmp( &files[i][flen - suflen], MUSIC_SUFFIX, suflen)==0) {

         /* grow the selection size */
         nmusic++;
      }

      /* Clean up. */
      free(files[i]);
   }

   DEBUG( ngettext("Loaded %d Song", "Loaded %d Songs", nmusic ), nmusic );

   /* More clean up. */
   free(files);

   return 0;
}


/**
 * @brief Sets the music volume.
 *
 *    @param vol Volume to set to (between 0 and 1).
 *    @return 0 on success.
 */
int music_volume( const double vol )
{
   if (music_disabled)
      return 0;

   soundLock();

   music_vol_lin = vol;
   if (vol > 0.) /* Floor of -48 dB (0.00390625 amplitude) */
      music_vol = 1 / pow(2, (1 - vol) * 8 );
   else
      music_vol = 0.;

   /* only needed if playing */
   if (music_isPlaying()) {
      alSourcef( music_source, AL_GAIN, music_vol );

      /* Check for errors. */
      al_checkErr();
   }

   soundUnlock();

   return 0;
}


/**
 * @brief Gets the current music volume (linear).
 *
 *    @return The current music volume.
 */
double music_getVolume (void)
{
   if (music_disabled)
      return 0.;

   return music_vol_lin;
}


/**
 * @brief Gets the current music volume (logarithmic).
 *
 *    @return The current music volume.
 */
double music_getVolumeLog(void)
{
   if (music_disabled)
      return 0.;

   return music_vol;
}


/**
 * @brief Loads the music by name.
 *
 *    @param name Name of the file to load.
 */
int music_load( const char* name )
{
   SDL_RWops *rw;
   char filename[PATH_MAX];

   if (music_disabled)
      return 0;

   /* Free current music if needed. */
   music_free();

   /* Load new music. */
   music_name  = strdup(name);
   music_start = SDL_GetTicks();
   nsnprintf( filename, PATH_MAX, MUSIC_PATH"%s"MUSIC_SUFFIX, name);
   rw = ndata_rwops( filename );
   if (rw == NULL) {
      WARN(_("Music '%s' not found."), filename);
      return -1;
   }
   music_al_load( name, rw );

   return 0;
}


/**
 * @brief Internal music loading routines.
 */
int music_al_load( const char* name, SDL_RWops *rw )
{
   int rg;
   ALfloat track_gain_db, track_peak;
   vorbis_comment *vc;
   char *tag = NULL;

   musicVorbisLock();

   /* set the new name */
   strncpy( music_vorbis.name, name, PATH_MAX );
   music_vorbis.name[PATH_MAX-1] = '\0';

   /* Load new ogg. */
   music_vorbis.rw = rw;
   if (ov_open_callbacks( music_vorbis.rw, &music_vorbis.stream,
            NULL, 0, sound_al_ovcall ) < 0) {
      WARN(_("Song '%s' does not appear to be a Vorbis bitstream."), name);
      musicUnlock();
      return -1;
   }
   music_vorbis.info = ov_info( &music_vorbis.stream, -1 );

   /* Get Replaygain information. */
   vc             = ov_comment( &music_vorbis.stream, -1 );
   track_gain_db  = 0.;
   track_peak     = 1.;
   rg             = 0;
   if ((tag = vorbis_comment_query(vc, "replaygain_track_gain", 0))) {
      track_gain_db  = atof(tag);
      rg             = 1;
   }
   if ((tag = vorbis_comment_query(vc, "replaygain_track_peak", 0))) {
      track_peak     = atof(tag);
      rg             = 1;
   }
   music_vorbis.rg_scale_factor = pow(10.0, (track_gain_db + RG_PREAMP_DB)/20);
   music_vorbis.rg_max_scale = 1.0 / track_peak;
   if (!rg)
      DEBUG(_("Song '%s' has no replaygain information."), name );

   /* Set the format */
   if (music_vorbis.info->channels == 1)
      music_vorbis.format = AL_FORMAT_MONO16;
   else
      music_vorbis.format = AL_FORMAT_STEREO16;

   musicVorbisUnlock();

   return 0;
}


/**
 * @brief Plays the loaded music.
 */
void music_play (void)
{
   if (music_disabled) return;

   musicLock();

   music_command = MUSIC_CMD_FADEIN;
   while (1) {
      SDL_CondWait( music_state_cond, music_state_lock );
      if (music_isPlaying())
         break;
   }

   musicUnlock();
}


/**
 * @brief Stops the loaded music.
 */
void music_stop (void)
{
   if (music_disabled) return;

   musicLock();

   music_command = MUSIC_CMD_FADEOUT;
   while (1) {
      SDL_CondWait( music_state_cond, music_state_lock );
      if ((music_state == MUSIC_STATE_IDLE) ||
            (music_state == MUSIC_STATE_FADEOUT))
         break;
   }

   musicUnlock();
}


/**
 * @brief Pauses the music.
 */
void music_pause (void)
{
   if (music_disabled) return;

   musicLock();

   music_command = MUSIC_CMD_PAUSE;
   while (1) {
      SDL_CondWait( music_state_cond, music_state_lock );
      if ((music_state == MUSIC_STATE_IDLE) ||
            (music_state == MUSIC_STATE_PAUSED))
         break;
   }

   musicUnlock();
}


/**
 * @brief Resumes the music.
 */
void music_resume (void)
{
   if (music_disabled) return;

   musicLock();

   music_command = MUSIC_CMD_PLAY;
   while (1) {
      SDL_CondWait( music_state_cond, music_state_lock );
      if (music_isPlaying())
         break;
   }

   musicUnlock();
}


/**
 * @brief Checks to see if the music is playing.
 *
 *    @return 0 if music isn't playing, 1 if is playing.
 */
int music_isPlaying (void)
{
   int ret;

   if (music_disabled)
      return 0; /* Always not playing when music is off. */

   musicLock();

   if ((music_state == MUSIC_STATE_PLAYING) ||
         (music_state == MUSIC_STATE_LOADING) ||
         (music_state == MUSIC_STATE_RESUMING) ||
         (music_state == MUSIC_STATE_FADEIN) ||
         (music_state == MUSIC_STATE_FADEOUT) ||
         (music_state == MUSIC_STATE_PAUSED))
      ret = 1;
   else
      ret = 0;

   musicUnlock();

   return ret;
}


/**
 * @brief Gets the name of the current playing song.
 *
 *    @return Name of the current playing song.
 */
const char *music_playingName (void)
{
   if (music_disabled)
      return NULL;

   return music_name;
}


/**
 * @brief Gets the time since the music started playing.
 *
 *    @return The time since the music started playing.
 */
double music_playingTime (void)
{
   if (music_disabled)
      return 0.;

   return (double)(SDL_GetTicks() - music_start) / 1000.;
}


/**
 * @brief Sets the music to a position in seconds.
 *
 *    @param sec Position to go to in seconds.
 */
void music_setPos( double sec )
{
   int ret;

   if (music_disabled)
      return;

   musicVorbisLock();

   ret = 0;
   if (music_vorbis.rw != NULL)
      ret = ov_time_seek( &music_vorbis.stream, sec );

   musicVorbisUnlock();

   if (ret != 0)
      WARN(_("Unable to seek Vorbis file."));
}


/*
 * music Lua stuff
 */
/**
 * @brief Initialize the music Lua control system.
 *
 *    @return 0 on success.
 */
static int music_luaInit (void)
{
   char *buf;
   size_t bufsize;

   if (music_disabled)
      return 0;

   if (music_env != LUA_NOREF)
      music_luaQuit();

   music_env = nlua_newEnv(1);
   nlua_loadStandard(music_env);
   nlua_loadMusic(music_env); /* write it */

   /* load the actual Lua music code */
   buf = ndata_read( MUSIC_LUA_PATH, &bufsize );
   if (nlua_dobufenv(music_env, buf, bufsize, MUSIC_LUA_PATH) != 0) {
      ERR(_("Error loading music file: %s\n"
          "%s\n"
          "Most likely Lua file has improper syntax, please check"),
            MUSIC_LUA_PATH, lua_tostring(naevL,-1) );
      return -1;
   }
   free(buf);

   return 0;
}


/**
 * @brief Quits the music Lua control system.
 */
static void music_luaQuit (void)
{
   if (music_disabled)
      return;

   if (music_env == LUA_NOREF)
      return;

   nlua_freeEnv(music_env);
   music_env = LUA_NOREF;
}


/**
 * @brief Actually runs the music stuff, based on situation.
 *
 *    @param situation Choose a new music to play.
 *    @return 0 on success.
 */
int music_choose( const char* situation )
{
   if (music_disabled)
      return 0;

   music_timer = 0.;
   music_runLua( situation );

   return 0;
}



/**
 * @brief Actually runs the music stuff, based on situation after a delay.
 *
 *    @param situation Choose a new music to play after delay.
 *    @param delay Delay in seconds to delay the rechoose.
 *    @return 0 on success.
 */
int music_chooseDelay( const char* situation, double delay )
{
   if (music_disabled)
      return 0;

   /* Lock so it doesn't run in between an update. */
   SDL_mutexP(music_lock);
   music_timer       = delay;
   music_runchoose   = 0;
   strncpy(music_situation, situation, PATH_MAX);
   music_situation[ PATH_MAX-1 ] = '\0';
   SDL_mutexV(music_lock);

   return 0;
}


/**
 * @brief Attempts to rechoose the music.
 *
 * DO NOT CALL MIX_* FUNCTIONS FROM WITHIN THE CALLBACKS!
 */
void music_rechoose (void)
{
   if (music_disabled)
      return;

   /* Lock so it doesn't run in between an update. */
   SDL_mutexP(music_lock);
   music_timer       = 0.;
   music_runchoose   = 1;
   strncpy(music_situation, "idle", PATH_MAX);
   music_situation[ PATH_MAX-1 ] = '\0';
   SDL_mutexV(music_lock);
}


/**
 * @brief The music thread.
 *
 *    @param unused Unused.
 */
static int music_thread( void* unused )
{
   (void)unused;

   int ret;
   int active = 0; /* active buffer */
   ALint state;
   ALuint removed[2];
   ALenum value;
   music_state_t cur_state;
   ALfloat gain;
   int fadein_start = 0;
   uint32_t fade, fade_timer = 0;

   while (1) {
      /* Handle states. */
      musicLock();

      /* Handle new command. */
      switch (music_command) {
         case MUSIC_CMD_KILL:
            if (music_state != MUSIC_STATE_IDLE)
               music_state = MUSIC_STATE_STOPPING;
            else
               music_state = MUSIC_STATE_DEAD;

            /* Does not clear command. */
            break;

         case MUSIC_CMD_STOP:
            /* Notify of stopped. */
            if (music_state == MUSIC_STATE_IDLE)
               SDL_CondBroadcast( music_state_cond );
            else
               music_state = MUSIC_STATE_STOPPING;
            break;

         case MUSIC_CMD_PLAY:
            /* Set appropriate state. */
            if (music_state == MUSIC_STATE_PAUSING)
               music_state = MUSIC_STATE_RESUMING;
            else if (music_state == MUSIC_STATE_FADEIN)
               fade_timer = SDL_GetTicks() - MUSIC_FADEIN_DELAY;
            else
               music_state = MUSIC_STATE_LOADING;
            /* Disable fadein. */
            fadein_start = 0;
            /* Clear command. */
            music_command = MUSIC_CMD_NONE;
            SDL_CondBroadcast( music_state_cond );
            break;

         case MUSIC_CMD_FADEOUT:
            /* Notify of stopped. */
            if (music_state != MUSIC_STATE_IDLE) {
               music_state = MUSIC_STATE_FADEOUT;
               /* Set timer. */
               fade_timer = SDL_GetTicks();
            }
            /* Clear command. */
            music_command = MUSIC_CMD_NONE;
            SDL_CondBroadcast( music_state_cond );
            break;

         case MUSIC_CMD_FADEIN:
            if ((music_state == MUSIC_STATE_FADEIN) ||
                  (music_state == MUSIC_STATE_PLAYING))
               SDL_CondBroadcast( music_state_cond );
            else {
               music_state = MUSIC_STATE_LOADING;
               /* Set timer. */
               fade_timer = SDL_GetTicks();
               fadein_start = 1;
            }
            /* Clear command. */
            music_command = MUSIC_CMD_NONE;
            break;

         case MUSIC_CMD_PAUSE:
            if (music_state == MUSIC_STATE_PAUSED)
               SDL_CondBroadcast( music_state_cond );
            else if ((music_state == MUSIC_STATE_PLAYING) ||
                  (music_state == MUSIC_STATE_FADEIN))
               music_state = MUSIC_STATE_PAUSING;
            music_command = MUSIC_CMD_NONE;
            break;

         case MUSIC_CMD_NONE:
            break;
      }
      cur_state = music_state;
      musicUnlock();

      /* Main processing loop. */
      switch (cur_state) {
         /* Basically send a message that thread is up and running. */
         case MUSIC_STATE_STARTUP:
            musicLock();
            music_state = MUSIC_STATE_IDLE;
            SDL_CondBroadcast( music_state_cond );
            musicUnlock();
            break;

         /* We died. */
         case MUSIC_STATE_DEAD:
            musicLock();
            music_state = MUSIC_STATE_DEAD;
            SDL_CondBroadcast( music_state_cond );
            musicUnlock();
            return 0;
            break;

         /* Delays at the end. */
         case MUSIC_STATE_PAUSED:
         case MUSIC_STATE_IDLE:
            break;

         /* Resumes the paused song. */
         case MUSIC_STATE_RESUMING:
            soundLock();
            alSourcePlay( music_source );
            alSourcef( music_source, AL_GAIN, music_vol );
            /* Check for errors. */
            al_checkErr();
            soundUnlock();

            musicLock();
            music_state = MUSIC_STATE_PLAYING;
            SDL_CondBroadcast( music_state_cond );
            musicUnlock();
            break;

         /* Pause the song. */
         case MUSIC_STATE_PAUSING:
            soundLock();
            alSourcePause( music_source );
            /* Check for errors. */
            al_checkErr();
            soundUnlock();

            musicLock();
            music_state = MUSIC_STATE_PAUSED;
            SDL_CondBroadcast( music_state_cond );
            musicUnlock();
            break;

         /* Stop song setting to IDLE. */
         case MUSIC_STATE_STOPPING:
            soundLock();

            /* Stop and remove buffers. */
            alSourceStop( music_source );
            alGetSourcei( music_source, AL_BUFFERS_PROCESSED, &value );
            if (value > 0)
               alSourceUnqueueBuffers( music_source, value, removed );
            /* Clear timer. */
            fade_timer = 0;

            /* Reset volume. */
            alSourcef( music_source, AL_GAIN, music_vol );

            soundUnlock();

            musicLock();
            music_state = MUSIC_STATE_IDLE;
            SDL_CondBroadcast( music_state_cond );
            if (!music_forced)
               music_rechoose();
            musicUnlock();
            break;

         /* Load the song. */
         case MUSIC_STATE_LOADING:
            /* Load buffer and start playing. */
            active = 0; /* load first buffer */
            ret = stream_loadBuffer( music_buffer[active] );
            soundLock();
            alSourceQueueBuffers( music_source, 1, &music_buffer[active] );

            /* Special case NULL file or error. */
            if (ret < 0) {
               soundUnlock();
               /* Force state to stopped. */
               musicLock();
               music_state = MUSIC_STATE_IDLE;
               SDL_CondBroadcast( music_state_cond );
               if (!music_forced)
                  music_rechoose();
               musicUnlock();
               break;
            }
            /* Force volume level. */
            alSourcef( music_source, AL_GAIN, (fadein_start) ? 0. : music_vol );

            /* Start playing. */
            alSourcePlay( music_source );

            /* Check for errors. */
            al_checkErr();

            soundUnlock();
            /* Special case of a very short song. */
            if (ret > 1) {
               active = -1;

               musicLock();
               if (fadein_start)
                  music_state = MUSIC_STATE_FADEIN;
               else
                  music_state = MUSIC_STATE_PLAYING;
               SDL_CondBroadcast( music_state_cond );
               musicUnlock();
               break;
            }

            /* Load second buffer. */
            active = 1;
            ret = stream_loadBuffer( music_buffer[active] );
            if (ret < 0)
               active = -1;
            else {
               soundLock();
               alSourceQueueBuffers( music_source, 1, &music_buffer[active] );
               /* Check for errors. */
               al_checkErr();
               soundUnlock();
               active = 1 - active;
            }

            musicLock();
            if (fadein_start)
               music_state = MUSIC_STATE_FADEIN;
            else
               music_state = MUSIC_STATE_PLAYING;
            SDL_CondBroadcast( music_state_cond );
            musicUnlock();
            break;

         /* Fades in the music. */
         case MUSIC_STATE_FADEOUT:
         case MUSIC_STATE_FADEIN:
            /* See if must still fade. */
            fade = SDL_GetTicks() - fade_timer;
            if (cur_state == MUSIC_STATE_FADEIN) {
               if (fade < MUSIC_FADEIN_DELAY) {
                  gain = (ALfloat)fade / (ALfloat)MUSIC_FADEIN_DELAY;
                  soundLock();
                  alSourcef( music_source, AL_GAIN, gain*music_vol );
                  /* Check for errors. */
                  al_checkErr();
                  soundUnlock();
               }
               /* No need to fade anymore. */
               else {
                  /* Set volume to normal level. */
                  soundLock();
                  alSourcef( music_source, AL_GAIN, music_vol );
                  /* Check for errors. */
                  al_checkErr();
                  soundUnlock();

                  /* Change state to playing. */
                  musicLock();
                  music_state = MUSIC_STATE_PLAYING;
                  musicUnlock();
               }
            }
            else if (cur_state == MUSIC_STATE_FADEOUT) {
               if (fade < MUSIC_FADEOUT_DELAY) {
                  gain = 1. - (ALfloat)fade / (ALfloat)MUSIC_FADEOUT_DELAY;
                  soundLock();
                  alSourcef( music_source, AL_GAIN, gain*music_vol );
                  /* Check for errors. */
                  al_checkErr();
                  soundUnlock();
               }
               else {
                  /* Music should stop. */
                  musicLock();
                  music_state = MUSIC_STATE_STOPPING;
                  musicUnlock();
                  break;
               }
            }
            /* fallthrough */

         /* Play the song if needed. */
         case MUSIC_STATE_PLAYING:
            /* Special case where file has ended. */
            if (active < 0) {
               soundLock();
               alGetSourcei( music_source, AL_SOURCE_STATE, &state );

               if (state == AL_STOPPED) {
                  alGetSourcei( music_source, AL_BUFFERS_PROCESSED, &value );
                  if (value > 0)
                     alSourceUnqueueBuffers( music_source, value, removed );
                  soundUnlock();

                  musicLock();
                  music_state = MUSIC_STATE_IDLE;
                  if (!music_forced)
                     music_rechoose();
                  musicUnlock();
                  break;
               }

               soundUnlock();

               break;
            }

            soundLock();

            /* See if needs another buffer set. */
            alGetSourcei( music_source, AL_BUFFERS_PROCESSED, &state );
            if (state > 0) {
               /* refill active buffer */
               alSourceUnqueueBuffers( music_source, 1, removed );
               ret = stream_loadBuffer( music_buffer[active] );
               if (ret < 0)
                  active = -1;
               else {
                  alSourceQueueBuffers( music_source, 1, &music_buffer[active] );
                  active = 1 - active;
               }
            }

            /* Check for errors. */
            al_checkErr();

            soundUnlock();
      }

      /* Global thread delay. */
      SDL_Delay(0);

   }

   return 0;
}


#ifdef HAVE_OV_READ_FILTER
/**
 * @brief This is the filter function for the decoded Ogg Vorbis stream.
 *
 * base on:
 * vgfilter.c (c) 2007,2008 William Poetra Yoga Hadisoeseno
 * based on:
 * vgplay.c 1.0 (c) 2003 John Morton
 */
static void rg_filter( float **pcm, long channels, long samples, void *filter_param )
{
   int i, j;
   float cur_sample;
   alMusic *param       = filter_param;
   float scale_factor   = param->rg_scale_factor;
   float max_scale      = param->rg_max_scale;

   /* Apply the gain, and any limiting necessary */
   if (scale_factor > max_scale) {
      for (i = 0; i < channels; i++)
         for (j = 0; j < samples; j++) {
            cur_sample = pcm[i][j] * scale_factor;
            /*
             * This is essentially the scaled hard-limiting algorithm
             * It looks like the soft-knee to me
             * I haven't found a better limiting algorithm yet...
             */
            if (cur_sample < -0.5)
               cur_sample = tanh((cur_sample + 0.5) / (1-0.5)) * (1-0.5) - 0.5;
            else if (cur_sample > 0.5)
               cur_sample = tanh((cur_sample - 0.5) / (1-0.5)) * (1-0.5) + 0.5;
            pcm[i][j] = cur_sample;
         }
   }
   else if (scale_factor > 0.0)
      for (i = 0; i < channels; i++)
         for (j = 0; j < samples; j++)
            pcm[i][j] *= scale_factor;
}
#endif /* HAVE_OV_READ_FILTER */


/**
 * @brief Loads a buffer.
 *
 *    @param buffer Buffer to load.
 */
static int stream_loadBuffer( ALuint buffer )
{
   int ret, size, section, result;

   musicVorbisLock();

   /* Make sure music is valid. */
   if (music_vorbis.rw == NULL) {
      musicVorbisUnlock();
      return -1;
   }

   ret  = 0;
   size = 0;
   while (size < music_bufSize) { /* file up the entire data buffer */

#ifdef HAVE_OV_READ_FILTER
      result = ov_read_filter(
            &music_vorbis.stream,   /* stream */
            &music_buf[size],       /* data */
            music_bufSize - size,   /* amount to read */
            HAS_BIGENDIAN,          /* big endian? */
            2,                      /* 16 bit */
            1,                      /* signed */
            &section,               /* current bitstream */
            rg_filter,              /* filter function */
            &music_vorbis );        /* filter parameter */
#else /* HAVE_OV_READ_FILTER */
      result = ov_read(
            &music_vorbis.stream,   /* stream */
            &music_buf[size],       /* data */
            music_bufSize - size,   /* amount to read */
            HAS_BIGENDIAN,          /* big endian? */
            2,                      /* 16 bit */
            1,                      /* signed */
            &section );             /* current bitstream */
#endif /* HAVE_OV_READ_FILTER */

      /* End of file. */
      if (result == 0) {
         if (size == 0) {
            musicVorbisUnlock();
            return -2;
         }
         ret = 1;
         break;
      }
      /* Hole error. */
      else if (result == OV_HOLE) {
         musicVorbisUnlock();
         WARN(_("OGG: Vorbis hole detected in music!"));
         return 0;
      }
      /* Bad link error. */
      else if (result == OV_EBADLINK) {
         musicVorbisUnlock();
         WARN(_("OGG: Invalid stream section or corrupt link in music!"));
         return -1;
      }

      size += result;
   }

   musicVorbisUnlock();

   /* load the buffer up */
   soundLock();
   alBufferData( buffer, music_vorbis.format,
         music_buf, size, music_vorbis.info->rate );
   soundUnlock();

   return ret;
}
