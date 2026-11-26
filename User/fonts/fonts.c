#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FONT_DIR "/usr/share/fonts"
#define CONSOLE "/lib/fonts/console.gray"

extern int lx_save_buffer(const char *, const void *, unsigned);

/* One guest snapshot for the entire payload, including on the TCC kernel. */
static int publish(char *tmp, const char *dest, const void *bytes, unsigned size) {
  int fd = mkstemp(tmp);
  if (fd < 0) {
    perror(tmp);
    return 1;
  }
  if (close(fd) || lx_save_buffer(tmp, bytes, size) || chmod(tmp, 0644) ||
      rename(tmp, dest)) {
    unlink(tmp);
    fprintf(stderr, "fonts: could not write %s\n", dest);
    return 1;
  }
  puts(dest);
  return 0;
}

static int install(const char *path, FT_Library ft) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *ext = strrchr(base, '.');
  char dest[512], tmp[] = FONT_DIR "/.install-XXXXXX";
  if (!ext ||
      (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf") &&
       strcasecmp(ext, ".ttc")) ||
      snprintf(dest, sizeof dest, FONT_DIR "/user-%s", base) >=
          (int)sizeof dest) {
    fputs("fonts: expected a TTF, OTF or TTC filename\n", stderr);
    return 1;
  }
  FILE *in = fopen(path, "rb");
  if (!in) {
    perror(path);
    return 1;
  }
  struct stat info;
  if (fstat(fileno(in), &info) || info.st_size <= 0 || info.st_size > UINT_MAX) {
    fclose(in);
    fputs("fonts: invalid or unreadable font\n", stderr);
    return 1;
  }
  unsigned size = (unsigned)info.st_size;
  unsigned char *data = malloc(size);
  FT_Face face;
  int result = 1;
  if (data && fread(data, 1, size, in) == size &&
      FT_New_Memory_Face(ft, data, size, 0, &face) == 0) {
    FT_Done_Face(face);
    result = publish(tmp, dest, data, size);
  } else {
    fputs("fonts: invalid or unreadable font\n", stderr);
  }
  free(data);
  fclose(in);
  return result;
}

static int console_font(FT_Face face) {
  enum { W = 22, H = 44 };
  unsigned char atlas[95][H][W] = {{{0}}};
  if (FT_Set_Pixel_Sizes(face, 0, 36))
    return 1;
  for (unsigned ch = 32; ch < 127; ++ch) {
    if (!FT_Get_Char_Index(face, ch) ||
        FT_Load_Char(face, ch, FT_LOAD_RENDER)) {
      fputs("fonts: the console font must contain printable ASCII\n", stderr);
      return 1;
    }
    FT_GlyphSlot glyph = face->glyph;
    FT_Bitmap *bitmap = &glyph->bitmap;
    if (bitmap->width && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY)
      return 1;
    int left = (W - (int)(glyph->advance.x >> 6)) / 2 + glyph->bitmap_left;
    int top = 34 - glyph->bitmap_top;
    for (int y = 0; y < (int)bitmap->rows; ++y) {
      if (top + y < 0 || top + y >= H)
        continue;
      const unsigned char *row = bitmap->buffer + y * bitmap->pitch;
      for (int x = 0; x < (int)bitmap->width; ++x)
        if (left + x >= 0 && left + x < W)
          atlas[ch - 32][top + y][left + x] = row[x];
    }
  }
  char tmp[] = "/lib/fonts/.console-XXXXXX";
  return publish(tmp, CONSOLE, atlas, sizeof atlas);
}

static int describe(FcPattern *p) {
  FcChar8 *family, *file;
  if (FcPatternGetString(p, FC_FAMILY, 0, &family) != FcResultMatch ||
      FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch)
    return 1;
  printf("%s\t%s\n", family, file);
  return 0;
}

static int query(const char *pattern) {
  FcConfig *config = FcConfigCreate();
  int status = 1;
  if (!config ||
      !FcConfigParseAndLoad(config, (const FcChar8 *)"/etc/fonts/fonts.conf",
                            FcTrue) ||
      !FcConfigBuildFonts(config))
    goto done;
  if (!pattern) {
    FcFontSet *set = FcConfigGetFonts(config, FcSetSystem);
    if (set)
      for (int i = 0; i < set->nfont; ++i)
        if (describe(set->fonts[i]) == 0)
          status = 0;
  } else {
    FcPattern *p = FcNameParse((const FcChar8 *)pattern);
    if (!p)
      goto done;
    if (FcConfigSubstitute(config, p, FcMatchPattern)) {
      FcDefaultSubstitute(p);
      FcResult result;
      FcPattern *match = FcFontMatch(config, p, &result);
      if (match) {
        status = describe(match);
        FcPatternDestroy(match);
      }
    }
    FcPatternDestroy(p);
  }
done:
  if (config)
    FcConfigDestroy(config);
  return status;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "list"))
    return query(NULL);
  if (argc == 3 && !strcmp(argv[1], "match"))
    return query(argv[2]);
  if (argc == 3 &&
      (!strcmp(argv[1], "install") || !strcmp(argv[1], "console"))) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft))
      return 1;
    int result = 1;
    if (!strcmp(argv[1], "install"))
      result = install(argv[2], ft);
    else if (!strcmp(argv[2], "--reset"))
      result = unlink(CONSOLE) == 0 || errno == ENOENT ? 0 : 1;
    else {
      FT_Face face;
      if (FT_New_Face(ft, argv[2], 0, &face))
        fprintf(stderr, "fonts: cannot open %s\n", argv[2]);
      else {
        result = console_font(face);
        FT_Done_Face(face);
      }
    }
    FT_Done_FreeType(ft);
    return result;
  }
  fputs("usage: fonts list | match PATTERN | install FILE | console FILE | "
        "console --reset\n",
        stderr);
  return 2;
}
