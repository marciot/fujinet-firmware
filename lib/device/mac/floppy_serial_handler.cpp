/****************************************************************************
 *   mac68k-fuji-drivers (c) 2025 Marcio Teixeira                           *
 *                                                                          *
 *   This program is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU General Public License as published by   *
 *   the Free Software Foundation, either version 3 of the License, or      *
 *   (at your option) any later version.                                    *
 *                                                                          *
 *   This program is distributed in the hope that it will be useful,        *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU General Public License for more details.                           *
 *                                                                          *
 *   To view a copy of the GNU General Public License, go to the following  *
 *   location: <http://www.gnu.org/licenses/>.                              *
 ****************************************************************************/

// Set one of the following to one to define the operating mode

#define MAC_SERIAL_LOOPBACK_TEST   1
#define MAC_SERIAL_USB_SERIAL_TEST 0

#include "../../include/debug.h"
#include "floppy_serial.h"

#include <cstring>

#define MIN(a,b) (((a)<(b))?(a):(b))

void printHexDump(const uint8_t *ptr, uint16_t len) {
    short n = MIN(15, len);
    Debug_printf("MacSerial: '");
    for (int i = 0; i < n; i++) Debug_printf("%c"  , isprint(ptr[i]) ? ptr[i] : '.');
    Debug_printf("' ");
    for (int i = 0; i < n; i++) Debug_printf("%02x ", ptr[i]);
    Debug_printf("\n");
}

/***************************** Fifo Queue Object *****************************/

typedef struct {
    uint16_t fifoLen;
    uint8_t  fifoData[2000];
} FifoBuffer;

uint16_t fifoBytesAvailable (FifoBuffer *fb) {
    return fb->fifoLen;
}

uint16_t fifoSpaceLeft (FifoBuffer *fb) {
    return sizeof(fb->fifoData) - fb->fifoLen;
}

uint16_t fifoGetData (FifoBuffer *fb, uint8_t *buf, uint16_t len) {
    const uint16_t dataToReturn = MIN(fb->fifoLen, len);
    memcpy (buf, fb->fifoData, dataToReturn);
    memmove (fb->fifoData, fb->fifoData + dataToReturn, fb->fifoLen - dataToReturn);
    fb->fifoLen -= dataToReturn;
    return dataToReturn;
}

void fifoPutData(FifoBuffer *fb, const uint8_t *buf, uint16_t len) {
    if ((fb->fifoLen + len) <= sizeof(fb->fifoData)) {
        memcpy (fb->fifoData + fb->fifoLen, buf, len);
        fb->fifoLen += len;
    } else {
        Debug_printf("MacSerial: Overflow in fifo buffer!\n");
    }
}

void fifoPutChar (FifoBuffer *fb, unsigned char c) {
    fifoPutData (fb, &c, 1);
}

/************************** End of Fifo Queue Object *************************/

uint16_t mac_serial_handler(unsigned char *buffer, uint16_t buffer_size, mac_serial_mode mode) {
    #if MAC_SERIAL_LOOPBACK_TEST || MAC_SERIAL_USB_SERIAL_TEST
        static FifoBuffer fifo = {0,0};
    #endif

   #if MAC_SERIAL_USB_SERIAL_TEST
        // If the queue is empty, read bytes from the console to fill it
        while (fifoSpaceLeft(&fifo)) {
            if (Serial.available() == 0) {
                break;
            }
            int c = Serial.read();
            fifoPutChar(&fifo, c);
        }
    #endif

    switch(mode) {
        case MAC_SERIAL_READ: {
            #if MAC_SERIAL_LOOPBACK_TEST || MAC_SERIAL_USB_SERIAL_TEST

                // Read *up to* buffer_size into buffer, but return how many
                // total bytes were available to be read at the time. This
                // allows the caller to know more data is available than fit
                // the buffer.

                const uint16_t availData = fifoBytesAvailable(&fifo);
                const uint16_t bytesRead = fifoGetData(&fifo, buffer, buffer_size);

                #if MAC_SERIAL_LOOPBACK_TEST
                    Debug_printf("MacSerial: Got I/O read request (availBytes = %d)\n", availData);
                    printHexDump (buffer, bytesRead);
                #endif
            #else
                // TODO: Get reply from FujiNet command processor
            #endif
            return availData;
        }
        case MAC_SERIAL_WRITE: {
            #if MAC_SERIAL_USB_SERIAL_TEST
                for (int i = 0; i < buffer_size; i++) {
                    Serial.write(buffer[i]);
                }
            #elif MAC_SERIAL_LOOPBACK_TEST
                Debug_printf("MacSerial: Got I/O write request (len = %d)\n", buffer_size);
                printHexDump (buffer, buffer_size);
                fifoPutData(&fifo, buffer, buffer_size);
            #else
                // TODO: Send data to FujiNet command processor
            #endif
        }
    }
    return 0;
}