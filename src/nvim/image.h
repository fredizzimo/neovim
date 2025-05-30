#pragma once
#include "nvim/api/keysets_defs.h"
#include "nvim/lib/queue_defs.h"

typedef struct DecorImage {
  LuaRef luaref;
  const char *data;
  size_t data_size;
  int width;
  int height;
  int id;
  int ref_count;
  QUEUE queue;
} DecorImage;


typedef struct {
  int img_id;
  int width;
  int height;
  bool keep_aspect;
} ImagePlacement;

extern char ImageMagicUtf8[3];


#define IMAGE_PLACEMENT_INITIALIZER {0}


#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "image.h.generated.h"
#endif
