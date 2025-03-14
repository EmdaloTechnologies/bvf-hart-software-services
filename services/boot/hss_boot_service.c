/*******************************************************************************
 * Copyright 2019-2025 Microchip FPGA Embedded Systems Solutions.
 *
 * SPDX-License-Identifier: MIT
 *
 * MPFS HSS Embedded Software
 *
 */

/*!
 * \file  Boot Service State Machine
 * \brief Boot Service
 */


#include "config.h"
#include "hss_types.h"
#include "hss_state_machine.h"
#include "hss_boot_service.h"
#include "opensbi_service.h"
#include "ddr_service.h"
#include "hss_boot_pmp.h"
#include "hss_sys_setup.h"
#include "hss_clock.h"
#include "hss_debug.h"
#include "hss_perfctr.h"
#include "common/mss_peripherals.h"
#include "hss_crc32.h"
#include "u54_state.h"
#include "hss_trigger.h"
#include "hss_boot_init.h"

#include <assert.h>
#include <string.h>
#include "riscv_atomic.h"

#include "mpfs_reg_map.h"

#if IS_ENABLED(CONFIG_OPENSBI)
#  include "sbi/riscv_asm.h"
#  define ffs SBI_FFS
#  define fls SBI_FLS
#  include "sbi/sbi_bitops.h"
#  undef ffs
#  undef fls
#  include "sbi/sbi_hart.h"
#  include "sbi_init.h"
#  include "sbi_scratch.h"
#  include "sbi_types.h"
#  include "sbi_platform.h"
#else
#  ifdef __riscv
#    include <machine/mtrap.h>
#    include <machine/encoding.h>
#  endif
#endif

#include "hss_atomic.h"

#include "mss_sysreg.h"

#include "hss_memcpy_via_pdma.h"
#include "system_startup.h"
#include "fpga_design_config/fpga_design_config.h"

#if IS_ENABLED(CONFIG_CRYPTO_SIGNING)
#  include "hss_boot_secure.h"
#endif

#if IS_ENABLED(CONFIG_SERVICE_OPENSBI_RPROC)
#  include "opensbi_rproc_ecall.h"
#endif

#if IS_ENABLED(CONFIG_SERVICE_GPIO_UI)
#  include "gpio_ui_service.h"
#endif

/* Timeouts */
#define BOOT_SETUP_PMP_COMPLETE_TIMEOUT (ONE_SEC * 1u)
#define BOOT_WAIT_TIMEOUT               (ONE_SEC * 5u)

#define BOOT_SUB_CHUNK_SIZE 256u

/*
 * Module Prototypes (states)
 */
static void boot_init_handler(struct StateMachine * const pMyMachine);
static void boot_setup_pmp_onEntry(struct StateMachine * const pMyMachine);
static void boot_setup_pmp_handler(struct StateMachine * const pMyMachine);
static void boot_setup_pmp_complete_onEntry(struct StateMachine * const pMyMachine);
static void boot_setup_pmp_complete_handler(struct StateMachine * const pMyMachine);
static void boot_zero_init_chunks_onEntry(struct StateMachine * const pMyMachine);
static void boot_zero_init_chunks_handler(struct StateMachine * const pMyMachine);
static void boot_download_chunks_onEntry(struct StateMachine * const pMyMachine);
static void boot_download_chunks_handler(struct StateMachine * const pMyMachine);
static void boot_download_chunks_onExit(struct StateMachine * const pMyMachine);
static void boot_opensbi_init_onEntry(struct StateMachine * const pMyMachine);
static void boot_opensbi_init_handler(struct StateMachine * const pMyMachine);
static void boot_opensbi_init_onExit(struct StateMachine * const pMyMachine);
static void boot_wait_onEntry(struct StateMachine * const pMyMachine);
static void boot_wait_handler(struct StateMachine * const pMyMachine);
static void boot_error_handler(struct StateMachine * const pMyMachine);
static void boot_complete_onEntry(struct StateMachine * const pMyMachine);
static void boot_complete_handler(struct StateMachine * const pMyMachine);
static void boot_idle_onEntry(struct StateMachine * const pMyMachine);
static void boot_idle_handler(struct StateMachine * const pMyMachine);

static void boot_do_download_chunk(struct HSS_BootChunkDesc const *pChunk,
    ptrdiff_t subChunkOffset, size_t subChunkSize);
static void boot_do_zero_init_chunk(struct HSS_BootZIChunkDesc const *pZiChunk);

static bool validateCrc_(struct HSS_BootImage *pImage);

/*!
 * \brief Boot Driver States
 *
 */
enum BootStatesEnum {
    BOOT_INITIALIZATION,
    BOOT_SETUP_PMP,
    BOOT_SETUP_PMP_COMPLETE,
    BOOT_ZERO_INIT_CHUNKS,
    BOOT_DOWNLOAD_CHUNKS,
    BOOT_OPENSBI_INIT,
    BOOT_WAIT,
    BOOT_COMPLETE,
    BOOT_IDLE,
    BOOT_ERROR,
    BOOT_NUM_STATES = BOOT_ERROR+1
};

/*!
 * \brief Boot Driver State Descriptors
 *
 */
static const struct StateDesc boot_state_descs[] = {
    { (const stateType_t)BOOT_INITIALIZATION,     (const char *)"Init",             NULL,                             NULL,                         &boot_init_handler },
    { (const stateType_t)BOOT_SETUP_PMP,          (const char *)"SetupPMP",         &boot_setup_pmp_onEntry,          NULL,                         &boot_setup_pmp_handler },
    { (const stateType_t)BOOT_SETUP_PMP_COMPLETE, (const char *)"SetupPMPComplete", &boot_setup_pmp_complete_onEntry, NULL,                         &boot_setup_pmp_complete_handler },
    { (const stateType_t)BOOT_ZERO_INIT_CHUNKS,   (const char *)"ZeroInit",         &boot_zero_init_chunks_onEntry,   NULL,                         &boot_zero_init_chunks_handler },
    { (const stateType_t)BOOT_DOWNLOAD_CHUNKS,    (const char *)"Download",         &boot_download_chunks_onEntry,    &boot_download_chunks_onExit, &boot_download_chunks_handler },
    { (const stateType_t)BOOT_OPENSBI_INIT,       (const char *)"OpenSBIInit",      &boot_opensbi_init_onEntry,       &boot_opensbi_init_onExit,    &boot_opensbi_init_handler },
    { (const stateType_t)BOOT_WAIT,               (const char *)"Wait",             &boot_wait_onEntry,               NULL,                         &boot_wait_handler },
    { (const stateType_t)BOOT_COMPLETE,           (const char *)"Complete",         &boot_complete_onEntry,           NULL,                         &boot_complete_handler },
    { (const stateType_t)BOOT_IDLE,               (const char *)"Idle",             &boot_idle_onEntry,               NULL,                         &boot_idle_handler },
    { (const stateType_t)BOOT_ERROR,              (const char *)"Error",            NULL,                             NULL,                         &boot_error_handler } };

/*!
 * \brief Boot Driver State Machine
 *
 */
struct HSS_Boot_LocalData {
    const enum HSSHartId target;
    struct HSS_BootChunkDesc const *pChunk;
    struct HSS_BootZIChunkDesc const *pZiChunk;
    size_t chunkCount;
    size_t ziChunkCount;
    size_t subChunkOffset;
    uint32_t msgIndex;
    uint32_t hartMask;
    int perfCtr;
    unsigned int iterator;
    uintptr_t ancilliaryData;
    uint32_t msgIndexAux[MAX_NUM_HARTS-1];
};


static struct HSS_Boot_LocalData localData[MAX_NUM_HARTS-1] = {
    { HSS_HART_U54_1, NULL, NULL, 0u, 0u, 0u, IPI_MAX_NUM_OUTSTANDING_COMPLETES, 0u, PERF_CTR_UNINITIALIZED, 0u, 0u, { 0u, 0u, 0u, 0u } },
    { HSS_HART_U54_2, NULL, NULL, 0u, 0u, 0u, IPI_MAX_NUM_OUTSTANDING_COMPLETES, 0u, PERF_CTR_UNINITIALIZED, 0u, 0u, { 0u, 0u, 0u, 0u } },
    { HSS_HART_U54_3, NULL, NULL, 0u, 0u, 0u, IPI_MAX_NUM_OUTSTANDING_COMPLETES, 0u, PERF_CTR_UNINITIALIZED, 0u, 0u, { 0u, 0u, 0u, 0u } },
    { HSS_HART_U54_4, NULL, NULL, 0u, 0u, 0u, IPI_MAX_NUM_OUTSTANDING_COMPLETES, 0u, PERF_CTR_UNINITIALIZED, 0u, 0u, { 0u, 0u, 0u, 0u } },
};

struct HSS_BootImage *pBootImage = NULL;
static bool pmpSetupFlag[HSS_HART_NUM_PEERS] = { false, false, false, false, false };

/*
 * individual boot machines, one per U54 hart
 */
struct StateMachine boot_service1 = {
    .state             = (stateType_t)BOOT_IDLE,
    .prevState         = (stateType_t)SM_INVALID_STATE,
    .numStates         = (const uint32_t)BOOT_NUM_STATES,
    .pMachineName      = (const char *)"boot_service(u54_1)",
    .startTime         = 0u,
    .lastExecutionTime = 0u,
    .executionCount    = 0u,
    .pStateDescs       = boot_state_descs,
    .debugFlag         = true,
    .priority          = 0u,
    .pInstanceData     = (void *)&localData[0]
};

struct StateMachine boot_service2 = {
    .state             = (stateType_t)BOOT_IDLE,
    .prevState         = (stateType_t)SM_INVALID_STATE,
    .numStates         = (const uint32_t)BOOT_NUM_STATES,
    .pMachineName      = (const char *)"boot_service(u54_2)",
    .startTime         = 0u,
    .lastExecutionTime = 0u,
    .executionCount    = 0u,
    .pStateDescs       = boot_state_descs,
    .debugFlag         = true,
    .priority          = 0u,
    .pInstanceData     = (void *)&localData[1]
};

struct StateMachine boot_service3 = {
    .state             = (stateType_t)BOOT_IDLE,
    .prevState         = (stateType_t)SM_INVALID_STATE,
    .numStates         = (const uint32_t)BOOT_NUM_STATES,
    .pMachineName      = (const char *)"boot_service(u54_3)",
    .startTime         = 0u,
    .lastExecutionTime = 0u,
    .executionCount    = 0u,
    .pStateDescs       = boot_state_descs,
    .debugFlag         = true,
    .priority          = 0u,
    .pInstanceData     = (void *)&localData[2]
};

struct StateMachine boot_service4 = {
    .state             = (stateType_t)BOOT_IDLE,
    .prevState         = (stateType_t)SM_INVALID_STATE,
    .numStates         = (const uint32_t)BOOT_NUM_STATES,
    .pMachineName      = (const char *)"boot_service(u54_4)",
    .startTime         = 0u,
    .lastExecutionTime = 0u,
    .executionCount    = 0u,
    .pStateDescs       = boot_state_descs,
    .debugFlag         = true,
    .priority          = 0u,
    .pInstanceData     = (void *)&localData[3]
};

/*
 * table of individual boot machines, for convenience
 */
const struct {
    struct StateMachine* const pMachine;
    const enum HSSHartId hartId;
} bootMachine[] = {
    { &boot_service1, HSS_HART_U54_1 },
    { &boot_service2, HSS_HART_U54_2 },
    { &boot_service3, HSS_HART_U54_3 },
    { &boot_service4, HSS_HART_U54_4 },
};

// ----------------------------------------------------------------------------
/*!
 * \brief Helper Functions
 */

/*!
 * \brief Download Chunks
 *
 * The E51 places chunks from the boot image to their correct location in
 * physical system memory *before* releasing the U54s from WFI.
 *
 * \warning It is vitally important that we permissions check the bounds of each of
 * these reads/writes w.r.t. the U54 that owns it to make sure that is a permitted
 * write. Otherwise, a boot image could be constructed to circumvent these
 * protections.
 *
 * This checks are done outside this function.
 *
 */
static void boot_do_download_chunk(struct HSS_BootChunkDesc const *pChunk, ptrdiff_t subChunkOffset,
    size_t subChunkSize)
{
    assert(pChunk);
    assert(pChunk->size);

    const uintptr_t execAddr = (uintptr_t)pChunk->execAddr + subChunkOffset;
    const uintptr_t loadAddr = (uintptr_t)pBootImage + (uintptr_t)pChunk->loadAddr + subChunkOffset;
    memcpy_via_pdma((void *)execAddr, (void*)loadAddr, subChunkSize);
}

static void boot_do_zero_init_chunk(struct HSS_BootZIChunkDesc const *pZiChunk)
{
    assert(pZiChunk);

    const uintptr_t execAddr = (uintptr_t)pZiChunk->execAddr;
    const size_t ziChunkSize = pZiChunk->size;

    memset((void *)execAddr, 0, ziChunkSize);
}

static void free_msg_index(struct HSS_Boot_LocalData * const pInstanceData)
{
    if (pInstanceData->msgIndex != IPI_MAX_NUM_OUTSTANDING_COMPLETES) {
        IPI_MessageFree(pInstanceData->msgIndex);
        pInstanceData->msgIndex = IPI_MAX_NUM_OUTSTANDING_COMPLETES;
    }
}

static void free_msg_index_aux(struct HSS_Boot_LocalData * const pInstanceData, enum HSSHartId peer)
{
    if (pInstanceData->msgIndexAux[peer-1] != IPI_MAX_NUM_OUTSTANDING_COMPLETES) {
        IPI_MessageFree(pInstanceData->msgIndexAux[peer-1]);
        pInstanceData->msgIndexAux[peer-1] = IPI_MAX_NUM_OUTSTANDING_COMPLETES;
    }
}


static bool check_for_ipi_acks(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    bool result = true;

    for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
        enum HSSHartId const peer = bootMachine[i].hartId;
        if (pInstanceData->msgIndexAux[peer-1] != IPI_MAX_NUM_OUTSTANDING_COMPLETES) {
            result = IPI_MessageCheckIfComplete(pInstanceData->msgIndexAux[peer-1]);

            if (result) {
                free_msg_index_aux(pInstanceData, peer);
            }
        }
    }

    if (pInstanceData->msgIndex != IPI_MAX_NUM_OUTSTANDING_COMPLETES) {
        result = result & IPI_MessageCheckIfComplete(pInstanceData->msgIndex);

        if (result) {
            free_msg_index(pInstanceData);
        }
    }

    return result;
}


// --------------------------------------------------------------------------------------------------
// Handlers for each state in the state machine
//
static void boot_init_handler(struct StateMachine * const pMyMachine)
{
    if (HSS_Trigger_IsNotified(EVENT_DDR_TRAINED) && HSS_Trigger_IsNotified(EVENT_STARTUP_COMPLETE)) {
        if (pBootImage) {
            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::\tstarting boot\n", pMyMachine->pMachineName);
            SYSREG->BOOT_FAIL_CR = 0;

            pMyMachine->startTime = HSS_GetTime();
            struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
            enum HSSHartId const target = pInstanceData->target;

            if (pBootImage->hart[target-1].flags & BOOT_FLAG_SKIP_OPENSBI) {
	           mHSS_DEBUG_PRINTF(LOG_STATUS, "%s:: BOOT_FLAG_SKIP_OPENSBI found\n", pMyMachine->pMachineName);
            }

            HSS_PerfCtr_Allocate(&pInstanceData->perfCtr, pMyMachine->pMachineName);
            pMyMachine->state = BOOT_SETUP_PMP;
        } else {
            // unexpected error state
            if (!pBootImage) {
                mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::\tNo Boot Image registered\n", pMyMachine->pMachineName);
            }
            pMyMachine->state = BOOT_ERROR;
        }
    }
}

/////////////////

static void register_harts(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    pInstanceData->msgIndex = IPI_MAX_NUM_OUTSTANDING_COMPLETES;

    bool const primary_boot_hart = (pBootImage->hart[target-1].numChunks) && (pBootImage->hart[target-1].entryPoint);

    for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
        enum HSSHartId peer = bootMachine[i].hartId;
        pInstanceData->msgIndexAux[peer-1] = IPI_MAX_NUM_OUTSTANDING_COMPLETES;

        if (primary_boot_hart) {
            if (pBootImage->hart[peer-1].flags & BOOT_FLAG_SKIP_OPENSBI) {
                // skipping OpenSBI => don't register as a hart
                mpfs_domains_deregister_hart(peer);
            } else if ((peer == target) ||
                (pBootImage->hart[peer-1].entryPoint == pBootImage->hart[target-1].entryPoint)) {
                pInstanceData->hartMask |= (1u << peer);
                //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Registering u54_%d to domain \"%s\"\n",
                //    pMyMachine->pMachineName, peer, pBootImage->hart[target-1].name);
                mpfs_domains_register_hart(peer, target);
            }
        }
    }

    if (primary_boot_hart) {
        if (pBootImage->hart[target-1].flags & BOOT_FLAG_SKIP_OPENSBI) {
            // skipping OpenSBI => don't register as a domain
        } else {
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Registering domain \"%s\" (hart mask 0x%x)\n",
                pMyMachine->pMachineName, pBootImage->hart[target-1].name, pInstanceData->hartMask);

            void *pArg1 = NULL;

            if (pInstanceData->ancilliaryData) {
                pArg1 = (void *)pInstanceData->ancilliaryData;
#if IS_ENABLED(CONFIG_PROVIDE_DTB)
            } else {
                extern unsigned long _binary_build_services_opensbi_mpfs_dtb_start;
                pArg1 = (void *)&_binary_build_services_opensbi_mpfs_dtb_start;
                mHSS_DEBUG_PRINTF(LOG_WARN, "%s::Using built-in DTB at 0x%p\n",
                    pMyMachine->pMachineName, pArg1);
#endif
            }

            mpfs_domains_register_boot_hart(pBootImage->hart[target-1].name,
                pInstanceData->hartMask, target,
                pBootImage->hart[target-1].privMode,
                (void *)pBootImage->hart[target-1].entryPoint, pArg1,
		pBootImage->hart[target-1].flags & BOOT_FLAG_ALLOW_COLD_REBOOT,
		pBootImage->hart[target-1].flags & BOOT_FLAG_ALLOW_WARM_REBOOT);
        }
    }
}

static void boot_setup_pmp_onEntry(struct StateMachine * const pMyMachine)
{
    /* Initially register harts, so that IPIs work for remainder of boot */
    register_harts(pMyMachine);
}

static void boot_setup_pmp_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    bool result = false;

    if (pInstanceData->msgIndex == IPI_MAX_NUM_OUTSTANDING_COMPLETES) {
        result = HSS_Boot_PMPSetupRequest(target, &(pInstanceData->msgIndex));
    } else {
        result = true;
    }

    if (result) {
        pMyMachine->state = BOOT_SETUP_PMP_COMPLETE;
    }
}

static void boot_setup_pmp_complete_onEntry(struct StateMachine * const pMyMachine)
{
    //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Checking for IPI ACKs: - -\n", pMyMachine->pMachineName);
}

static void boot_setup_pmp_complete_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    if (HSS_Timer_IsElapsed(pMyMachine->startTime, BOOT_SETUP_PMP_COMPLETE_TIMEOUT)) {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::Timeout after %" PRIu64 " iterations\n",
            pMyMachine->pMachineName, pMyMachine->executionCount);

        for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
            enum HSSHartId peer = bootMachine[i].hartId;
            free_msg_index_aux(pInstanceData, peer);
        }

        free_msg_index(pInstanceData);

        pMyMachine->state = BOOT_ERROR;
    } else {
        // need to free as received, not all at once...

        if (check_for_ipi_acks(pMyMachine)) {
            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Checking for IPI ACKs: ACK/IDLE ACK\n", pMyMachine->pMachineName);
            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::PMP setup completed\n", pMyMachine->pMachineName);

        if (pBootImage->hart[target-1].flags & BOOT_FLAG_SKIP_AUTOBOOT)
            pMyMachine->state = BOOT_COMPLETE;
        else
            pMyMachine->state = BOOT_ZERO_INIT_CHUNKS;
        }
    }
}

/////////////////

static void boot_zero_init_chunks_onEntry(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;

    assert(pBootImage != NULL);

    pInstanceData->pZiChunk =
            (struct HSS_BootZIChunkDesc const *)((char *)pBootImage + pBootImage->ziChunkTableOffset);
}

static void boot_zero_init_chunks_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);
    struct HSS_BootZIChunkDesc const *pZiChunk = pInstanceData->pZiChunk;

    if (pZiChunk->size != 0u) {
        if (target == pZiChunk->owner) {
            if (HSS_DDR_IsAddrInDDR((uintptr_t)pZiChunk->execAddr) && !HSS_Trigger_IsNotified(EVENT_DDR_TRAINED)) {
                ; // need to wait until DDR is initialized
            } else {
#if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
                mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::%d:ziChunk->0x%x, %u bytes\n",
                    pMyMachine->pMachineName, pInstanceData->ziChunkCount,
                    (uintptr_t)pZiChunk->execAddr, pZiChunk->size);
#endif
                boot_do_zero_init_chunk(pZiChunk);
                pInstanceData->pZiChunk++;
            }
        } else {
            pInstanceData->pZiChunk++;
        }
    } else {
        pMyMachine->state = BOOT_DOWNLOAD_CHUNKS;
    }
}

/////////////////

static void boot_download_chunks_onEntry(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);

    if (pBootImage->hart[target-1].numChunks) {
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Processing boot image: \"%s\"\n",
            pMyMachine->pMachineName, pBootImage->hart[target-1].name);
        pInstanceData->pChunk =
            (struct HSS_BootChunkDesc *)((char *)pBootImage + pBootImage->chunkTableOffset);

#if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::firstChunk is %u\n",
            pMyMachine->pMachineName, pBootImage->hart[target-1].firstChunk);
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::lastChunk is %u\n",
            pMyMachine->pMachineName, pBootImage->hart[target-1].lastChunk);
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::numChunks is %u\n",
            pMyMachine->pMachineName, pBootImage->hart[target-1].numChunks);
#endif

        pInstanceData->chunkCount = 0u;
        pInstanceData->subChunkOffset = 0u;
        pInstanceData->pChunk += pBootImage->hart[target-1].firstChunk;
    } else {
        // nothing to do for this machine, numChunks is zero...
    }
}

static void boot_download_chunks_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);

    if (pBootImage->hart[target-1].numChunks) {
        //
        // end of image is denoted by sentinel chunk with zero size...
        // so if we're not on the sentinel chunk
        struct HSS_BootChunkDesc const *pChunk = pInstanceData->pChunk;
        if ((pInstanceData->chunkCount <= pBootImage->hart[target-1].lastChunk) && (pChunk->size)) {
            //
            // and it is for us, then download it if we have permission
            if (((pChunk->owner & ~BOOT_FLAG_ANCILLIARY_DATA) == target)
                && (HSS_PMP_CheckWrite(target, pChunk->execAddr, pChunk->size))) {
#if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
                if (!pInstanceData->subChunkOffset) {
                    mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::%d:chunk@0x%x->0x%x, %u bytes\n",
                        pMyMachine->pMachineName, pInstanceData->chunkCount,
                        (uintptr_t)pChunk->loadAddr,
                        (uintptr_t)pChunk->execAddr, pChunk->size);
                }
#endif
                // check each hart to see if it wants to transmit
                boot_do_download_chunk(pChunk,
#ifdef BOOT_SUB_CHUNK_SIZE
                    pInstanceData->subChunkOffset, BOOT_SUB_CHUNK_SIZE
#else
                    0u, pChunk->size
#endif
                );

                if ((pChunk->owner & BOOT_FLAG_ANCILLIARY_DATA)
                    && (!pInstanceData->ancilliaryData)) {
                    mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::%d:ancilliary data found at 0x%x\n",
                        pMyMachine->pMachineName, pInstanceData->chunkCount, pChunk->execAddr);
                    pInstanceData->ancilliaryData = pChunk->execAddr;
                }

#ifdef BOOT_SUB_CHUNK_SIZE
                pInstanceData->subChunkOffset += BOOT_SUB_CHUNK_SIZE;
                if (pInstanceData->subChunkOffset > pChunk->size) {
#  if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
                    mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::%d:sub-chunk finished at 0x%x\n",
                        pMyMachine->pMachineName, pInstanceData->chunkCount, pInstanceData->subChunkOffset);
#  endif
                    pInstanceData->subChunkOffset = 0u;
                    pInstanceData->chunkCount++;
                    pInstanceData->pChunk++;
                }
#else
                pInstanceData->chunkCount++;
                pInstanceData->pChunk++;
#endif
            } else {
                if (pChunk->owner == target) {
                    mHSS_DEBUG_PRINTF(LOG_ERROR,
                        "%s::Skipping chunk %p due to invalid permissions\n", pMyMachine->pMachineName, pChunk);
                } else {
                    mHSS_DEBUG_PRINTF(LOG_WARN,
                        "%s::Skipping chunk %p due to ownership %d\n", pMyMachine->pMachineName, pChunk, pChunk->owner);
                }

                pInstanceData->pChunk++;
            }
            //
            // either way, with a non-sentinel chunk,
            // move onto next chunk, which will be handled by next time into state machine...
	} else {
            //
            // otherwise we are on a sentinel chunk and thus we are
            // finished processing the image
            pMyMachine->state = BOOT_OPENSBI_INIT;
        }
    } else {
        pMyMachine->state = BOOT_COMPLETE;
    }
}

static void boot_download_chunks_onExit(struct StateMachine * const pMyMachine)
{
    /* Re-register harts now that we've fully parsed the boot image (ancillary data etc) */
    register_harts(pMyMachine);
}

/////////////////

static void boot_opensbi_init_onEntry(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);

    if (pBootImage->hart[target-1].entryPoint) {
        pInstanceData->iterator = 0u;
    }
}

static void common_boot_message_delivery(struct StateMachine * const pMyMachine, enum HSSHartId const target)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;

    bool result = IPI_MessageAlloc(&(pInstanceData->msgIndexAux[target-1]));
    assert(result);

    if (pBootImage->hart[target-1].flags & BOOT_FLAG_SKIP_OPENSBI) {
        //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::u54_%u:goto %p\n", pMyMachine->pMachineName,
        //    target, pBootImage->hart[target-1].entryPoint);

        result = IPI_MessageDeliver(pInstanceData->msgIndexAux[target-1], target,
            IPI_MSG_GOTO,
            pBootImage->hart[target-1].privMode,
            (void *)pBootImage->hart[target-1].entryPoint,
            (void *)pInstanceData->ancilliaryData);

        if (!result) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::u54_%u:sbi_init failed\n",
                pMyMachine->pMachineName, target);

            pMyMachine->state = BOOT_ERROR;
        }
    } else {
        //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::u54_%u:sbi_init %p\n", pMyMachine->pMachineName,
        //    target, pBootImage->hart[target-1].entryPoint);

        result = IPI_MessageDeliver(pInstanceData->msgIndexAux[target-1], target,
            IPI_MSG_OPENSBI_INIT,
            pBootImage->hart[target-1].privMode,
            (void *)pBootImage->hart[target-1].entryPoint,
            (void *)pInstanceData->ancilliaryData);

        if (!result) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::u54_%u:sbi_init failed\n",
                pMyMachine->pMachineName, target);

            pMyMachine->state = BOOT_ERROR;
        }
    }
}

static void boot_opensbi_init_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);

    // if target has a valid entry point, allocate a message for it and send a OPENSBI_INIT IPI
    bool const primary_boot_hart = (pBootImage->hart[target-1].numChunks) && (pBootImage->hart[target-1].entryPoint);

    if (primary_boot_hart) {
        if (pInstanceData->iterator < ARRAY_SIZE(bootMachine)) {
            enum HSSHartId peer = bootMachine[pInstanceData->iterator].hartId;

            if (peer == target) {
                // skip myself for now
            } else if (pBootImage->hart[peer-1].entryPoint == pBootImage->hart[target-1].entryPoint) {
                common_boot_message_delivery(pMyMachine, peer); // found another hart in same boot set as me...
            }
            pInstanceData->iterator++;
        } else {
            pMyMachine->state = BOOT_WAIT;
        }
    }
}

static void boot_opensbi_init_onExit(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    assert(pBootImage != NULL);

    if (pBootImage->hart[target-1].entryPoint) {
        common_boot_message_delivery(pMyMachine, target);
    } else {
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::target is %u, pBootImage is %p, skipping goto/sbi_init %p\n",
            pMyMachine->pMachineName, target, pBootImage, pBootImage->hart[target-1].entryPoint);
    }
}

/////////////////

static void boot_wait_onEntry(struct StateMachine * const pMyMachine)
{
    (void)pMyMachine;
    //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Checking for IPI ACKs: - -\n", pMyMachine->pMachineName);
}

static void boot_wait_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const target = pInstanceData->target;

    pMyMachine->startTime = HSS_GetTime();

    if (!pBootImage->hart[target-1].entryPoint) {
        // nothing for me to do, not expecting GOTO ack...
        HSS_U54_SetState_Ex(target, HSS_State_Idle);
        pMyMachine->state = BOOT_COMPLETE;
    } else if (HSS_Timer_IsElapsed(pMyMachine->startTime, BOOT_WAIT_TIMEOUT)) {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::IPI ACK Timeout after %" PRIu64 " iterations\n",
            pMyMachine->pMachineName, pMyMachine->executionCount);

        for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
            enum HSSHartId peer = bootMachine[i].hartId;
            free_msg_index_aux(pInstanceData, peer);
        }

        free_msg_index(pInstanceData);

        pMyMachine->state = BOOT_ERROR;
    } else {
        // need to free as received, not all at once...
        if (check_for_ipi_acks(pMyMachine)) {
            // turn appropriate bit on in SYSREGSCB:MSS_STATUS:BOOT_STATUS to indicate it is up
            // note: this bit is a status indicator to SW only, and is not functional/does not have side effects
            mHSS_ReadModWriteRegU32(SYSREGSCB, MSS_STATUS, 0xFFFFu, 1u << (target-1));

            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s::Checking for IPI ACKs: ACK/IDLE ACK\n",
            //    pMyMachine->pMachineName);
            pMyMachine->state = BOOT_COMPLETE;
        }
    }
}

/////////////////

static void boot_error_handler(struct StateMachine * const pMyMachine)
{
    mHSS_DEBUG_PRINTF(LOG_ERROR, "%s::\n"
        "*******************************************************************\n"
        "* WARNING: Boot Error - transitioning to IDLE                     *\n"
        "*******************************************************************\n",
        pMyMachine->pMachineName);

    // Set BOOT_FAIL_CR to indicate to the fabric that boot process failed...
    SYSREG->BOOT_FAIL_CR = 1;

    pMyMachine->state = BOOT_COMPLETE;
}


/////////////////

atomic_t bootComplete[5] = {
     ATOMIC_INITIALIZER(0),
     ATOMIC_INITIALIZER(0),
     ATOMIC_INITIALIZER(0),
     ATOMIC_INITIALIZER(0),
     ATOMIC_INITIALIZER(0),
};

static void boot_complete_onEntry(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData const * const pInstanceData = pMyMachine->pInstanceData;
    enum HSSHartId const hartId = pInstanceData->target;
    atomic_write(&bootComplete[hartId], 1);
}

static void boot_complete_handler(struct StateMachine * const pMyMachine)
{
    bool all_complete = atomic_read(&bootComplete[1]) ? true : false;
    all_complete &= atomic_read(&bootComplete[2]) ? true : false;
    all_complete &= atomic_read(&bootComplete[3]) ? true : false;
    all_complete &= atomic_read(&bootComplete[4]) ? true : false;

    if (all_complete) {
       HSS_Trigger_Notify(EVENT_BOOT_COMPLETE);
       pMyMachine->state = BOOT_IDLE;
   }
}
/////////////////

static void boot_idle_onEntry(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData const * const pInstanceData = pMyMachine->pInstanceData;
    HSS_PerfCtr_Lap(pInstanceData->perfCtr);
    //mHSS_DEBUG_PRINTF(LOG_ERROR, "%s:: now at state %d\n", pMyMachine->pMachineName, pMyMachine->state);
}

static void boot_idle_handler(struct StateMachine * const pMyMachine)
{
    struct HSS_Boot_LocalData const * const pInstanceData = pMyMachine->pInstanceData;
    IPI_ConsumeIntent(pInstanceData->target, IPI_MSG_BOOT_REQUEST); // check for boot requests
}



// ----------------------------------------------------------------------------
// PUBLIC API
//

static bool boot_using_hart_bitmask_(const union HSSHartBitmask restartHartBitmask);
static bool boot_using_hart_bitmask_(const union HSSHartBitmask restartHartBitmask)
{
    bool result = false;

    for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
        if (restartHartBitmask.uint & (1u << bootMachine[i].hartId)) {
            struct StateMachine * const pMachine = bootMachine[i].pMachine;

            switch (pMachine->state) {
            case BOOT_OPENSBI_INIT:
                pMachine->state = (stateType_t)BOOT_OPENSBI_INIT;
                result = true;
                break;

            case BOOT_SETUP_PMP_COMPLETE:
                __attribute__((fallthrough)); // deliberate fallthrough
            case BOOT_IDLE:
                __attribute__((fallthrough)); // deliberate fallthrough
            case BOOT_INITIALIZATION:
                pMachine->state = (stateType_t)BOOT_INITIALIZATION;
                result = true;
                break;

            default:
                mHSS_DEBUG_PRINTF(LOG_ERROR, "invalid hart state %d for u54_%u\n", pMachine->state, i+1u);
                // try to recover anyway
                pMachine->state = (stateType_t)BOOT_INITIALIZATION;
                result = true;
                break;
            }
        }
    }

    HSS_Trigger_Notify(EVENT_POST_BOOT);
    return result;
}

enum IPIStatusCode HSS_Boot_RestartCore(enum HSSHartId source)
{
    enum IPIStatusCode result = IPI_FAIL;

    assert(pBootImage);

    if (source != HSS_HART_ALL) {
        union HSSHartBitmask restartHartBitmask = { .uint = BIT(source) };

        result = HSS_Boot_RestartCores_Using_Bitmask(restartHartBitmask);
    } else {
        if (!HSS_Boot_ValidateImage(pBootImage)) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "validation failed for u54_%u\n", source);
        } else {
            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "called for all harts\n");

            const union HSSHartBitmask restartHartBitmask = {
                .s = { .u54_1 = 1, .u54_2 = 1, .u54_3 = 1, .u54_4 = 1, }
            };

            if (boot_using_hart_bitmask_(restartHartBitmask)) {
                result = IPI_SUCCESS;
            }
        }
    }

    return result;
}

enum IPIStatusCode HSS_Boot_RestartCores_Using_Bitmask(union HSSHartBitmask restartHartBitmask)
{
    enum IPIStatusCode result = IPI_FAIL;

    if (!pBootImage) {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "pBootImage is NULL\n");
    } else if (!HSS_Boot_ValidateImage(pBootImage)) {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "validation failed for Hart bitmask %x\n", restartHartBitmask.uint);
    } else {
        for (unsigned int source = HSS_HART_U54_1;
             (source < HSS_HART_NUM_PEERS); source++) {
            union HSSHartBitmask localRestartHartBitmask = { .uint = 0u };

            if (!(restartHartBitmask.uint & (unsigned int)BIT(source))) { continue; }

            // in interrupts-always-enabled world of the HSS, it would appear less
            // racey to boot secondary cores first and have them all wait...
            for (unsigned int i = 0u; i < ARRAY_SIZE(bootMachine); i++) {
                enum HSSHartId peer = bootMachine[i].hartId;

                if (peer == source) { continue; } // skip myself

                if (pBootImage->hart[peer-1].entryPoint == pBootImage->hart[source-1].entryPoint) {
                    // found another hart in same boot set as me...
                    localRestartHartBitmask.uint |= (1u << peer);
                }
            }

            localRestartHartBitmask.uint |= (1u << source);

            if (pBootImage->hart[source-1].numChunks && boot_using_hart_bitmask_(localRestartHartBitmask)) {
                    result = IPI_SUCCESS;
            }

            restartHartBitmask.uint &= (~localRestartHartBitmask.uint);
        }
    }

    return result;
}


bool HSS_SkipBoot_IsSet(enum HSSHartId target)
{
    bool result = false;

    assert(pBootImage != NULL);

    if (pBootImage->hart[target-1].flags & BOOT_FLAG_SKIP_AUTOBOOT)
        result = true;

    return result;
}

enum IPIStatusCode HSS_Boot_IPIHandler(TxId_t transaction_id, enum HSSHartId source,
    uint32_t immediate_arg, void *p_extended_buffer_in_ddr, void *p_ancilliary_buffer_in_ddr)
{
    (void)transaction_id;
    (void)source;
    (void)immediate_arg;
    (void)p_extended_buffer_in_ddr;
    (void)p_ancilliary_buffer_in_ddr;

    // boot strap IPI received from one of the U54s...
    //mHSS_DEBUG_PRINTF(LOG_ERROR, "called for %d\n", source);

    /* Remoteproc use case 1.1:
       Elf file loaded by Linux using rproc elf loader, so
       no need to reload the payload on the HSS
    */
#if IS_ENABLED(CONFIG_SERVICE_OPENSBI_RPROC)
    if(immediate_arg == RPROC_BOOT)
    {
        struct RemoteProcMsg *rproc_data = (struct RemoteProcMsg *)p_extended_buffer_in_ddr;
        source = rproc_data->target;
        struct StateMachine * const pMachine = bootMachine[source - 1].pMachine;
        pMachine->state = (stateType_t)BOOT_OPENSBI_INIT;
    }
#endif

    return HSS_Boot_RestartCore(source);
}


static bool validateCrc_(struct HSS_BootImage *pImageHdr)
{
    bool result = false;
    uint32_t headerCrc;

    struct HSS_BootImage shadowHdr = *pImageHdr;

    shadowHdr.headerCrc = 0u;
    memset(&(shadowHdr.signature), 0, sizeof(shadowHdr.signature));

    size_t crcLen;
    switch (pImageHdr->version) {
    case 0u:
        // pre crypto-signing, the BootImage format was slightly different, so to ensure
        // no CRC failures on older images, we use a legacy structure size...
        crcLen = sizeof(struct HSS_BootImage_v0);
        break;

    default:
        crcLen = sizeof(struct HSS_BootImage);
        break;
    }

    headerCrc = CRC32_calculate((const uint8_t *)&shadowHdr, crcLen);
    if (headerCrc == pImageHdr->headerCrc) {
        result = true;
    } else {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "Checked HSS_BootImage header CRC (%p->%p): calculated %08x vs expected %08x\n",
            pImageHdr, (char *)pImageHdr + sizeof(struct HSS_BootImage), headerCrc, pImageHdr->headerCrc);
    }

    return result;
}

bool HSS_Boot_ValidateImage(struct HSS_BootImage *pImage)
{
    bool result = false;

#if IS_ENABLED(CONFIG_SERVICE_BOOT)
    //
    // now have a Full Boot Image, let's check it is a valid one...
    //
    {
        if (!pImage) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "Boot Image NULL, ignoring\n");
        } else if (pImage->magic != mHSS_BOOT_MAGIC) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "Boot Image magic invalid, ignoring\n");
#  if IS_ENABLED(CONFIG_CRYPTO_SIGNING)
        } else if (!HSS_Boot_Secure_CheckCodeSigning(pImage)) {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "Boot Image failed code signing\n");
#  endif
        } else if (validateCrc_(pImage)) {
            //mHSS_DEBUG_PRINTF(LOG_STATUS, "Boot image passed CRC\n");

#if IS_ENABLED(CONFIG_SERVICE_GPIO_UI)
            HSS_GPIO_UI_ReportImageGoodCRC();
#endif

        // GCC 9.x appears to dislike the pImage cast, and sees dereferencing the
        // set name as an out-of-bounds... So we'll disable that warning just for
        // this print...
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "Boot image set name: \"%s\"\n", pImage->set_name);
#  pragma GCC diagnostic pop

#  if defined(CONFIG_SERVICE_BOOT_CUSTOM_FLOW)
            result = HSS_Boot_Custom();
#  else
            result = true;
#  endif
        } else {
            mHSS_DEBUG_PRINTF(LOG_ERROR, "Boot image failed CRC\n");
        }
    }
#endif

    return result;
}

bool HSS_Boot_VerifyMagic(struct HSS_BootImage const * const pImage)
{
    bool result = false;

    if ((pImage->magic == mHSS_BOOT_MAGIC) || (pImage->magic == mHSS_COMPRESSED_MAGIC)) {
        result = true;
    } else {
        mHSS_DEBUG_PRINTF(LOG_WARN, "magic is %08x vs expected %08x or %08x\n",
            pImage->magic, mHSS_BOOT_MAGIC, mHSS_COMPRESSED_MAGIC);
    }

    return result;
}

void HSS_Register_Boot_Image(struct HSS_BootImage *pImage)
{
    pBootImage = pImage;
}

bool HSS_Boot_Custom(void)
{
    int i;
    size_t numChunks = 0u;
    size_t firstChunk = 0u;
    size_t chunkNum = 0u;
    size_t subChunkOffset = 0u;
    enum HSSHartId target = 0;
    struct HSS_BootChunkDesc const *pChunk;
    struct HSS_BootZIChunkDesc const *pZiChunk;

    if (!pBootImage)
        return false;

    for (i = 0; i < (MAX_NUM_HARTS-1); i++) {
        if (pBootImage->hart[i].numChunks) {
            target = i + 1;
            numChunks = pBootImage->hart[i].numChunks;
            firstChunk = pBootImage->hart[i].firstChunk;
        }
    }

    if (!numChunks || !target) {
        mHSS_DEBUG_PRINTF(LOG_ERROR, "Failed to find target HART\n");
        return false;
    }

    mHSS_DEBUG_PRINTF(LOG_NORMAL, "Zeroing chunks for HART%d\n", target);
    pZiChunk = (struct HSS_BootZIChunkDesc const *)((char *)pBootImage + pBootImage->ziChunkTableOffset);
    while (pZiChunk->size != 0u) {
        if (target == pZiChunk->owner) {
#if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "%d:ziChunk->0x%x, %u bytes\n",
                chunkNum, (uintptr_t)pZiChunk->execAddr, pZiChunk->size);
#endif
            boot_do_zero_init_chunk(pZiChunk);
            chunkNum++;
        }
        pZiChunk++;
    }

    pChunk = (struct HSS_BootChunkDesc *)((char *)pBootImage + pBootImage->chunkTableOffset);
    chunkNum = 0u;
    pChunk += firstChunk;
    mHSS_DEBUG_PRINTF(LOG_NORMAL, "Downloading chunks for HART%d at 0x%x\n", target, (uintptr_t)pChunk->execAddr);
    while (pChunk->size != 0u) {
        if ((pChunk->owner == target) && (HSS_PMP_CheckWrite(target, pChunk->execAddr, pChunk->size))) {
#if IS_ENABLED(CONFIG_DEBUG_CHUNK_DOWNLOADS)
           if (!subChunkOffset) {
                mHSS_DEBUG_PRINTF(LOG_NORMAL, "%d:chunk@0x%x->0x%x, %u bytes\n",
                        chunkNum, (uintptr_t)pChunk->loadAddr,
                        (uintptr_t)pChunk->execAddr, pChunk->size);
            }
#endif
            // check each hart to see if it wants to transmit
#ifdef BOOT_SUB_CHUNK_SIZE
            boot_do_download_chunk(pChunk, subChunkOffset, BOOT_SUB_CHUNK_SIZE);

            subChunkOffset += BOOT_SUB_CHUNK_SIZE;
            if (subChunkOffset > pChunk->size) {
                subChunkOffset = 0u;
                chunkNum++;
                pChunk++;
            }
#else
            boot_do_download_chunk(pChunk, 0u, pChunk->size);
            chunkNum++;
            pChunk++;
            (void)subChunkOffset;
#endif
        } else {
            pChunk++;
        }
    }

#if IS_ENABLED(CONFIG_SERVICE_BOOT_CUSTOM_FLOW)
    // For custom boot-flow, all U54 HARTs and E51 HART
    // should jump to common entry point in M-mode
    uintptr_t custom_entryPoint = pBootImage->hart[target - 1].entryPoint;
    uint8_t custom_privMode = PRV_M;

    mHSS_DEBUG_PRINTF(LOG_NORMAL, "All HARTs jumping to entry address 0x%lx in M-mode\n", custom_entryPoint);
    for (i = 1; i < MAX_NUM_HARTS; i++) {
        IPI_Send(i, IPI_MSG_OPENSBI_INIT, 0, custom_privMode, (void * const)custom_entryPoint, NULL);
    }

    ((void (*)(uintptr_t, uintptr_t))custom_entryPoint)(current_hartid(), 0);
#endif

    return true;
}

/*!
 * \brief PMP Setup Handler
 *
 * Handle request to U54 from E51 to setup PMPs
 *
 */
enum IPIStatusCode HSS_Boot_PMPSetupHandler(TxId_t transaction_id, enum HSSHartId source,
    uint32_t immediate_arg, void *p_extended_buffer_in_ddr, void *p_ancilliary_buffer_in_ddr)
{
    enum IPIStatusCode result = IPI_FAIL;
    (void)transaction_id;
    (void)source;
    (void)immediate_arg;
    (void)p_extended_buffer_in_ddr;
    (void)p_ancilliary_buffer_in_ddr;

    // request to setup PMP by E51 received
    enum HSSHartId myHartId = current_hartid();
    HSS_U54_SetState(HSS_State_Booting);

    //mHSS_DEBUG_PRINTF(LOG_NORMAL, "Hart%u ", myHartId);

    if (!pmpSetupFlag[myHartId]) {
        pmpSetupFlag[myHartId] = true; // PMPs can be setup only once without reboot....

        // The E51 ensures that hardware separate is ensured before the U54 code starts running.
        // To do this, it needs to partition memory and peripheral access, based on configuration
        // information provided at build time.
        //
        // In order to setup RISC-V PMPs, the E51 instructs the U54s to  run code routines through
        // setting their reset vectors and temporarily bringing them out of WFI. This is because
        // the U54-specific PMP registers are CSRs and thus are only available locally on the
        // individual U54 and not memory mapped.
        //
        // The PMPs will be setup in M-Mode on the U54s and locked so that their configuration
        // cannot subsequently be changed without reboot to prevent accidental or malicious
        // modification through software defect.
        //
	init_pmp(myHartId);
	(void)mss_set_apb_bus_cr((uint32_t)LIBERO_SETTING_APBBUS_CR);
        //

        //mHSS_DEBUG_PRINTF_EX("setup complete\n");
        result =  IPI_SUCCESS;
    } else {
        //mHSS_DEBUG_PRINTF_EX("PMPs already configured\n");
        result =  IPI_SUCCESS;
    }

    return result;
}

/*!
 * \brief PMP Setup Request
 *
 * Make request to U54 from E51 to setup PMPs
 *
 */
bool HSS_Boot_PMPSetupRequest(enum HSSHartId target, uint32_t *indexOut)
{
    // setup PMP for each hart ...
    bool result;

    assert(target != HSS_HART_ALL); // need to setup PMPs on each Hart individually

    result = IPI_MessageAlloc(indexOut);
    assert(result);

    if (result) {
        result = IPI_MessageDeliver(*indexOut, target, IPI_MSG_PMP_SETUP, 0u, NULL, NULL);

        // couldn't send message, so free up resources...
        if (!result) {
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "u54_%u: failed to send message, so freeing\n", target);
            IPI_MessageFree(*indexOut);
        }
    }

    return result;
}

/*!
 * \brief SBI Setup Request
 *
 * Make request to U54 from E51 to setup SBIs
 *
 */
bool HSS_Boot_SBISetupRequest(enum HSSHartId target, uint32_t *indexOut)
{
    // setup SBI for each hart ...
    bool result;

    assert(target != HSS_HART_ALL); // need to setup SBIs on each Hart individually

    //mHSS_DEBUG_PRINTF(LOG_NORMAL, "called for u54_%u\n", target);

    result = IPI_MessageAlloc(indexOut);
    assert(result);

    if (result) {
        result = IPI_MessageDeliver(*indexOut, target, IPI_MSG_OPENSBI_INIT, 0u, NULL, NULL);

        // couldn't send message, so free up resources...
        if (!result) {
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "u54_%u: failed to send message, so freeing\n", target);
            IPI_MessageFree(*indexOut);
        }
    }

    return result;
}
