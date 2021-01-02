/*
 * See Licensing and Copyright notice in naev.h
 */

/**
 * @file nlua_tex.c
 *
 * @brief Handles the Lua texture bindings.
 */

/** @cond */
#include <lauxlib.h>
#include "SDL.h"

#include "naev.h"
/** @endcond */

#include "physfsrwops.h"

#include "nlua_tex.h"

#include "log.h"
#include "ndata.h"
#include "nlua_data.h"
#include "nlua_file.h"
#include "nluadef.h"
#include "npng.h"


/* Helpers. */
static inline uint32_t get_pixel(SDL_Surface *surface, int x, int y);


/* Texture metatable methods. */
static int texL_close( lua_State *L );
static int texL_new( lua_State *L );
static int texL_readData( lua_State *L );
static int texL_dim( lua_State *L );
static int texL_sprites( lua_State *L );
static int texL_spriteFromDir( lua_State *L );
static int texL_setFilter( lua_State *L );
static int texL_setWrap( lua_State *L );
static const luaL_Reg texL_methods[] = {
   { "__gc", texL_close },
   { "new", texL_new },
   { "open", texL_new },
   { "readData", texL_readData },
   { "dim", texL_dim },
   { "sprites", texL_sprites },
   { "spriteFromDir", texL_spriteFromDir },
   { "setFilter", texL_setFilter },
   { "setWrap", texL_setWrap },
   {0,0}
}; /**< Texture metatable methods. */




/**
 * @brief Loads the texture library.
 *
 *    @param env Environment to load texture library into.
 *    @return 0 on success.
 */
int nlua_loadTex( nlua_env env )
{
   nlua_register(env, TEX_METATABLE, texL_methods, 1);
   return 0;
}


/**
 * @brief Lua bindings to interact with OpenGL textures.
 *
 * This will allow you to load textures.
 *
 * An example would be:
 * @code
 * t  = tex.open( "foo/bar.png" ) -- Loads the texture
 * w,h, sw,sh = t:dim()
 * sprites, sx,sy = t:sprites()
 * @endcode
 *
 * @luamod tex
 */
/**
 * @brief Gets texture at index.
 *
 *    @param L Lua state to get texture from.
 *    @param ind Index position to find the texture.
 *    @return Texture found at the index in the state.
 */
glTexture* lua_totex( lua_State *L, int ind )
{
   return *((glTexture**) lua_touserdata(L,ind));
}
/**
 * @brief Gets texture at index or raises error if there is no texture at index.
 *
 *    @param L Lua state to get texture from.
 *    @param ind Index position to find texture.
 *    @return Texture found at the index in the state.
 */
glTexture* luaL_checktex( lua_State *L, int ind )
{
   if (lua_istex(L,ind))
      return lua_totex(L,ind);
   luaL_typerror(L, ind, TEX_METATABLE);
   return NULL;
}
/**
 * @brief Pushes a texture on the stack.
 *
 *    @param L Lua state to push texture into.
 *    @param texture Texture to push.
 *    @return Newly pushed texture.
 */
glTexture** lua_pushtex( lua_State *L, glTexture *texture )
{
   glTexture **t;
   t = (glTexture**) lua_newuserdata(L, sizeof(glTexture*));
   *t = texture;
   luaL_getmetatable(L, TEX_METATABLE);
   lua_setmetatable(L, -2);
   return t;
}
/**
 * @brief Checks to see if ind is a texture.
 *
 *    @param L Lua state to check.
 *    @param ind Index position to check.
 *    @return 1 if ind is a texture.
 */
int lua_istex( lua_State *L, int ind )
{
   int ret;

   if (lua_getmetatable(L,ind)==0)
      return 0;
   lua_getfield(L, LUA_REGISTRYINDEX, TEX_METATABLE);

   ret = 0;
   if (lua_rawequal(L, -1, -2))  /* does it have the correct mt? */
      ret = 1;

   lua_pop(L, 2);  /* remove both metatables */
   return ret;
}


/**
 * @brief Frees the texture.
 *
 *    @luatparam Tex t Texture to free.
 * @luafunc __gc( t )
 */
static int texL_close( lua_State *L )
{
   /* Free texture. */
   gl_freeTexture( luaL_checktex( L, 1 ) );

   return 0;
}


/**
 * @brief Opens a texture.
 *
 * @note open( path, (sx=1), (sy=1) )
 * @note open( file, (sx=1), (sy=1) )
 * @note open( data, w, h, (sx=1), (sy=1) )
 *
 * @usage t = tex.open( "no_sprites.png" )
 * @usage t = tex.open( "spritesheet.png", 6, 6 )
 *
 *    @luatparam string|File|Data path Path, File, or Data to open.
 *    @luatparam[opt=1] number w Width when Data or optional number of x sprites otherwise.
 *    @luatparam[opt=1] number h Height when Data or optional number of y sprites otherwise.
 *    @luatparam[opt=1] number sx Optional number of x sprites when path is Data.
 *    @luatparam[opt=1] number sy Optional number of y sprites when path is Data.
 *    @luatreturn Tex The opened texture or nil on error.
 * @luafunc open( path, w, h, sx, sy )
 */
static int texL_new( lua_State *L )
{
   const char *path;
   glTexture *tex;
   LuaFile_t *lf;
   LuaData_t *ld;
   int sx, sy, isopen;

   NLUA_CHECKRW(L);

   /* Defaults. */
   lf = NULL;
   ld = NULL;
   path = NULL;

   /* Get path. */
   if (lua_isdata(L,1)) {
      int w, h;
      ld = luaL_checkdata(L,1);
      /* Since we don't know the size we need the width and height separately. */
      w = luaL_checkinteger(L,2);
      h = luaL_checkinteger(L,3);
      if ((w < 0 ) || (h < 0))
         NLUA_ERROR( L, _("Texture dimensions must be positive") );
      sx = luaL_optinteger(L,4,1);
      sy = luaL_optinteger(L,5,1);
      if ((sx < 0 ) || (sy < 0))
         NLUA_ERROR( L, _("Spritesheet dimensions must be positive") );
      if (ld->type != LUADATA_NUMBER)
         NLUA_ERROR( L, _("Data has invalid type for texture") );
      if (w*h*ld->elem*4 != ld->size)
         NLUA_ERROR( L, _("Texture dimensions don't match data size!") );
      tex = gl_loadImageData( (void*)ld->data, w, h, w, sx, sy );
      if (tex==NULL)
         return 0;
      lua_pushtex(L, tex);
      return 1;
   }
   else if (lua_isfile(L,1))
      lf = luaL_checkfile(L,1);
   else
      path = luaL_checkstring(L, 1);

   sx = luaL_optinteger(L,2,1);
   sy = luaL_optinteger(L,3,1);
   if ((sx < 0 ) || (sy < 0))
      NLUA_ERROR( L, _("Spritesheet dimensions must be positive") );

   /* Push new texture. */
   if (path != NULL)
      tex = gl_newSprite( path, sx, sy, 0 );
   else {
      isopen = (lf->rw != NULL);
      if (!isopen)
         lf->rw = PHYSFSRWOPS_openRead( lf->path );
      if (lf->rw==NULL)
         NLUA_ERROR(L, _("Unable to open '%s' to load texture"), lf->path);
      tex = gl_newSpriteRWops( lf->path, lf->rw, sx, sy, 0 );
      if (!isopen) {
         SDL_RWclose( lf->rw );
         lf->rw = NULL;
      }
   }

   /* Failed to load. */
   if (tex == NULL)
      return 0;
   /* Properly loaded. */
   lua_pushtex( L, tex );
   return 1;
}


static inline uint32_t get_pixel(SDL_Surface *surface, int x, int y)
{
   int bpp = surface->format->BytesPerPixel;
   /* Here p is the address to the pixel we want to retrieve */
   uint8_t *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

   switch(bpp) {
      case 1:
         return *p;
         break;

      case 2:
         return *(Uint16 *)p;
         break;

      case 3:
         if(SDL_BYTEORDER == SDL_BIG_ENDIAN)
            return p[0] << 16 | p[1] << 8 | p[2];
         else
            return p[0] | p[1] << 8 | p[2] << 16;
         break;

      case 4:
         return *(Uint32 *)p;
         break;

      default:
         return 0;       /* shouldn't happen, but avoids warnings */
   }
}


/**
 * @brief Reads image data from a file.
 *
 *    @luatparam file File|string File or filename of the file to read the data from.
 *    @luatreturn Data Data containing the image data.
 *    @luatreturn number Width of the loaded data.
 *    @luatreturn number Height of the loaded data.
 * @luafunc readData( file )
 */
static int texL_readData( lua_State *L )
{
   LuaFile_t *lf;
   LuaData_t ld;
   SDL_Surface *surface;
   SDL_RWops *rw;
   npng_t *npng;
   const char *s;
   size_t size;
   uint8_t r, g, b, a;
   uint32_t pix;
   int i, j;
   float *data;

   s = NULL;
   if (lua_isfile(L,1)) {
      lf = luaL_checkfile(L,1);
      s = lf->path;
   }
   else
      s = luaL_checkstring(L,1);
   rw = PHYSFSRWOPS_openRead( s );
   if (rw == NULL)
      NLUA_ERROR(L, _("problem opening file '%s' for reading"), s );

   /* Try to read the png. */
   npng = npng_open( rw );
   if (npng == NULL)
      NLUA_ERROR(L, _("problem opening png for reading") );
   surface = npng_readSurface( npng, 0, 0 );
   if (surface == NULL)
      NLUA_ERROR(L, _("problem reading png to surface") );
   npng_close( npng );

   /* Convert surface to LuaData_t */
   SDL_LockSurface( surface );
   size = surface->w * surface->h;
   ld.elem = sizeof(float);
   ld.size = ld.elem * size * 4;
   ld.data = calloc( ld.elem*4, size );
   ld.type = LUADATA_NUMBER;
   data = (float*)ld.data;
   for (i=0; i<surface->h; i++) {
      for (j=0; j<surface->w; j++) {
         pix = get_pixel( surface, j, i );
         SDL_GetRGBA( pix, surface->format, &r, &g, &b, &a );
         size_t pos = 4*((surface->h-i-1)*surface->w+j);
         data[ pos+0 ] = ((float)r)/255.;
         data[ pos+1 ] = ((float)g)/255.;
         data[ pos+2 ] = ((float)b)/255.;
         data[ pos+3 ] = ((float)a)/255.;
      }
   }
   SDL_UnlockSurface( surface );

   /* Return parameters. */
   lua_pushdata(L, ld);
   lua_pushinteger(L, surface->w);
   lua_pushinteger(L, surface->h);

   /* Clean up. */
   SDL_FreeSurface( surface );
   SDL_RWclose( rw );

   return 3;
}


/**
 * @brief Gets the dimensions of the texture.
 *
 * @usage w,h, sw,sh = t:dim()
 *
 *    @luatparam Tex t Texture to get dimensions of.
 *    @luatreturn number The width the total image.
 *    @luatreturn number The height the total image.
 *    @luatreturn number The width the sprites.
 *    @luatreturn number The height the sprites.
 * @luafunc dim( t )
 */
static int texL_dim( lua_State *L )
{
   glTexture *tex;

   /* Get texture. */
   tex = luaL_checktex( L, 1 );

   /* Get all 4 values. */
   lua_pushnumber( L, tex->w  );
   lua_pushnumber( L, tex->h  );
   lua_pushnumber( L, tex->sw );
   lua_pushnumber( L, tex->sh );
   return 4;
}


/**
 * @brief Gets the number of sprites in the texture.
 *
 * @usage sprites, sx,sy = t:sprites()
 *
 *    @luatparam Tex t Texture to get sprites of.
 *    @luatreturn number The total number of sprites.
 *    @luatreturn number The number of X sprites.
 *    @luatreturn number The number of Y sprites.
 * @luafunc sprites( t )
 */
static int texL_sprites( lua_State *L )
{
   glTexture *tex;

   /* Get texture. */
   tex = luaL_checktex( L, 1 );

   /* Get sprites. */
   lua_pushnumber( L, tex->sx * tex->sy );
   lua_pushnumber( L, tex->sx );
   lua_pushnumber( L, tex->sy );
   return 3;
}


/**
 * @brief Gets the sprite that corresponds to a direction.
 *
 * @usage sx, sy = t:spriteFromdir( math.pi )
 *
 *    @luatparam Tex t Texture to get sprite of.
 *    @luatparam number a Direction to have sprite facing (in degrees).
 *    @luatparam[opt=false] boolean b Whether radians should be used instead of degrees.
 *    @luatreturn number The x position of the sprite.
 *    @luatreturn number The y position of the sprite.
 * @luafunc spriteFromDir( t, a, b )
 */
static int texL_spriteFromDir( lua_State *L )
{
   double a;
   glTexture *tex;
   int sx, sy;

   NLUA_CHECKRW(L);

   /* Params. */
   tex = luaL_checktex( L, 1 );

   /* Use radians if requested, otherwise convert to degrees. */
   if (lua_gettop(L) > 2 && (lua_toboolean(L, 3)))
      a = luaL_checknumber( L, 2 );
   else
      a = luaL_checknumber( L, 2 ) / 180. * M_PI;

   /* Calculate with parameter validity.. */
   gl_getSpriteFromDir( &sx, &sy, tex, a );

   /* Return. */
   lua_pushinteger( L, sx+1 );
   lua_pushinteger( L, sy+1 );
   return 2;
}


/**
 * @brief Sets the texture minification and magnification filters.
 *
 *    @luatparam Tex tex Texture to set filter.
 *    @luatparam string min Minification filter ("nearest" or "linear")
 *    @luatparam[opt] string mag Magnification filter ("nearest" or "linear"). Defaults to min.
 * @luafunc setFilter( tex, min, mag )
 */
static int texL_setFilter( lua_State *L )
{
   glTexture *tex = luaL_checktex(L,1);
   const char *smin = luaL_checkstring(L,2);
   const char *smag = luaL_optstring(L,3,smin);
   GLint min, mag;

   min = gl_stringToFilter( smin );
   mag = gl_stringToFilter( smag );

   if (min==0 || mag==0)
      NLUA_INVALID_PARAMETER(L);

   glBindTexture( GL_TEXTURE_2D, tex->texture );
   glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag );
   glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min );
   gl_checkErr();

   return 0;
}


/**
 * @brief Sets the texture wrapping.
 *
 *    @luatparam Tex tex Texture to set filter.
 *    @luatparam string horiz Horizontal wrapping ("clamp", "repeat", or "mirroredrepeat" )
 *    @luatparam[opt] string vert Vertical wrapping ("clamp", "repeat", or "mirroredrepeat" )
 *    @luatparam[opt] string depth Depth wrapping ("clamp", "repeat", or "mirroredrepeat" )
 * @luafunc setWrap( tex, horiz, vert, depth )
 */
static int texL_setWrap( lua_State *L )
{
   glTexture *tex = luaL_checktex(L,1);
   const char *shoriz = luaL_checkstring(L,2);
   const char *svert = luaL_optstring(L,3,shoriz);
   const char *sdepth = luaL_optstring(L,4,shoriz);
   GLint horiz, vert, depth;

   horiz = gl_stringToClamp( shoriz );
   vert = gl_stringToClamp( svert );
   depth = gl_stringToClamp( sdepth );

   if (horiz==0 || vert==0 || depth==0)
      NLUA_INVALID_PARAMETER(L);

   glBindTexture( GL_TEXTURE_2D, tex->texture );
   glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, horiz );
   glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, vert );
   glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, depth );
   gl_checkErr();

   return 0;
}

