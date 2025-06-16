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

#include "../../include/debug.h"
#include "floppy_serial.h"

#include <cstring>

/*
 * This is a virtual device that can be accessed from Mac OS.
 * It is activated by a special sequence of sector I/O that
 * selects a particular "magic" sector for subsequent I/O.
 *
 * The disk interface should allow the virtual device first
 * dibs to handle any disk I/O prior to to the disk. This
 * allows the virtual device to interpret and respond to
 * requests by the Mac FujiNet serial drivers.
 *
 * The actual requests are passed to a handler,
 * "mac_serial_handler" which is defined in elsewhere.
 */

#define MAC_SERIAL_KNOCK_SEQ    {0,70,85,74,73}  // Macintosh -> FujiNet
#define MAC_SERIAL_REQUEST_TAG  "NDEV"           // Macintosh -> FujiNet
#define MAC_SERIAL_REPLY_TAG    "FUJI"           // FujiNet -> Macintosh
#define MAC_SERIAL_HEADER_LEN   12
#define MAC_SERIAL_NEGATIVE_LBA 0x007FFFFF

#define NELEMENTS(a) (sizeof(a)/sizeof(a[0]))
#define CHARS_TO_UINT16(a,b) ((((uint16_t)a) << 8) | (((uint16_t)(b)) & 0xFF))
#define UINT16_HI_BYTE(a) (((a) >> 8) & 0xFF)
#define UINT16_LO_BYTE(a) (((a)     ) & 0xFF)

enum {
    MAC_SERIAL_WAIT_KNOCK,
    MAC_SERIAL_WAIT_MAGIC_WRITE,
    MAC_SERIAL_WAIT_MAGIC_READ,
    MAC_SERIAL_WAIT_MAGIC_SECTOR
} mac_serial_state;

uint8_t  mac_serial_knock = 0;
uint8_t  mac_serial_drive;
uint32_t mac_serial_sector;

/* The following functions read and write 12 bytes of header data
 * that are used for communications between the Mac and the ESP32.
 * Along with a maximum payload size of 500, this fills a 512 byte
 * block. It may also be used as sector tags in certain points of
 * the initial handshaking.
 */

void mac_serial_put_header(uint8_t buff[], uint16_t len) {
    buff[ 0] = MAC_SERIAL_REPLY_TAG[0];
    buff[ 1] = MAC_SERIAL_REPLY_TAG[1];
    buff[ 2] = MAC_SERIAL_REPLY_TAG[2];
    buff[ 3] = MAC_SERIAL_REPLY_TAG[3];
    buff[ 4] = 0;
    buff[ 5] = 0;
    buff[ 6] = UINT16_HI_BYTE(len);
    buff[ 7] = UINT16_LO_BYTE(len);
    buff[ 8] = 0;
    buff[ 9] = 0;
    buff[10] = 0;
    buff[11] = 0;
}

bool mac_serial_get_header(uint8_t buff[], uint16_t *len) {
    if (memcmp(buff, MAC_SERIAL_REQUEST_TAG, 4)) {
        //Debug_printf("MacSerial: Invalid tag on I/O request: %4s\n", buff);
        return false;
    } else {
        *len = CHARS_TO_UINT16(buff[6], buff[7]);
        return true;
    }
}

/* This function is a state machine that follows a special sequence
 * of sector acccesses and returns "true" on the last block of the
 * sequence. It is used during handshaking to allow the Mac FujiNet
 * serial driver to announce its presence.
 */
bool mac_serial_detect_knock_sequence(uint32_t sector) {
    const int mac_serial_knock_sequence[5] = MAC_SERIAL_KNOCK_SEQ;

    if (sector == mac_serial_knock_sequence[mac_serial_knock]) {
        Debug_printf("MacSerial: Got knock %d\n", mac_serial_knock);
        if (++mac_serial_knock == NELEMENTS(mac_serial_knock_sequence)) {
            Debug_printf("MacSerial: Knock sequence complete!\n");
            mac_serial_knock = 0;
            return true;
        }
    } else {
        mac_serial_knock = 0;
    }
    return false;
}

/* This function processes reads and writes to the special magic sector and
 * passes it to "mac_serial_handler" for further processing.
 */
bool mac_serial_magic_sector_io(uint8_t *tagPtr, uint8_t *blkPtr, mac_serial_mode mode) {
    switch(mode) {
        case MAC_SERIAL_READ: {
            const uint16_t availBytes = mac_serial_handler(blkPtr + MAC_SERIAL_HEADER_LEN, 512 - MAC_SERIAL_HEADER_LEN, mode);
            mac_serial_put_header (blkPtr, availBytes);
            return true;
        }
        case MAC_SERIAL_WRITE: {
            uint16_t len;
            const bool headerInTags = mac_serial_get_header(tagPtr, &len);
            if (headerInTags || mac_serial_get_header(blkPtr, &len)) {
                const uint8_t headerSize = headerInTags ? 0 : MAC_SERIAL_HEADER_LEN;
                if (!headerInTags) {
                    tagPtr = blkPtr;
                }
                if (len > (512 - headerSize)) {
                    Debug_printf("MacSerial: Got invalid write len (len = %d)\n", len);
                    len = 512 - headerSize;
                }
                mac_serial_handler(blkPtr + headerSize, len, mode);
                return true;
            } else {
                Debug_printf("\nMacSerial: Got write request to magic sector without tags");
                return false;
            }
        }
    }
    return false;
}

/* Prior to reading or writing data to the disk, the disk I/O code
 * should call "is_mac_serial_io" to check whether the request is
 * special I/O.
 *
 * If "is_mac_serial_io" returns "true", the block data will have
 * been filled with appropriate values to fullfill the request.
 *
 * During a handshaking, the tags may be modified and should be
 * returned to the host as modified, regardless of the return value.
 *
 * The arguments are as follows:
 *
 *    drive_num : A disk identifier
 *    block_num : A logical block address on disk
 *    tags_ptr  : Pointer to the 12 or 20 byte MacOS sector tags
 *    block_ptr : Pointer to the 512 byte block buffer
 *    mode      : Either MAC_SERIAL_READ or MAC_SERIAL_WRITE
 */

bool is_mac_serial_io (uint8_t drive, uint32_t sector, uint8_t *tagPtr, uint8_t *blkPtr, mac_serial_mode mode) {
    //Debug_printf("MacSerial: drive %d; sector %ld; mode %d; state %d; knock %d; magic %d/%ld\n", drive, sector, mode, mac_serial_state, mac_serial_knock, mac_serial_drive, mac_serial_sector);

    if (sector == MAC_SERIAL_NEGATIVE_LBA) {
        // If we get a negative LBA, it must be special I/O...
        Debug_printf("MacSerial: Got negative LBA!\n");
        mac_serial_magic_sector_io (tagPtr, blkPtr, mode);
        if (mac_serial_state != MAC_SERIAL_WAIT_MAGIC_SECTOR) {
            // Finish partially complete handshake, as
            // the host is using negative LBA instead.
            mac_serial_state = MAC_SERIAL_WAIT_KNOCK;
        }
        return true;
    }

    // Listen for the knock sequence, which may be sent at any time
    // to start designated I/O sector selection.

    if (mac_serial_detect_knock_sequence(sector)) {
        mac_serial_state  = MAC_SERIAL_WAIT_MAGIC_WRITE;
        mac_serial_drive  = drive;
        mac_serial_sector = 0;
        Debug_printf("MacSerial: Will use drive number %d for I/O\n", mac_serial_drive);

        // When the knocking sequence is complete, send
        // back special tags to let the host know a
        // FujiNet device is present.

        mac_serial_put_header(tagPtr, 0);
    }

    // Handle the current run state

    switch (mac_serial_state) {
        case MAC_SERIAL_WAIT_KNOCK:
            /* STEP 1: Device idle, waiting for a valid knock sequence.
             */
            //Debug_printf("MacSerial: waiting for knock\n");
            break;

        case MAC_SERIAL_WAIT_MAGIC_WRITE:
            /* STEP 2: After knocking, the Mac will either do a negative LBA
             *         request, or write 512 bytes of magic data to a file.
             *         If we detect this, we save the sector number for
             *         subsequent I/O.
             */
            Debug_printf("MacSerial: waiting for magic write\n");
            if ((mode  == MAC_SERIAL_WRITE) &&
                (drive == mac_serial_drive)) {
                // Check whether the whole sector consists of
                // repetitions of the magic value.
                const char *magic = MAC_SERIAL_REQUEST_TAG;
                for (int i = 0; i < 512; i++) {
                    const char expected = magic[i & 3];
                    const char received = blkPtr[i];
                    if (expected != received) {
                        Debug_printf("MacSerial: Magic sector rejected at byte %d, %c != %c\n", i, received, expected);
                        break;
                    }
                }
                // We've got a magic sector!
                mac_serial_sector = sector;
                mac_serial_state = MAC_SERIAL_WAIT_MAGIC_READ;
                Debug_printf("MacSerial: Will use sector number %ld for I/O\n", mac_serial_sector);
                return true;
            }
            break;

        case MAC_SERIAL_WAIT_MAGIC_READ:
            /* STEP 3: The Mac client will now immediately read back
             *         from the file. We should return a special message
             *         with a tag and the logical block number. At this
             *         point, both the host and FujiNet have agreed on
             *         a special I/O block and handshaking is complete.
             */
            Debug_printf("MacSerial: waiting for magic read\n");
            if ((mode  == MAC_SERIAL_READ) &&
                (drive == mac_serial_drive) &&
                (sector == mac_serial_sector)) {
                mac_serial_put_header(tagPtr, 8);
                blkPtr[0] = MAC_SERIAL_REPLY_TAG[0];
                blkPtr[1] = MAC_SERIAL_REPLY_TAG[1];
                blkPtr[2] = MAC_SERIAL_REPLY_TAG[2];
                blkPtr[3] = MAC_SERIAL_REPLY_TAG[3];
                blkPtr[4] = (mac_serial_sector & 0xFF000000) >> 24;
                blkPtr[5] = (mac_serial_sector & 0x00FF0000) >> 16;
                blkPtr[6] = (mac_serial_sector & 0x0000FF00) >>  8;
                blkPtr[7] = (mac_serial_sector & 0x000000FF) >>  0;
                Debug_printf("MacSerial: Sent I/O sector to Mac host.\n");
                Debug_printf("MacSerial: Handshake complete.\n");
                mac_serial_state = MAC_SERIAL_WAIT_MAGIC_SECTOR;
                return true;
            } else {
                Debug_printf("MacSerial: Got %s to sector %ld, drive %d instead\n",
                    mode  == MAC_SERIAL_READ ? "read" : "write",
                    sector, drive
                );
            }
            break;

        case MAC_SERIAL_WAIT_MAGIC_SECTOR:
            /* STEP 4: We can now intercept all reads and writes to the
             *         magic sector as I/O.
             */
            if ((drive == mac_serial_drive) && (sector == mac_serial_sector)) {
                //Debug_printf("MacSerial: Magic sector access\n");
                return mac_serial_magic_sector_io(tagPtr, blkPtr, mode);
            } else if (sector == mac_serial_sector) {
                Debug_printf("MacSerial: Magic sector request to wrong drive? %d != %d\n", drive, mac_serial_drive);
            }
            break;
        default:
            Debug_printf("MacSerial: Invalid state %d\n", mac_serial_state);
    }
    return false;
}