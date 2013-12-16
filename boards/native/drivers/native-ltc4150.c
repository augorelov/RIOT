/**
 * Native Board ltc4150_arch.h implementation
 *
 * Only measures time at the moment. Uses POSIX real-time extension
 * timer to generate periodic signal/interrupt.
 *
 * Copyright (C) 2013 Ludwig Ortmann
 *
 * This file is subject to the terms and conditions of the LGPLv2. See
 * the file LICENSE in the top level directory for more details.
 *
 * @ingroup native_board
 * @ingroup ltc4150
 * @{
 * @file
 * @author  Ludwig Ortmann <ludwig.ortmann@fu-berlin.de>
 */

#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <err.h>

#include "ltc4150_arch.h"
#include "debug.h"

#include "cpu.h"
#include "cpu-conf.h"
#include "hwtimer.h"

#define LTC_TIMER_INTERVAL (10 * 1000UL) // 10 ms

static int _int_enabled;
static int hwtimer_id;

/**
 * native ltc4150 hwtimer - interrupt handler proxy
 */
static void _int_handler()
{
    DEBUG("ltc4150 _int_handler()\n");
    ltc4150_interrupt();
    if (_int_enabled == 1) {
        hwtimer_id = hwtimer_set(LTC_TIMER_INTERVAL, _int_handler, NULL);
        if (hwtimer_id == -1) {
            errx(1, "_int_handler: hwtimer_set");
        }
    }
}

/**
 * unregister signal handler
 */
void ltc4150_disable_int(void)
{
    DEBUG("ltc4150_disable_int()\n");
    _int_enabled = 0;
    hwtimer_remove(hwtimer_id);
}

/**
 * register signal handler
 */
void ltc4150_enable_int(void)
{
    DEBUG("ltc4150_enable_int()\n");
    _int_enabled = 1;
    hwtimer_id = hwtimer_set(LTC_TIMER_INTERVAL, _int_handler, NULL);
    if (hwtimer_id == -1) {
        errx(1, "ltc4150_enable_int: hwtimer_set");
    }
}

/**
 * elaborate nop
 */
void ltc4150_sync_blocking(void)
{
    DEBUG("ltc4150_sync_blocking()\n");
}

/**
 * set up posix real-time timer to simulate coloumb counter ticks
 */
void ltc4150_arch_init(void)
{
    ltc4150_disable_int();

    puts("Native LTC4150 initialized.");
}
/** @} */
