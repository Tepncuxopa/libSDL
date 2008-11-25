/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2006 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
    
    SDL1.3 implementation by couriersud@arcor.de
    
*/
#include "SDL_config.h"

#include "SDL_DirectFB_video.h"
#include "SDL_DirectFB_render.h"
#include "../SDL_rect_c.h"
#include "../SDL_yuv_sw_c.h"

/* the following is not yet tested ... */
#define USE_DISPLAY_PALETTE			(0)

/* GDI renderer implementation */

static SDL_Renderer *DirectFB_CreateRenderer(SDL_Window * window,
                                             Uint32 flags);
static int DirectFB_DisplayModeChanged(SDL_Renderer * renderer);
static int DirectFB_ActivateRenderer(SDL_Renderer * renderer);
static int DirectFB_CreateTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture);
static int DirectFB_QueryTexturePixels(SDL_Renderer * renderer,
                                       SDL_Texture * texture, void **pixels,
                                       int *pitch);
static int DirectFB_SetTexturePalette(SDL_Renderer * renderer,
                                      SDL_Texture * texture,
                                      const SDL_Color * colors,
                                      int firstcolor, int ncolors);
static int DirectFB_GetTexturePalette(SDL_Renderer * renderer,
                                      SDL_Texture * texture,
                                      SDL_Color * colors, int firstcolor,
                                      int ncolors);
static int DirectFB_SetTextureAlphaMod(SDL_Renderer * renderer,
                                       SDL_Texture * texture);
static int DirectFB_SetTextureColorMod(SDL_Renderer * renderer,
                                       SDL_Texture * texture);
static int DirectFB_SetTextureBlendMode(SDL_Renderer * renderer,
                                        SDL_Texture * texture);
static int DirectFB_SetTextureScaleMode(SDL_Renderer * renderer,
                                        SDL_Texture * texture);
static int DirectFB_UpdateTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture,
                                  const SDL_Rect * rect, const void *pixels,
                                  int pitch);
static int DirectFB_LockTexture(SDL_Renderer * renderer,
                                SDL_Texture * texture, const SDL_Rect * rect,
                                int markDirty, void **pixels, int *pitch);
static void DirectFB_UnlockTexture(SDL_Renderer * renderer,
                                   SDL_Texture * texture);
static void DirectFB_DirtyTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture, int numrects,
                                  const SDL_Rect * rects);
static int DirectFB_RenderFill(SDL_Renderer * renderer, Uint8 r, Uint8 g,
                               Uint8 b, Uint8 a, const SDL_Rect * rect);
static int DirectFB_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                               const SDL_Rect * srcrect,
                               const SDL_Rect * dstrect);
static void DirectFB_RenderPresent(SDL_Renderer * renderer);
static void DirectFB_DestroyTexture(SDL_Renderer * renderer,
                                    SDL_Texture * texture);
static void DirectFB_DestroyRenderer(SDL_Renderer * renderer);

SDL_RenderDriver DirectFB_RenderDriver = {
    DirectFB_CreateRenderer,
    {
     "directfb",
     (SDL_RENDERER_SINGLEBUFFER | SDL_RENDERER_PRESENTCOPY |
      SDL_RENDERER_PRESENTFLIP2 | SDL_RENDERER_PRESENTFLIP3 |
      SDL_RENDERER_PRESENTDISCARD | SDL_RENDERER_ACCELERATED),
     (SDL_TEXTUREMODULATE_NONE | SDL_TEXTUREMODULATE_COLOR |
      SDL_TEXTUREMODULATE_ALPHA),
     (SDL_TEXTUREBLENDMODE_NONE | SDL_TEXTUREBLENDMODE_MASK |
      SDL_TEXTUREBLENDMODE_BLEND | SDL_TEXTUREBLENDMODE_ADD |
      SDL_TEXTUREBLENDMODE_MOD),
     (SDL_TEXTURESCALEMODE_NONE | SDL_TEXTURESCALEMODE_FAST |
      SDL_TEXTURESCALEMODE_SLOW | SDL_TEXTURESCALEMODE_BEST),
     14,
     {
      SDL_PIXELFORMAT_INDEX4LSB,
      SDL_PIXELFORMAT_INDEX8,
      SDL_PIXELFORMAT_RGB332,
      SDL_PIXELFORMAT_RGB555,
      SDL_PIXELFORMAT_RGB565,
      SDL_PIXELFORMAT_RGB888,
      SDL_PIXELFORMAT_ARGB8888,
      SDL_PIXELFORMAT_ARGB4444,
      SDL_PIXELFORMAT_ARGB1555,
      SDL_PIXELFORMAT_RGB24,
      SDL_PIXELFORMAT_YV12,
      SDL_PIXELFORMAT_IYUV,
      SDL_PIXELFORMAT_YUY2,
      SDL_PIXELFORMAT_UYVY},
     0,
     0}
};

typedef struct
{
    IDirectFBSurface *surface;
    DFBSurfaceFlipFlags flipflags;
    int isyuvdirect;
    int size_changed;
} DirectFB_RenderData;

typedef struct
{
    IDirectFBSurface *surface;
    Uint32 format;
    void *pixels;
    int pitch;
    IDirectFBPalette *palette;
    SDL_VideoDisplay *display;
    SDL_DirtyRectList dirty;
#if (DIRECTFB_MAJOR_VERSION == 1) && (DIRECTFB_MINOR_VERSION >= 2)
    DFBSurfaceRenderOptions render_options;
#endif
} DirectFB_TextureData;

static __inline__ void
SDLtoDFBRect(const SDL_Rect * sr, DFBRectangle * dr)
{
    dr->x = sr->x;
    dr->y = sr->y;
    dr->h = sr->h;
    dr->w = sr->w;
}

void
DirectFB_AddRenderDriver(_THIS)
{
    int i;
    for (i = 0; i < _this->num_displays; i++)
        SDL_AddRenderDriver(i, &DirectFB_RenderDriver);
}

static int
DisplayPaletteChanged(void *userdata, SDL_Palette * palette)
{
#if USE_DISPLAY_PALETTE
    DirectFB_RenderData *data = (DirectFB_RenderData *) userdata;
    IDirectFBPalette *surfpal;

    int ret;
    int i;
    int ncolors;
    DFBColor entries[256];

    SDL_DFB_CHECKERR(data->surface->GetPalette(data->surface, &surfpal));

    /* FIXME: number of colors */
    ncolors = (palette->ncolors < 256 ? palette->ncolors : 256);

    for (i = 0; i < ncolors; ++i) {
        entries[i].r = palette->colors[i].r;
        entries[i].g = palette->colors[i].g;
        entries[i].b = palette->colors[i].b;
        entries[i].a = palette->colors[i].unused;
    }
    SDL_DFB_CHECKERR(surfpal->SetEntries(surfpal, entries, ncolors, 0));
    return 0;
  error:
#endif
    return -1;
}


SDL_Renderer *
DirectFB_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    SDL_DFB_WINDOWDATA(window);
    SDL_VideoDisplay *display = SDL_GetDisplayFromWindow(window);
    SDL_Renderer *renderer = NULL;
    DirectFB_RenderData *data = NULL;
    DFBResult ret;
    DFBSurfaceCapabilities scaps;
    char *p;

    SDL_DFB_CALLOC(renderer, 1, sizeof(*renderer));
    SDL_DFB_CALLOC(data, 1, sizeof(*data));

    renderer->DisplayModeChanged = DirectFB_DisplayModeChanged;
    renderer->ActivateRenderer = DirectFB_ActivateRenderer;
    renderer->CreateTexture = DirectFB_CreateTexture;
    renderer->QueryTexturePixels = DirectFB_QueryTexturePixels;
    renderer->SetTexturePalette = DirectFB_SetTexturePalette;
    renderer->GetTexturePalette = DirectFB_GetTexturePalette;
    renderer->SetTextureAlphaMod = DirectFB_SetTextureAlphaMod;
    renderer->SetTextureColorMod = DirectFB_SetTextureColorMod;
    renderer->SetTextureBlendMode = DirectFB_SetTextureBlendMode;
    renderer->SetTextureScaleMode = DirectFB_SetTextureScaleMode;
    renderer->UpdateTexture = DirectFB_UpdateTexture;
    renderer->LockTexture = DirectFB_LockTexture;
    renderer->UnlockTexture = DirectFB_UnlockTexture;
    renderer->DirtyTexture = DirectFB_DirtyTexture;
    renderer->RenderFill = DirectFB_RenderFill;
    renderer->RenderCopy = DirectFB_RenderCopy;
    renderer->RenderPresent = DirectFB_RenderPresent;
    renderer->DestroyTexture = DirectFB_DestroyTexture;
    renderer->DestroyRenderer = DirectFB_DestroyRenderer;
    renderer->info = DirectFB_RenderDriver.info;
    renderer->window = window->id;      /* SDL window id */
    renderer->driverdata = data;

    renderer->info.flags =
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTDISCARD;

    data->surface = windata->surface;
    data->surface->AddRef(data->surface);

    data->flipflags = DSFLIP_PIPELINE | DSFLIP_BLIT;

    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        data->flipflags = DSFLIP_ONSYNC;
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    SDL_DFB_CHECKERR(data->surface->GetCapabilities(data->surface, &scaps));
    if (scaps & DSCAPS_DOUBLE)
        renderer->info.flags |= SDL_RENDERER_PRESENTFLIP2;
    else if (scaps & DSCAPS_TRIPLE)
        renderer->info.flags |= SDL_RENDERER_PRESENTFLIP3;
    else
        renderer->info.flags |= SDL_RENDERER_SINGLEBUFFER;

    data->isyuvdirect = 0;      /* default is off! */
    p = getenv(DFBENV_USE_YUV_DIRECT);
    if (p)
        data->isyuvdirect = atoi(p);

    /* Set up a palette watch on the display palette */
    if (display->palette) {
        SDL_AddPaletteWatch(display->palette, DisplayPaletteChanged, data);
    }

    return renderer;

  error:
    SDL_DFB_FREE(renderer);
    SDL_DFB_FREE(data);
    return NULL;
}

static DFBSurfacePixelFormat
SDLToDFBPixelFormat(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_INDEX4LSB:
        return DSPF_ALUT44;
    case SDL_PIXELFORMAT_INDEX8:
        return DSPF_LUT8;
    case SDL_PIXELFORMAT_RGB332:
        return DSPF_RGB332;
    case SDL_PIXELFORMAT_RGB555:
        return DSPF_ARGB1555;
    case SDL_PIXELFORMAT_ARGB4444:
        return DSPF_ARGB4444;
    case SDL_PIXELFORMAT_ARGB1555:
        return DSPF_ARGB1555;
    case SDL_PIXELFORMAT_RGB565:
        return DSPF_RGB16;
    case SDL_PIXELFORMAT_RGB24:
        return DSPF_RGB24;
    case SDL_PIXELFORMAT_RGB888:
        return DSPF_RGB32;
    case SDL_PIXELFORMAT_ARGB8888:
        return DSPF_ARGB;
    case SDL_PIXELFORMAT_YV12:
        return DSPF_YV12;       /* Planar mode: Y + V + U  (3 planes) */
    case SDL_PIXELFORMAT_IYUV:
        return DSPF_I420;       /* Planar mode: Y + U + V  (3 planes) */
    case SDL_PIXELFORMAT_YUY2:
        return DSPF_YUY2;       /* Packed mode: Y0+U0+Y1+V0 (1 plane) */
    case SDL_PIXELFORMAT_UYVY:
        return DSPF_UYVY;       /* Packed mode: U0+Y0+V0+Y1 (1 plane) */
    case SDL_PIXELFORMAT_YVYU:
        return DSPF_UNKNOWN;    /* Packed mode: Y0+V0+Y1+U0 (1 plane) */
    case SDL_PIXELFORMAT_INDEX1LSB:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_INDEX1MSB:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_INDEX4MSB:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_RGB444:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_BGR24:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_BGR888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_RGBA8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_ABGR8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_BGRA8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_ARGB2101010:
        return DSPF_UNKNOWN;
    default:
        return DSPF_UNKNOWN;
    }
}

static int
DirectFB_ActivateRenderer(SDL_Renderer * renderer)
{
    SDL_DFB_RENDERERDATA(renderer);
    SDL_Window *window = SDL_GetWindowFromID(renderer->window);
    SDL_DFB_WINDOWDATA(window);

    if (renddata->size_changed) {
        int cw, ch;
        int ret;

        SDL_DFB_CHECKERR(windata->surface->
                         GetSize(windata->surface, &cw, &ch));
        if (cw != window->w || ch != window->h)
            SDL_DFB_CHECKERR(windata->window->
                             ResizeSurface(windata->window, window->w,
                                           window->h));
    }
    return 0;
  error:
    return -1;
}

static int
DirectFB_DisplayModeChanged(SDL_Renderer * renderer)
{
    SDL_DFB_RENDERERDATA(renderer);

    renddata->size_changed = SDL_TRUE;
    return 0;
}

static int
DirectFB_AcquireVidLayer(SDL_Renderer * renderer, SDL_Texture * texture)
{
    SDL_DFB_RENDERERDATA(renderer);
    SDL_Window *window = SDL_GetWindowFromID(renderer->window);
    SDL_VideoDisplay *display = SDL_GetDisplayFromWindow(window);
    SDL_DFB_DEVICEDATA(display->device);
    DFB_DisplayData *dispdata = (DFB_DisplayData *) display->driverdata;
    DirectFB_TextureData *data = texture->driverdata;
    DFBDisplayLayerConfig layconf;
    int ret;

    if (renddata->isyuvdirect && (dispdata->vidID >= 0)
        && (!dispdata->vidIDinuse)
        && SDL_ISPIXELFORMAT_FOURCC(data->format)) {
        layconf.flags =
            DLCONF_WIDTH | DLCONF_HEIGHT | DLCONF_PIXELFORMAT |
            DLCONF_SURFACE_CAPS;
        layconf.width = texture->w;
        layconf.height = texture->h;
        layconf.pixelformat = SDLToDFBPixelFormat(data->format);
        layconf.surface_caps = DSCAPS_VIDEOONLY | DSCAPS_DOUBLE;

        SDL_DFB_CHECKERR(devdata->dfb->
                         GetDisplayLayer(devdata->dfb, dispdata->vidID,
                                         &dispdata->vidlayer));
        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         SetCooperativeLevel(dispdata->vidlayer,
                                             DLSCL_EXCLUSIVE));

        if (devdata->use_yuv_underlays) {
            ret = dispdata->vidlayer->SetLevel(dispdata->vidlayer, -1);
            if (ret != DFB_OK)
                SDL_DFB_DEBUG("Underlay Setlevel not supported\n");
        }
        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         SetConfiguration(dispdata->vidlayer, &layconf));
        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         GetSurface(dispdata->vidlayer, &data->surface));
        dispdata->vidIDinuse = 1;
        data->display = display;
        return 0;
    }
    return 1;
  error:
    if (dispdata->vidlayer) {
        SDL_DFB_RELEASE(data->surface);
        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         SetCooperativeLevel(dispdata->vidlayer,
                                             DLSCL_ADMINISTRATIVE));
        SDL_DFB_RELEASE(dispdata->vidlayer);
    }
    return 1;
}

static int
DirectFB_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    SDL_Window *window = SDL_GetWindowFromID(renderer->window);
    SDL_VideoDisplay *display = SDL_GetDisplayFromWindow(window);
    SDL_DFB_DEVICEDATA(display->device);
    DirectFB_TextureData *data;
    DFBResult ret;
    DFBSurfaceDescription dsc;

    SDL_DFB_CALLOC(data, 1, sizeof(*data));
    texture->driverdata = data;

    data->format = texture->format;
    data->pitch = (texture->w * SDL_BYTESPERPIXEL(data->format));

    if (DirectFB_AcquireVidLayer(renderer, texture) != 0) {
        /* fill surface description */
        dsc.flags =
            DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_CAPS;
        dsc.width = texture->w;
        dsc.height = texture->h;
        /* <1.2 Never use DSCAPS_VIDEOONLY here. It kills performance
         * No DSCAPS_SYSTEMONLY either - let dfb decide
         * 1.2: DSCAPS_SYSTEMONLY boosts performance by factor ~8
         * Depends on other settings as well. Let dfb decide.
         */
        dsc.caps = DSCAPS_PREMULTIPLIED;
#if 0
        if (texture->access == SDL_TEXTUREACCESS_STREAMING)
            dsc.caps |= DSCAPS_SYSTEMONLY;
        else
            dsc.caps |= DSCAPS_VIDEOONLY;
#endif

        /* find the right pixelformat */

        dsc.pixelformat = SDLToDFBPixelFormat(data->format);
        if (dsc.pixelformat == DSPF_UNKNOWN) {
            SDL_SetError("Unknown pixel format %d\n", data->format);
            goto error;
        }

        data->pixels = NULL;

        /* Create the surface */
        SDL_DFB_CHECKERR(devdata->dfb->
                         CreateSurface(devdata->dfb, &dsc, &data->surface));
        if (SDL_ISPIXELFORMAT_INDEXED(data->format)
            && !SDL_ISPIXELFORMAT_FOURCC(data->format)) {
            SDL_DFB_CHECKERR(data->surface->
                             GetPalette(data->surface, &data->palette));
        }

    }
#if (DIRECTFB_MAJOR_VERSION == 1) && (DIRECTFB_MINOR_VERSION >= 2)
    data->render_options = DSRO_NONE;
#endif

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        data->pitch = texture->w * SDL_BYTESPERPIXEL(texture->format);
        SDL_DFB_CALLOC(data->pixels, 1, texture->h * data->pitch);
    }

    return 0;

  error:
    SDL_DFB_RELEASE(data->palette);
    SDL_DFB_RELEASE(data->surface);
    SDL_DFB_FREE(texture->driverdata);
    return -1;
}

static int
DirectFB_QueryTexturePixels(SDL_Renderer * renderer, SDL_Texture * texture,
                            void **pixels, int *pitch)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;

    if (texturedata->display) {
        return -1;
    } else {
        *pixels = texturedata->pixels;
        *pitch = texturedata->pitch;
    }
    return 0;
}

static int
DirectFB_SetTexturePalette(SDL_Renderer * renderer, SDL_Texture * texture,
                           const SDL_Color * colors, int firstcolor,
                           int ncolors)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    DFBResult ret;

    if (SDL_ISPIXELFORMAT_INDEXED(data->format)
        && !SDL_ISPIXELFORMAT_FOURCC(data->format)) {
        DFBColor entries[256];
        int i;

        for (i = 0; i < ncolors; ++i) {
            entries[i].r = colors[i].r;
            entries[i].g = colors[i].g;
            entries[i].b = colors[i].b;
            entries[i].a = 0xFF;
        }
        SDL_DFB_CHECKERR(data->palette->
                         SetEntries(data->palette, entries, ncolors,
                                    firstcolor));
        return 0;
    } else {
        SDL_SetError("YUV textures don't have a palette");
        return -1;
    }
  error:
    return -1;
}

static int
DirectFB_GetTexturePalette(SDL_Renderer * renderer, SDL_Texture * texture,
                           SDL_Color * colors, int firstcolor, int ncolors)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    DFBResult ret;

    if (SDL_ISPIXELFORMAT_INDEXED(data->format)
        && !SDL_ISPIXELFORMAT_FOURCC(data->format)) {
        DFBColor entries[256];
        int i;

        SDL_DFB_CHECKERR(data->palette->
                         GetEntries(data->palette, entries, ncolors,
                                    firstcolor));

        for (i = 0; i < ncolors; ++i) {
            colors[i].r = entries[i].r;
            colors[i].g = entries[i].g;
            colors[i].b = entries[i].b;
            colors->unused = SDL_ALPHA_OPAQUE;
        }
        return 0;
    } else {
        SDL_SetError("YUV textures don't have a palette");
        return -1;
    }
  error:
    return -1;
}

static int
DirectFB_SetTextureAlphaMod(SDL_Renderer * renderer, SDL_Texture * texture)
{
    return 0;
}

static int
DirectFB_SetTextureColorMod(SDL_Renderer * renderer, SDL_Texture * texture)
{
    return 0;
}

static int
DirectFB_SetTextureBlendMode(SDL_Renderer * renderer, SDL_Texture * texture)
{
    switch (texture->blendMode) {
    case SDL_TEXTUREBLENDMODE_NONE:
    case SDL_TEXTUREBLENDMODE_MASK:
    case SDL_TEXTUREBLENDMODE_BLEND:
    case SDL_TEXTUREBLENDMODE_ADD:
    case SDL_TEXTUREBLENDMODE_MOD:
        return 0;
    default:
        SDL_Unsupported();
        texture->blendMode = SDL_TEXTUREBLENDMODE_NONE;
        return -1;
    }
}

static int
DirectFB_SetTextureScaleMode(SDL_Renderer * renderer, SDL_Texture * texture)
{
#if (DIRECTFB_MAJOR_VERSION == 1) && (DIRECTFB_MINOR_VERSION >= 2)

    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;

    switch (texture->scaleMode) {
    case SDL_TEXTURESCALEMODE_NONE:
    case SDL_TEXTURESCALEMODE_FAST:
        data->render_options = DSRO_NONE;
        break;
    case SDL_TEXTURESCALEMODE_SLOW:
        data->render_options = DSRO_SMOOTH_UPSCALE | DSRO_SMOOTH_DOWNSCALE;
        break;
    case SDL_TEXTURESCALEMODE_BEST:
        data->render_options =
            DSRO_SMOOTH_UPSCALE | DSRO_SMOOTH_DOWNSCALE | DSRO_ANTIALIAS;
        break;
    default:
        SDL_Unsupported();
        data->render_options = DSRO_NONE;
        texture->scaleMode = SDL_TEXTURESCALEMODE_NONE;
        return -1;
    }
#endif
    return 0;
}

static int
DirectFB_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                       const SDL_Rect * rect, const void *pixels, int pitch)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    DFBResult ret;
    Uint8 *dpixels;
    int dpitch;
    Uint8 *src, *dst;
    int row;
    size_t length;

    SDL_DFB_CHECKERR(data->surface->Lock(data->surface,
                                         DSLF_WRITE | DSLF_READ,
                                         ((void **) &dpixels), &dpitch));
    src = (Uint8 *) pixels;
    dst =
        (Uint8 *) dpixels + rect->y * dpitch +
        rect->x * SDL_BYTESPERPIXEL(texture->format);
    length = rect->w * SDL_BYTESPERPIXEL(texture->format);
    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += dpitch;
    }
    SDL_DFB_CHECKERR(data->surface->Unlock(data->surface));
    return 0;
  error:
    return 1;

}

static int
DirectFB_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                     const SDL_Rect * rect, int markDirty, void **pixels,
                     int *pitch)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;
    DFBResult ret;

    if (markDirty) {
        SDL_AddDirtyRect(&texturedata->dirty, rect);
    }

    if (texturedata->display) {
        void *fdata;
        int fpitch;

        SDL_DFB_CHECKERR(texturedata->surface->Lock(texturedata->surface,
                                                    DSLF_WRITE | DSLF_READ,
                                                    &fdata, &fpitch));
        *pitch = fpitch;
        *pixels = fdata;
    } else {
        *pixels =
            (void *) ((Uint8 *) texturedata->pixels +
                      rect->y * texturedata->pitch +
                      rect->x * SDL_BYTESPERPIXEL(texture->format));
        *pitch = texturedata->pitch;
    }
    return 0;

  error:
    return -1;
}

static void
DirectFB_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;

    if (texturedata->display) {
        texturedata->surface->Unlock(texturedata->surface);
        texturedata->pixels = NULL;
    }
}

static void
DirectFB_DirtyTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                      int numrects, const SDL_Rect * rects)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    int i;

    for (i = 0; i < numrects; ++i) {
        SDL_AddDirtyRect(&data->dirty, &rects[i]);
    }
}

static int
DirectFB_RenderFill(SDL_Renderer * renderer, Uint8 r, Uint8 g, Uint8 b,
                    Uint8 a, const SDL_Rect * rect)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    DFBResult ret;

    SDL_DFB_CHECKERR(data->surface->SetColor(data->surface, r, g, b, a));
    SDL_DFB_CHECKERR(data->surface->
                     FillRectangle(data->surface, rect->x, rect->y, rect->w,
                                   rect->h));

    return 0;
  error:
    return -1;
}

static int
DirectFB_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                    const SDL_Rect * srcrect, const SDL_Rect * dstrect)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;
    DFBResult ret;

    if (texturedata->display) {
        int px, py;
        SDL_Window *window = SDL_GetWindowFromID(renderer->window);
        SDL_DFB_WINDOWDATA(window);
        SDL_VideoDisplay *display = texturedata->display;
        DFB_DisplayData *dispdata = (DFB_DisplayData *) display->driverdata;

        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         SetSourceRectangle(dispdata->vidlayer, srcrect->x,
                                            srcrect->y, srcrect->w,
                                            srcrect->h));
        windata->window->GetPosition(windata->window, &px, &py);
        SDL_DFB_CHECKERR(dispdata->vidlayer->
                         SetScreenRectangle(dispdata->vidlayer,
                                            px + dstrect->x, py + dstrect->y,
                                            dstrect->w, dstrect->h));
    } else {
        DFBRectangle sr, dr;
        DFBSurfaceBlittingFlags flags = 0;

        if (texturedata->dirty.list) {
            SDL_DirtyRect *dirty;
            void *pixels;
            int bpp = SDL_BYTESPERPIXEL(texture->format);
            int pitch = texturedata->pitch;

            for (dirty = texturedata->dirty.list; dirty; dirty = dirty->next) {
                SDL_Rect *rect = &dirty->rect;
                pixels =
                    (void *) ((Uint8 *) texturedata->pixels +
                              rect->y * pitch + rect->x * bpp);
                DirectFB_UpdateTexture(renderer, texture, rect,
                                       texturedata->pixels,
                                       texturedata->pitch);
            }
            SDL_ClearDirtyRects(&texturedata->dirty);
        }
#if (DIRECTFB_MAJOR_VERSION == 1) && (DIRECTFB_MINOR_VERSION >= 2)
        SDL_DFB_CHECKERR(data->surface->SetRenderOptions(data->surface,
                                                         texturedata->
                                                         render_options));
#endif

        SDLtoDFBRect(srcrect, &sr);
        SDLtoDFBRect(dstrect, &dr);

        if (texture->
            modMode & (SDL_TEXTUREMODULATE_COLOR | SDL_TEXTUREMODULATE_ALPHA))
        {
            Uint8 alpha = 0xFF;
            if (texture->modMode & SDL_TEXTUREMODULATE_ALPHA) {
                alpha = texture->a;
                flags |= DSBLIT_SRC_PREMULTCOLOR;
                SDL_DFB_CHECKERR(data->surface->SetColor(data->surface, 0xFF,
                                                         0xFF, 0xFF, alpha));
            }
            if (texture->modMode & SDL_TEXTUREMODULATE_COLOR) {
                SDL_DFB_CHECKERR(data->surface->
                                 SetColor(data->surface, texture->r,
                                          texture->g, texture->b, alpha));
                /* Only works together .... */
                flags |= DSBLIT_COLORIZE | DSBLIT_SRC_PREMULTCOLOR;
            }
        }

        switch (texture->blendMode) {
        case SDL_TEXTUREBLENDMODE_NONE:
                                       /**< No blending */
            flags |= DSBLIT_NOFX;
            data->surface->SetSrcBlendFunction(data->surface, DSBF_ONE);
            data->surface->SetDstBlendFunction(data->surface, DSBF_ZERO);
            break;
        case SDL_TEXTUREBLENDMODE_MASK:
            flags |= DSBLIT_BLEND_ALPHACHANNEL;
            data->surface->SetSrcBlendFunction(data->surface, DSBF_SRCALPHA);
            data->surface->SetDstBlendFunction(data->surface,
                                               DSBF_INVSRCALPHA);
            break;
        case SDL_TEXTUREBLENDMODE_BLEND:
            flags |= DSBLIT_BLEND_ALPHACHANNEL;
            data->surface->SetSrcBlendFunction(data->surface, DSBF_SRCALPHA);
            data->surface->SetDstBlendFunction(data->surface,
                                               DSBF_INVSRCALPHA);
            break;
        case SDL_TEXTUREBLENDMODE_ADD:
            flags |= DSBLIT_BLEND_ALPHACHANNEL;
            data->surface->SetSrcBlendFunction(data->surface, DSBF_SRCALPHA);
            data->surface->SetDstBlendFunction(data->surface, DSBF_ONE);
            break;
        case SDL_TEXTUREBLENDMODE_MOD:
            flags |= DSBLIT_BLEND_ALPHACHANNEL;
            data->surface->SetSrcBlendFunction(data->surface, DSBF_DESTCOLOR);
            data->surface->SetDstBlendFunction(data->surface, DSBF_ZERO);
            break;
        }

        SDL_DFB_CHECKERR(data->surface->
                         SetBlittingFlags(data->surface, flags));

        if (srcrect->w == dstrect->w && srcrect->h == dstrect->h) {
            SDL_DFB_CHECKERR(data->surface->
                             Blit(data->surface, texturedata->surface,
                                  &sr, dr.x, dr.y));
        } else {
            SDL_DFB_CHECKERR(data->surface->
                             StretchBlit(data->surface, texturedata->surface,
                                         &sr, &dr));
        }
    }
    return 0;
  error:
    return -1;
}

static void
DirectFB_RenderPresent(SDL_Renderer * renderer)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_Window *window = SDL_GetWindowFromID(renderer->window);

    DFBRectangle sr;
    DFBResult ret;

    sr.x = 0;
    sr.y = 0;
    sr.w = window->w;
    sr.h = window->h;

    /* Send the data to the display */
    SDL_DFB_CHECKERR(data->surface->
                     Flip(data->surface, NULL, 0 * data->flipflags));

    return;
  error:
    return;
}

static void
DirectFB_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;

    if (!data) {
        return;
    }
    SDL_DFB_RELEASE(data->palette);
    SDL_DFB_RELEASE(data->surface);
    if (data->display) {
        DFB_DisplayData *dispdata =
            (DFB_DisplayData *) data->display->driverdata;
        dispdata->vidIDinuse = 0;
        dispdata->vidlayer->SetCooperativeLevel(dispdata->vidlayer,
                                                DLSCL_ADMINISTRATIVE);
        SDL_DFB_RELEASE(dispdata->vidlayer);
    }
    SDL_FreeDirtyRects(&data->dirty);
    SDL_DFB_FREE(data->pixels);
    SDL_free(data);
    texture->driverdata = NULL;
}

static void
DirectFB_DestroyRenderer(SDL_Renderer * renderer)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;

    if (data) {
        SDL_DFB_RELEASE(data->surface);
        SDL_free(data);
    }
    SDL_free(renderer);
}

/* vi: set ts=4 sw=4 expandtab: */