/*
 * Copyright (C) 2016-2018 Unwired Devices LLC <info@unwds.com>

 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software
 * is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file        umdk-usonic.c
 * @brief       umdk-usonic module implementation
 * @author      Dmitry Golik <info@unwds.com>
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_USONIC_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "usonic"

#include "periph/gpio.h"

#include "board.h"

#include "usonicrange.h"

#include "umdk-ids.h"
#include "unwds-common.h"
#include "umdk-usonic.h"
#include "unwds-gpio.h"

#include "thread.h"
#include "lptimer.h"

static usonicrange_t usonic_dev;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;
static kernel_pid_t timer_24hrs_pid;

static msg_t timer_msg = {};
static msg_t timer_24hrs_msg = {};
static lptimer_t timer;
static lptimer_t timer_24hrs;

static bool is_polled = false;

static struct {
    uint8_t is_valid;
    uint8_t publish_period_min;
    uint16_t sensitivity;
    uint16_t min_distance;
    uint16_t max_distance;
    uint16_t threshold;
    uint8_t  mode;
} usonicrange_config;

static bool init_sensor(void) {
    printf("[umdk-" _UMDK_NAME_ "] Initializing usonic distance meter\n");
    return true;
}

static int prepare_result(module_data_t *buf) {
    gpio_init(GPIO_PIN(PORT_B, 1), GPIO_OUT);
    gpio_clear(GPIO_PIN(PORT_B, 1));
    
    lptimer_sleep(500);
    
    //usonicrange_calibrate(&usonic_dev);
    int range = usonicrange_measure(&usonic_dev);
    
    gpio_set(GPIO_PIN(PORT_B, 1));
    
    if (range > 0) {
        printf("[umdk-" _UMDK_NAME_ "] Distance %d mm\n", range);
    } else {
        switch (range) {
            case -USONICRANGE_MINDISTANCE:
                puts("[umdk-" _UMDK_NAME_ "] Obstacle too close");
                break;
            case -USONICRANGE_MAXDISTANCE:
                puts("[umdk-" _UMDK_NAME_ "] No obstacles in sight");
                break;
            default:
                printf("[umdk-" _UMDK_NAME_ "] Unknown sensor error %d\n", range);
                break;
        }
    }

    if (buf) {
        buf->length = 1 + sizeof(range); /* Additional byte for module ID */
        buf->data[0] = _UMDK_MID_;
        /* Copy measurements into response */
        memcpy(buf->data + 1, (uint8_t *) &range, sizeof(range));
    }
    
    return range;
}

static void *timer_thread(void *arg) {
    (void)arg;
    
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    puts("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        int range = prepare_result(&data);

        if ((usonicrange_config.mode == UMDK_USONIC_MODE_DISTANCE) ||
            ((usonicrange_config.mode == UMDK_USONIC_MODE_THRESHOLD) &&
                (range < usonicrange_config.threshold) && (range > 0))) {
                    
            /* Notify the application */
            callback(&data);
        } else {
            puts("[umdk-" _UMDK_NAME_ "] Distance above threshold, ignoring");
        }

        /* Restart after delay */
        if (usonicrange_config.publish_period_min) {
            lptimer_set_msg(&timer, 60 * 1000 * usonicrange_config.publish_period_min, &timer_msg, timer_pid);
        }
    }

    return NULL;
}

static void *timer_24hrs_thread(void *arg) {
    (void)arg;
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    puts("[umdk-" _UMDK_NAME_ "] 24 hrs publisher thread started");

    while (1) {
        msg_receive(&msg);
        
        /* Send empty data every 24 hrs to check usonic_device's status */

        module_data_t data = {};
        data.as_ack = false;
        data.length = 5;
        data.data[0] = _UMDK_MID_;
        memset(&data.data[1], 0, 4);
        
        callback(&data);
        
        /* Restart after delay */
        lptimer_set_msg(&timer_24hrs, 1000 * 60*60*24, &timer_24hrs_msg, timer_24hrs_pid);
    }

    return NULL;
}

static inline void save_config(void) {
    usonicrange_config.is_valid = 1;
    unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &usonicrange_config, sizeof(usonicrange_config));
}

static void reset_config(void) {
    usonicrange_config.publish_period_min = 15;
    usonicrange_config.sensitivity = 50;
    usonicrange_config.min_distance = 400;
    usonicrange_config.max_distance = 6000;
    usonicrange_config.threshold = 500;
    usonicrange_config.mode = UMDK_USONIC_MODE_DISTANCE;
    
    save_config();
    return;
}

static void init_config(void) {
    if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &usonicrange_config, sizeof(usonicrange_config))) {
        reset_config();
        return;
    }

    if ((usonicrange_config.is_valid == 0xFF) || (usonicrange_config.is_valid == 0)) {
        reset_config();
        return;
    }
}

static void set_period (int period) {
    lptimer_remove(&timer);

    usonicrange_config.publish_period_min = period;
    save_config();

    /* Don't restart timer if new period is zero */
    if (usonicrange_config.publish_period_min) {
        lptimer_set_msg(&timer, 60 * 1000 * usonicrange_config.publish_period_min, &timer_msg, timer_pid);
        printf("[umdk-" _UMDK_NAME_ "] Period set to %d minutes\n", usonicrange_config.publish_period_min);
    } else {
        printf("[umdk-" _UMDK_NAME_ "] Timer stopped");
    }
}

static void umdk_usonic_print_settings(void) {
    puts("[umdk-" _UMDK_NAME_ "] Current settings:");
    printf("period: %d m\n", usonicrange_config.publish_period_min);
    printf("sens: %d\n", usonicrange_config.sensitivity);
    printf("min: %d mm\n", usonicrange_config.min_distance);
    printf("max: %d mm\n", usonicrange_config.max_distance);
    printf("mode: %s\n", (usonicrange_config.mode == UMDK_USONIC_MODE_DISTANCE)? "distance":"threshold");
    printf("threshold: %d mm\n", usonicrange_config.threshold);
}

int umdk_usonic_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts (_UMDK_NAME_ " - usonic rangefinder");
        puts (_UMDK_NAME_ " get - get results now");
        puts (_UMDK_NAME_ " send - get and send results now");
        puts (_UMDK_NAME_ " period <N> - set period to N minutes");
        puts (_UMDK_NAME_ " sens <N> - set echo detection sensitivity");
        puts (_UMDK_NAME_ " min <N> - set minimum distance in mm");
        puts (_UMDK_NAME_ " max <N> - set maximum distance in mm");
        puts (_UMDK_NAME_ " mode <distance|threshold> - set sensor mode");
        puts (_UMDK_NAME_ " threshold <N> - set threshold in mm for threshold mode");
        puts (_UMDK_NAME_ " reset - reset settings to default\n");

        umdk_usonic_print_settings();
        return 0;
    }
    
    char *cmd = argv[1];
    
    if (strcmp(cmd, "get") == 0) {
        prepare_result(NULL);
        return 1;
    }

    if (strcmp(cmd, "send") == 0) {
        is_polled = true;
        /* Send signal to publisher thread */
        msg_send(&timer_msg, timer_pid);
        return 1;
    }
    
    if (strcmp(cmd, "period") == 0) {
        int val = atoi(argv[2]);
        set_period(val);
        return 1;
    }
    
    if (strcmp(cmd, "sens") == 0) {
        int val = atoi(argv[2]);
        usonicrange_config . sensitivity = val;
//        usonic_dev                    . sensitivity = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "min") == 0) {
        int val = atoi(argv[2]);
        usonicrange_config . min_distance = val;
//        usonic_dev                    . min_distance = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "max") == 0) {
        int val = atoi(argv[2]);
        usonicrange_config . max_distance = val;
//        usonic_dev                    . max_distance = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "threshold") == 0) {
        int val = atoi(argv[2]);
        usonicrange_config . threshold = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "mode") == 0) {
        if (strcmp(argv[2], "threshold") == 0) {
            puts("[umdk-" _UMDK_NAME_ "] Threshold mode");
            usonicrange_config.mode = UMDK_USONIC_MODE_THRESHOLD;
        } else {
            if (strcmp(argv[2], "distance") == 0) {
                usonicrange_config.mode = UMDK_USONIC_MODE_DISTANCE;
                puts("[umdk-" _UMDK_NAME_ "] Distance mode");
            } else {
                puts("[umdk-" _UMDK_NAME_ "] Unknown mode");
            }
        }
        save_config();
        return 1;
    }

    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
        return 1;
    }

    puts("[umdk-" _UMDK_NAME_ "] Unknown command");
    
    return 1;
}

void umdk_usonic_init(uwnds_cb_t *event_callback) {

    callback = event_callback;

    init_config();
    umdk_usonic_print_settings();

    if (!init_sensor()) {
        printf("[umdk-" _UMDK_NAME_ "] Unable to init sensor!");
        return;
    }
    
	/* Create handler thread */
	char *stack_24hrs = (char *) allocate_stack(UMDK_USONIC_STACK_SIZE);
	if (!stack_24hrs) {
		return;
	}
    
    timer_24hrs_pid = thread_create(stack_24hrs, UMDK_USONIC_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_24hrs_thread, NULL, "usonic 24hrs thread");
    
    /* Start 24 hrs timer */
    lptimer_set_msg(&timer_24hrs, 1000 * 60 * 60 * 24, &timer_24hrs_msg, timer_24hrs_pid);

    char *stack = (char *) allocate_stack(UMDK_USONIC_STACK_SIZE);
	if (!stack) {
		return;
	}
    
    /* double sized to accomodate UINT16_T */
    usonic_dev.dmabuffer = (void *) allocate_stack(2*USONICRANGE_DMABUF_SIZE);
	if (!usonic_dev.dmabuffer) {
		return;
	}
    
    /* double sized to accomodate UINT16_T */
    usonic_dev.signalbuffer = (void *) allocate_stack(2*USONICRANGE_SIGNALBUF_SIZE);
	if (!usonic_dev.signalbuffer) {
		return;
	}
    
    usonic_dev.timer = 1;
    usonic_dev.pwm = 0;
    usonic_dev.pwm_channel = 3;
    usonic_dev.adc = 3;
    usonic_dev.frequency = 40000;
    usonic_dev.temperature = USONICRANGE_TEMPERATURE_NONE;
    usonic_dev.mode = USONICRANGE_MODE_STRONGEST_ECHO;
    usonic_dev.signal_pin = GPIO_PIN(PORT_A, 1);
    usonic_dev.suppress_pin = GPIO_PIN(PORT_A, 2);
    usonic_dev.suppress_time = USONICRANGE_SUPPRESS_PERIOD_US;
    
    usonicrange_init(&usonic_dev);

    timer_pid = thread_create(stack, UMDK_USONIC_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "usonic thread");
    
    /* Start publishing timer */
    if (usonicrange_config.publish_period_min) {
        lptimer_set_msg(&timer, usonicrange_config.publish_period_min * 1000 * 60, &timer_msg, timer_pid);
    }
    
    unwds_add_shell_command("usonic", "type 'usonic' for commands list", umdk_usonic_shell_cmd);
}

static void reply_fail(module_data_t *reply) {
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = 255;
}

static void reply_ok(module_data_t *reply) {
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = 0;
}

bool umdk_usonic_cmd(module_data_t *cmd, module_data_t *reply) {
    if (cmd->length < 1) {
        reply_fail(reply);
        return true;
    }

    umdk_usonic_cmd_t c = cmd->data[0];
    switch (c) {
    case UMDK_USONIC_CMD_SET_PERIOD: {
        if (cmd->length != 2) {
            reply_fail(reply);
            break;
        }

        uint8_t period = cmd->data[1];
        set_period(period);

        reply_ok(reply);
        break;
    }

    case UMDK_USONIC_CMD_POLL:
        is_polled = true;

        /* Send signal to publisher thread */
        msg_send(&timer_msg, timer_pid);

        return false; /* Don't reply */

    case UMDK_USONIC_CMD_INIT_SENSOR: {
        init_sensor();

        reply_ok(reply);
        break;
    }

    default:
        reply_fail(reply);
        break;
    }

    return true;
}

#ifdef __cplusplus
}
#endif
