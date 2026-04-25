#include <pspsdk.h>
#include <pspkernel.h>
#include <pspge.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>
#include <systemctrl.h>
#include "ge_constants.h"

#undef FindProc
#define FindProc sctrlHENFindFunction

PSP_MODULE_INFO("GePatch", 0x1007, 1, 0);

#define DRAW_NATIVE 0xABCDEF00

#define PITCH 960
#define WIDTH 960
#define HEIGHT 544
#define PIXELFORMAT GE_FORMAT_565

#define FAKE_VRAM 0x0A000000
#define DISPLAY_BUFFER 0x0A400000
#define VERTICES_BUFFER 0x0A600000
#define RENDER_LIST 0x0A800000

#define VRAM_DRAW_BUFFER_OFFSET 0x04000000
#define VRAM_DEPTH_BUFFER_OFFSET 0x04100000
#define VRAM_1KB 0x041ff000
#define VERTEX_CACHE_SIZE 512//改一下缓存大小
#define log(...)

// void logmsg(char *msg) {
//   int k1 = pspSdkSetK1(0);

//   SceUID fd = sceIoOpen("ms0:/ge_patch.txt", PSP_O_WRONLY | PSP_O_CREAT, 0777);
//   if (fd >= 0) {
//     sceIoLseek(fd, 0, PSP_SEEK_END);
//     sceIoWrite(fd, msg, strlen(msg));
//     sceIoClose(fd);
//   }

//   pspSdkSetK1(k1);
// }

static const u8 tcsize[4] = { 0, 2, 4, 8 }, tcalign[4] = { 0, 1, 2, 4 };
static const u8 colsize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, colalign[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
static const u8 nrmsize[4] = { 0, 3, 6, 12 }, nrmalign[4] = { 0, 1, 2, 4 };
static const u8 possize[4] = { 3, 3, 6, 12 }, posalign[4] = { 1, 1, 2, 4 };
static const u8 wtsize[4] = { 0, 1, 2, 4 }, wtalign[4] = { 0, 1, 2, 4 };

#define ALIGN(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

void getVertexInfo(u32 op, u8 *vertex_size, u8 *pos_off, u8 *visit_off) {
  int tc = (op & GE_VTYPE_TC_MASK) >> GE_VTYPE_TC_SHIFT;
  int col = (op & GE_VTYPE_COL_MASK) >> GE_VTYPE_COL_SHIFT;
  int nrm = (op & GE_VTYPE_NRM_MASK) >> GE_VTYPE_NRM_SHIFT;
  int pos = (op & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
  int weight = (op & GE_VTYPE_WEIGHT_MASK) >> GE_VTYPE_WEIGHT_SHIFT;
  int weightCount = ((op & GE_VTYPE_WEIGHTCOUNT_MASK) >> GE_VTYPE_WEIGHTCOUNT_SHIFT) + 1;
  int morphCount = ((op & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT) + 1;

  u8 biggest = 0;
  u8 size = 0;
  u8 aligned_size = 0;
  // u8 weightoff = 0, tcoff = 0, coloff = 0, nrmoff = 0;
  u8 posoff = 0;
  u8 visitoff = 0;

  if (weight) {
    // size = ALIGN(size, wtalign[weight]);
    // weightoff = size;
    size += wtsize[weight] * weightCount;
    if (wtalign[weight] > biggest)
      biggest = wtalign[weight];
  }

  if (tc) {
    aligned_size = ALIGN(size, tcalign[tc]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // tcoff = size;
    size += tcsize[tc];
    if (tcalign[tc] > biggest)
      biggest = tcalign[tc];
  }

  if (col) {
    aligned_size = ALIGN(size, colalign[col]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // coloff = size;
    size += colsize[col];
    if (colalign[col] > biggest)
      biggest = colalign[col];
  }

  if (nrm) {
    aligned_size = ALIGN(size, nrmalign[nrm]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // nrmoff = size;
    size += nrmsize[nrm];
    if (nrmalign[nrm] > biggest)
      biggest = nrmalign[nrm];
  }

  if (pos) {
    aligned_size = ALIGN(size, posalign[pos]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    posoff = size;
    size += possize[pos];
    if (posalign[pos] > biggest)
      biggest = posalign[pos];
  }

  aligned_size = ALIGN(size, biggest);
  if (!visitoff && aligned_size != size)
    visitoff = size;
  size = aligned_size;
  size *= morphCount;

  *vertex_size = size;
  *pos_off = posoff;
  *visit_off = visitoff;
}

void GetIndexBounds(const void *inds, int count, u32 vertType, u16 *indexLowerBound, u16 *indexUpperBound) {
  int i;
  int lowerBound = 0x7FFFFFFF;
  int upperBound = 0;
  u32 idx = vertType & GE_VTYPE_IDX_MASK;
  if (idx == GE_VTYPE_IDX_8BIT) {
    const u8 *ind8 = (const u8 *)inds;
    for (i = 0; i < count; i++) {
      u8 value = ind8[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_16BIT) {
    const u16 *ind16 = (const u16 *)inds;
    for (i = 0; i < count; i++) {
      u16 value = ind16[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_32BIT) {
    const u32 *ind32 = (const u32 *)inds;
    for (i = 0; i < count; i++) {
      u16 value = (u16)ind32[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else {
    lowerBound = 0;
    upperBound = count - 1;
  }
  *indexLowerBound = (u16)lowerBound;
  *indexUpperBound = (u16)upperBound;
}

typedef struct {
  u32 list;
  u32 offset;
} StackEntry;

typedef struct {
  u32 addr;
  u32 type;
  u16 count;
} VertexCacheEntry;//缓存

typedef struct {
  u32 ge_cmds[0x100];

  u32 texbufptr[8];
  u32 texbufwidth[8];
  u32 framebufptr;
  u32 framebufwidth;
  u32 *framebufwidth_addr;

  u32 base;
  u32 offset;
  u32 address;

  u32 index_addr;
  u32 vertex_addr;
  u32 vertex_type;

  u32 ignore_framebuf;
  u32 ignore_texture;

  u32 sync;
  u32 finished;

  StackEntry stack[64];
  u32 curr_stack;
   
  u32 framebuf_addr[16];
  u32 framebuf_count;

  u32 last_vertex_addr;//缓存
  u32 last_vertex_count;//缓存

  VertexCacheEntry vcache[VERTEX_CACHE_SIZE];
  u32 vcache_pos;
} GeState;

static GeState state;

static int rendered_in_sync = 0;
static int framebuf_set = 0;

static u32 last_list = 0;//新增加状态

static int fb_dirty = 1;//FrameBuf
static void *last_fb = NULL;//FrameBuf



static int fb_pending = 0;
static int fb_last_vsync = 0;
static int fb_copy_lock = 0;//节拍器

static int skip = 0;//移动速度加速，降低调用api频率


static int dirty_x = 0;
static int dirty_y = 0;
static int dirty_w = WIDTH;
static int dirty_h = HEIGHT;

static inline void tryFrameCopy()
{
  if (!fb_pending)
    return;

  if (fb_copy_lock)
    return;

  fb_copy_lock = 1;

  // 👉 强制“每帧最多一次”
  copyFrameBuffer();

  fb_dirty = 0;
  fb_pending = 0;

  fb_copy_lock = 0;
}

static inline int checkVertexCache(u32 addr, u32 type, u16 count) {
  u32 hash = (addr >> 4) ^ type ^ count;
  u32 idx = hash & (VERTEX_CACHE_SIZE - 1);

  VertexCacheEntry *e = &state.vcache[idx];

  if (e->addr == addr && e->type == type && e->count == count) {
    return 1; // 命中
  }

  // 写入（覆盖式）
  e->addr = addr;
  e->type = type;
  e->count = count;

  return 0;
}
extern u32 last_list; //新加参数1
extern int was_paused;//新加参数2

void resetGeState() {
  memset(&state, 0, sizeof(GeState));
}

int push(StackEntry data) {
  if (state.curr_stack < (sizeof(state.stack) / sizeof(StackEntry))) {
    state.stack[state.curr_stack++] = data;
    return 0;
  }
  return -1;
}

StackEntry pop() {
  if (state.curr_stack > 0)
    return state.stack[--state.curr_stack];
  StackEntry stack_entry;
  stack_entry.list = -1;
  stack_entry.offset = -1;
  return stack_entry;
}

int findFramebuf(u32 framebuf) {
  int i;
  for (i = 0; i < state.framebuf_count; i++) {
    // Some textures point to within the framebuf.
    // If we want to be correct we should record the pixelsize, linewidth and height.
    if (framebuf >= state.framebuf_addr[i] && framebuf < (state.framebuf_addr[i] + 0x400)) {
      return i;
    }
  }

  return -1;
}

int insertFramebuf(u32 framebuf) {
  int res = findFramebuf(framebuf);
  if (res >= 0)
    return res;

  if (state.framebuf_count < (sizeof(state.framebuf_addr) / sizeof(u32))) {
    state.framebuf_addr[state.framebuf_count] = framebuf;
    return state.framebuf_count++;
  }

  return -1;
}

void AdvanceVerts(int count, int vertex_size) {
  if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
    int index_shift = ((state.vertex_type & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
    state.index_addr += count << index_shift;
  } else {
    state.vertex_addr += count * vertex_size;
  }
}

// TODO: when ignore_framebuf=1 or ignore_texture=1, dummy all non-control-flow instructions
u32 *handleControlFlowCommands(u32 *list) {
  StackEntry stack_entry;

  u32 op = *list;
  u32 cmd = op >> 24;
  u32 data = op & 0xffffff;

  switch (cmd) {
    case GE_CMD_BASE:
      state.base = (data << 8) & 0x0f000000;
      break;

    case GE_CMD_OFFSETADDR:
      state.offset = data << 8;
      break;

    case GE_CMD_ORIGIN:
      state.offset = (u32)list;
      break;

    // TODO: need to save other states, too?
    case GE_CMD_CALL:
      state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
      stack_entry.list = (u32)list;
      stack_entry.offset = state.offset;
      if (push(stack_entry) == 0)
        list = (u32 *)(state.address - 4);
      break;

    // TODO: is it okay to always take the branch?
    case GE_CMD_BJUMP:
      state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
      list = (u32 *)(state.address - 4);
      break;

    case GE_CMD_JUMP:
      state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
      list = (u32 *)(state.address - 4);
      break;

    case GE_CMD_RET:
    {
      // Ignore returns when the stack is empty
      stack_entry = pop();
      if (stack_entry.list != -1) {
        list = (u32 *)stack_entry.list;
        state.offset = stack_entry.offset;
      }
      break;
    }

    case GE_CMD_END:
    {
      u32 prev = *(list-1);
      switch (prev >> 24) {
        case GE_CMD_SIGNAL:
        {
          u8 behaviour = (prev >> 16) & 0xff;
          u16 signal = prev & 0xffff;
          u16 enddata = data & 0xffff;
          u32 target;

          state.sync = 0;

          switch (behaviour) {
            case PSP_GE_SIGNAL_SYNC:
              state.sync = 1;
              break;

            case PSP_GE_SIGNAL_JUMP:
              target = (((signal << 16) | enddata) & 0x0ffffffc);
              list = (u32 *)(target - 4);
              break;

            case PSP_GE_SIGNAL_CALL:
              target = (((signal << 16) | enddata) & 0x0ffffffc);
              stack_entry.list = (u32)list;
              stack_entry.offset = state.offset;
              if (push(stack_entry) == 0)
                list = (u32 *)(target - 4);
              break;

            case PSP_GE_SIGNAL_RET:
              // Ignore returns when the stack is empty
              stack_entry = pop();
              if (stack_entry.list != -1) {
                list = (u32 *)stack_entry.list;
                state.offset = stack_entry.offset;
              }
              break;

            default:
              break;
          }
          break;
        }

        case GE_CMD_FINISH:
          // After syncing, finish is ignored?
          if (state.sync) {
            state.sync = 0;
            break;
          }
          // resetGeState();
          state.finished = 1;
          return NULL;

        默认:
          break;
      }
      break;
    }

    default:
      break;
  }

  return list;
}

void patchGeList(u32 *list, u32 *stall) {
  union {
    float f;
    unsigned int i;
  } t;

  for (; !stall || (stall && list != stall); list++) {
    u32 op = *list;
    u32 cmd = op >> 24;
    u32 data = op & 0xffffff;

    state.ge_cmds[cmd] = op;

    switch (cmd) {
      case GE_CMD_IADDR:
        state.index_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        break;

      case GE_CMD_VADDR:
        state.vertex_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        break;

      case GE_CMD_VERTEXTYPE:
        state.vertex_type = data;
        break;

      case GE_CMD_BEZIER:
      case GE_CMD_SPLINE:
      {
        if (state.ignore_framebuf || state.ignore_texture) {
          *list = 0;
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u8 num_points_u = data & 0xff;
          u8 num_points_v = (data >> 8) & 0xff;

          u32 count = num_points_u * num_points_v;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      case GE_CMD_BOUNDINGBOX:
      {
        if (state.ignore_framebuf || state.ignore_texture)
          break;

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u32 count = data;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

     case GE_CMD_PRIM://顶点优化
{
  u16 count = data & 0xffff;

  // =========================================================
  // 🚀 0. THROUGH 快速过滤（必须最前）
  // =========================================================
  if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH) {
    AdvanceVerts(count, 0);
    break;
  }

  // =========================================================
  // 🚀 1. 顶点信息解析
  // =========================================================
  u8 vertex_size = 0, pos_off = 0, visit_off = 0;
  getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

  u16 lower = 0;
  u16 upper = count;

  if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
    GetIndexBounds((void *)state.index_addr, count, state.vertex_type, &lower, &upper);
    upper += 1;
  }

  int vertCount = upper - lower;

  // =========================================================
  // 🚀 2. 大规模直接跳过（地形/草/远景）
  // =========================================================
  if (vertCount > 1024) {
    AdvanceVerts(count, vertex_size);
    break;
  }

  // =========================================================
  // 🚀 3. 顶点缓存（核心：跨帧跳过 CPU）
  // =========================================================
  u32 base_addr = state.vertex_addr + lower * vertex_size;

  if (checkVertexCache(base_addr, state.vertex_type, vertCount)) {
    AdvanceVerts(count, vertex_size);
    break;
  }

  // =========================================================
  // 🚀 4. 小批次优化（UI / 掉帧关键来源）
  // =========================================================
  if (count < 200) {
    static u32 ui_frame_skip = 0;

    // ⚠️ 只对 UI 做节流，不影响战斗动作
    if ((ui_frame_skip++ & 1) == 0) {
      AdvanceVerts(count, vertex_size);
      break;
    }
  }

  // =========================================================
  // 🚀 5. 正常推进（不再做重计算）
  // =========================================================
  AdvanceVerts(count, vertex_size);
  break;
}
       
  int pos = (state.vertex_type & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
  int pos_size = possize[pos] / 3;

  u8 *vptr = (u8 *)base_addr;

  // =========================================================
  // 🚀 🚀 🚀 核心优化路径（单层循环 + branchless + 连续内存）
  // =========================================================

  if (pos_size == 2) {
    // ===== short 路径（主力路径）=====
    for (int i = 0; i < vertCount; i++) {

      short *vx = (short *)(vptr + pos_off);
      short *vy = (short *)(vptr + pos_off + 2);

      short x = *vx;
      short y = *vy;

      // ===== branchless x =====
      int x_is_special = (x == 480) | (x == 960);
      int x_in_range   = (x > -1024) & (x < 1024);
      int x_scaled     = x << 1;

      x = x_is_special ? 960 : (x_in_range ? x_scaled : x);

      // ===== branchless y =====
      int y_is_special = (y == 272) | (y == 544);
      int y_in_range   = (y > -1024) & (y < 1024);
      int y_scaled     = y << 1;

      y = y_is_special ? 544 : (y_in_range ? y_scaled : y);

      *vx = x;
      *vy = y;

      vptr += vertex_size;
    }

  } else if (pos_size == 4) {
    // ===== float 路径（保守优化版）=====
    for (int i = 0; i < vertCount; i++) {

      float *vx = (float *)(vptr + pos_off);
      float *vy = (float *)(vptr + pos_off + 4);

      float x = *vx;
      float y = *vy;

      if (x != 0.0f) {
        if (x == 480.0f || x == 960.0f) x = 960.0f;
        else if (x > -1024.0f && x < 1024.0f) x = x * 2.0f;
      }

      if (y != 0.0f) {
        if (y == 272.0f || y == 544.0f) y = 544.0f;
        else if (y > -1024.0f && y < 1024.0f) y = y * 2.0f;
      }

      *vx = x;
      *vy = y;

      vptr += vertex_size;
    }
  }

  // 🚀 5. 推进顶点指针
  AdvanceVerts(count, vertex_size);

  break;
}
  
  int pos = (state.vertex_type & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
  int pos_size = possize[pos] / 3;

  u32 vertex_addr = state.vertex_addr;

  for (int i = lower; i < upper; i++, vertex_addr += vertex_size) {

    for (int j = 0; j < 2; j++) {

      u32 addr = vertex_addr + pos_off + j * pos_size;

      if (pos_size == 2) {
        short *v = (short *)addr;

        if (*v != 0) {
          if (*v == 480 || *v == 960)
            *v = 960;
          else if (*v == 272 || *v == 544)
            *v = 544;
          else if (*v > -1024 && *v < 1024)
            *v <<= 1;   // 🚀 比 *2 更快
        }

      } else if (pos_size == 4) {
        float *f = (float *)addr;

        if (*f != 0.0f) {
          if (*f == 480.0f || *f == 960.0f)
            *f = 960.0f;
          else if (*f == 272.0f || *f == 544.0f)
            *f = 544.0f;
          else if (*f > -1024.0f && *f < 1024.0f)
            *f *= 2.0f;
        }
      }
    }
  }

  AdvanceVerts(count, vertex_size);
  break;
}

      case GE_CMD_FRAMEBUFPIXFORMAT:
        *list = (cmd << 24) | PIXELFORMAT;
        break;

      case GE_CMD_FRAMEBUFPTR:
      case GE_CMD_FRAMEBUFWIDTH:
      {
        if (cmd == GE_CMD_FRAMEBUFPTR) {
          *list = (cmd << 24) | (VRAM_DRAW_BUFFER_OFFSET & 0xffffff);
          state.framebufptr = op;
        } else {
          *list = (cmd << 24) | ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | PITCH;
          state.framebufwidth = op;
          state.framebufwidth_addr = list;
        }
      
        // =========================================================
        // 👉 等 ptr + width 都到齐
        // =========================================================
        if (state.framebufptr && state.framebufwidth) {
      
          u32 framebuf = FAKE_VRAM | (state.framebufptr & 0xffffff);
          u32 pitch = state.framebufwidth & 0xffff;
      
          // =========================================================
          // 🚀 分类 framebuffer 类型（只分类，不节流）
          // =========================================================
      
          if (pitch == 512 || pitch == 480) {
      
            // 👉 UI / 小buffer
            fb_dirty = 1;
      
            dirty_x = 0;
            dirty_y = 0;
            dirty_w = WIDTH;
            dirty_h = HEIGHT / 2;
      
          }
          else if (pitch == 960) {
      
            // 👉 主场景 framebuffer（永远标记 dirty）
            fb_dirty = 1;
      
            dirty_x = 0;
            dirty_y = 0;
            dirty_w = WIDTH;
            dirty_h = HEIGHT;
      
          }
          else {
      
            // 👉 非标准 buffer：忽略
            state.ignore_framebuf = 1;
            fb_dirty = 0;
          }
      
          // =========================================================
          // 👉 是否允许处理 framebuffer
          // =========================================================
      
          if (pitch == 512 || pitch == 480 || pitch == 960) {
            state.ignore_framebuf = 0;
          } else {
            state.ignore_framebuf = 1;
          }
      
          insertFramebuf(framebuf);
      
          state.framebufptr = 0;
          state.framebufwidth = 0;
        }
      
        break;
      }

      case GE_CMD_ZBUFPTR:
        *list = (cmd << 24) | (VRAM_DEPTH_BUFFER_OFFSET & 0xffffff);
        break;

      case GE_CMD_ZBUFWIDTH:
        *list = (cmd << 24) | ((VRAM_DEPTH_BUFFER_OFFSET >> 24) << 16) | PITCH;
        break;

      case GE_CMD_TEXADDR0:
      case GE_CMD_TEXADDR1:
      case GE_CMD_TEXADDR2:
      case GE_CMD_TEXADDR3:
      case GE_CMD_TEXADDR4:
      case GE_CMD_TEXADDR5:
      case GE_CMD_TEXADDR6:
      case GE_CMD_TEXADDR7:
      case GE_CMD_TEXBUFWIDTH0:
      case GE_CMD_TEXBUFWIDTH1:
      case GE_CMD_TEXBUFWIDTH2:
      case GE_CMD_TEXBUFWIDTH3:
      case GE_CMD_TEXBUFWIDTH4:
      case GE_CMD_TEXBUFWIDTH5:
      case GE_CMD_TEXBUFWIDTH6:
      case GE_CMD_TEXBUFWIDTH7:
      {
        int index;
        if (cmd >= GE_CMD_TEXADDR0 && cmd <= GE_CMD_TEXADDR7) {
          index = cmd - GE_CMD_TEXADDR0;
          state.texbufptr[index] = op;
        } else {
          index = cmd - GE_CMD_TEXBUFWIDTH0;
          state.texbufwidth[index] = op;
        }

        if (state.texbufptr[index] && state.texbufwidth[index]) {
          u32 texaddr = ((state.texbufwidth[index] & 0x0f0000) << 8) | (state.texbufptr[index] & 0xffffff);
          if (findFramebuf(texaddr) >= 0) {
            // Causes issues in Tekken 6
            state.ignore_texture = 1;
          } else {
            state.ignore_texture = 0;
          }

          state.texbufptr[index] = 0;
          state.texbufwidth[index] = 0;
        }

        break;
      }

      case GE_CMD_VIEWPORTXSCALE:
        t.i = data << 8;
        t.f = (t.f < 0) ? -(WIDTH / 2) : (WIDTH / 2);
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTYSCALE:
        t.i = data << 8;
        t.f = (t.f < 0) ? -(HEIGHT / 2) : (HEIGHT / 2);
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTXCENTER:
        t.f = 2048;
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTYCENTER:
        t.f = 2048;
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_OFFSETX:
        *list = (cmd << 24) | ((2048 - (WIDTH / 2)) << 4);
        break;

      case GE_CMD_OFFSETY:
        *list = (cmd << 24) | ((2048 - (HEIGHT / 2)) << 4);
        break;

      case GE_CMD_REGION2:
      case GE_CMD_SCISSOR2:
        *list = (cmd << 24) | ((HEIGHT - 1) << 10) | (WIDTH - 1);
        break;

      default:
        list = handleControlFlowCommands(list);
        if (!list)
          return;
        break;
    }

void *(* _sceGeEdramGetAddr)(void);
unsigned int *(* _sceGeEdramGetSize)(void);
int (* _sceGeGetList)(int qid, void *list, int *flag);
int (* _sceGeListUpdateStallAddr)(int qid, void *stall);
int (* _sceGeListEnQueue)(const void *list, void *stall, int cbid, PspGeListArgs *arg);
int (* _sceGeListEnQueueHead)(const void *list, void *stall, int cbid, PspGeListArgs *arg);
int (* _sceGeListSync)(int qid, int syncType);
int (* _sceGeDrawSync)(int syncType);

int (* _sceDisplaySetFrameBuf)(void *topaddr, int bufferwidth, int pixelformat, int sync);

void *sceGeEdramGetAddrPatched(void) {
  return (void *)FAKE_VRAM;
}

unsigned int sceGeEdramGetSizePatched(void) {
  return 4 * 1024 * 1024;
}

int sceGeListUpdateStallAddrPatched(int qid, void *stall)//关键变化 → 必执行，解决50%调用被随机丢弃的问题，原来函数太简单
{
  int k1 = pspSdkSetK1(0);

  // 👉 用 qid 做索引（PSP GE list 数量不大）
  static void *last_stall[128] = {0};

  void *prev = last_stall[qid];

  // =========================================================
  // 🚀 1. 完全重复 → 直接跳过（最高收益）
  // =========================================================
  if (prev == stall) {
    pspSdkSetK1(k1);
    return 0; // 不调用底层
  }

  // =========================================================
  // 🚀 2. 小幅变化 → 合并（核心优化）
  // =========================================================
  if (prev != NULL) {
    u32 p = (u32)prev & 0x0FFFFFFF;
    u32 s = (u32)stall & 0x0FFFFFFF;

    // 👉 差距太小（例如 < 64 字节），认为是“抖动更新”
    // if ((u32)(s - p) <128) {//修改
      // 不更新，等更大的推进
      pspSdkSetK1(k1);
      return 0;
    // }
  }

  // =========================================================
  // 🚀 3. 记录新状态
  // =========================================================
  last_stall[qid] = stall;

  // =========================================================
  // 🚀 4. 调用原函数（必须！）
  // =========================================================
  int res = _sceGeListUpdateStallAddr(qid, stall);

  pspSdkSetK1(k1);
  return res;
}


int sceGeListEnQueuePatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {//改动3
  u32 list_addr = (u32)list & 0x0fffffff;

  // ✅ 1. 避免重复 patch 同一个 list
  if (list_addr != last_list) {
    resetGeState();

    patchGeList(
      (u32 *)list_addr,
      (u32 *)((u32)stall & 0x0fffffff)
    );

    // ✅ 2. 只在真正修改时刷新 cache（先简化为局部）
   sceKernelDcacheWritebackInvalidateAll();

    last_list = list_addr;
  }

  return _sceGeListEnQueue(list, stall, cbid, arg);
}

int sceGeListEnQueueHeadPatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  sceKernelDcacheWritebackInvalidateAll();
  return _sceGeListEnQueueHead(list, stall, cbid, arg);
}

int sceGeListSyncPatched(int qid, int syncType) {
  tryFrameCopy();
  return _sceGeListSync(qid, syncType);
}

void copyFrameBuffer()
{
  if (!fb_dirty)
    return;

  // ❗ 不建议提前清（改为尾部清）
  sceGuStart(GU_DIRECT, (void *)(RENDER_LIST | 0xA0000000));

  sceGuCopyImage(
    PIXELFORMAT,
    dirty_x, dirty_y,
    dirty_w, dirty_h,
    PITCH,
    (void *)VRAM_DRAW_BUFFER_OFFSET,
    dirty_x, dirty_y,
    PITCH,
    (void *)DISPLAY_BUFFER
  );

  sceGuFinish();

  // ✔ 放最后更安全
  fb_dirty = 0;
}


int sceGeDrawSyncPatched(int syncType)
{
  int res = _sceGeDrawSync(syncType);
  if (!framebuf_set) {
    if (syncType == PSP_GE_LIST_DONE ||
        syncType == PSP_GE_LIST_DRAWING_DONE) {
      copyFrameBuffer();
      rendered_in_sync = 1;
    }
  }
  if (framebuf_set > 0)
    framebuf_set--;
  return res;
}

int sceDisplaySetFrameBufPatched(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
  if (topaddr != last_fb) {
    last_fb = topaddr;
    fb_dirty = 1;
    fb_pending = 1;
  }

  return _sceDisplaySetFrameBuf(
    (void *)VRAM_DRAW_BUFFER_OFFSET,
    PITCH,
    PIXELFORMAT,
    sync
  );
}

// int draw_thread(SceSize args, void *argp) {
//   while (1) {
//     _sceGeDrawSync(0);
//     copyFrameBuffer();
//     sceKernelDelayThread(30 * 1000);
//   }

//   return 0;
// }删除节拍器

int module_start(SceSize args, void *argp) {
  _sceGeEdramGetAddr = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0xE47E40E4);
  _sceGeEdramGetSize = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0x1F6752AD);
  _sceGeGetList = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0x67B01D8E);
  _sceGeListUpdateStallAddr = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0xE0D68148);
  _sceGeListEnQueue = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0xAB49E76A);
  _sceGeListEnQueueHead = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0x1C0D95A6);
  _sceGeListSync = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0x03444EB4);
  _sceGeDrawSync = (void *)FindProc("sceGE_Manager", "sceGe_driver", 0xB287BD61);

  sctrlHENPatchSyscall((u32)_sceGeEdramGetAddr, sceGeEdramGetAddrPatched);
  sctrlHENPatchSyscall((u32)_sceGeEdramGetSize, sceGeEdramGetSizePatched);
  sctrlHENPatchSyscall((u32)_sceGeListUpdateStallAddr, sceGeListUpdateStallAddrPatched);
  sctrlHENPatchSyscall((u32)_sceGeListEnQueue, sceGeListEnQueuePatched);
  sctrlHENPatchSyscall((u32)_sceGeListEnQueueHead, sceGeListEnQueueHeadPatched);
  // sctrlHENPatchSyscall((u32)_sceGeListSync, sceGeListSyncPatched);
  sctrlHENPatchSyscall((u32)_sceGeDrawSync, sceGeDrawSyncPatched);

  _sceDisplaySetFrameBuf = (void *)FindProc("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
  sctrlHENPatchSyscall((u32)_sceDisplaySetFrameBuf, sceDisplaySetFrameBufPatched);

  // SceUID thid = sceKernelCreateThread("draw_thread", draw_thread, 0x11, 0x4000, 0, NULL);
  // if (thid >= 0)
    // sceKernelStartThread(thid, 0, NULL);

  sceKernelDcacheWritebackInvalidateAll();
  sceKernelIcacheClearAll();

  return 0;
}
