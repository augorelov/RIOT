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
 * @file        umdk-st95.c
 * @brief       umdk-st95 module implementation
 * @author      Mikhail Perkov

 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_ST95_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "st95"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "board.h"

#include "st95.h"

#include "umdk-ids.h"
#include "unwds-common.h"
#include "include/umdk-st95.h"

#include "thread.h"
#include "lptimer.h"

#define ENABLE_DEBUG (1)
#include "debug.h"

static msg_t msg_wu = { .type = UMDK_ST95_MSG_EVENT, };
static msg_t msg_rx = { .type = UMDK_ST95_MSG_UID, };

static kernel_pid_t radio_pid;
static uwnds_cb_t *callback;

static st95_t dev;

static st95_params_t st95_params = { .iface = ST95_IFACE_UART,
                                .uart = UMDK_ST95_UART_DEV, .baudrate = UMDK_ST95_UART_BAUD_DEF,
                                .spi = UMDK_ST95_SPI_DEV, .cs_spi = UMDK_ST95_SPI_CS, 
                                .irq_in = UMDK_ST95_IRQ_IN, .irq_out = UMDK_ST95_IRQ_OUT, 
                                .ssi_0 = UMDK_ST95_SSI_0, .ssi_1 = UMDK_ST95_SSI_1,
                                .vcc = UMDK_ST95_VCC_ENABLE };

static uint8_t length_uid = 0;
static uint8_t uid_full[10];
static uint8_t sak = 0;

static volatile uint8_t mode = UMDK_ST95_MODE_GET_UID;
static volatile uint8_t status = UMDK_ST95_STATUS_READY;

// static uint8_t ndef_data[255] = { 0x00 };
static uint8_t buff_data[10] = { 0x00 };


static void umdk_st95_get_uid(void);
static void umdk_st95_set_uid(void);

#if ENABLE_DEBUG
    #define PRINTBUFF _printbuff
    static void _printbuff(uint8_t *buff, unsigned len)
    {
        while (len) {
            len--;
            printf("%02X ", *buff++);
        }
        printf("\n");
    }
#else
    #define PRINTBUFF(...)
#endif


static void *radio_send(void *arg)
{
    (void) arg;
    msg_t msg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);
      
    while (1) {
        msg_receive(&msg);
        
        module_data_t data;
        data.as_ack = false;
        data.data[0] = _UMDK_MID_;
        data.length = 1;

        switch(msg.type) {
            case UMDK_ST95_MSG_EVENT: {
                if(mode == UMDK_ST95_MODE_SET_UID) {
                    if(st95_is_field_detect(&dev) != ST95_ERROR) {   
                        umdk_st95_set_uid();
                    }
                }
                else {
                    if(st95_is_wake_up(&dev) == ST95_WAKE_UP) {
                        if(mode == UMDK_ST95_MODE_READ_DATA) {
                            if(st95_read_data(&dev, buff_data , sizeof(buff_data)) == ST95_ERROR) {
                                puts("Read ERR");
                            }
                            else {
                                printf("Data: ");
                                PRINTBUFF(buff_data, sizeof(buff_data));
                            }
                        lptimer_sleep(UMDK_ST95_DELAY_DETECT_MS);
                        st95_sleep(&dev);
                        }
                        else {
                            umdk_st95_get_uid();
                        }                     
                    }                       
                }
                                
                break;
            }
            case UMDK_ST95_MSG_UID: {
                if(msg.content.value == UMDK_ST95_UID_OK) {
                   
                    memcpy(data.data + 1, uid_full, length_uid);
                    data.length += length_uid;
                }
                else {
                    DEBUG("[ERROR]\n");
                    PRINTBUFF(uid_full, length_uid);
                    
                    data.data[1] = UMDK_ST95_ERROR_REPLY;
                    data.length = 2;
                }
                
                DEBUG("RADIO: ");
                PRINTBUFF(data.data, data.length);

                callback(&data);
                
                if(mode == UMDK_ST95_MODE_DETECT_TAG) {
                    lptimer_sleep(UMDK_ST95_DELAY_DETECT_MS);
                    st95_sleep(&dev);
                }
                status = UMDK_ST95_STATUS_READY;
                break;
            }
                        
            default: 
            break;            
        }
    }
    return NULL;
}

static void umdk_st95_get_uid(void)
{
    length_uid = 0;
    sak = 0;
    memset(uid_full, 0x00, sizeof(uid_full));

    if(st95_get_uid(&dev, &length_uid, uid_full, &sak) == ST95_OK) {
        msg_rx.content.value = UMDK_ST95_UID_OK;        
    }
    else {
        length_uid = 0;
        msg_rx.content.value = UMDK_ST95_UID_ERROR;
    }
    
    msg_try_send(&msg_rx, radio_pid);
}

static void umdk_st95_set_uid(void)
{
    uint8_t length = 0;
    uint8_t atqa[2] = { 0 };
    uint8_t sak = 0;
    uint8_t uid[10] = { 0 };
    
    /*ATQA first byte. 
    Default value:  Single size(4 bytes):  0x04 
                    Double size(7 bytes):  0x44 
                    Triple size(10 bytes): 0x84*/
    atqa[0] = 0x44; // ATQA
    /*ATQA second byte. Default value: 0x00 */
    atqa[1] = 0x00; // ATQA
    
    /*SAK byte. 
    Single size(4 bytes):  0x08 
    Double size(7 bytes):  0x20
    Triple size(10 bytes): 0x00 */
    sak = 0x20; // SAK

    uid[0] = 0xDE; // UID
    uid[1] = 0xAD; // UID
    uid[2] = 0xAB; // UID
    uid[3] = 0xBA; // UID
    
    uid[4] = 0x11; // CT    
    uid[5] = 0xFA; // UID  
    uid[6] = 0xCE; // UID

    uid[7] = 0x55; // UID
    uid[8] = 0xBE; // UID
    uid[9] = 0xEF; // UID
      
    length = 7; // 4, 7, 10

     if(st95_set_uid(&dev, length, atqa, sak, uid) == ST95_OK) {

    }
}

static void wake_up_cb(void * arg)
{
    (void) arg;

    msg_try_send(&msg_wu, radio_pid);
}

void umdk_st95_init(uwnds_cb_t *event_callback)
{
    callback = event_callback;
   
    dev.cb = wake_up_cb;
                                                                   
     /* Create handler thread */
    char *stack = (char *) allocate_stack(UMDK_ST95_STACK_SIZE);
    if (!stack) {
        return;
    }

    radio_pid = thread_create(stack, UMDK_ST95_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, radio_send, NULL, "st95 thread");

    st95_params.iface = ST95_IFACE_SPI;

    // mode = UMDK_ST95_MODE_DETECT_TAG;
    mode = UMDK_ST95_MODE_READ_DATA;
    // mode = UMDK_ST95_MODE_SET_UID;

    if(st95_init(&dev, &st95_params) != ST95_OK){
        puts("[umdk-" _UMDK_NAME_ "] st95 driver initialization error");
        return;
    }
    else {
        if(st95_params.iface == ST95_IFACE_SPI) {
            puts("[umdk-" _UMDK_NAME_ "] st95 driver over SPI initialization success");
        }
        else {
            puts("[umdk-" _UMDK_NAME_ "] st95 driver over UART initialization success");
        }
        
        if(mode == UMDK_ST95_MODE_SET_UID) {
            puts("[umdk-" _UMDK_NAME_ "] Card emulation mode");
            umdk_st95_set_uid();
        }
        else if(mode == UMDK_ST95_MODE_DETECT_TAG) {
            puts("[umdk-" _UMDK_NAME_ "] UID reader mode");
            st95_sleep(&dev);
        }
        else if(mode == UMDK_ST95_MODE_READ_DATA) {
            puts("[umdk-" _UMDK_NAME_ "] Data reader mode");
            st95_sleep(&dev);
        }
        else if(mode == UMDK_ST95_MODE_WRITE_DATA) {
            puts("[umdk-" _UMDK_NAME_ "] Data writer mode");
            // st95_sleep(&dev);
        }
        
    }
 
}

static inline void reply_code(module_data_t *reply, uint8_t code) 
{
    reply->as_ack = false;
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
        /* Add reply-code */
    reply->data[1] = code;
}

bool umdk_st95_cmd(module_data_t *cmd, module_data_t *reply)
{         
    if(cmd->length < 1) {
        reply_code(reply, UMDK_ST95_ERROR_REPLY);
        return true;        
    }
    
    if(cmd->data[0] == UMDK_ST95_DETECT_TAG) {
        if(cmd->length != 1) {
            reply_code(reply, UMDK_ST95_ERROR_REPLY);
            return true;        
        }
        puts("[umdk-" _UMDK_NAME_ "] UID reader mode");
        mode = UMDK_ST95_MODE_DETECT_TAG;
        status = UMDK_ST95_STATUS_PROCCESSING;
        umdk_st95_get_uid();
        
        return false;
    }
    else if(cmd->data[0] == UMDK_ST95_GET_UID) {
        if(cmd->length != 1) {
            reply_code(reply, UMDK_ST95_ERROR_REPLY);
            return true;        
        }
        
        status = UMDK_ST95_STATUS_PROCCESSING;
        
        if(mode == UMDK_ST95_MODE_DETECT_TAG) {
            mode = UMDK_ST95_MODE_GET_UID;
            st95_sleep(&dev);
        }
        else {
            mode = UMDK_ST95_MODE_GET_UID;
            umdk_st95_get_uid();
        } 
 
        return false;       
    }
    else if(cmd->data[0] == UMDK_ST95_CARD_EMUL) {
        puts("[umdk-" _UMDK_NAME_ "] Card emulation mode");
        mode = UMDK_ST95_MODE_SET_UID;
        umdk_st95_set_uid();

        reply_code(reply, UMDK_ST95_OK_REPLY);
        return true;      
    }
    else if(cmd->data[0] == UMDK_ST95_WRITE_DATA) {
        puts("[WRITing...]");
        uint8_t buff_data[5] = { 0x4E, 0x49, 0x53, 0x48, 0x41 };
        if(st95_write_data(&dev, buff_data ,sizeof(buff_data)== ST95_ERROR)) {
            puts("Write ERR");
        }
    }
    else {
        reply_code(reply, UMDK_ST95_ERROR_REPLY);
        return true;  
    }
   
    return false;
}


#ifdef __cplusplus
}
#endif
