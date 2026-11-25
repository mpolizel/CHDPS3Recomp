/*
 * port_resc.c - cellResc overrides for Castlevania: Harmony of Despair.
 *
 * The game renders each frame into an off-screen GCM surface and presents it
 * through RESC: cellRescGcmSurface2RescSrc -> cellRescSetSrc ->
 * cellRescSetConvertAndFlip. RESC itself owns the display buffers, so the game
 * never calls cellGcmSetDisplayBuffer. The stock HLE in ext/ps3recomp gets
 * three things wrong for that flow:
 *
 *   1. cellRescGcmSurface2RescSrc and cellRescVideoOutResolutionId2RescBufferMode
 *      are not implemented, so the CellRescSrc the game hands to SetSrc is
 *      uninitialized stack memory.
 *   2. cellRescSetConvertAndFlip reads the source index from r3, but the SDK
 *      signature is (CellGcmContextData* con, int32_t idx): the index is in r4.
 *   3. It flips through cellGcm's display buffers, and there are none.
 *
 * What this does instead:
 *   - SetBufferAddress registers RESC's own display buffers (two 1280x720
 *     buffers inside the colorBuf the game hands over), as real RESC does, so
 *     GCM has something to flip to. Nothing renders into them.
 *   - The source surface stays an offscreen render target and is made the
 *     D3D12 backend's persistent scanout: each flip composites it onto the
 *     backbuffer, scaled (composite_scaled in
 *     ext/ps3recomp/libs/video/rsx_d3d12_backend.c). That is RESC's
 *     scale/convert pass.
 *   - SetConvertAndFlip queues the flip into the game's command buffer (con),
 *     as real RESC does, so it lands after the frame's draws. Flipping from
 *     the PPU at call time presented half-drawn frames.
 */

#include <stdio.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/nid.h"
#include "ppu_context.h"
#include "ppu_memory.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch, u32 width, u32 height);
extern s32 cellGcmAddressToOffset(u32 address, u32* offset);
extern s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId);
extern void cellGcmSetWaitFlip(void);
extern void rsx_d3d12_set_scanout_offscreen(u32 raw_off);

#define RESC_DISPLAY_BUFFERS 2
#define RESC_DISPLAY_W       1280
#define RESC_DISPLAY_H       720
#define RESC_DISPLAY_PITCH   (RESC_DISPLAY_W * 4)

#define RESC_ERROR_BAD_ARGUMENT 0x80210303u
#define RESC_MAX_SRC 8

/* SDK CellRescBufferMode values */
#define RESC_720x480   0x01
#define RESC_720x576   0x02
#define RESC_1280x720  0x04
#define RESC_1920x1080 0x08

/* SDK CellGcmSurface layout (big-endian guest struct). */
#define SURF_TYPE           0
#define SURF_ANTIALIAS      1
#define SURF_COLOR_FORMAT   2
#define SURF_COLOR_OFFSET0  8
#define SURF_COLOR_PITCH0   24
#define SURF_WIDTH          52
#define SURF_HEIGHT         54

/* SDK CellRescSrc layout: format u32, pitch u32, width u16, height u16, offset u32. */
typedef struct { u32 format, pitch, width, height, offset; int valid; } resc_src;

static resc_src s_src[RESC_MAX_SRC];
static u32 s_flip_handler_opd;
static u32 s_display_count;
static u32 s_flip_target;

/* CELL_GCM_SURFACE_* color format -> CELL_GCM_TEXTURE_* format, as RESC does. */
static u32 surface_to_texture_format(u8 color_format, u8 type)
{
    u32 fmt;
    switch (color_format) {
    case 0x03: fmt = 0x84; break;   /* R5G6B5        -> R5G6B5 */
    case 0x0B: fmt = 0x9A; break;   /* F_W16Z16Y16X16 -> W16_Z16_Y16_X16_FLOAT */
    case 0x0C: fmt = 0x9B; break;   /* F_W32Z32Y32X32 -> W32_Z32_Y32_X32_FLOAT */
    case 0x0D: fmt = 0x91; break;   /* F_X32         -> X32_FLOAT */
    default:   fmt = 0x85; break;   /* X8R8G8B8 / A8R8G8B8 family -> A8R8G8B8 */
    }
    if (type == 1) fmt |= 0x20;     /* CELL_GCM_SURFACE_PITCH -> CELL_GCM_TEXTURE_LN */
    return fmt;
}

/* int32_t cellRescGcmSurface2RescSrc(const CellGcmSurface* gcmSurface, CellRescSrc* rescSrc) */
static void resc_gcm_surface_to_src(ppu_context* ctx)
{
    const u32 surf = (u32)ctx->gpr[3];
    const u32 out  = (u32)ctx->gpr[4];
    if (!surf || !out) { ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT; return; }

    const u8 type = vm_read8(surf + SURF_TYPE);
    const u8 aa   = vm_read8(surf + SURF_ANTIALIAS);
    /* CELL_GCM_SURFACE_*: an antialiased surface is stored at sample resolution. */
    u32 xw = 1, xh = 1;
    if (aa == 3)                { xw = 2; }          /* DIAGONAL_CENTERED_2: 2x1 */
    else if (aa == 4 || aa == 5) { xw = 2; xh = 2; } /* SQUARE_CENTERED_4 / SQUARE_ROTATED_4: 2x2 */
    printf("[port_resc] GcmSurface2RescSrc: %ux%u aa=%u type=%u offset=0x%X\n",
           vm_read16(surf + SURF_WIDTH), vm_read16(surf + SURF_HEIGHT), aa, type,
           vm_read32(surf + SURF_COLOR_OFFSET0));

    vm_write32(out + 0,  surface_to_texture_format(vm_read8(surf + SURF_COLOR_FORMAT), type));
    vm_write32(out + 4,  vm_read32(surf + SURF_COLOR_PITCH0));
    vm_write16(out + 8,  (u16)(vm_read16(surf + SURF_WIDTH) * xw));
    vm_write16(out + 10, (u16)(vm_read16(surf + SURF_HEIGHT) * xh));
    vm_write32(out + 12, vm_read32(surf + SURF_COLOR_OFFSET0));
    ctx->gpr[3] = CELL_OK;
}

/* int32_t cellRescVideoOutResolutionId2RescBufferMode(CellVideoOutResolutionId id, uint32_t* mode) */
static void resc_resolution_to_buffer_mode(ppu_context* ctx)
{
    const u32 id  = (u32)ctx->gpr[3];
    const u32 out = (u32)ctx->gpr[4];
    u32 mode;
    switch (id) {
    case 1: mode = RESC_1920x1080; break;
    case 2: mode = RESC_1280x720;  break;
    case 4: mode = RESC_720x480;   break;
    case 5: mode = RESC_720x576;   break;
    default: ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT; return;
    }
    if (!out) { ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT; return; }
    vm_write32(out, mode);
    ctx->gpr[3] = CELL_OK;
}

/* int32_t cellRescSetSrc(int32_t idx, const CellRescSrc* src) */
static void resc_set_src(ppu_context* ctx)
{
    const s32 idx = (s32)ctx->gpr[3];
    const u32 src = (u32)ctx->gpr[4];
    if (idx < 0 || idx >= RESC_MAX_SRC || !src) { ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT; return; }

    resc_src* s = &s_src[idx];
    s->format = vm_read32(src + 0);
    s->pitch  = vm_read32(src + 4);
    s->width  = vm_read16(src + 8);
    s->height = vm_read16(src + 10);
    s->offset = vm_read32(src + 12);
    s->valid  = 1;
    printf("[port_resc] SetSrc(%d): offset=0x%X pitch=%u %ux%u fmt=0x%X\n",
           idx, s->offset, s->pitch, s->width, s->height, s->format);
    if (idx == 0) rsx_d3d12_set_scanout_offscreen(s->offset);
    ctx->gpr[3] = CELL_OK;
}

/* int32_t cellRescSetBufferAddress(void* colorBuffers, void* vertexArray, void* fragmentShader) */
static void resc_set_buffer_address(ppu_context* ctx)
{
    const u32 color_ea = (u32)ctx->gpr[3];
    if (!color_ea) { ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT; return; }

    /* cellGcmAddressToOffset writes its answer to a guest address; the first
     * word of colorBuf is RESC's own memory and free to use as that scratch. */
    u32 off = 0;
    if (cellGcmAddressToOffset(color_ea, (u32*)(uintptr_t)color_ea) == CELL_OK)
        off = vm_read32(color_ea);
    for (u32 i = 0; i < RESC_DISPLAY_BUFFERS; i++)
        cellGcmSetDisplayBuffer(i, off + i * RESC_DISPLAY_PITCH * RESC_DISPLAY_H,
                                RESC_DISPLAY_PITCH, RESC_DISPLAY_W, RESC_DISPLAY_H);
    s_display_count = RESC_DISPLAY_BUFFERS;
    printf("[port_resc] SetBufferAddress: colorBuf ea=0x%08X offset=0x%X, %u display buffers\n",
           color_ea, off, s_display_count);
    ctx->gpr[3] = CELL_OK;
}

/* int32_t cellRescSetConvertAndFlip(CellGcmContextData* con, int32_t idx) */
static void resc_set_convert_and_flip(ppu_context* ctx)
{
    const s32 idx = (s32)ctx->gpr[4];
    if (idx < 0 || idx >= RESC_MAX_SRC || !s_src[idx].valid || !s_display_count) {
        static int warned;
        if (!warned++) printf("[port_resc] SetConvertAndFlip: bad source index %d (displays=%u)\n",
                              idx, s_display_count);
        ctx->gpr[3] = RESC_ERROR_BAD_ARGUMENT;
        return;
    }
    const u32 con = (u32)ctx->gpr[3];
    rsx_d3d12_set_scanout_offscreen(s_src[idx].offset);
    /* Queued into the game's command buffer, as real RESC does. A direct
     * cellGcmSetFlipCommand here left the game waiting at its boot screens. */
    _cellGcmSetFlipCommand((void*)(uintptr_t)con, s_flip_target);
    s_flip_target = (s_flip_target + 1) % s_display_count;
    if (s_flip_handler_opd)
        ps3_invoke_guest(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
    ctx->gpr[3] = CELL_OK;
}

/* void cellRescSetFlipHandler(void (*handler)(uint32_t head)) */
static void resc_set_flip_handler(ppu_context* ctx)
{
    s_flip_handler_opd = (u32)ctx->gpr[3];
    ctx->gpr[3] = CELL_OK;
}

/* void cellRescSetWaitFlip(CellGcmContextData* con): wait for the queued flip
 * to be shown, as libgcm's WaitFlip does. As a no-op the game ran unthrottled,
 * the FIFO piled up hundreds of KB past its flips and the drain ran across
 * frame boundaries, so a present showed the next frame half drawn: the HUD,
 * the pause menu and the player flickered from the lobby on. */
static void resc_set_wait_flip(ppu_context* ctx)
{
    cellGcmSetWaitFlip();
    ctx->gpr[3] = CELL_OK;
}

void port_resc_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellRescGcmSurface2RescSrc"), "cellRescGcmSurface2RescSrc", resc_gcm_surface_to_src);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescVideoOutResolutionId2RescBufferMode"), "cellRescVideoOutResolutionId2RescBufferMode", resc_resolution_to_buffer_mode);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescSetSrc"), "cellRescSetSrc", resc_set_src);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescSetBufferAddress"), "cellRescSetBufferAddress", resc_set_buffer_address);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescSetConvertAndFlip"), "cellRescSetConvertAndFlip", resc_set_convert_and_flip);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescSetFlipHandler"), "cellRescSetFlipHandler", resc_set_flip_handler);
    ps3_hle_register_ctx(ps3_compute_nid("cellRescSetWaitFlip"), "cellRescSetWaitFlip", resc_set_wait_flip);
}
