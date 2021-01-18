/*
 * See Licensing and Copyright notice in naev.h
 */


#ifndef OPENGL_TEX_H
#  define OPENGL_TEX_H


/** @cond */
#include <stdint.h>
#include "SDL.h"
/** @endcond */

#include "colour.h"
#include "ncompat.h"
#include "physics.h"


/* Recommended for compatibility and such */
#if HAS_BIGENDIAN
#  define RMASK   0xff000000 /**< Red bit mask. */
#  define GMASK   0x00ff0000 /**< Green bit mask. */
#  define BMASK   0x0000ff00 /**< Blue bit mask. */
#  define AMASK   0x000000ff /**< Alpha bit mask. */
#else
#  define RMASK   0x000000ff /**< Red bit mask. */
#  define GMASK   0x0000ff00 /**< Green bit mask. */
#  define BMASK   0x00ff0000 /**< Blue bit mask. */
#  define AMASK   0xff000000 /**< Alpha bit mask. */
#endif
#define RGBAMASK  RMASK,GMASK,BMASK,AMASK


/*
 * Texture flags.
 */
#define OPENGL_TEX_MAPTRANS   (1<<0) /**< Create a transparency map. */
#define OPENGL_TEX_MIPMAPS    (1<<1) /**< Creates mipmaps. */
#define OPENGL_TEX_VFLIP      (1<<2) /**< Assume loaded from an image (where positive y means down). */

/**
 * @brief Abstraction for rendering sprite sheets.
 *
 * The basic unit all the graphic rendering works with.
 */
typedef struct glTexture_ {
   char *name; /**< name of the graphic */

   /* dimensions */
   double w; /**< Real width of the image. */
   double h; /**< Real height of the image. */

   /* sprites */
   double sx; /**< Number of sprites on the x axis. */
   double sy; /**< Number of sprites on the y axis. */
   double sw; /**< Width of a sprite. */
   double sh; /**< Height of a sprite. */
   double srw; /**< Sprite render width - equivalent to sw/w. */
   double srh; /**< Sprite render height - equivalent to sh/h. */

   /* data */
   GLuint texture; /**< the opengl texture itself */
   uint8_t* trans; /**< maps the transparency */

   /* properties */
   uint8_t flags; /**< flags used for texture properties */
} glTexture;


/*
 * Init/exit.
 */
int gl_initTextures (void);
void gl_exitTextures (void);

/*
 * Creating.
 */
glTexture* gl_loadImageData( float *data, int w, int h, int sx, int sy );
glTexture* gl_loadImagePad( const char *name, SDL_Surface* surface,
      unsigned int flags, int w, int h, int sx, int sy, int freesur );
glTexture* gl_loadImagePadTrans( const char *name, SDL_Surface* surface, SDL_RWops *rw,
      unsigned int flags, int w, int h, int sx, int sy, int freesur );
glTexture* gl_loadImage( SDL_Surface* surface, const unsigned int flags ); /* Frees the surface. */
glTexture* gl_newImage( const char* path, const unsigned int flags );
glTexture* gl_newImageRWops( const char* path, SDL_RWops *rw, const unsigned int flags ); /* Does not close the RWops. */
glTexture* gl_newSprite( const char* path, const int sx, const int sy,
      const unsigned int flags );
glTexture* gl_newSpriteRWops( const char* path, SDL_RWops *rw,
   const int sx, const int sy, const unsigned int flags );
glTexture* gl_dupTexture( glTexture *texture );

/*
 * Clean up.
 */
void gl_freeTexture( glTexture* texture );

/*
 * Info.
 */
int gl_texHasMipmaps (void);
int gl_texHasCompress (void);

/*
 * Misc.
 */
int gl_isTrans( const glTexture* t, const int x, const int y );
void gl_getSpriteFromDir( int* x, int* y, const glTexture* t, const double dir );
glTexture** gl_copyTexArray( glTexture **tex, int texn, int *n );
glTexture** gl_addTexArray( glTexture **tex, int *n, glTexture *t );


#endif /* OPENGL_TEX_H */

