/*
 * LinkM firmware - USB HID to I2C adapter for BlinkM
 *
 * Command format:  (from host perspective)
 *
 * pos description
 *  0    <startbyte>      ==  0xDA
 *  1    <linkmcmdbyte>   ==  0x01 = i2c transaction, 0x02 = i2c bus scan, etc. 
 *  2    <num_bytes_send> ==  starting at byte 4
 *  3    <num_bytes_recv> ==  may be zero if nothing to send back
 *  4..N <cmdargs>        == command 
 *
 * For most common command, i2c transaction (0x01):
 * pos  description
 *  0   0xDA
 *  1   0x01
 *  2   <num_bytes_to_send>
 *  3   <num_bytes_to_receive>
 *  4   <i2c_addr>   ==  0x00-0x7f
 *  5   <send_byte_0>
 *  6   <send_byte_1>
 *  7   ...
 *
 * Command byte values
 *  0x00 = no command, do not use
 *  0x01 = i2c transact: read + opt. write (N arguments)
 *  0x02 = i2c read                        (N arguments)
 *  0x03 = i2c write                       (N arguments)
 *  0x04 = i2c bus scan                    (2 arguments, start addr, end addr)
 *  0x05 = i2c bus connect/disconnect      (1 argument, connect/disconect)
 *  0x06 = i2c init                        (0 arguments)
 *
 *  0x100 = set status LED                  (1 argument)
 *  0x101 = get status LED
 * 
 * Response / Output buffer format:
 * pos description
 *  0   transaction counter (8-bit, wraps around)
 *  1   response code (0 = okay, other = error)
 *  2   <resp_byte_0>
 *  3   <resp_byte_1>
 *  4   ...
 *
 * 2009, Tod E. Kurt, ThingM, http://thingm.com/
 *
 */

#include <avr/io.h>
#include <avr/wdt.h>        // watchdog
#include <avr/eeprom.h>     //
#include <avr/interrupt.h>  // for sei() 
#include <util/delay.h>     // for _delay_ms() 
#include <string.h>

#include <avr/pgmspace.h>   // required by usbdrv.h 
#include "usbdrv.h"
#include "oddebug.h"        // This is also an example for using debug macros 

#include "i2cmaster.h"
#include "uart.h"

#include "linkm-lib.h"

// these aren't used anywhere, just here to note them
#define PIN_LED_STATUS         PB4
#define PIN_I2C_ENABLE         PB0

#define PIN_I2C_SCL            PC5
#define PIN_I2C_SDA            PC4

#define PIN_USB_DPLUS          PD2
#define PIN_USB_DMINUS         PD3

// uncomment to enable debugging to serial port
#define DEBUG   1

/* ------------------------------------------------------------------------- */
/* ----------------------------- USB interface ----------------------------- */
/* ------------------------------------------------------------------------- */

#define REPORT_COUNT 16

PROGMEM char usbHidReportDescriptor[22] = {    // USB report descriptor
    0x06, 0x00, 0xff,              // USAGE_PAGE (Generic Desktop)
    0x09, 0x01,                    // USAGE (Vendor Usage 1)
    0xa1, 0x01,                    // COLLECTION (Application)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x00,              //   LOGICAL_MAXIMUM (255)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x95, REPORT_COUNT,            //   REPORT_COUNT (32)  // was 32
    0x09, 0x00,                    //   USAGE (Undefined)
    0xb2, 0x02, 0x01,              //   FEATURE (Data,Var,Abs,Buf)
    0xc0                           // END_COLLECTION
};
/* Since we define only one feature report, we don't use report-IDs (which
 * would be the first byte of the report). The entire report consists of
 * REPORT_COUNT opaque data bytes.
 */

/* The following variables store the status of the current data transfer */
static uchar    currentAddress;
static uchar    bytesRemaining;

static int numWrites;

static uint8_t msgbuf[REPORT_COUNT];
static uint8_t outmsgbuf[REPORT_COUNT];

// setup serial routines to become stdio
extern int uart_putchar(char c, FILE *stream);
extern int uart_getchar(FILE *stream);
#ifdef DEBUG
FILE uart_str = FDEV_SETUP_STREAM(uart_putchar, uart_getchar, _FDEV_SETUP_RW);
#endif

/* ------------------------------------------------------------------------- */
void statusLedToggle(void)
{
    PORTB ^= (1<< PB4);  // toggles LED
}
void statusLedSet(int v)
{
    if( v ) PORTB  |=  (1<<PB4);
    else    PORTB  &=~ (1<<PB4);
}
uint8_t statusLedGet(void)
{
    return (PINB & (1<<PB4)) ? 1 : 0;
}

void i2cEnable(int v) {
    if( v ) PORTB  |=  (1<<PB0);
    else    PORTB  &=~ (1<<PB0);
}

// 
void handleMessage(void)
{
    outmsgbuf[0]++;                   // say we've handled a msg
    outmsgbuf[1] = LINKM_ERR_NONE;    // be optimistic
    // outmsgbuf[2] starts the actual received data

    uint8_t* mbufp = msgbuf;  // was +1 because had forgot to send repotid

    if( mbufp[0] != START_BYTE  ) {   // byte 0: start marker
        outmsgbuf[1] = LINKM_ERR_BADSTART;
        return;
    }
    
    uint8_t cmd      = mbufp[1];      // byte 1: command
    uint8_t num_sent = mbufp[2];      // byte 2: number of bytes sent
    uint8_t num_recv = mbufp[3];      // byte 3: number of bytes to return back

#ifdef DEBUG
    printf("cmd:%d, sent:%d, recv:%d\n",cmd,num_sent,num_recv);
#endif

    // i2c transaction
    // outmsgbuf[0] = transaction counter 
    // outmsgbuf[1] = response code
    // outmsgbuf[2] = i2c response byte 0  (if any)
    // outmsgbuf[3] = i2c response byte 1  (if any)
    // ...
    if( cmd == LINKM_CMD_I2CTRANS ) {
        uint8_t addr      = mbufp[4];  // byte 4: i2c addr or command

        printf("A.");
        if( addr >= 0x80 ) {   // invalid valid I2C address
            outmsgbuf[1] = LINKM_ERR_BADARGS;
            return;
        }

        printf("B.");
        if( i2c_start( (addr<<1) | I2C_WRITE ) == 1) {  // start i2c transaction
            printf("!");
            outmsgbuf[1] = LINKM_ERR_I2C;
            i2c_stop();
            return;
        }
        printf("C.");
        // start succeeded, so send data
        for( uint8_t i=0; i<num_sent-1; i++) {
            i2c_write( mbufp[5+i] );   // byte 5-N: i2c command to send
        } 

        printf("D.");
        if( num_recv != 0 ) {
            statusLedSet(1);
            if( i2c_rep_start( (addr<<1) | I2C_READ ) == 1 ) { // start i2c
                outmsgbuf[1] = LINKM_ERR_I2CREAD;
            }
            else {
                for( uint8_t i=0; i<num_recv; i++) {
                    //uint8_t c = i2c_read( (i!=(num_recv-1)) );// read from i2c
                    int c = i2c_read( (i!=(num_recv-1)) ); // read from i2c
                    if( c == -1 ) {  // timeout, get outx
                        outmsgbuf[1] = LINKM_ERR_I2CREAD;
                        break;
                    }
                    outmsgbuf[2+i] = c;             // store in response buff
                }
            }
            statusLedSet(0);
        }
        printf("Z.\n");
        i2c_stop();  // done!
    }
    // i2c write
    // outmsgbuf[0] = transaction counter 
    // outmsgbuf[1] = response code
    else if( cmd == LINKM_CMD_I2CWRITE ) {
        uint8_t addr      = mbufp[4];  // byte 4: i2c addr or command
        uint8_t doread    = mbufp[5];  // byte 5: do read or not after this

        if( addr >= 0x80 ) {   // invalid valid I2C address
            outmsgbuf[1] = LINKM_ERR_BADARGS;
            return;
        }

        if( i2c_start( (addr<<1) | I2C_WRITE ) == 1) {  // start i2c transaction
            outmsgbuf[1] = LINKM_ERR_I2C;
            i2c_stop();
            return;
        }
        // start succeeded, so send data
        for( uint8_t i=0; i<num_sent-1; i++) {
            i2c_write( mbufp[5+i] );   // byte 5-N: i2c command to send
        } 
        if( !doread ) {
            i2c_stop();   // done!
        }
    }
    // i2c read
    // outmsgbuf[0] = transaction counter 
    // outmsgbuf[1] = response code
    // outmsgbuf[2] = i2c response byte 0  (if any)
    // outmsgbuf[3] = i2c response byte 1  (if any)
    // ...
    else if( cmd == LINKM_CMD_I2CREAD ) {
        uint8_t addr      = mbufp[4];  // byte 4: i2c addr or command

        if( num_recv == 0 ) {
            outmsgbuf[1] = LINKM_ERR_BADARGS;
            return;
        }
        statusLedSet(1);

        if( i2c_rep_start( (addr<<1) | I2C_READ ) == 1 ) { // start i2c
            outmsgbuf[1] = LINKM_ERR_I2CREAD;
        }
        else {
            for( uint8_t i=0; i<num_recv; i++) {
                uint8_t c = i2c_read( (i!=(num_recv-1)) ); // read from i2c
                outmsgbuf[2+i] = c;             // store in response buff
            }
        }
        statusLedSet(0);
        i2c_stop();
    }
    // i2c bus scan
    // outmsgbuf[0] == transaction counter
    // outmsgbuf[1] == response code
    // outmsgbuf[2] == number of devices found
    // outmsgbuf[3] == addr of 1st device
    // outmsgbuf[4] == addr of 2nd device
    // ...
    else if( cmd == LINKM_CMD_I2CSCAN ) {
        uint8_t addr_start = mbufp[4];  // byte 4: start addr of scan
        uint8_t addr_end   = mbufp[5];  // byte 5: end addr of scan
        if( addr_start >= 0x80 || addr_end >= 0x80 || addr_start > addr_end ) {
            outmsgbuf[1] = LINKM_ERR_BADARGS;
            return;
        }
        int numfound = 0;
        for( uint8_t a = 0; a < (addr_end-addr_start); a++ ) {
            if( i2c_start( ((addr_start+a)<<1)|I2C_WRITE)==0 ) { // device found
                outmsgbuf[3+numfound] = addr_start+a;  // save the address 
                numfound++;
            }
            i2c_stop();
        }
        outmsgbuf[2] = numfound;
    }
    // i2c bus connect/disconnect
    // outmsgbuf[0] == transaction counter
    // outmsgbuf[1] == response code
    else if( cmd == LINKM_CMD_I2CCONN  ) {
        uint8_t conn = mbufp[4];        // byte 4: connect/disconnect boolean
        i2cEnable( conn );
    }
    // i2c init
    // outmsgbuf[0] == transaction counter
    // outmsgbuf[1] == response code
    else if( cmd == LINKM_CMD_I2CINIT ) {  // FIXME: what's the real soln here?
        i2c_stop();
        _delay_ms(1);
        i2c_init();
    }
    // set status led state
    // outmsgbuf[0] == transaction counter
    // outmsgbuf[1] == response code
    else if( cmd == LINKM_CMD_STATLED ) {
        uint8_t led = mbufp[4];        // byte 4: connect/disconnect boolean
        statusLedSet( led );
    }
    // cmd 0x98 == get status led state
    // outmsgbuf[0] == transaction counter
    // outmsgbuf[1] == response code
    // outmsgbuf[2] == state of status LED
    else if( cmd == LINKM_CMD_STATLEDGET ) {
        // no arguments, just a single return byte
        outmsgbuf[2] = statusLedGet();
    }
    // cmd xxxx == 
}

/* usbFunctionRead() is called when the host requests a chunk of data from
 * the device. For more information see the docs in usbdrv/usbdrv.h.
 */
uchar   usbFunctionRead(uchar *data, uchar len)
{
    if(len > bytesRemaining)
        len = bytesRemaining;
    
    //statusLedToggle();
    memcpy( data, outmsgbuf + currentAddress, len);
    numWrites = 0;
    currentAddress += len;
    bytesRemaining -= len;
    return len;
}

/* usbFunctionWrite() is called when the host sends a chunk of data to the
 * device. For more information see the documentation in usbdrv/usbdrv.h.
 */
uchar   usbFunctionWrite(uchar *data, uchar len)
{
    if(bytesRemaining == 0)
        return 1;               // end of transfer 
    if(len > bytesRemaining)
        len = bytesRemaining;
    
    memcpy( msgbuf+currentAddress, data, len );
    currentAddress += len;
    bytesRemaining -= len;
    
    if( bytesRemaining == 0 )  {   // got it all
        handleMessage();
    }
    
    return bytesRemaining == 0; // return 1 if this was the last chunk 
}

/* ------------------------------------------------------------------------- */

usbMsgLen_t usbFunctionSetup(uchar data[8])
{
    usbRequest_t    *rq = (void *)data;
    
    // HID class request 
    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS) {
        // wValue: ReportType (highbyte), ReportID (lowbyte) 
        if(rq->bRequest == USBRQ_HID_GET_REPORT){  
            // since we have only one report type, we can ignore the report-ID 
            bytesRemaining = REPORT_COUNT;
            currentAddress = 0;
            return USB_NO_MSG;  // use usbFunctionRead() to obtain data 
        }
        else if(rq->bRequest == USBRQ_HID_SET_REPORT) {
            // since we have only one report type, we can ignore the report-ID 
            bytesRemaining = REPORT_COUNT;
            currentAddress = 0;
            return USB_NO_MSG; // use usbFunctionWrite() to recv data from host 
        }
    } else {
        // ignore vendor type requests, we don't use any 
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    uchar   i;
    
    wdt_enable(WDTO_1S);
    // Even if you don't use the watchdog, turn it off here. On newer devices,
    // the status of the watchdog (on/off, period) is PRESERVED OVER RESET!
    DBG1(0x00, 0, 0);       // debug output: main starts
    // RESET status: all port bits are inputs without pull-up.
    // That's the way we need D+ and D-. Therefore we don't need any
    // additional hardware initialization.
    
    DDRB |= (1<< PB4)|(1<< PB0);      // make PB4 output for LED, PB0 for enable

    statusLedSet( 1 );
    
    for( i=0; i<10; i++)  // wait for power to stabilize & blink status
        _delay_ms(10);

#ifdef DEBUG
    uart_init();                // initialize UART hardware
    stdout = stdin = &uart_str; // setup stdio = RS232 port
    puts("linkm dongle test");
#endif

    PORTC |= _BV(PC5) | _BV(PC4);    // enable pullups on SDA & SCL
    i2c_init();                      // init I2C interface

    i2cEnable(1);                    // enable i2c buffer chip

    // debug
    outmsgbuf[8] = 0xde;
    outmsgbuf[9] = 0xad;
    outmsgbuf[10] = 0xbe;
    outmsgbuf[11] = 0xef;
    for( int i=0; i<4; i++ ) {
        outmsgbuf[12+i] = 0x60 + i;
    }

    odDebugInit();
    usbInit();
    usbDeviceDisconnect();  
    // enforce re-enumeration, do this while interrupts are disabled!
    i = 0;
    while(--i){             // fake USB disconnect for > 250 ms 
        wdt_reset();
        _delay_ms(1);
    }
    usbDeviceConnect();
    
    sei();
    DBG1(0x01, 0, 0);       // debug output: main loop starts 
    for(;;){                // main event loop 
        DBG1(0x02, 0, 0);     // debug output: main loop iterates 
        wdt_reset();
        usbPoll();
    }
    return 0;
}


// ------------------------------------------------------------------------- 

/**
 * Originally from:
 * Name: main.c
 * Project: hid-data, example how to use HID for data transfer
 * Author: Christian Starkjohann
 * Creation Date: 2008-04-11
 * Tabsize: 4
 * Copyright: (c) 2008 by OBJECTIVE DEVELOPMENT Software GmbH
 * License: GNU GPL v2 (see License.txt), GNU GPL v3 or proprietary (CommercialLicense.txt)
 * This Revision: $Id: main.c 692 2008-11-07 15:07:40Z cs $
 */

/*
This example should run on most AVRs with only little changes. No special
hardware resources except INT0 are used. You may have to change usbconfig.h for
different I/O pins for USB. Please note that USB D+ must be the INT0 pin, or
at least be connected to INT0 as well.
*/
