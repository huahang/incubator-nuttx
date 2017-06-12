/****************************************************************************
 * arch/arm/src/stm32/stm32_hrtim.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Mateusz Szafoni <raiden00@railab.me>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arch/board/board.h>

#include "chip.h"
#include "stm32.h"
#include "stm32_gpio.h"
#include "stm32_hrtim.h"

#if defined(CONFIG_STM32_HRTIM1)

/* Only STM32F33XXX  */

#if defined(CONFIG_STM32_STM32F33XX)

#ifdef CONFIG_STM32_HRTIM_ADC
#  error HRTIM ADC Triggering not supported yet
#endif

#ifdef CONFIG_STM32_HRTIM_FAULT
#  error HRTIM Faults not supported yet
#endif

#ifdef CONFIG_STM32_HRTIM_EEV
#  error HRTIM External Events not supported yet
#endif

#ifdef CONFIG_STM32_HRTIM_BURST
#  error HRTIM Burst mode not supported yet
#endif

#ifdef CONFIG_STM32_HRTIM_IRQ
#  error HRTIM Interrupts not supported yet
#endif

#ifdef CONFIG_STM32_HRTIM_DMA
#  error HRTIM DMA not supported yet
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* HRTIM default configuration **********************************************/

#ifndef HRTIM_TIMER_MASTER
#  define HRTIM_MASTER_PRESCALER HRTIM_PRESCALER_2
#endif

/* HRTIM clock source configuration */

#ifdef CONFIG_STM32_HRTIM_CLK_FROM_PLL
#  if STM32_SYSCLK_SW == RCC_CFGR_SW_PLL
#    if (STM32_RCC_CFGR_PPRE2 != RCC_CFGR_PPRE2_HCLK) && \
        (STM32_RCC_CFGR_PPRE2 != RCC_CFGR_PPRE2_HCLKd2)
#      error "APB2 prescaler factor can not be greater than 2"
#    else
#      define HRTIM_HAVE_CLK_FROM_PLL 1
#      define HRTIM_CLOCK 2*STM32_PLL_FREQUENCY
#    endif
#  else
#    error "Clock system must be set to PLL"
#  endif
#else
#  define HRTIM_HAVE_CLK_FROM_APB2 1
#  if STM32_RCC_CFGR_PPRE2 ==  RCC_CFGR_PPRE2_HCLK
#      define HRTIM_CLOCK STM32_PCLK2_FREQUENCY
#  else
#      define HRTIM_CLOCK 2*STM32_PCLK2_FREQUENCY
#  endif
#endif

#if defined(CONFIG_STM32_HRTIM_TIMA) || defined(CONFIG_STM32_HRTIM_TIMB) || \
    defined(CONFIG_STM32_HRTIM_TIMC) || defined(CONFIG_STM32_HRTIM_TIMD) || \
    defined(CONFIG_STM32_HRTIM_TIME)
#  define HRTIM_HAVE_SLAVE 1
#endif

#if defined(CONFIG_STM32_HRTIM_TIMA_PWM) || defined(CONFIG_STM32_HRTIM_TIMB_PWM) || \
    defined(CONFIG_STM32_HRTIM_TIMC_PWM) || defined(CONFIG_STM32_HRTIM_TIMD_PWM) || \
    defined(CONFIG_STM32_HRTIM_TIME_PWM)
#  define HRTIM_HAVE_PWM 1
#endif

#if defined(CONFIG_STM32_HRTIM_TIMA_CAP) || defined(CONFIG_STM32_HRTIM_TIMB_CAP) || \
    defined(CONFIG_STM32_HRTIM_TIMC_CAP) || defined(CONFIG_STM32_HRTIM_TIMD_CAP) || \
    defined(CONFIG_STM32_HRTIM_TIME_CAP)
#  define HRTIM_HAVE_CAPTURE 1
#endif

#if defined(CONFIG_STM32_HRTIM_TIMA_DT) || defined(CONFIG_STM32_HRTIM_TIMB_DT) || \
    defined(CONFIG_STM32_HRTIM_TIMC_DT) || defined(CONFIG_STM32_HRTIM_TIMD_DT) || \
    defined(CONFIG_STM32_HRTIM_TIME_DT)
#  define HRTIM_HAVE_DEADTIME 1
#endif

#if defined(CONFIG_STM32_HRTIM_TIMA_CHOP) || defined(CONFIG_STM32_HRTIM_TIMB_CHOP) || \
    defined(CONFIG_STM32_HRTIM_TIMC_CHOP) || defined(CONFIG_STM32_HRTIM_TIMD_CHOP) || \
    defined(CONFIG_STM32_HRTIM_TIME_CHOP)
#  define HRTIM_HAVE_CHOPPER 1
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef HRTIM_HAVE_PWM

/* HRTIM TimerX Single Output Set/Reset Configuration */

struct stm32_hrtim_timout_s
{
  uint32_t set;                 /* Set events*/
  uint32_t rst;                 /* Reset events*/
};

/* HRTIM TimerX Output Chopper Configuration */

#ifdef HRTIM_HAVE_CHOPPER
struct stm32_hrtim_chopper_s
{
  uint32_t reserved;             /* reserved for future use */
};
#endif

/* HRTIM TimerX Output Deadtime Configuration*/

#ifdef HRTIM_HAVE_DEADTIME
struct stm32_hrtim_deadtime_s
{
  uint32_t reserved;             /* reserved for future use */
};
#endif

/* HRTIM Timer PWM structure */

struct stm32_hrtim_pwm_s
{
  struct stm32_hrtim_timout_s ch1; /* Channel 1 Set/Reset configuration*/
  struct stm32_hrtim_timout_s ch2; /* Channel 2 Set/Reset configuration */

#ifdef HRTIM_HAVE_CHOPPER
  struct stm32_hrtim_chopper_s chp;
#endif
#ifdef HRTIM_HAVE_DEADTIME
  struct stm32_hrtim_deadtime_s dt;
#endif
};

#endif

#ifdef HRTIM_HAVE_CAPTURE
struct stm32_hrtim_capture_s
{
  uint32_t reserved;            /* reserved for future use */
}
#endif

/* Common data structure for Master Timer and Slave Timers*/

struct stm32_hrtim_timcmn_s
{
  uint16_t cmp[4];              /* Compare registers */
  uint32_t base;                /* The base adress of the timer */
  uint32_t frequency;           /* Current frequency setting */
  uint32_t pclk;                /* The frequency of the peripheral clock
                                 * that drives the timer module */
#ifdef CONFIG_STM32_HRTIM_DMA
  uint32_t dmaburst;
#endif
};

/* Master Timer and Slave Timers structure */

struct stm32_hrtim_tim_s
{
  struct stm32_hrtim_timcmn_s tim; /* Common Timer data */
  FAR void *priv;                  /* Timer private data */
};

/* Master Timer private data structure */

struct stm32_hrtim_master_priv_s
{
  uint32_t reserved;            /* reserved for future use */
};

/* Slave Timer (A-E) private data structure */

struct stm32_hrtim_slave_priv_s
{
  uint32_t reset;                   /* Timer reset events */
#ifdef HRTIM_HAVE_PWM
  struct stm32_hrtim_pwm_s pwm;     /* PWM configuration */
#endif
#ifdef HRTIM_HAVE_CAPTURE
  struct stm32_hrtim_capture_s cap; /* Capture configuration */
#endif
};

/* This structure describes the configuration of HRTIM device */

struct stm32_hrtim_s
{
  uint32_t base;                     /* Base adress of HRTIM block */
  struct stm32_hrtim_tim_s *master;  /* Master Timer */
#ifdef CONFIG_STM32_HRTIM_TIMA
  struct stm32_hrtim_tim_s *tima;    /* HRTIM Timer A */
#endif
#ifdef CONFIG_STM32_HRTIM_TIMB
  struct stm32_hrtim_tim_s *timb;    /* HRTIM Timer B */
#endif
#ifdef CONFIG_STM32_HRTIM_TIMC
  struct stm32_hrtim_tim_s *timc;    /* HRTIM Timer C */
#endif
#ifdef CONFIG_STM32_HRTIM_TIMD
  struct stm32_hrtim_tim_s *timd;    /* HRTIM Timer D */
#endif
#ifdef CONFIG_STM32_HRTIM_TIME
  struct stm32_hrtim_tim_s *time;    /* HRTIM Timer E */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* HRTIM Driver Methods */

static int stm32_hrtim_open(FAR struct file *filep);
static int stm32_hrtim_close(FAR struct file *filep);
static int stm32_hrtim_ioctl(FAR struct file *filep, int cmd, unsigned long arg);

/* HRTIM Register access */

#ifdef HRTIM_HAVE_CLK_FROM_PLL
static void stm32_modifyreg32(unsigned int addr, uint32_t clrbits,
                              uint32_t setbits);
#endif
static uint32_t hrtim_getreg(FAR struct stm32_hrtim_s *priv, int offset);
static void hrtim_putreg(FAR struct stm32_hrtim_s *priv, int offset,
                             uint32_t value);
static void hrtim_modifyreg(FAR struct stm32_hrtim_s *priv, int offset,
                                uint32_t clrbits, uint32_t setbits);
static void hrtim_tim_putreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                             int offset, uint32_t value);
static void hrtim_tim_modifyreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                                int offset, uint32_t clrbits, uint32_t setbits);

/* HRTIM helper */

static uint32_t hrtim_tim_getreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                                 int offset);
static FAR struct stm32_hrtim_tim_s *hrtim_tim_get(FAR struct stm32_hrtim_s *priv,
                                                     uint8_t index);

/* Configuration */

static int hrtim_dll_cal(FAR struct stm32_hrtim_s *priv);
static int hrtim_tim_clock_config(FAR struct stm32_hrtim_s *priv, uint8_t index,
                                  uint8_t pre);
static int hrtim_tim_clocks_config(FAR struct stm32_hrtim_s *priv);
#if defined(HRTIM_HAVE_CAPTURE) || defined(HRTIM_HAVE_PWM) || defined(HRTIM_HAVE_SYNC)
static int hrtim_gpios_config(FAR struct stm32_hrtim_s *priv);
#endif
static void hrtim_preload_config(FAR struct stm32_hrtim_s *priv);
#if defined(HRTIM_HAVE_CAPTURE)
static int hrtim_inputs_config(FAR struct stm32_hrtim_s *priv);
#endif
#if defined(HRTIM_HAVE_SYNC)
static int hrtim_synch_config(FAR struct stm32_hrtim_s *priv);
#endif
#if defined(HRTIM_HAVE_PWM)
static int hrtim_outputs_config(FAR struct stm32_hrtim_s *priv);
#endif
#ifdef HRTIM_HAVE_ADC
static int hrtim_adc_config(FAR struct stm32_hrtim_s *priv);
#endif
#ifdef HRTIM_HAVE_FAULTS
static int hrtim_faults_config(FAR struct stm32_hrtim_s *priv);
#endif
#ifdef HRTIM_HAVE_EEV
static int hrtim_eev_config(FAR struct stm32_hrtim_s *priv);
#endif
#ifdef HRTIM_HAVE_INTERRUPTS
static int hrtim_irq_config(FAR struct stm32_hrtim_s *priv);
#endif

/* Initialization */

static int stm32_hrtimconfig(FAR struct stm32_hrtim_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations hrtim_fops =
{
  stm32_hrtim_open,   /* open */
  stm32_hrtim_close,  /* close */
  NULL,               /* read */
  NULL,               /* write */
  NULL,               /* seek */
  stm32_hrtim_ioctl   /* ioctl */
#ifndef CONFIG_DISABLE_POLL
  , NULL              /* poll */
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , NULL              /* unlink */
#endif
};

/* Master Timer data */

static struct stm32_hrtim_tim_s g_master =
{
  .tim =
  {
    .base  = STM32_HRTIM1_MASTER_BASE,
    .pclk  = HRTIM_CLOCK/HRTIM_MASTER_PRESCALER
  },
  .priv = NULL,
};

/* NOTE: only TIMER A data defined at this time */

#ifdef CONFIG_STM32_HRTIM_TIMA

/* Timer A private data */

static struct stm32_hrtim_slave_priv_s g_tima_priv =
{
#ifdef CONFIG_STM32_HRTIM_TIMA_PWM
  .pwm =
  {
    .ch1 =
    {
      .set = HRTIM_TIMA_CH1_SET,
      .rst = HRTIM_TIMA_CH1_RST
    },
    .ch2 =
    {
      .set = HRTIM_TIMA_CH2_SET,
      .rst = HRTIM_TIMA_CH2_RST
    },
#ifdef CONFIG_STM32_HRTIM_TIMA_CHOP
    .chp =
    {
      .reserved = 0
    },
#endif
#ifdef CONFIG_STM32_HRTIM_TIMA_DT
    .dt =
    {
      .reserved = 0
    }
#endif
  },
#endif
#ifdef CONFIG_STM32_HRTIM_TIMA_CAP
  .cap =
  {
    .reserved = 0
  }
#endif
};

/* Timer A data */

static struct stm32_hrtim_tim_s g_tima =
{
  .tim =
  {
    .base  = STM32_HRTIM1_TIMERA_BASE,
    .pclk  = HRTIM_CLOCK/HRTIM_TIMA_PRESCALER
  },
  .priv = &g_tima_priv
};

#endif

/* HRTIM1 private data */

static struct stm32_hrtim_s g_hrtim1priv =
{
  .master   = &g_master,
  .base     = STM32_HRTIM1_BASE,
#ifdef CONFIG_STM32_HRTIM_TIMA
  .tima     = &g_tima,
#endif
#ifdef CONFIG_STM32_HRTIM_TIMB
  .timb     = &g_timb,
#endif
#ifdef CONFIG_STM32_HRTIM_TIMC
  .timc     = &g_timc,
#endif
#ifdef CONFIG_STM32_HRTIM_TIMD
  .timd     = &g_timd,
#endif
#ifdef CONFIG_STM32_HRTIM_TIME
  .time     = &g_time,
#endif
};

struct hrtim_dev_s g_hrtim1dev =
{
  .hd_priv  = &g_hrtim1priv,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_hrtim_open
 *
 * Description:
 *   This function is called whenever the HRTIM device is opened.
 *
 ****************************************************************************/

static int stm32_hrtim_open(FAR struct file *filep)
{
#warning "stm32_hrtim_open: missing logic"
  return OK;
}

/****************************************************************************
 * Name: stm32_hrtim_close
 *
 * Description:
 *   This function is called when the HRTIM device is closed.
 *
 ****************************************************************************/

static int stm32_hrtim_close(FAR struct file *filep)
{
#warning "smt32_hrtim_close: missing logic"
  return OK;
}

/****************************************************************************
 * Name: stm32_hrtim_ioctl
 *
 * Description:
 *   The standard ioctl method.  This is where ALL of the HRTIM work is done.
 *
 ****************************************************************************/

static int stm32_hrtim_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct hrtim_dev_s  *dev;
  FAR struct stm32_hrtim_s *hrtim;
  int ret;

  tmrinfo("cmd: %d arg: %ld\n", cmd, arg);
  dev = inode->i_private;
  DEBUGASSERT(dev != NULL);
  hrtim = dev->hd_priv;

  UNUSED(hrtim);

#warning "smt32_hrtim_ioctl: missing logic"

  /* Handle HRTIM ioctl commands */

  switch (cmd)
    {

      default:
        {
          ret = -ENOSYS;
          break;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: stm32_modifyreg32
 *
 * Description:
 *   Modify the value of a 32-bit register (not atomic).
 *
 * Input Parameters:
 *   addr    - The address of the register
 *   clrbits - The bits to clear
 *   setbits - The bits to set
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef HRTIM_HAVE_CLK_FROM_PLL
static void stm32_modifyreg32(unsigned int addr, uint32_t clrbits,
                              uint32_t setbits)
{
  putreg32((getreg32(addr) & ~clrbits) | setbits, addr);
}
#endif

/****************************************************************************
 * Name: hrtim_getreg
 *
 * Description:
 *   Read the value of an HRTIM register.
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM block
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *   The current contents of the specified register
 *
 ****************************************************************************/

static uint32_t hrtim_getreg(FAR struct stm32_hrtim_s *priv, int offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: hrtim_putreg
 *
 * Description:
 *   Write a value to an HRTIM register.
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM block
 *   offset - The offset to the register to write to
 *   value  - The value to write to the register
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void hrtim_putreg(FAR struct stm32_hrtim_s *priv, int offset,
                             uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: hrtim__modifyreg
 *
 * Description:
 *   Modify the value of an HRTIM register (not atomic).
 *
 * Input Parameters:
 *   priv    - A reference to the HRTIM block
 *   offset  - The offset to the register to modify
 *   clrbits - The bits to clear
 *   setbits - The bits to set
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void hrtim_modifyreg(FAR struct stm32_hrtim_s *priv, int offset,
                                uint32_t clrbits, uint32_t setbits)
{
  hrtim_putreg(priv, offset, (hrtim_getreg(priv, offset) & ~clrbits) | setbits);
}


/****************************************************************************
 * Name: hrtim_tim_get
 *
 * Description:
 *   Get Timer data structure for given HRTIM Timer index
 *
 * Input Parameters:
 *   priv    - A reference to the HRTIM block
 *   index   - An HRTIM Timer index to get
 *
 * Returned Value:
 *   Base adress offset for given timer index
 *
 ****************************************************************************/

static FAR struct stm32_hrtim_tim_s *hrtim_tim_get(FAR struct stm32_hrtim_s *priv, uint8_t index)
{
  FAR struct stm32_hrtim_tim_s *tim;

  switch (index)
    {
      case HRTIM_TIMER_MASTER:
        {
          tim = priv->master;
          break;
        }

#ifdef CONFIG_STM32_HRTIM_TIMA
      case HRTIM_TIMER_TIMA:
        {
          tim = priv->tima;
          break;
        }
#endif

#ifdef CONFIG_STM32_HRTIM_TIMB
      case HRTIM_TIMER_TIMB:
        {
          tim = &priv->timb;
          break;
        }
#endif

#ifdef CONFIG_STM32_HRTIM_TIMC
      case HRTIM_TIMER_TIMC:
        {
          tim = &priv->timc;
          break;
        }
#endif

#ifdef CONFIG_STM32_HRTIM_TIMD
      case HRTIM_TIMER_TIMD:
        {
          tim = &priv->timd;
          break;
        }
#endif

#ifdef CONFIG_STM32_HRTIM_TIME
      case HRTIM_TIMER_TIME:
        {
          tim = &priv->time;
          break;
        }
#endif

      default:
        {
          tmrerr("ERROR: No such timerx index: %d\n", index);
          tim = NULL;
        }
    }

  return tim;
}

/****************************************************************************
 * Name: hrtim_base_get
 *
 * Description:
 *   Get base adress offset for given HRTIM Timer index
 *
 * Input Parameters:
 *   priv    - A reference to the HRTIM block
 *   index   - An HRTIM Timer index to get
 *
 * Returned Value:
 *   Base adress offset for given timer index
 *
 ****************************************************************************/

static uint32_t hrtim_base_get(FAR struct stm32_hrtim_s* priv, uint8_t index)
{
  FAR struct stm32_hrtim_tim_s* tim;
  uint32_t base;

  tim = hrtim_tim_get(priv,index);
  if (tim == NULL)
    {
      base = 0;
      goto errout;
    }

  base = tim->tim.base;

errout:
  return base;
}

/****************************************************************************
 * Name: hrtim_tim_getreg
 *
 * Description:
 *   Read the value of an HRTIM Timer register.
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM block
 *   tim    - An HRTIM timer index
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *   The current contents of the specified register
 *
 ****************************************************************************/

static uint32_t hrtim_tim_getreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                                 int offset)
{
  uint32_t base;

  base = hrtim_base_get(priv, index);
  if (base < 0)
    {
      return 0;
    }

  return getreg32(base + offset);
}

/****************************************************************************
 * Name: hrtim_tim_putreg
 *
 * Description:
 *   Write a value to an HRTIM Timer register.
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM block
 *   index    - An HRTIM timer index
 *   offset - The offset to the register to write to
 *   value  - The value to write to the register
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void hrtim_tim_putreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                             int offset, uint32_t value)
{
  uint32_t base;

  base = hrtim_base_get(priv, index);
  if (base > 0)
    {
      putreg32(value, base + offset);
    }
}

/****************************************************************************
 * Name: hrtim_tim_modifyreg
 *
 * Description:
 *   Modify the value of an HRTIM Timer register (not atomic).
 *
 * Input Parameters:
 *   priv    - A reference to the HRTIM block
 *   index    - An HRTIM timer index
 *   offset  - The offset to the register to modify
 *   clrbits - The bits to clear
 *   setbits - The bits to set
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void hrtim_tim_modifyreg(FAR struct stm32_hrtim_s *priv, uint8_t index,
                                int offset, uint32_t clrbits, uint32_t setbits)
{
  hrtim_tim_putreg(priv, index, offset,
                   (hrtim_tim_getreg(priv, index, offset) & ~clrbits) | setbits);
}

/****************************************************************************
 * Name: stm32_dll_cal
 *
 * Description:
 *   Calibrate HRTIM DLL
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

static int hrtim_dll_cal(FAR struct stm32_hrtim_s *priv)
{
  uint32_t regval = 0;

#ifdef CONFIG_STM32_HRTIM_PERIODIC_CAL

  /* Configure calibration rate */

  regval |= HRTIM_DLLCR_CAL_RATE;

  /* Enable Periodic calibration */

  regval |= HRTIM_DLLCR_CALEN;

#endif

  /* DLL Calibration Start */

  regval |= HRTIM_DLLCR_CAL;

  hrtim_putreg(priv, STM32_HRTIM_CMN_DLLCR, regval);

  /* Wait for HRTIM ready flag */

  while(!(hrtim_getreg(priv, STM32_HRTIM_CMN_ISR) & HRTIM_ISR_DLLRDY));

  return OK;
}

/****************************************************************************
 * Name: stm32_tim_clock_config
 *
 * Description:
 *   Configure HRTIM Timer clock
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *   index  - An HRTIM timer index
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

static int hrtim_tim_clock_config(FAR struct stm32_hrtim_s *priv, uint8_t index, uint8_t pre)
{
  int ret = OK;
  uint32_t regval = 0;

  regval = hrtim_tim_getreg(priv, index, STM32_HRTIM_TIM_CR_OFFSET);

  switch (pre)
    {
      case HRTIM_PRESCALER_1:
        {
          regval |= HRTIM_CMNCR_CKPSC_NODIV;
          break;
        }
      case HRTIM_PRESCALER_2:
        {
          regval |= HRTIM_CMNCR_CKPSC_d2;
          break;
        }
      case HRTIM_PRESCALER_4:
        {
          regval |= HRTIM_CMNCR_CKPSC_d4;
          break;
        }
      case HRTIM_PRESCALER_8:
        {
          regval |= HRTIM_CMNCR_CKPSC_d8;
          break;
        }
      case HRTIM_PRESCALER_16:
        {
          regval |= HRTIM_CMNCR_CKPSC_d16;
          break;
        }
      case HRTIM_PRESCALER_32:
        {
          regval |= HRTIM_CMNCR_CKPSC_d32;
          break;
        }
      case HRTIM_PRESCALER_64:
        {
          regval |= HRTIM_CMNCR_CKPSC_d64;
          break;
        }
      case HRTIM_PRESCALER_128:
        {
          regval |= HRTIM_CMNCR_CKPSC_d128;
          break;
        }
      default:
        {
          tmrerr("ERROR: invalid prescaler value %d for timer %d\n", index,
                   pre);
          ret = -EINVAL;
          goto errout;
        }
    }

errout:
  return ret;
}


/****************************************************************************
 * Name: stm32_tim_clocks_config
 *
 * Description:
 *   Configure HRTIM Timers Clocks
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

static int hrtim_tim_clocks_config(FAR struct stm32_hrtim_s *priv)
{
  int ret = OK;

  /* Configure Master Timer clock */

  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_MASTER, HRTIM_MASTER_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }

  /* Configure Timer A clock */

#ifdef CONFIG_STM32_HRTIM_TIMA
  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_TIMA, HRTIM_TIMA_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure Timer B clock */

#ifdef CONFIG_STM32_HRTIM_TIMB
  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_TIMB, HRTIM_TIMB_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure Timer C clock */

#ifdef CONFIG_STM32_HRTIM_TIMC
  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_TIMC, HRTIM_TIMC_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure Timer D clock */

#ifdef CONFIG_STM32_HRTIM_TIMD
  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_TIMD, HRTIM_TIMD_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure Timer E clock */

#ifdef CONFIG_STM32_HRTIM_TIME
  ret = hrtim_tim_clock_config(priv, HRTIM_TIMER_TIME, HRTIM_TIME_PRESCALER);
  if (ret < 0)
    {
      goto errout;
    }
#endif

errout:
  return ret;
}

/****************************************************************************
 * Name: stm32_gpios_config
 *
 * Description:
 *   Configure HRTIM GPIO
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#if defined(HRTIM_HAVE_CAPTURE) || defined(HRTIM_HAVE_PWM) || defined(HRTIM_HAVE_SYNC)
static int hrtim_gpios_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_gpios_config: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_inputs_config
 *
 * Description:
 *   Configure HRTIM Inputs
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#if defined(HRTIM_HAVE_CAPTURE)
static int hrtim_inputs_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_inputs_config: missing logic"

  /* source */

  /* polarity */

  /* edge-sensitivity */

  return OK;
}
#endif


/****************************************************************************
 * Name: stm32_synch_config
 *
 * Description:
 *   Configure HRTIM Synchronization Input/Output
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#if defined(HRTIM_HAVE_SYNC)
static int hrtim_synch_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_synch_config: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_tim_outputs_config
 *
 * Description:
 *   Configure HRTIM Slave Timer Outputs (CH1 and CH2)
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#if defined(HRTIM_HAVE_PWM)
static int hrtim_tim_outputs_config(FAR struct stm32_hrtim_s *priv, uint8_t index)
{
  FAR struct stm32_hrtim_tim_s* tim;
  FAR struct stm32_hrtim_slave_priv_s* slave;

  int ret = OK;
  uint32_t regval = 0;

  /* Master Timer has no outputs */

  if (index == HRTIM_TIMER_MASTER)
    {
      ret = -EINVAL;
      goto errout;
    }

  /* Get Timer data strucutre */

  tim = hrtim_tim_get(priv, index);
  if (tim == NULL)
    {
      ret = -EINVAL;
      goto errout;
    }

  slave = (struct stm32_hrtim_slave_priv_s*)tim->priv;

  /* Configure CH1 SET events */

  regval = slave->pwm.ch1.set;
  hrtim_tim_putreg(priv, index, STM32_HRTIM_TIM_SET1R_OFFSET, regval);

  /* Configure CH1 RESET events */

  regval = slave->pwm.ch1.rst;
  hrtim_tim_putreg(priv, index, STM32_HRTIM_TIM_RST1R_OFFSET, regval);

  /* Configure CH2 SET events */

  regval = slave->pwm.ch2.set;
  hrtim_tim_putreg(priv, index, STM32_HRTIM_TIM_SET2R_OFFSET, regval);

  /* Configure CH2 RESET events */

  regval = slave->pwm.ch2.rst;
  hrtim_tim_putreg(priv, index, STM32_HRTIM_TIM_RST2R_OFFSET, regval);

errout:
  return ret;
}
#endif

/****************************************************************************
 * Name: stm32_outputs_config
 *
 * Description:
 *   Configure HRTIM Outputs
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#if defined(HRTIM_HAVE_PWM)
static int hrtim_outputs_config(FAR struct stm32_hrtim_s *priv)
{
  int ret = OK;

  /* Configure HRTIM TIMER A Outputs */

#ifdef CONFIG_STM32_HRTIM_TIMA_PWM
  ret = hrtim_tim_outputs_config(priv, HRTIM_TIMER_TIMA);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure HRTIM TIMER B Outputs */

#ifdef CONFIG_STM32_HRTIM_TIMB_PWM
  ret = hrtim_tim_outputs_config(priv, HRTIM_TIMER_TIMB);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure HRTIM TIMER C Outputs */

#ifdef CONFIG_STM32_HRTIM_TIMC_PWM
  ret = hrtim_tim_outputs_config(priv, HRTIM_TIMER_TIMC);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure HRTIM TIMER D Outputs */

#ifdef CONFIG_STM32_HRTIM_TIMD_PWM
  ret = hrtim_tim_outputs_config(priv, HRTIM_TIMER_TIMD);
  if (ret < 0)
    {
      goto errout;
    }
#endif

  /* Configure HRTIM TIMER E Outputs */

#ifdef CONFIG_STM32_HRTIM_TIME_PWM
  ret = hrtim_tim_outputs_config(priv, HRTIM_TIMER_TIME);
  if (ret < 0)
    {
      goto errout;
    }
#endif

errout:
  return ret;
}
#endif

/****************************************************************************
 * Name: stm32_adc_config
 *
 * Description:
 *   Configure HRTIM ADC triggers
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#ifdef HRTIM_HAVE_ADC
static int hrtim_adc_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_adc_config: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_faults_config
 *
 * Description:
 *   Configure HRTIM Faults
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#ifdef HRTIM_HAVE_FAULTS
static int hrtim_faults_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_faults_config: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_eev_config
 *
 * Description:
 *   Configure HRTIM External Events
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#ifdef HRTIM_HAVE_EEV
static int hrtim_eev_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_eev_confi: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_irq_config
 *
 * Description:
 *   Configure HRTIM interrupts
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

#ifdef HRTIM_HAVE_INTERRUPTS
static int hrtim_irq_config(FAR struct stm32_hrtim_s *priv)
{
#warning "hrtim_irq_config: missing logic"
  return OK;
}
#endif

/****************************************************************************
 * Name: stm32_preload_config
 *
 * Description:
 *   Configure HRTIM preload registers
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  None
 *
 ****************************************************************************/

static void hrtim_preload_config(FAR struct stm32_hrtim_s *priv)
{

#ifndef CONFIG_STM32_HRTIM_MASTER_PRELOAD_DIS
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_MASTER, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

#if defined(CONFIG_ST32_HRTIM_TIMA) && defined(CONFIG_STM32_HRTIM_TIMA_PRELOAD_DIS)
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_TIMA, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

#if defined(CONFIG_ST32_HRTIM_TIMB) && defined(CONFIG_STM32_HRTIM_TIMB_PRELOAD_DIS)
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_TIMB, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

#if defined(CONFIG_ST32_HRTIM_TIMC) && defined(CONFIG_STM32_HRTIM_TIMC_PRELOAD_DIS)
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_TIMC, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

#if defined(CONFIG_ST32_HRTIM_TIMD) && defined(CONFIG_STM32_HRTIM_TIMD_PRELOAD_DIS)
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_TIMD, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

#if defined(CONFIG_ST32_HRTIM_TIME) && defined(CONFIG_STM32_HRTIM_TIME_PRELOAD_DIS)
  hrtim_tim_modifyreg(priv, HRTIM_TIMER_TIME, STM32_HRTIM_TIM_CR_OFFSET,
                      0, HRTIM_CMNCR_PREEN);
#endif

}

/****************************************************************************
 * Name: stm32_hrtimconfig
 *
 * Description:
 *   Configure HRTIM
 *
 * Input Parameters:
 *   priv   - A reference to the HRTIM structure
 *
 * Returned Value:
 *  0 on success, a negated errno value on failure
 *
 ****************************************************************************/

static int stm32_hrtimconfig(FAR struct stm32_hrtim_s *priv)
{
  int ret;
  uint32_t regval = 0;

  /* Configure PLL VCO output as HRTIM clock source */

#ifdef HRTIM_HAVE_CLK_FROM_PLL
  stm32_modifyreg32(STM32_RCC_CFGR3, 0, RCC_CFGR3_HRTIM1SW);
#endif

  /* HRTIM DLL calibration */

  ret = hrtim_dll_cal(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM DLL calibration failed!\n");
      goto errout;
    }

  /* Configure Timers Clocks */

  ret = hrtim_tim_clocks_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM timers clock configuration failed!\n");
      goto errout;
    }

  /* Configure HRTIM GPIOs */

#if defined(HRTIM_HAVE_CAPTURE) || defined(HRTIM_HAVE_PWM) || defined(HRTIM_HAVE_SYNC)
  ret = hrtim_gpios_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM GPIOs configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure HRTIM inputs */

#if defined(HRTIM_HAVE_CAPTURE)
  ret = hrtim_inputs_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM inputs configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure Synchronisation IOs */

#if defined(HRTIM_HAVE_SYNC)
  ret = hrtim_synch_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM synchronisation configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure HRTIM outputs GPIOs */

#if defined(HRTIM_HAVE_PWM)
  ret = hrtim_outputs_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM outputs configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure ADC triggers */

#ifdef HRTIM_HAVE_ADC
  ret = hrtim_adc_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM ADC configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure Faults */

#ifdef HRTIM_HAVE_FAULTS
  ret = hrtim_faults_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM faults configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure Events */

#ifdef HRTIM_HAVE_EEV
  ret = hrtim_eev_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM EEV configuration failed!\n");
      goto errout;
    }
#endif

  /* Configure interrupts */

#ifdef HRTIM_HAVE_INTERRUPTS
  ret = hrtim_irq_config(priv);
  if (ret != OK)
    {
      tmrerr("ERROR: HRTIM IRQ configuration failed!\n");
      goto errout;
    }
#endif

  /* Enable registers preload */

  hrtim_preload_config(priv);

  /* Enable Master Timer */

  regval |= HRTIM_MCR_MCEN;

  /* Enable Slave Timers */

#ifdef CONFIG_STM32_HRTIM_TIMA
  regval |= HRTIM_MCR_TACEN;
#endif

#ifdef CONFIG_STM32_HRTIM_TIMB
  regval |= HRTIM_MCR_TBCEN;
#endif

#ifdef CONFIG_STM32_HRTIM_TIMC
  regval |= HRTIM_MCR_TCCEN;
#endif

#ifdef CONFIG_STM32_HRTIM_TIMD
  regval |= HRTIM_MCR_TDCEN;
#endif

#ifdef CONFIG_STM32_HRTIM_TIME
  regval |= HRTIM_MCR_TECEN;
#endif

  /* Write enable bits at once */

  hrtim_tim_modifyreg(priv, HRTIM_TIMER_MASTER, STM32_HRTIM_TIM_CR_OFFSET, 0, regval);

errout:
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_hrtiminitialize
 *
 * Description:
 *   Initialize the HRTIM.
 *
 * Returned Value:
 *   Valid HRTIM device structure reference on succcess; a NULL on failure.
 *
 * Assumptions:
 *   1. Clock to the HRTIM block has enabled,
 *   2. Board-specific logic has already configured
 *
 ****************************************************************************/

FAR struct hrtim_dev_s* stm32_hrtiminitialize(void)
{
  FAR struct hrtim_dev_s  *dev;
  FAR struct stm32_hrtim_s *hrtim;
  int ret;

  dev = &g_hrtim1dev;

  hrtim = dev->hd_priv;

  ret = stm32_hrtimconfig(hrtim);
  if (ret < 0)
    {
      tmrerr("ERROR: Failed to initialize HRTIM1: %d\n", ret);
      errno = -ret;
      return NULL;
    }

  return dev;
}

/****************************************************************************
 * Name: hrtim_register
 ****************************************************************************/

int hrtim_register(FAR const char *path, FAR struct hrtim_dev_s *dev)
{
  int ret ;

  /* Initialize the HRTIM device structure */

  dev->hd_ocount = 0;

  /* Initialize semaphores */

  sem_init(&dev->hd_closesem, 0, 1);

  /* Register the HRTIM character driver */

  ret =  register_driver(path, &hrtim_fops, 0444, dev);
  if (ret < 0)
    {
      sem_destroy(&dev->hd_closesem);
    }

  return ret;
}

#endif  /* CONFIG_STM32_STM32F33XX */

#endif  /* CONFIG_STM32_HRTIM1 */