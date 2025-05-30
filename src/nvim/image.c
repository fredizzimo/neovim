#include <assert.h>
#include <lua.h>
#include <sys/types.h>

#include "nvim/api/extmark.h"
#include "nvim/api/keysets_defs.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/dispatch.h"
#include "nvim/api/private/helpers.h"
#include "nvim/grid.h"
#include "nvim/image.h"
#include "nvim/extmark.h"
#include "nvim/lib/queue_defs.h"
#include "nvim/lua/converter.h"
#include "nvim/lua/executor.h"
#include "nvim/ui.h"
#include "nvim/map_defs.h"
#include "nvim/mbyte.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "image.c.generated.h"
#endif

static size_t current_size = SIZE_MAX;
static QUEUE cache;

#define MAX_CACHE_SIZE 32 * 1024 * 1024
#define MAX_IMAGE_CELLS = 108
#define MAGIC 0xF8FF

char ImageMagicUtf8[3] =  {(char)0xef, (char)0xa3, (char)0xbf};

// Use a set of dacritics that fits in two bytes when encoded using UTF-8
// There's 108 of them, so we are using base-108
static uint16_t diacritics[] = {
  0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B,
  0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367,
  0x0368, 0x0369, 0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485,
  0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D,
  0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610,
  0x0611, 0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A,
  0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF,
  0x06E0, 0x06E1, 0x06E2, 0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
  0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747, 0x0749,
  0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF, 0x07F0, 0x07F1, 0x07F3,
};

static uint8_t diacritic_lookup[0x7F3 - 0x305];

#define BASE (sizeof(diacritics) / sizeof(uint16_t))
// We use three symbols, which gives a maximum of 1 259 712 active cell
#define MAX_ACTIVE_CELLS (BASE * BASE * BASE)

// TODO(fredizzimo), this should be a fifo
static int next_cell = 0;


static int next_placement_id = 1;
static Map(ImagePlacement, int) placed_images = MAP_INIT;
static Map(int, ImagePlacement) id_to_placed_image = MAP_INIT;

// Use two arrays instead of one to save some memory due to alignment requirements
// It cell needs an image id and a cell nr
static int* cell_img_ids = NULL;
static uint16_t* cell_nr = NULL;

static void init_image_support(void) {
  if (cell_img_ids == NULL) {
    // TODO(fredizzimo): Free up the memory
    cell_img_ids = malloc(sizeof(int) * MAX_ACTIVE_CELLS); 
    cell_nr = malloc(sizeof(uint16_t) * MAX_ACTIVE_CELLS); 

    for(size_t i = 0; i<BASE;i++) {
      diacritic_lookup[diacritics[i] - 0x305] = i;
    }

    QUEUE_INIT(&cache);
    current_size = 0;
  }
}

static void remove_image(DecorImage *image)
{
  current_size -= image->data_size;
  QUEUE_REMOVE(&image->queue);
  // Make sure the it's completely uninitialized
  image->queue.next = NULL;
  image->queue.prev = NULL;
}

void access_image(DecorImage *image)
{
  if (image->queue.prev) {
    // Already added to the cache
    QUEUE_REMOVE(&image->queue);
  } else {
    // A new image
    size_t entry_size = image->data_size;

    // Evict old entries if exceeding the byte limit
    while (current_size + entry_size > MAX_CACHE_SIZE) {
      assert(cache.next && cache.next != &cache);
      DecorImage *old_tail = QUEUE_DATA(cache.next, DecorImage, queue);
      remove_image(old_tail);
    }
    // current_size += image->data_size;
    // String data = cbuf_as_string((char*)image->data, image->data_size);
    // ui_call_img_add(image->id, data, image->width, image->height);
  }
  QUEUE_INIT(&image->queue);
  QUEUE_INSERT_HEAD(&cache, &image->queue);
}

static void create_placeholder(void) {

  //int hl_id = syn_check_group(name.data, name.size);
}

DecorImage *init_image(Dict(set_extmark) *opts, Arena *arena, Error *err)
{
  init_image_support();

  KeyDict_set_extmark_image image = KEYDICT_INIT;
  //TODO(fredizzimo): what is this?
  char *err_param = 0;

  lua_State *const lstate = get_global_lstate();
  nlua_pushref(lstate, opts->image);
  nlua_pop_keydict(lstate, &image, KeyDict_set_extmark_image_get_field, &err_param, arena, err);
  DecorImage *decor_image = NULL;
  if (HAS_KEY(&image, set_extmark_image, _decor_image)) {
    nlua_pushref(lstate, image._decor_image);
    decor_image = lua_touserdata(lstate, -1);
    // The ref count can be 0, if the image cached by the user and added again
    assert(decor_image->ref_count >= 0);
    // But it should not be present in the cache
    assert(decor_image->ref_count > 0
           || (decor_image->queue.next == NULL && decor_image->queue.prev == NULL));
    decor_image->ref_count++;


  } else {
    nlua_pushref(lstate, opts->image);
    decor_image = lua_newuserdata(lstate, sizeof(DecorImage));
    lua_setfield(lstate, -2, "_decor_image");
    decor_image->id = 1;
    decor_image->width = image.width;
    decor_image->height = image.height;
    decor_image->ref_count = 1;
    decor_image->queue.prev = NULL;
    decor_image->queue.next = NULL;

    //TODO(fredizzimo): Validate all fields
    if (HAS_KEY(&image, set_extmark_image, data)) {
      nlua_pushref(lstate, image.data);
      decor_image->data = lua_tolstring(lstate, -1, &decor_image->data_size);
      lua_pop(lstate, 1);
    }
  }

  return decor_image;
}

void image_free(DecorImage *image)
{
  image->ref_count--;
  if (image->ref_count == 0 && image->queue.next) {
    remove_image(image);
  }
}


uint32_t image_create_placement(ImagePlacement placement) {
  init_image_support();
  int placement_id = 0;
  lua_State *const lstate = get_global_lstate();
  lua_getglobal(lstate, "vim");
  lua_getfield(lstate, -1, "ui");
  lua_getfield(lstate, -1, "img");
  lua_getfield(lstate, -1, "_id_to_image");
  lua_pushinteger(lstate, placement.img_id);
  lua_gettable(lstate, -2);
  if(lua_istable(lstate, -1)) {
    // The image exists
    lua_getmetatable(lstate, -1);
    lua_getfield(lstate, -1, "sent");
    bool sent = lua_toboolean(lstate, -1);
    lua_pop(lstate, 1);
    if (!sent) {
      // TODO(fredizzimo): Maybe queue the sending
      lua_getfield(lstate, -1, "data");
      size_t len;
      char* data = (char*)lua_tolstring(lstate, -1, &len);
      String ui_data = cbuf_as_string(data, len);
      ui_call_img_add(placement.img_id, ui_data);
      lua_pop(lstate, 1);
      lua_pushboolean(lstate, true);
      lua_setfield(lstate, -2, "sent");
    }
    lua_pop(lstate, 1);


    placement_id = map_get(ImagePlacement, int)(&placed_images, placement);
    if (placement_id == 0) {
      placement_id = next_placement_id;
      map_put(ImagePlacement, int)(&placed_images, placement, placement_id);
      map_put(int, ImagePlacement)(&id_to_placed_image, placement_id, placement);
      next_placement_id++;
      ui_call_img_show(placement_id, placement.img_id, placement.width, placement.height, placement.keep_aspect);
    }


  }
  lua_pop(lstate, 1);
  return placement_id;
}

char* image_convert_to_text(int placement_id, int start_col, int num_cols, int start_row) {
  const ImagePlacement *placement = map_ref(int, ImagePlacement)(&id_to_placed_image, placement_id, NULL);
  if (!placement) {
    return NULL;
  }
  size_t current_cell = 0;
  // Magic is 3 bytes
  char* output = malloc(3 + num_cols*2*3 + 1);
  char* cur_char = output;
  for (int i = 0; i<num_cols; i++) {
    if (i % 16 == 0) {
      current_cell = next_cell++;
      cell_img_ids[current_cell] = placement_id;
      cell_nr[current_cell] = i + start_col + (start_row * placement->width);
    }
    cur_char += utf_char2bytes(MAGIC, cur_char);
    uint32_t cur_output = (current_cell << 4) + i % 16;
    if (cur_output == 0) {
        // Avoid special casing 0 when decoding by ensuring that we are using at least 4 bytes 
        cur_char += utf_char2bytes(diacritics[cur_output % BASE], cur_char);
    } else {
      while(cur_output) {
        cur_char += utf_char2bytes(diacritics[cur_output % BASE], cur_char);
        cur_output /= BASE;
      }
    }

  }
  *cur_char = 0;
  return realloc(output, cur_char - output);
}

uint32_t schar_idx_from_image(String str) {
  const char* cur = str.data;
  const char* end = cur + str.size;
  int magic = mb_cptr2char_adv(&cur);
  assert(magic == MAGIC);
  uint32_t result = 0;
  while(cur < end) {
    int val = mb_cptr2char_adv(&cur);
    val = diacritic_lookup[val - 0x305];
    result = result * BASE + val;
  }
  return result;
}

void get_image_from_schar(schar_T sc, size_t* placement_id, size_t* cell) {
  size_t idx = sc_image_idx(sc);
  if (idx == SIZE_MAX) {
    *placement_id = SIZE_MAX;
    *cell = SIZE_MAX;
    return;
  }
  *cell  = idx & 0xF;
  size_t index = idx >> 4;
  *cell += cell_nr[index];
  *placement_id = cell_img_ids[index];
}


//TODO(fredizzimo): Clean up placements
