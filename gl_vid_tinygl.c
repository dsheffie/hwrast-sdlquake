// SDL2 + TinyGL + hardware-engine video layer for GLQuake.
//
// Replaces gl_vidlinuxglx.c (X11/GLX).  TinyGL provides the GL pipeline; our
// hardware engine (via tgl_engine) rasterizes the triangles TinyGL emits; SDL2
// owns the window and presents the engine's framebuffer each swap.  Multitexture,
// 8-bit shared palette, and gamma are forced off (the simple path).

#include <SDL.h>
#include "quakedef.h"
#include "zbuffer.h"     // TinyGL: ZBuffer, ZB_open/close, ZB_MODE_RGBA
#include "hw_rast.h"     // engine: hw_open/clear/fb_read/close
#include "gfx.h"         // engine: load_texture

void tgl_set_device(hw_rast *d);   // tgl_engine.cc

#define WARP_WIDTH   320
#define WARP_HEIGHT  200

// --- globals other GL files expect (formerly defined in gl_vidlinuxglx.c) ---
unsigned short d_8to16table[256];
unsigned       d_8to24table[256];
unsigned char  d_15to8table[65536];

cvar_t  vid_mode = {"vid_mode", "0", false};
int     texture_mode = GL_LINEAR;
int     texture_extension_number = 1;
float   gldepthmin, gldepthmax;
cvar_t  gl_ztrick = {"gl_ztrick", "1"};

const char *gl_vendor;
const char *gl_renderer;
const char *gl_version;
const char *gl_extensions;

void (*qglColorTableEXT)(int, int, int, int, int, const void *);
void (*qgl3DfxSetPaletteEXT)(GLuint *);

qboolean is8bit = false;
qboolean isPermedia = false;
qboolean gl_mtexable = false;

// --- our backend state ---
static hw_rast      *engine;
static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *fbtex;
static ZBuffer      *zb;
static int           scr_width, scr_height;
static unsigned char *fbrgb;

void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}

qboolean VID_Is8bit(void) { return is8bit; }
void     VID_ShiftPalette(unsigned char *palette) {}

void VID_SetPalette(unsigned char *palette) {
  byte     *pal = palette;
  unsigned *table = d_8to24table;
  int i;
  for (i = 0; i < 256; i++) {
    unsigned r = pal[0], g = pal[1], b = pal[2];
    pal += 3;
    *table++ = (255u << 24) | (r << 0) | (g << 8) | (b << 16);   // ABGR (matches GL upload)
  }
  d_8to24table[255] &= 0xffffff;   // 255 is transparent
}

static void CheckMultiTextureExtensions(void) { gl_mtexable = false; }

void GL_Init(void) {
  gl_vendor     = (const char *)glGetString(GL_VENDOR);
  gl_renderer   = (const char *)glGetString(GL_RENDERER);
  gl_version    = (const char *)glGetString(GL_VERSION);
  gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
  Con_Printf("GL_RENDERER: %s\n", gl_renderer ? gl_renderer : "(null)");

  CheckMultiTextureExtensions();

  glClearColor(0, 0, 0, 0);
  glCullFace(GL_FRONT);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.666);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glShadeModel(GL_FLAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

void GL_BeginRendering(int *x, int *y, int *width, int *height) {
  *x = *y = 0;
  *width  = scr_width;
  *height = scr_height;
  hw_clear(engine);          // clear OUR color + depth each frame
}

extern long tgl_tris, tgl_culls;   // tgl_engine.cc per-frame counters

void GL_EndRendering(void) {
  hw_fb_read(engine, fbrgb);                            // engine FB (RGB, top-down)

  // headless capture: GEN_FRAME_PPM_FILES dumps frameNNNN.ppm; HW_MAX_FRAMES=N exits
  {
    static int frame = 0;
    // per-frame heartbeat (HW_STATS=1): triangles rasterized this frame
    // (0-ish = 2D/loading, thousands = the 3D world view)
    static int hb = -1;
    if (hb < 0) hb = getenv("HW_STATS") ? 1 : 0;
    if (hb) fprintf(stderr, "[hb] frame %d: %ld tris, %ld culled\n", frame, tgl_tris, tgl_culls);
    tgl_tris = 0; tgl_culls = 0;
    if (getenv("GEN_FRAME_PPM_FILES")) {
      char name[64]; FILE *fp;
      sprintf(name, "frame%04d.ppm", frame);
      fp = fopen(name, "wb");
      if (fp) { fprintf(fp, "P6\n%d %d\n255\n", scr_width, scr_height);
                fwrite(fbrgb, 1, scr_width * scr_height * 3, fp); fclose(fp); }
    }
    frame++;
    { const char *mx = getenv("HW_MAX_FRAMES"); if (mx && frame >= atoi(mx)) exit(0); }
  }

  SDL_UpdateTexture(fbtex, NULL, fbrgb, scr_width * 3);
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, fbtex, NULL, NULL);
  SDL_RenderPresent(renderer);
}

void VID_Shutdown(void) {
  if (zb) ZB_close(zb);
  if (engine) hw_close(engine);
  SDL_Quit();
}

void VID_Init(unsigned char *palette) {
  int width = 640, height = 480;
  int i;

  Cvar_RegisterVariable(&vid_mode);
  Cvar_RegisterVariable(&gl_ztrick);

  if ((i = COM_CheckParm("-width")) != 0)  width  = Q_atoi(com_argv[i + 1]);
  if ((i = COM_CheckParm("-height")) != 0) height = Q_atoi(com_argv[i + 1]);

  vid.maxwarpwidth  = WARP_WIDTH;
  vid.maxwarpheight = WARP_HEIGHT;
  vid.colormap   = host_colormap;
  vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
  vid.conwidth   = width;
  vid.conheight  = height;
  vid.width      = width;
  vid.height     = height;
  vid.aspect     = ((float)height / (float)width) * (320.0 / 240.0);
  vid.numpages   = 2;
  scr_width  = width;
  scr_height = height;

  // SDL2 window + present surface
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
    Sys_Error("SDL_Init failed: %s", SDL_GetError());
  window   = SDL_CreateWindow("GLQuake (HW engine)", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, width, height, 0);
  renderer = SDL_CreateRenderer(window, -1, 0);
  fbtex    = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                               SDL_TEXTUREACCESS_STREAMING, width, height);
  fbrgb    = malloc(width * height * 3);

  // our engine + TinyGL context.  Bind a white texture so the (currently
  // untextured) fills pass vertex color through the texel*color modulate.
  engine = hw_open(width, height);
  tgl_set_device(engine);
  {
    static unsigned char white[128 * 128 * 3];
    memset(white, 0xff, sizeof white);
    load_texture(engine, white, 128);
  }
  zb = ZB_open(width, height, ZB_MODE_RGBA, 0);
  if (!zb) Sys_Error("ZB_open failed");
  glInit(zb);

  GL_Init();
  VID_SetPalette(palette);
  vid.recalc_refdef = 1;
  Con_SafePrintf("Video mode %dx%d (TinyGL + HW engine)\n", width, height);
}

// --- input ---

static int SDLKey_to_Quake(SDL_Keycode k) {
  switch (k) {
  case SDLK_ESCAPE:    return K_ESCAPE;
  case SDLK_RETURN:    return K_ENTER;
  case SDLK_TAB:       return K_TAB;
  case SDLK_BACKSPACE: return K_BACKSPACE;
  case SDLK_DELETE:    return K_DEL;
  case SDLK_UP:        return K_UPARROW;
  case SDLK_DOWN:      return K_DOWNARROW;
  case SDLK_LEFT:      return K_LEFTARROW;
  case SDLK_RIGHT:     return K_RIGHTARROW;
  case SDLK_LALT: case SDLK_RALT:     return K_ALT;
  case SDLK_LCTRL: case SDLK_RCTRL:   return K_CTRL;
  case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
  case SDLK_F1: return K_F1;  case SDLK_F2: return K_F2;
  case SDLK_F3: return K_F3;  case SDLK_F4: return K_F4;
  case SDLK_F5: return K_F5;  case SDLK_F6: return K_F6;
  case SDLK_F7: return K_F7;  case SDLK_F8: return K_F8;
  case SDLK_F9: return K_F9;  case SDLK_F10: return K_F10;
  case SDLK_F11: return K_F11; case SDLK_F12: return K_F12;
  case SDLK_PAGEUP: return K_PGUP;  case SDLK_PAGEDOWN: return K_PGDN;
  case SDLK_HOME: return K_HOME;    case SDLK_END: return K_END;
  default:
    if (k >= SDLK_SPACE && k < 127) return k;   // printable ASCII
    return 0;
  }
}

void Sys_SendKeyEvents(void) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
    case SDL_QUIT:
      Sys_Quit();
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      int qk = SDLKey_to_Quake(ev.key.keysym.sym);
      if (qk) Key_Event(qk, ev.type == SDL_KEYDOWN);
      break;
    }
    }
  }
}

void IN_Init(void) {}
void IN_Shutdown(void) {}
void IN_Commands(void) {}
void IN_Move(usercmd_t *cmd) {}

// dedicated-server console input; unused for the windowed client
char *Sys_ConsoleInput(void) { return NULL; }
