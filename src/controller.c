/*
 * ps1-bare-metal - (C) 2023 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 *
 * This example implements a simple controller tester, showing how to send
 * commands to and obtain data from connected controllers.
 *
 * Compared to other consoles where the state of all inputs is sometimes easily
 * accessible through registers, the PS1 has its controllers and memory cards
 * interfaced via a serial bus (similar to SPI). Both communicate using a simple
 * packet-based protocol, listening for request packets sent by the console and
 * replying with appropriate responses. Each packet consists of an address, a
 * command and a series of parameters, while responses will typically contain
 * information about the controller and the current state of its buttons in
 * addition to any data returned by the command.
 *
 * Communication is done over the SIO0 serial interface, similar to the SIO1
 * interface we used in the hello world example. All ports share the same bus,
 * so an addressing system is used to specify which device shall respond to each
 * packet. Understanding this example may require some basic familiarity with
 * serial ports and their usage, although I tried to add as many comments as I
 * could to explain what is going on under the hood.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ps1/registers.h"
#include "controller.h"
#include "psxproject/system.h"
 
 void initControllerBus(void) {
     // Reset the serial interface, initialize it with the settings used by
     // controllers and memory cards (250000bps, 8 data bits) and configure it to
     // send a signal to the interrupt controller whenever the DSR input is
     // pulsed (see below).
     SIO_CTRL(0) = SIO_CTRL_RESET;
 
     SIO_MODE(0) = 0
         | SIO_MODE_BAUD_DIV1
         | SIO_MODE_DATA_8;
     SIO_BAUD(0) = F_CPU / 250000;
     SIO_CTRL(0) = 0
         | SIO_CTRL_TX_ENABLE
         | SIO_CTRL_RX_ENABLE
         | SIO_CTRL_DSR_IRQ_ENABLE;
 }
 
bool waitForAcknowledge(int timeout) {
     // Controllers and memory cards will acknowledge bytes received by sending
     // short pulses over the DSR line, which will be forwarded by the serial
     // interface to the interrupt controller. This is not guaranteed to happen
     // (it will not if e.g. no device is connected), so we have to implement a
     // timeout to avoid waiting forever in such cases.
     for (; timeout > 0; timeout -= 10) {
         if (IRQ_STAT & (1 << IRQ_SIO0)) {
             // Reset the interrupt controller and serial interface's flags to
             // ensure the interrupt can be triggered again.
             IRQ_STAT     = ~(1 << IRQ_SIO0);
             SIO_CTRL(0) |= SIO_CTRL_ACKNOWLEDGE;
 
             return true;
         }
 
         delayMicroseconds(10);
     }
 
     return false;
 }
 

 
 #define DTR_DELAY       150
 #define DTR_PRE_DELAY   10
 #define DTR_POST_DELAY  10
 #define DTR_PACKET_DELAY 200
 #define DSR_TIMEOUT     120
 
 void selectPort(int port) {
     // Set or clear the bit that controls which set of controller and memory
     // card ports is going to have its DTR (port select) signal asserted. The
     // actual serial bus is shared between all ports, however devices will not
     // process packets if DTR is not asserted on the port they are plugged into.
     if (port)
         SIO_CTRL(0) |= SIO_CTRL_CS_PORT_2;
     else
         SIO_CTRL(0) &= ~SIO_CTRL_CS_PORT_2;
 }
 
 uint8_t exchangeByte(uint8_t value) {
     // Wait until the interface is ready to accept a byte to send, then wait for
     // it to finish receiving the byte sent by the device.
     while (!(SIO_STAT(0) & SIO_STAT_TX_NOT_FULL))
         __asm__ volatile("");
 
     SIO_DATA(0) = value;
 
     while (!(SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY))
         __asm__ volatile("");
 
     return SIO_DATA(0);
 }
 
 int exchangePacket(
     DeviceAddress address, const uint8_t *request, uint8_t *response,
     int reqLength, int maxRespLength
 ) {
     // Reset the interrupt flag and assert the DTR signal to tell the controller
     // or memory card that we're about to send a packet. Devices may take some
     // time to prepare for incoming bytes so we need a small delay here.
     delayMicroseconds(DTR_PRE_DELAY);
     IRQ_STAT     = ~(1 << IRQ_SIO0);
     SIO_CTRL(0) |= SIO_CTRL_DTR | SIO_CTRL_ACKNOWLEDGE;
     delayMicroseconds(DTR_DELAY);
 
     int respLength = 0;
 
     // Send the address byte and wait for the device to respond with a pulse on
     // the DSR line. If no response is received assume no device is connected,
     // otherwise make sure the serial interface's data buffer is empty to
     // prepare for the actual packet transfer.
     SIO_DATA(0) = address;
 
     if (waitForAcknowledge(DSR_TIMEOUT)) {
         while (SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY)
             SIO_DATA(0);
 
         // Send and receive the packet simultaneously one byte at a time,
         // padding it with zeroes if the packet we are receiving is longer than
         // the data being sent.
         while (respLength < maxRespLength) {
             if (reqLength > 0) {
                 *(response++) = exchangeByte(*(request++));
                 reqLength--;
             } else {
                 *(response++) = exchangeByte(0);
             }
 
             respLength++;
 
             // The device will keep sending DSR pulses as long as there is more
             // data to transfer. If no more pulses are received, terminate the
             // transfer.
             if (!waitForAcknowledge(DSR_TIMEOUT))
                 break;
         }
     }
 
     // Release DSR, allowing the device to go idle.
     delayMicroseconds(DTR_DELAY);
     SIO_CTRL(0) &= ~SIO_CTRL_DTR;
     delayMicroseconds(DTR_POST_DELAY);
     return respLength;
 }
 
 uint16_t getButtonPress(int port) {
     // Build the request packet.
     uint8_t request[4], response[32];

 
     request[0] = CMD_POLL; // Command
     request[1] = 0x00;     // Multitap address
     request[2] = 0x00;     // Rumble motor control 1
     request[3] = 0x00;     // Rumble motor control 2
 
     // Send the request to the specified controller port and grab the response.
     // Note that this is a relatively slow process and should be done only once
     // per frame, unless higher polling rates are desired.
     selectPort(port);
     int respLength = exchangePacket(
         ADDR_CONTROLLER, request, response, sizeof(request), sizeof(response)
     );
 
     if (respLength < 4 || response[1] != 0x5A) {
         // All controllers reply with at least 4 bytes of data.
    //     ptr += sprintf(ptr, "  No controller connected");
         return 0x00;
     }
     
     // Bytes 2 and 3 hold a bitfield representing the state all buttons. As each
     // bit is active low (i.e. a zero represents a button being pressed), the
     // entire field must be inverted.
     uint16_t buttons = (response[2] | (response[3] << 8)) ^ 0xffff;
     return buttons;
 }


void sendPacketNoAcknowledge(
    DeviceAddress address, const uint8_t *request, int reqLength
) {
    IRQ_STAT     = ~(1 << IRQ_SIO0);
    SIO_CTRL(0) |= SIO_CTRL_DTR | SIO_CTRL_ACKNOWLEDGE;
    delayMicroseconds(DTR_DELAY);

    SIO_DATA(0) = address;
    delayMicroseconds(BYTE_DELAY);
    while (SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY)
        SIO_DATA(0);

    for (; reqLength > 0; reqLength--) {
        exchangeByte(*(request++));
        delayMicroseconds(BYTE_DELAY);
    }

    delayMicroseconds(DTR_DELAY);
    SIO_CTRL(0) &= ~SIO_CTRL_DTR;
}

uint8_t checkMCPpresent(void)
{
	uint8_t ret = 0;
	uint8_t request[5] = {CMD_GAME_ID_PING, 0, 0, 0, 0 };
	uint8_t response[5] = { 0, 0, 0, 0, 0 };
	
	for (int i = 0; i < 2; i++)
	{
		selectPort(i);
		int respLength = exchangePacket(ADDR_MEMORY_CARD, request, response, sizeof(request), sizeof(response));
		
		if (respLength == 5 && response[2] == 0x27 && response[3] == 0xFF)
		{
			ret |= (1 << i);
		}
	}
	
	return ret;
}

void sendGameID(const char *str, uint8_t card) {
    uint8_t request[64];
    size_t  length = strlen(str) + 1;

    request[0] = CMD_GAME_ID_SEND;
    request[1] = 0;
    request[2] = length;
    __builtin_strncpy((char *)&request[3], str, length);

    // Send the ID on both ports.
    for (int i = 0; i < 2; i++)
    {
		if (card & (1 << i))
		{
			selectPort(i);
			sendPacketNoAcknowledge(ADDR_MEMORY_CARD, request, length+3);
        }
    }
}
