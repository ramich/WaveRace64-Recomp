/**
 * @file rsp_microcode.cpp
 * @brief RSP microcode dispatch implementation for WaveRace64-Recomp.
 *
 * Wave Race 64 uses two RSP task types:
 *   - M_GFXTASK (1): Graphics display list processing (F3DEX / Fast3D variant)
 *   - M_AUDTASK (2): Audio processing
 *
 * The graphics tasks are handled by RT64 through the HLE renderer (the display
 * list is submitted via RendererContext::send_dl, so we don't need a separate
 * RSP microcode for GFX tasks). Audio RSP tasks use the N64 audio microcode.
 *
 * This callback is invoked by librecomp's RSP task runner to determine which
 * recompiled microcode function should handle a given task.
 */

#include "rsp_microcode.h"

#include <cstdio>
#include "ultramodern/ultra64.h"

// N64 audio microcode, statically recompiled from the ROM by RSPRecomp
// (recomp/aspMain.us.rev1.toml -> rsp/aspMain.cpp).
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace wr64 {

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    // OSTask type field at task->t.type
    switch (task->t.type) {
        case M_GFXTASK:
            // Graphics tasks are handled via HLE by RT64.
            // Return nullptr to signal that the RSP runner should skip this task
            // (it will be picked up by the renderer thread via send_dl instead).
            return nullptr;

        case M_AUDTASK: {
            // Log the task layout once so the ucode's ROM location can be found
            // (needed to set up RSPRecomp for real audio processing).
            static bool logged = false;
            if (!logged) {
                logged = true;
                fprintf(stderr,
                    "[WR64-RSP] AUDTASK: ucode=0x%08X ucode_size=0x%X ucode_data=0x%08X ucode_data_size=0x%X data=0x%08X data_size=0x%X\n",
                    (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_size,
                    (uint32_t)task->t.ucode_data, (uint32_t)task->t.ucode_data_size,
                    (uint32_t)task->t.data_ptr, (uint32_t)task->t.data_size);
            }
            // Audio microcode — statically recompiled from this ROM.
            return aspMain;
        }

        default:
            fprintf(stderr, "[WR64-RSP] Unknown RSP task type: %d (ucode_boot=0x%08X)\n",
                    task->t.type, task->t.ucode_boot);
            return nullptr;
    }
}

} // namespace wr64
