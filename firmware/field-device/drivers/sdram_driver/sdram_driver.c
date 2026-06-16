/**
 * @file sdram_driver.c
 * @brief SdramDriver — FMC SDRAM Bank 1 initialisation (IS42S32400F-6BL).
 *
 * Register values derived from the ST F469 Discovery BSP
 * (stm32469i_discovery_sdram.c) for HCLK = 180 MHz, SDCLK = 90 MHz.
 * Validated on hardware via TC-SDRAM-005..007.
 */

#include "sdram_driver.h"

#include <stddef.h>
/* ===================================================================== */
/* Pins definition                                                   */
/* ===================================================================== */

typedef struct
{
    GPIO_TypeDef *port;
    uint8_t pin;
} fmc_pin_t;

static const fmc_pin_t s_fmc_pins[] = {
    /* Control */
    {GPIOC, 0},  /* SDNWE   */
    {GPIOG, 15}, /* SDNCAS  */
    {GPIOF, 11}, /* SDNRAS  */
    {GPIOH, 3},  /* SDNE0   */
    {GPIOH, 2},  /* SDCKE0  */
    {GPIOG, 8},  /* SDCLK   */
    /* Byte-enable lanes (32-bit bus → 4 lanes) */
    {GPIOE, 0}, /* NBL0 */
    {GPIOE, 1}, /* NBL1 */
    {GPIOI, 4}, /* NBL2 */
    {GPIOI, 5}, /* NBL3 */
    /* Address A0..A11 */
    {GPIOF, 0},
    {GPIOF, 1},
    {GPIOF, 2},
    {GPIOF, 3},
    {GPIOF, 4},
    {GPIOF, 5},
    {GPIOF, 12},
    {GPIOF, 13},
    {GPIOF, 14},
    {GPIOF, 15},
    {GPIOG, 0},
    {GPIOG, 1},
    /* Bank BA0, BA1 */
    {GPIOG, 4},
    {GPIOG, 5},
    /* Data D0..D31 */
    {GPIOD, 14},
    {GPIOD, 15},
    {GPIOD, 0},
    {GPIOD, 1},
    {GPIOE, 7},
    {GPIOE, 8},
    {GPIOE, 9},
    {GPIOE, 10},
    {GPIOE, 11},
    {GPIOE, 12},
    {GPIOE, 13},
    {GPIOE, 14},
    {GPIOE, 15},
    {GPIOD, 8},
    {GPIOD, 9},
    {GPIOD, 10},
    {GPIOH, 8},
    {GPIOH, 9},
    {GPIOH, 10},
    {GPIOH, 11},
    {GPIOH, 12},
    {GPIOH, 13},
    {GPIOH, 14},
    {GPIOH, 15},
    {GPIOI, 0},
    {GPIOI, 1},
    {GPIOI, 2},
    {GPIOI, 3},
    {GPIOI, 6},
    {GPIOI, 7},
    {GPIOI, 9},
    {GPIOI, 10},
};

static void fmc_pins_configure(void)
{
    for (size_t i = 0U; i < sizeof(s_fmc_pins) / sizeof(s_fmc_pins[0]); i++)
    {
        GPIO_TypeDef *p = s_fmc_pins[i].port;
        uint8_t n = s_fmc_pins[i].pin;
        uint32_t m2 = (uint32_t) n * 2U;
        uint32_t m4 = (uint32_t) (n & 0x07U) * 4U;
        volatile uint32_t *afr = (n < 8U) ? &p->AFR[0] : &p->AFR[1];

        /* Mode = AF (10) */
        p->MODER = (p->MODER & ~(0x3UL << m2)) | (0x2UL << m2);
        /* Output type = push-pull (0) */
        p->OTYPER &= ~(0x1UL << n);
        /* Speed = very high (11) */
        p->OSPEEDR = (p->OSPEEDR & ~(0x3UL << m2)) | (0x3UL << m2);
        /* Pull = none (00) */
        p->PUPDR &= ~(0x3UL << m2);
        /* Alternate function = AF12 */
        *afr = (*afr & ~(0xFUL << m4)) | (12UL << m4);
    }
}

/* ===================================================================== */
/* SDCR1 field values                                                   */
/* ===================================================================== */

#define SDCR_NC_8_BITS (0x00UL)      /**< 8-bit column address.        */
#define SDCR_NR_12_BITS (0x01UL)     /**< 12-bit row address.          */
#define SDCR_MWID_32 (0x02UL)        /**< 32-bit data bus width.       */
#define SDCR_NB_4_BANKS (0x01UL)     /**< 4 internal banks.            */
#define SDCR_CAS_3 (0x03UL)          /**< CAS latency = 3 cycles.      */
#define SDCR_WP_DISABLED (0x00UL)    /**< Write protection off.        */
#define SDCR_SDCLK_DIV2 (0x02UL)     /**< SDCLK = HCLK / 2 = 90 MHz.  */
#define SDCR_RBURST_ENABLED (0x01UL) /**< Burst read enabled.          */
#define SDCR_RPIPE_NO_DELAY (0x00UL) /**< No read pipe delay.          */

/* SDCR1 bit positions (RM0386 §37.7.1) */
#define SDCR_NC_POS (0U)
#define SDCR_NR_POS (2U)
#define SDCR_MWID_POS (4U)
#define SDCR_NB_POS (6U)
#define SDCR_CAS_POS (7U)
#define SDCR_WP_POS (9U)
#define SDCR_SDCLK_POS (10U)
#define SDCR_RBURST_POS (12U)
#define SDCR_RPIPE_POS (13U)

#define SDCR1_VALUE                                                                                \
    ((SDCR_NC_8_BITS << SDCR_NC_POS) | (SDCR_NR_12_BITS << SDCR_NR_POS) |                          \
     (SDCR_MWID_32 << SDCR_MWID_POS) | (SDCR_NB_4_BANKS << SDCR_NB_POS) |                          \
     (SDCR_CAS_3 << SDCR_CAS_POS) | (SDCR_WP_DISABLED << SDCR_WP_POS) |                            \
     (SDCR_SDCLK_DIV2 << SDCR_SDCLK_POS) | (SDCR_RBURST_ENABLED << SDCR_RBURST_POS) |              \
     (SDCR_RPIPE_NO_DELAY << SDCR_RPIPE_POS))

/* ===================================================================== */
/* SDTR1 timing values (cycles - 1 each, 90 MHz SDCLK ≈ 11.1 ns/cycle) */
/* ===================================================================== */

#define SDTR_TMRD (1UL) /**< Mode register delay: 2 cycles.      */
#define SDTR_TXSR (6UL) /**< Exit self-refresh: 7 cycles (≥67ns).*/
#define SDTR_TRAS (3UL) /**< Active-to-precharge: 4 cycles (≥42ns).*/
#define SDTR_TRC (5UL)  /**< Row cycle: 6 cycles (≥60ns).        */
#define SDTR_TWR (1UL)  /**< Write recovery: 2 cycles.           */
#define SDTR_TRP (1UL)  /**< Precharge period: 2 cycles (≥18ns). */
#define SDTR_TRCD (1UL) /**< Row-to-column: 2 cycles (≥18ns).   */

/* SDTR bit positions (RM0386 §37.7.2) */
#define SDTR_TMRD_POS (0U)
#define SDTR_TXSR_POS (4U)
#define SDTR_TRAS_POS (8U)
#define SDTR_TRC_POS (12U)
#define SDTR_TWR_POS (16U)
#define SDTR_TRP_POS (20U)
#define SDTR_TRCD_POS (24U)

#define SDTR1_VALUE                                                                                \
    ((SDTR_TMRD << SDTR_TMRD_POS) | (SDTR_TXSR << SDTR_TXSR_POS) | (SDTR_TRAS << SDTR_TRAS_POS) |  \
     (SDTR_TRC << SDTR_TRC_POS) | (SDTR_TWR << SDTR_TWR_POS) | (SDTR_TRP << SDTR_TRP_POS) |        \
     (SDTR_TRCD << SDTR_TRCD_POS))

/* ===================================================================== */
/* SDCMR — command mode register constants (RM0386 §37.7.4)            */
/* ===================================================================== */

#define SDCMR_MODE_NORMAL (0x00UL)
#define SDCMR_MODE_CLK_ENABLE (0x01UL)
#define SDCMR_MODE_PALL (0x02UL)
#define SDCMR_MODE_AUTO_REFRESH (0x03UL)
#define SDCMR_MODE_LOAD_MODE (0x04UL)

#define SDCMR_CTB1 (0x01UL << 4U) /**< Target Bank 1.   */
#define SDCMR_NRFS_POS (5U)
#define SDCMR_MRD_POS (9U)

/* Mode register value: CAS=3, burst length=1, sequential, no write burst */
#define SDRAM_MRD_BURST_LENGTH_1 (0x0000UL)
#define SDRAM_MRD_BURST_TYPE_SEQ (0x0000UL)
#define SDRAM_MRD_CAS_LATENCY_3 (0x0030UL)
#define SDRAM_MRD_NO_WRITE_BURST (0x0200UL)

#define SDRAM_MODE_REGISTER_VALUE                                                                  \
    (SDRAM_MRD_BURST_LENGTH_1 | SDRAM_MRD_BURST_TYPE_SEQ | SDRAM_MRD_CAS_LATENCY_3 |               \
     SDRAM_MRD_NO_WRITE_BURST)

/* Number of auto-refresh cycles issued during init sequence */
#define SDRAM_AUTOREFRESH_COUNT (8UL)

/* ===================================================================== */
/* SDRTR — auto-refresh timer (RM0386 §37.7.5)                         */
/* COUNT = (refresh_period_s × SDCLK_Hz) / rows - 20                   */
/*       = (64e-3 × 90e6) / 4096 - 20 ≈ 1386                           */
/* ===================================================================== */

#define SDRAM_SDRTR_COUNT (1386UL)
#define FMC_SDRTR_COUNT_POS (1U)

/* ===================================================================== */
/* FMC SDSR busy flag                                                   */
/* ===================================================================== */

/* Bounded-wait iteration limit for BUSY flag polling */
#define SDRAM_BUSY_TIMEOUT_ITERS (0xFFFFUL)

/* SDRAM base address — FMC SDRAM Bank 1 (SDNE0) */
#define SDRAM_BASE_ADDR (0xC0000000UL)

/* ===================================================================== */
/* Module state                                                          */
/* ===================================================================== */

SDRAM_TEST_VISIBLE bool s_initialised = false;

/* ===================================================================== */
/* Private helpers                                                       */
/* ===================================================================== */

static sdram_err_t wait_busy(void)
{
    uint32_t timeout = SDRAM_BUSY_TIMEOUT_ITERS;

    while ((FMC_Bank5_6->SDSR & FMC_SDSR_BUSY) != 0U)
    {
        if (timeout == 0U)
        {
            return SDRAM_ERR_TIMEOUT;
        }
        timeout--;
    }

    return SDRAM_ERR_OK;
}

static sdram_err_t send_cmd(uint32_t mode, uint32_t nrfs, uint32_t mrd)
{
    FMC_Bank5_6->SDCMR = mode | SDCMR_CTB1 | (nrfs << SDCMR_NRFS_POS) | (mrd << SDCMR_MRD_POS);

    return wait_busy();
}

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

sdram_err_t sdram_init(void)
{
    sdram_err_t err;

    /* Step 0: Enable FMC and GPIO clock. */
    RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOEEN |
                    RCC_AHB1ENR_GPIOFEN | RCC_AHB1ENR_GPIOGEN | RCC_AHB1ENR_GPIOHEN |
                    RCC_AHB1ENR_GPIOIEN;
    (void) RCC->AHB1ENR; /* read-back to ensure clock is stable */

    /* Step 1: FMC pin AF mapping */
    fmc_pins_configure();

    /* Step 2: Program SDCR1 — geometry, bus width, CAS, clock. */
    FMC_Bank5_6->SDCR[0] = SDCR1_VALUE;

    /* Step 3: Program SDTR1 — timing in SDCLK cycles. */
    FMC_Bank5_6->SDTR[0] = SDTR1_VALUE;

    /* Step 4: CLK_ENABLE command. */
    err = send_cmd(SDCMR_MODE_CLK_ENABLE, 0U, 0U);
    if (err != SDRAM_ERR_OK)
    {
        return err;
    }

    /* Step 5: Wait ≥ 100 µs for SDRAM power-on stabilisation.
     * At 180 MHz, each iteration is several cycles; 10 000 iters ≈ 500 µs. */
    for (volatile uint32_t i = 0U; i < 10000UL; i++)
    {
        __NOP();
    }

    /* Step 6: Precharge All. */
    err = send_cmd(SDCMR_MODE_PALL, 0U, 0U);
    if (err != SDRAM_ERR_OK)
    {
        return err;
    }

    /* Step 7: Two auto-refresh cycles (8 issued for robustness). */
    err = send_cmd(SDCMR_MODE_AUTO_REFRESH, SDRAM_AUTOREFRESH_COUNT, 0U);
    if (err != SDRAM_ERR_OK)
    {
        return err;
    }

    /* Step 8: Load mode register. */
    err = send_cmd(SDCMR_MODE_LOAD_MODE, 0U, SDRAM_MODE_REGISTER_VALUE);
    if (err != SDRAM_ERR_OK)
    {
        return err;
    }

    /* Step 9: Program auto-refresh timer. */
    FMC_Bank5_6->SDRTR |= (SDRAM_SDRTR_COUNT << FMC_SDRTR_COUNT_POS);

    s_initialised = true;

    return SDRAM_ERR_OK;
}

uint32_t sdram_get_base_addr(void)
{
    return SDRAM_BASE_ADDR;
}

/* ===================================================================== */
/* Test-only                                                             */
/* ===================================================================== */

#ifdef TEST
void sdram_driver_reset(void)
{
    s_initialised = false;
}
#endif /* TEST */
