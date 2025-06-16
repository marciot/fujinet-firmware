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

#pragma once

#include <cstdint>

typedef enum {
    MAC_SERIAL_READ,
    MAC_SERIAL_WRITE
} mac_serial_mode;

uint16_t mac_serial_handler(uint8_t *buffer, uint16_t buffer_size, mac_serial_mode mode);
bool is_mac_serial_io (uint8_t drive, uint32_t sector, uint8_t *tagPtr, uint8_t *blkPtr, mac_serial_mode mode);