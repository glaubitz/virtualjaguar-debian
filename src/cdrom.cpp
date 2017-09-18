//
// CD handler
//
// Originally by David Raingeard
// GCC/SDL port by Niels Wagenaar (Linux/WIN32) and Caz (BeOS)
// Extensive rewrites/cleanups/fixes by James Hammons
// BUTCH reverse engineering by Stephan Kapfer/James Hammons
// (C) 2016 Underground Software
//
// JLH = James Hammons <jlhamm@acm.org>
//
// Who  When        What
// ---  ----------  ------------------------------------------------------------
// JLH  01/16/2010  Created this log ;-)
//

#include "cdrom.h"

#include <string.h>				// For memset, etc.
#include "cdintf.h"				// System agnostic CD interface functions
#include "dac.h"
#include "dsp.h"
#include "eeprom.h"
#include "event.h"
#include "jaguar.h"
#include "log.h"


//#define CDROM_LOG				// For CDROM logging, obviously

/*
;
; Butch's hardware registers
;
;BUTCH     equ  $DFFF00		;base of Butch=interrupt control register, R/W
;
;  When written (Long):
;
;  bit0 - set to enable interrupts
;  bit1 - enable CD data FIFO half full interrupt
;  bit2 - enable CD subcode frame-time interrupt (@ 2x spped = 7ms.)
;  bit3 - enable pre-set subcode time-match found interrupt
;  bit4 - CD module command transmit buffer empty interrupt
;  bit5 - CD module command receive buffer full
;  bit6 - CIRC failure interrupt
;
;  bit7-31  reserved, set to 0
;
;  When read (Long):
;
;  bit0-8 reserved
;
;  bit9  - CD data FIFO half-full flag pending
;  bit10 - Frame pending
;  bit11 - Subcode data pending
;  bit12 - Command to CD drive pending (trans buffer empty if 1)
;  bit13 - Response from CD drive pending (rec buffer full if 1)
;  bit14 - CD uncorrectable data error pending
;
;   Offsets from BUTCH
;
O_DSCNTRL   equ  4		; DSA control register, R/W
O_DS_DATA   equ  $A		; DSA TX/RX data, R/W
;
O_I2CNTRL   equ  $10		; i2s bus control register, R/W
;
;  When read:
;
;  b0 - I2S data from drive is ON if 1
;  b1 - I2S path to Jerry is ON if 1
;  b2 - reserved
;  b3 - host bus width is 16 if 1, else 32
;  b4 - FIFO state is not empty if 1
;
O_SBCNTRL   equ  $14		; CD subcode control register, R/W
O_SUBDATA   equ  $18		; Subcode data register A
O_SUBDATB   equ  $1C		; Subcode data register B
O_SB_TIME   equ  $20		; Subcode time and compare enable (D24)
O_FIFODAT   equ  $24		; i2s FIFO data
O_I2SDAT2   equ  $28		; i2s FIFO data (old)
*/

/*
Commands sent through DS_DATA:

$01nn - ? Play track nn ? Seek to track nn ?
$0200 - Stop CD
$03nn - Read session nn TOC (short)
$0400 - Pause CD
$0500 - Unpause CD
$10nn - Goto (min?)
$11nn - Goto (sec?)
$12nn - Goto (frm?)
$14nn - Read session nn TOC (full)
$15nn - Set CD mode
$18nn - Spin up CD to session nn
$5000 - ?
$5100 - Mute CD (audio mode only)
$51FF - Unmute CD (audio mode only)
$5400 - Read # of sessions on CD
$70nn - Set oversampling mode
*/

// BUTCH registers
#define BUTCH		0xDFFF00		// Interrupt control register, R/W
#define DSCNTRL 	BUTCH + 0x04	// DSA control register, R/W
#define DS_DATA		BUTCH + 0x0A	// DSA TX/RX data, R/W
#define I2CNTRL		BUTCH + 0x10	// i2s bus control register, R/W
#define SBCNTRL		BUTCH + 0x14	// CD subcode control register, R/W
#define SUBDATA		BUTCH + 0x18	// Subcode data register A
#define SUBDATB		BUTCH + 0x1C	// Subcode data register B
#define SB_TIME		BUTCH + 0x20	// Subcode time and compare enable (D24)
#define FIFO_DATA	BUTCH + 0x24	// i2s FIFO data
#define I2SDAT2		BUTCH + 0x28	// i2s FIFO data (old)
#define I2SBUS		BUTCH + 0x2C	// I2S interface to CD EEPROM

// Lines used by CD EEPROM bus (at $DFFF2C, long word)
#define CDI2DBUS_ACK		0x01	// bit0 - Chip Select (CS)
#define CDI2SBUS_STB		0x02	// bit1 - clock
#define CDI2SBUS_TXD		0x04	// bit2 - Data Out
#define CDI2SBUS_RXD		0x08	// bit3 - busy if 0 after write cmd, or Data
									//        In after read cmd 

// BUTCH interrupt/etc lines
#define BUTCH_INTS_ENABLE		0x0001
#define DSA_FIFO_INT_ENABLE		0x0002
#define DSA_FRAME_INT_ENABLE	0x0004
#define DSA_SUBCODE_INT_ENABLE	0x0008
#define DSA_TX_INT_ENABLE		0x0010
#define DSA_RX_INT_ENABLE		0x0020
#define DSA_ERROR_INT_ENABLE	0x0040
// bits 7,8 unused
#define DSA_FIFO_INT_PENDING	0x0200
#define DSA_FRAME_INT_PENDING	0x0400
#define DSA_SUBCODE_INT_PENDING	0x0800
#define DSA_TX_INT_PENDING		0x1000
#define DSA_RX_INT_PENDING 		0x2000
#define DSA_ERROR_INT_PENDING	0x4000
// This speculation from the CD BIOS
// Set bit16 -> Enable DSA (in DSCNTRL)
// Set bit17 -> Reset CD module
// Set bit18 -> Override CD BIOS in cart space, even if cart plugged in
// Set bit19 -> Reboot if CD lid is opened
// Set bit20 -> Reboot if cartridge is pulled
#define DSA_ENABLE				0x010000
#define BUTCH_RESET_CD			0x020000
#define BUTCH_BIOS_ENABLE		0x040000
#define BUTCH_REBOOT_LID		0x080000
#define BUTCH_REBOOT_CART		0x100000

// I2SCNTRL
// b0 - I2S data from drive is ON if 1
// b1 - I2S path to Jerry is ON if 1
// b2 - Enable data transfer (subcode ???)
// b3 - Host bus width is 16 if 1, else 32
// b4 - FIFO state is not empty if 1
#define I2S_DATA_FROM_CD	0x01
#define I2S_DATA_TO_JERRY	0x02
#define I2S_DATA_ENABLE		0x04
#define I2S_BUS_WIDTH16		0x08
#define I2S_DATA_IN_FIFO	0x10

// BUTCH Commands (ones we know about :-P)
#define CMD_PLAY_TRACK		0x01
#define CMD_STOP			0x02
#define CMD_READ_TOC		0x03
#define CMD_PAUSE			0x04
#define CMD_UNPAUSE			0x05
#define CMD_GET_MSF			0x0D
#define CMD_GOTO_MIN		0x10
#define CMD_GOTO_SEC		0x11
#define CMD_GOTO_FRM		0x12
#define CMD_READ_LONG_TOC	0x14
#define CMD_SET_PLAY_MODE	0x15
#define CMD_GET_LAST_ERROR	0x16
#define CMD_GET_LAST_ERR2	0x17
#define CMD_SET_SESSION		0x18
#define CMD_SET_START_MIN	0x20
#define CMD_SET_START_SEC	0x21
#define CMD_SET_START_FRM	0x22
#define CMD_SET_STOP_MIN	0x23
#define CMD_SET_STOP_SEC	0x24
#define CMD_SET_STOP_FRM	0x25
#define CMD_GET_STATUS		0x50
#define CMD_SET_VOLUME		0x51
#define CMD_SESSION_INFO	0x54
#define CMD_CLEAR_TOC_READ	0x6A
#define CMD_OVRSAMPLE_MODE	0x70

const char * BReg[12] = { "BUTCH", "DSCNTRL", "DS_DATA", "???", "I2CNTRL",
	"SBCNTRL", "SUBDATA", "SUBDATB", "SB_TIME", "FIFO_DATA", "I2SDAT2",
	"I2SBUS" };

// BUTCH internal shite
static bool haveCDGoodness;
static uint32_t min, sec, frm, block;
static uint16_t cdCmd = 0;
static uint32_t cdPtr = 0;
static uint8_t cdBuffer[2352 + 96];
static uint32_t cdBufPtr = 2352;
static uint8_t trackNum = 1, minTrack, maxTrack;
static uint8_t wordStrobe;
static uint32_t currentSector, sectorRead;
static uint32_t cdSpeed;

// BUTCH internal FIFO
#define FIFO_MASK 0x1FF
static uint16_t dsfifo[FIFO_MASK + 1];
static uint16_t dsfStart, dsfEnd;

// BUTCH registers
// N.B.: At some point, need to change these out to use the ones in memory.cpp
//uint32_t butchControl;
uint32_t butchDSCntrl;
uint32_t butchI2Cntrl;
uint32_t butchSBCntrl;
uint32_t butchSubDatA;
uint32_t butchSubDatB;
uint32_t butchSBTime;
uint32_t butchFIFOData;
uint32_t butchI2SDat2;

// Private function prototypes
static void QueueDSFIFO(uint16_t data);
static uint32_t ReadDSFIFO(void);
static void ButchCommand(uint16_t cmd);
static void HandleButchControl(void);
void BUTCHI2SCallback(void);
uint16_t BUTCHGetDataFromCD(void);


void CDROMInit(void)
{
	haveCDGoodness = CDIntfInit();
	dsfStart = dsfEnd = 0;
	currentSector = -1;
	sectorRead = 0;
}


void CDROMReset(void)
{
//	memset(cdRam, 0x00, 0x100);
	cdCmd = 0;
	dsfStart = dsfEnd = 0;
	currentSector = -1;
	sectorRead = 0;
}


void CDROMDone(void)
{
	CDIntfDone();
}


//
// We're leveraging the timing subsystem to handle this properly...
//
static uint16_t lastLeft, lastRight;
void BUTCHI2SCallback(void)
{
	// Figure callback interval
	double interval = 1000000.0 / (cdSpeed == 1 ? 44100.0 : 88200.0);
	// Do we need to do this because there's L&R for each 1/44100th of a second?
	// I.e., 32 bits of data for each interval, and we're only stuffing 16?
	// [Not any more, we stuff 32 bits/interval]
//	interval /= 2.0;
	// Word strobe just clocks through mindlessly
	wordStrobe = (wordStrobe + 1) & 0x01;
	// Set which part of the word strobe we're looking for (0x10 == FALLING)
	uint8_t sendType = (smode & 0x10 ? 1 : 0);

	uint16_t left = BUTCHGetDataFromCD();
	uint16_t right = BUTCHGetDataFromCD();

	// Set the appropriate spot for our data, depending on WS setting...
#if 0
	if (wordStrobe == 0)
		lrxd = data;
	else
		rrxd = data;
#endif
// [last L][last R][curr L][curr R]
//          ------  ------
// This is working, BTW...
	if (sendType == 1)
	{
		lrxd = left;
		rrxd = lastRight;
	}
	else
	{
		lrxd = left;
		rrxd = right;
	}

//WriteLog("BUTCH: LL LR CL CR = [%02X %02X %02X %02X] [%02X %02X]\n", lastLeft, lastRight, left, right, lrxd, rrxd);

	lastLeft = left;
	lastRight = right;

	// Send IRQ at the appropriate time...
//	if (wordStrobe == sendType)
		DSPSetIRQLine(DSPIRQ_SSI, ASSERT_LINE);

	// If all 3 bits aren't set, get outta here...
	if ((butchI2Cntrl & (I2S_DATA_FROM_CD | I2S_DATA_TO_JERRY | I2S_DATA_ENABLE)) != (I2S_DATA_FROM_CD | I2S_DATA_TO_JERRY | I2S_DATA_ENABLE))
		return;
	
	// Should turn this off once the I2CNTRL is no longer set... [DONE above]
	SetCallbackTime(BUTCHI2SCallback, interval, EVENT_JERRY);
}


//
// The bread and butter--get data from the CD drive!
//
uint16_t BUTCHGetDataFromCD(void)
{
	if (currentSector != sectorRead)
	{
		bool status = CDIntfReadBlock(currentSector, cdBuffer);

		if (status == false)
		{
			// Handle the error (need to do more than this!)...
			return 0xFFFF;
		}

		sectorRead = currentSector;
		cdPtr = 0;
	}

	uint16_t data = cdBuffer[cdPtr] | (cdBuffer[cdPtr + 1] << 8);
	cdPtr += 2;

	// If we run past our buffer, signal a read to the next sector
	if (cdPtr >= 2352)
		currentSector++;

	return data;
}


//
// This approach is probably wrong, but let's do it for now.
// What's needed is a complete overhaul of the interrupt system so that
// interrupts are handled as they're generated--instead of the current scheme
// where they're handled on scanline boundaries. [This is DONE now.]
//
void BUTCHExec(uint32_t cycles)
{
#if 1
// We're chickening out for now...
return;
#else
//	extern uint8_t * jerry_ram_8;					// Hmm.

	// For now, we just do the FIFO interrupt. Timing is also likely to be WRONG as well.
	uint32_t cdState = GET32(cdRam, BUTCH);

	if (!(cdState & 0x01))						// No BUTCH interrupts enabled
		return;

	if (!(cdState & 0x22))
		return;									// For now, we only handle FIFO/buffer full interrupts...

	// From what I can make out, it seems that each FIFO is 32 bytes long

//	DSPSetIRQLine(DSPIRQ_EXT, ASSERT_LINE);
//I'm *sure* this is wrong--prolly need to generate DSP IRQs as well!
	if (jerry_ram_8[0x23] & 0x3F)				// Only generate an IRQ if enabled!
		GPUSetIRQLine(GPUIRQ_DSP, ASSERT_LINE);
#endif
}
/*
EEPROM: Butch cmd received: $130 [EE[$30] = $FFFF]
EEPROM: Butch CS strobed...
EEPROM: Butch cmd received: $145 [EE[$5] = $3607]
EEPROM: BUTCH write $D374 to cell $5
EEPROM: Butch CS strobed...
CDROM: Write of $0018 to BUTCH+0 [M68K]
CDROM: Write of $0000 to BUTCH+2 [M68K]
Write to DSP CTRL by M68K: 00002000 (DSP PC=$00F1B020)
Write to DSP CTRL by M68K: 00000001 (DSP PC=$00F1B020)
DSP: Modulo data FFFFF800 written by DSP.
DAC: M68K writing to SMODE. Bits: WSEN FALLING  [68K PC=00050984]
CDROM: I2CNTRL+0 is $0 [M68K]
CDROM: I2CNTRL+2 is $1 [M68K]
CDROM: Write of $0000 to I2CNTRL+0 [M68K]
CDROM: Write of $0007 to I2CNTRL+2 [M68K]
CDROM: Write of $1008 to DS_DATA [M68K]
CDROM: BUTCH+0 is $18 [M68K]
CDROM: BUTCH+2 is $1000 [M68K]
CDROM: DSCNTRL+0 is $1 [M68K]
CDROM: DSCNTRL+2 is $0 [M68K]
CDROM: Write of $1122 to DS_DATA [M68K]
CDROM: BUTCH+0 is $18 [M68K]
CDROM: BUTCH+2 is $1000 [M68K]
CDROM: DSCNTRL+0 is $1 [M68K]
CDROM: DSCNTRL+2 is $0 [M68K]
CDROM: Write of $1236 to DS_DATA [M68K]
CDROM: BUTCH+0 is $18 [M68K]
CDROM: BUTCH+2 is $1000 [M68K]
CDROM: DSCNTRL+0 is $1 [M68K]
CDROM: DSCNTRL+2 is $0 [M68K]
CDROM: BUTCH+0 is $18 [DSP]
CDROM: BUTCH+2 is $1000 [DSP]
CDROM: BUTCH+0 is $18 [DSP]
CDROM: BUTCH+2 is $1000 [DSP]

So basically, how it works is like so. 68K calls CD_jeri, which sets JERRY's
I2S control to WSEN (word strobe enable) and FALLING (trigger I2S interrupts on
the falling edge of the word strobe; this basically calls the interrupt service
routine once the LEFT channel data has been stuffed). The 68K then calls CD_play
and does 3 goto calls (goto minutes, goto seconds, goto frames) and then hands
off BUTCH processing to a DSP program. The DSP waits for a $100 acknowledge for
the goto calls, and then goes from there.

So, because JERRY's I2S is set to FALLING, it will grab longs from the CD that
are shifted over one word. This explains why the DSP program looks for the long
as (RIGHT << 16) | LEFT.

Then, 68K sets BUTCH's I2S control to DATA_ENABLE, DATA_FROM_CD & DATA_TO_JERRY.
So it would seem that because JERRY's I2S INTERNAL bit is *not* set, BUTCH is
the bus master and drives the I2S.

So it would seem that once DATA_ENABLE is set, the CD starts pumping data over
the I2S channel (as long as DATA_FROM_CD & DATA_TO_JERRY are set). It probably
depends on the playback rate set by the 68K (1x or 2x, 44100 or 88200 Hz).

So once we get this go ahead, how do we pump the data in? Could set up a thread
to do it. Basically have it write the words to L/RRXD and fire off the interrupt
(I2S Jerry) at the appropriate time set by JERRY's I2S control. Normally, timing
would be done using... Should be able to use the timing subsystem to run a
BUTCH callback... Then the timing shouldn't be an issue. But there are *two*
timing subsystems, which one to use? Probably the JERRY one, then we know for
sure that it will be synchronized with JERRY...

(We need to figure out how to make those two separate timing systems play nice
with each other... One can currently run way ahead of the other!)


I AR|PRAP|EDOV|AT D  -->  ARI APPROVED DAT
HEA |ERAD|TR A|<.I)  -->  A HEADER ATRI)<.

004000: 41 54 41 52 49 20 41 50 50 52 4F 56 45 44 20 44 | ATARI APPROVED D
004010: 41 54 41 20 48 45 41 44 45 52 20 41 54 52 49 29 | ATA HEADER ATRI)
004020: 3C EC B2 59 E9 FE 0F C0 30 CB 74 29 E5 F0 42 4F | <..Y....0.t)..BO
004030: C9 B1 0A 18 E4 E4 75 C6 F7 D9 D0 93 55 B5 82 CE | ......u.....U...
004040: 98 9D CF 1A 96 8C BF 0F A6 D0 71 66 B9 9D E2 A0 | ..........qf....
004050: 58 26 F5 4E 43 F3 08 57 D3 61 3E 42 53 C1 67 1A | X&.NC..W.a>BS.g.
004060: 13 73 67 3C 30 21 F5 CF 8E 70 78 1B EE CD E6 57 | .sg<0!...px....W
004070: 23 A3 56 6C FF B7 9B 40 3D 18 BF 53 66 FE 26 A9 | #.Vl...@=..Sf.&.
004080: 73 2C 38 F5 C3 4D 4C CD 00 00 00 00 00 00 00 00 | s,8..ML.........

CD public key (@ $F1B4CC):
00 00 00 2C 80 1E 32 56 F3 58 0F 1F 73 48 8A 32
20 3E B7 E8 C7 03 17 11 51 6F 8F 92 DC 64 C2 4B
AE E6 E0 C9 CA 38 35 0E 07 03 EC 4E 3B A8 F3 1F
2F 90 A6 43 C2 CD A0 FF 2D 5B 26 8E 4A A9 3B 4A
63 A6 AA 27


Baldies (word swapped):
02B0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
02C0: 00 00 00 00 00 00 00 00 00 00 41 54 52 49 41 54   ..........ATRIAT
02D0: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
02E0: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
02F0: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
0300: 52 49 41 54 52 49 41 54 52 49 41 54 41 52 49 20   RIATRIATRIATARI 
0310: 41 50 50 52 4F 56 45 44 20 44 41 54 41 20 48 45   APPROVED DATA HE
0320: 41 44 45 52 20 41 54 52 49 29 3C EC B2 59 E9 FE   ADER ATRI)<..Y..
0330: 0F C0 30 CB 74 29 E5 F0 42 4F C9 B1 0A 18 E4 E4   ..0.t)..BO......
0340: 75 C6 F7 D9 D0 93 55 B5 82 CE 98 9D CF 1A 96 8C   u.....U.........

RAW:
02C0: 00 00 00 00 00 00 00 00 00 00 54 41 49 52 54 41   ..........TAIRTA
02D0: 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41   IRTAIRTAIRTAIRTA
02E0: 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41   IRTAIRTAIRTAIRTA
02F0: 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41   IRTAIRTAIRTAIRTA
0300: 49 52 54 41 49 52 54 41 49 52 54 41 52 41 20 49   IRTAIRTAIRTARA I
0310: 50 41 52 50 56 4F 44 45 44 20 54 41 20 41 45 48   PARPVODED TA AEH
0320: 44 41 52 45 41 20 52 54 29 49 EC 3C 59 B2 FE E9   DAREA RT)I.<Y...

Reversed longs:
0300: 49 52 54 41 49 52 54 41 49 52 54 41 52 41 20 49   IRTAIRTAIRTARA I
0300: 49 52 54 41 49 52 54 41 49 52 54 41 52 41 20 49   ATRIATRIATRII AR
0310: 50 41 52 50 56 4F 44 45 44 20 54 41 20 41 45 48   PARPVODED TA AEH
0310: 50 41 52 50 56 4F 44 45 44 20 54 41 20 41 45 48   PRAPEDOVAT DHEA 
0320: 44 41 52 45 41 20 52 54 29 49 EC 3C 59 B2 FE E9   DAREA RT)I.<Y...
0320: 44 41 52 45 41 20 52 54 29 49 EC 3C 59 B2 FE E9   ERADTR A<.I)...Y

Longs read in from the I2S channel are done as (RIGHT << 16) | LEFT, where
LEFT and RIGHT are 16-bit words. So there doesn't seem to be any way to make
a long read work properly

Superfly (word swapped):
0170: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0180: 00 00 41 54 52 49 41 54 52 49 41 54 52 49 41 54   ..ATRIATRIATRIAT
0190: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
01A0: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
01B0: 52 49 41 54 52 49 41 54 52 49 41 54 52 49 41 54   RIATRIATRIATRIAT
01C0: 52 49 41 54 41 52 49 20 41 50 50 52 4F 56 45 44   RIATARI APPROVED
01D0: 20 44 41 54 41 20 48 45 41 44 45 52 20 41 54 52    DATA HEADER ATR
01E0: 49 23 56 B4 56 49 80 68 79 36 C7 DC C8 84 86 B7   I#V.VI.hy6......
01F0: 29 16 90 15 7B 49 7E 9A 81 4D B3 54 8C 82 76 FD   )...{I~..M.T..v.
0200: AE 79 94 3F 1D 52 30 FF F8 46 24 8B F9 1E 3B 50   .y.?.R0..F$...;P

Baldies audio (ripped):
00000000  52 49 46 46 14 24 44 02  57 41 56 45 66 6d 74 20  |RIFF.$D.WAVEfmt |
00000010  10 00 00 00 01 00 02 00  44 ac 00 00 10 b1 02 00  |........D.......|
00000020  04 00 10 00 64 61 74 61  f0 23 44 02|00 00 00 00  |....data.#D.....|
00000030  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000014b0  00 00 00 00 00 00 00 00  00 00 00 00 55 ff 43 ff  |............U.C.|
000014c0  fd fe e9 fe 89 fe 76 fe  2e fe 1a fe 0c fe f5 fd  |......v.........|
000014d0  24 fe 0c fe 4a fe 34 fe  3e fe 2b fe 0a fe f6 fd  |$...J.4.>.+.....|
000014e0  0c fe f6 fd 5f fe 48 fe  a3 fe 8c fe be fe aa fe  |...._.H.........|
000014f0  48 ff 34 ff 7b 00 63 00  6a 01 49 01 1b 01 f4 00  |H.4.{.c.j.I.....|
00001500  ef ff c9 ff 11 ff f2 fe  ee fe d3 fe 0b ff ee fe  |................|
00001510  02 ff e4 fe e8 fe cb fe  df fe c5 fe f2 fe da fe  |................|
00001520  3c ff 21 ff bb ff a1 ff  2b 00 15 00 44 00 31 00  |<.!.....+...D.1.|
00001530  1c 00 07 00 07 00 ee ff  1b 00 03 00 07 00 f5 ff  |................|
00001540  96 ff 88 ff 1d ff 09 ff  1c ff 00 ff 8d ff 6d ff  |..............m.|
00001550  f5 ff d8 ff 16 00 fe ff  19 00 02 00 04 00 ec ff  |................|
00001560  af ff 95 ff 32 ff 19 ff  dd fe c5 fe bd fe a4 fe  |....2...........|
00001570  a6 fe 8b fe b7 fe 9c fe  23 ff 09 ff 9d ff 84 ff  |........#.......|
00001580  a0 ff 84 ff 3f ff 21 ff  1f ff 04 ff 78 ff 62 ff  |....?.!.....x.b.|
00001590  be ff a9 ff 6b ff 54 ff  c7 fe ae fe 91 fe 7a fe  |....k.T.......z.|
000015a0  0a ff f8 fe a1 ff 8f ff  ae ff 97 ff 48 ff 2e ff  |............H...|
000015b0  17 ff 00 ff 6e ff 5b ff  ed ff da ff 23 00 0b 00  |....n.[.....#...|
000015c0  10 00 f7 ff fb ff e4 ff  ff ff eb ff 0c 00 f6 ff  |................|
000015d0  10 00 f6 ff 0c 00 f0 ff  09 00 ee ff 0a 00 ef ff  |................|

Baldies audio (raw, word swapped):
0200: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0210: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0220: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0230: FF 55 FF 43 FE FD FE E9 FE 89 FE 76 FE 2E FE 1A   .U.C.......v....
0240: FE 0C FD F5 FE 24 FE 0C FE 4A FE 34 FE 3E FE 2B   .....$...J.4.>.+
0250: FE 0A FD F6 FE 0C FD F6 FE 5F FE 48 FE A3 FE 8C   ........._.H....
0260: FE BE FE AA FF 48 FF 34 00 7B 00 63 01 6A 01 49   .....H.4.{.c.j.I
0270: 01 1B 00 F4 FF EF FF C9 FF 11 FE F2 FE EE FE D3   ................
0280: FF 0B FE EE FF 02 FE E4 FE E8 FE CB FE DF FE C5   ................
0290: FE F2 FE DA FF 3C FF 21 FF BB FF A1 00 2B 00 15   .....<.!.....+..
02A0: 00 44 00 31 00 1C 00 07 00 07 FF EE 00 1B 00 03   .D.1............
02B0: 00 07 FF F5 FF 96 FF 88 FF 1D FF 09 FF 1C FF 00   ................
02C0: FF 8D FF 6D FF F5 FF D8 00 16 FF FE 00 19 00 02   ...m............
02D0: 00 04 FF EC FF AF FF 95 FF 32 FF 19 FE DD FE C5   .........2......
02E0: FE BD FE A4 FE A6 FE 8B FE B7 FE 9C FF 23 FF 09   .............#..
02F0: FF 9D FF 84 FF A0 FF 84 FF 3F FF 21 FF 1F FF 04   .........?.!....
0300: FF 78 FF 62 FF BE FF A9 FF 6B FF 54 FE C7 FE AE   .x.b.....k.T....
0310: FE 91 FE 7A FF 0A FE F8 FF A1 FF 8F FF AE FF 97   ...z............
0320: FF 48 FF 2E FF 17 FF 00 FF 6E FF 5B FF ED FF DA   .H.......n.[....
*/

//
// CD-ROM memory access functions
//
uint16_t CDROMReadWord(uint32_t offset, uint32_t who/*=UNKNOWN*/)
{
	uint16_t data = 0x0000;

	switch (offset)
	{
	case BUTCH:
		data = butch >> 16;
		WriteLog("CDROM: BUTCH+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case BUTCH + 2:
		data = butch & 0xFFFF;
		WriteLog("CDROM: BUTCH+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case DSCNTRL:
		data = butchDSCntrl >> 16;
		WriteLog("CDROM: DSCNTRL+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case DSCNTRL + 2:
		data = butchDSCntrl & 0xFFFF;
		WriteLog("CDROM: DSCNTRL+2 is $%X [%s]\n", data, whoName[who]);
		// We know it does this much, does it do more than this?
		if (dsfStart == dsfEnd)
			butch &= ~DSA_RX_INT_PENDING;

		break;
	case DS_DATA:
		data = ReadDSFIFO();
		WriteLog("CDROM: DS_DATA is $%X [%s]\n", data, whoName[who]);
		break;
	case I2CNTRL:
		data = butchI2Cntrl >> 16;
		WriteLog("CDROM: I2CNTRL+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case I2CNTRL + 2:
		data = butchI2Cntrl & 0xFFFF;
		WriteLog("CDROM: I2CNTRL+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case SBCNTRL:
		WriteLog("CDROM: SBCNTRL+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case SBCNTRL + 2:
		WriteLog("CDROM: SBCNTRL+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case SUBDATA:
		WriteLog("CDROM: SUBDATA+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case SUBDATA + 2:
		WriteLog("CDROM: SUBDATA+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case SUBDATB:
		WriteLog("CDROM: SUBDATB+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case SUBDATB + 2:
		WriteLog("CDROM: SUBDATB+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case SB_TIME:
		WriteLog("CDROM: SB_TIME+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case SB_TIME + 2:
		WriteLog("CDROM: SB_TIME+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case FIFO_DATA:
		WriteLog("CDROM: FIFO_DATA+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case FIFO_DATA + 2:
		WriteLog("CDROM: FIFO_DATA+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case I2SDAT2:
		WriteLog("CDROM: I2SDAT2+0 is $%X [%s]\n", data, whoName[who]);
		break;
	case I2SDAT2 + 2:
		WriteLog("CDROM: I2SDAT2+2 is $%X [%s]\n", data, whoName[who]);
		break;
	case I2SBUS + 2:
		data = (uint16_t)ButchEEReadLong();
//		WriteLog("CDROM: EERead returned $%X [%s]\n", data, whoName[who]);
		break;
	// Knowns we don't want logged...
	case I2SBUS:
		break;
	default:
		WriteLog("CDROM: Unknown read at $%06X by %s...\n", offset, whoName[who]);
		break;
	}

	return data;
}


void CDROMWriteWord(uint32_t offset, uint16_t data, uint32_t who/*=UNKNOWN*/)
{
	switch (offset)
	{
	case BUTCH:
		WriteLog("CDROM: Write of $%04X to BUTCH+0 [%s]\n", data, whoName[who]);
		butch = (butch & 0x0000FFFF) | (data << 16);
		HandleButchControl();
		break;
	case BUTCH + 2:
		WriteLog("CDROM: Write of $%04X to BUTCH+2 [%s]\n", data, whoName[who]);
		butch = (butch & 0xFFFF0000) | data;
		HandleButchControl();
		break;
	case DSCNTRL:
		WriteLog("CDROM: Write of $%04X to DSCNTRL+0 [%s]\n", data, whoName[who]);
		butchDSCntrl = (butchDSCntrl & 0x0000FFFF) | (data << 16);
		break;
	case DSCNTRL + 2:
		WriteLog("CDROM: Write of $%04X to DSCNTRL+2 [%s]\n", data, whoName[who]);
		butchDSCntrl = (butchDSCntrl & 0xFFFF0000) | data;
		break;
	case DS_DATA:
		WriteLog("CDROM: Write of $%04X to DS_DATA [%s]\n", data, whoName[who]);
		ButchCommand(data);
		break;
	case I2CNTRL:
		WriteLog("CDROM: Write of $%04X to I2CNTRL+0 [%s]\n", data, whoName[who]);
		butchI2Cntrl = (butchI2Cntrl & 0x0000FFFF) | (data << 16);
		break;
	case I2CNTRL + 2:
		WriteLog("CDROM: Write of $%04X to I2CNTRL+2 [%s]\n", data, whoName[who]);
		butchI2Cntrl = (butchI2Cntrl & 0xFFFF0000) | data;

		if ((butchI2Cntrl & (I2S_DATA_FROM_CD | I2S_DATA_TO_JERRY | I2S_DATA_ENABLE)) == (I2S_DATA_FROM_CD | I2S_DATA_TO_JERRY | I2S_DATA_ENABLE))
		{
			wordStrobe = 1;
			SetCallbackTime(BUTCHI2SCallback, 1000000.0 / 44100.0, EVENT_JERRY);
		}

		break;
	case SBCNTRL:
		WriteLog("CDROM: Write of $%04X to SBCNTRL+0 [%s]\n", data, whoName[who]);
		butchSBCntrl = (butchI2Cntrl & 0x0000FFFF) | (data << 16);
		break;
	case SBCNTRL + 2:
		WriteLog("CDROM: Write of $%04X to SBCNTRL+2 [%s]\n", data, whoName[who]);
		butchSBCntrl = (butchSBCntrl & 0xFFFF0000) | data;
		break;
	case SB_TIME:
		WriteLog("CDROM: Write of $%04X to SB_TIME+0 [%s]\n", data, whoName[who]);
		butchSBTime = (butchSBTime & 0x0000FFFF) | (data << 16);
		break;
	case SB_TIME + 2:
		WriteLog("CDROM: Write of $%04X to SB_TIME+2 [%s]\n", data, whoName[who]);
		butchSBTime = (butchSBTime & 0xFFFF0000) | data;
		break;
	case I2SBUS + 2:
//		WriteLog("CDROM: Write of $%04X to I2SBUS+2 [%s]\n", data, whoName[who]);
		ButchEEWriteLong((uint32_t)data);
		break;
	// Knowns we don't want logged...
	case I2SBUS:
		break;
	default:
		WriteLog("CDROM: Unknown write of $%04X at $%06X by %s...\n", data, offset, whoName[who]);
		break;
	}
}


void SetButchLine(uint16_t data)
{
	// hmm.
	butch |= data;
}


//
// Queue up a word in the DS_DATA FIFO (internal to BUTCH)
//
static void QueueDSFIFO(uint16_t data)
{
	// Should signal an error somehow...
	if (dsfStart == ((dsfEnd + 1) & FIFO_MASK))
	{
		WriteLog("CDROM: DS FIFO overflowed! :-(\n");
		return;
	}

	dsfifo[dsfEnd] = data;
	dsfEnd = (dsfEnd + 1) & FIFO_MASK;
	SetButchLine(DSA_RX_INT_PENDING);
}


//
// Read a word from the DS_DATA FIFO (internal to BUTCH)
//
static uint32_t ReadDSFIFO(void)
{
	uint32_t data = 0xFFFF;

	// We return data only if there is any in the FIFO
	if (dsfStart != dsfEnd)
	{
		data = (uint32_t)dsfifo[dsfStart];
		dsfStart = (dsfStart + 1) & FIFO_MASK;
	}

	return data;
}


//
// Handle changes in BUTCH requested by users
//
static void HandleButchControl(void)
{
	// Did they turn off the CD BIOS?
	if (!(butch & BUTCH_BIOS_ENABLE))
	{
		// Need to set up cart shite, since this is all done thru the BIOS
		// For now, we'll just blank out the cart
		memset(jaguarMainROM, 0xFF, 0x200000);
	}
}


//
// Handle command sent to BUTCH thru DS_DATA
//
static void ButchCommand(uint16_t cmd)
{
	// Sanity check
	if (haveCDGoodness == false)
		return;

	// Needed??? Maybe...
	if (cmd == 0)
		return;

	SetButchLine(DSA_TX_INT_PENDING);

	// Split off parameter from the command, isolate the command
	uint16_t param = cmd & 0x00FF;
	cmd >>= 8;

	switch (cmd)
	{
	case CMD_PLAY_TRACK:	// Play track <param>
		break;
	case CMD_STOP:			// Stop CD
		; // No return value
		break;
	case CMD_READ_TOC:		// Read TOC, session #<param> (1st session is zero)
	{
		uint16_t status = CDIntfReadShortTOC(param);

		if (status != 0)
			QueueDSFIFO(status);
		else
		{
			QueueDSFIFO(0x2000 | track[0].firstTrack);
			QueueDSFIFO(0x2100 | track[0].lastTrack);
			QueueDSFIFO(0x2200 | track[0].mins);
			QueueDSFIFO(0x2300 | track[0].secs);
			QueueDSFIFO(0x2400 | track[0].frms);
		}

		break;
	}
	case CMD_PAUSE:			// Pause CD
		break;
	case CMD_UNPAUSE:		// Unpause CD
		break;
	case CMD_GET_MSF:		// Get CD time in MSF format
		break;
	case CMD_GOTO_MIN:		// Goto time at <param> minutes
		min = (uint32_t)param;
		break;
	case CMD_GOTO_SEC:		// Goto time at <param> seconds
		sec = (uint32_t)param;
		break;
	case CMD_GOTO_FRM:		// Goto time at <param> frames
		frm = (uint32_t)param;
		// Convert MSF into block #
		currentSector = ((((min * 60) + sec) * 75) + frm) - 150;
		// Send back seek response OK
		QueueDSFIFO(0x0100);
		break;
	case CMD_READ_LONG_TOC:	// Read long TOC, session #<param> (1st session is zero)
	{
		uint16_t status = CDIntfReadLongTOC(param);

		if (status != 0)
			QueueDSFIFO(status);
		else
		{
			for(int i=startTrack; i<=endTrack; i++)
			{
				QueueDSFIFO(0x6000 | track[i].firstTrack);
				QueueDSFIFO(0x6100 | track[i].lastTrack);
				QueueDSFIFO(0x6200 | track[i].mins);
				QueueDSFIFO(0x6300 | track[i].secs);
				QueueDSFIFO(0x6400 | track[i].frms);
			}

			QueueDSFIFO(0x6000);
			QueueDSFIFO(0x6100);
			QueueDSFIFO(0x6200);
			QueueDSFIFO(0x6300);
			QueueDSFIFO(0x6400);
		}

		break;
	}
	case CMD_SET_PLAY_MODE:	// Set CD playback mode (speed, audio/data)
		// Bit 0 - Speed (0 = single, 1 = double)
		// Bit 1 - Mode (0 = audio, 1 = data)
		// Bit 2 - ???
		// Bit 3 - ??? (CD BIOS sets this in data mode for some reason... :-P)
		QueueDSFIFO(0x1700 | param);
		cdSpeed = param & 0x01;
		break;
	case CMD_GET_LAST_ERROR:	// Get last error
		break;
	case CMD_GET_LAST_ERR2:	// Get last error (2)
		break;
	case CMD_SET_SESSION:	// Spin up session #<param>
		break;
	case CMD_SET_START_MIN:	// Play start time at <param> minutes
		break;
	case CMD_SET_START_SEC:	// Play start time at <param> seconds
		break;
	case CMD_SET_START_FRM:	// Play start time at <param> frames
		break;
	case CMD_SET_STOP_MIN:	// Play stop time at <param> minutes
		break;
	case CMD_SET_STOP_SEC:	// Play stop time at <param> seconds
		break;
	case CMD_SET_STOP_FRM:	// Play stop time at <param> frames
		break;
	case CMD_GET_STATUS:	// Get disc and carousel status
		break;
	case CMD_SET_VOLUME:	// Set playback volume
		break;
	case CMD_SESSION_INFO:	// Get session info
		break;
	case CMD_CLEAR_TOC_READ:	// Clear TOC read flag
		break;
	case CMD_OVRSAMPLE_MODE:	// Set DAC mode <param>
		// Need to set vars here...
		QueueDSFIFO((cmd << 8) | param);
		break;
	default:
		WriteLog("CDROM: Unhandled BUTCH command: %04X\n", (cmd << 8) | param);
		break;
	}
}


/*
[18667]
TOC for MYST

CDINTF: Disc summary
        # of sessions: 2, # of tracks: 10
        Session info:
        1: min track= 1, max track= 1, lead out= 1:36:67
        2: min track= 2, max track=10, lead out=55:24:71
        Track info:
         1: start= 0:02:00
         2: start= 4:08:67
         3: start= 4:16:65
         4: start= 4:29:19
         5: start=29:31:03
         6: start=33:38:50
         7: start=41:38:60
         8: start=44:52:18
         9: start=51:51:22
        10: start=55:18:73

CDROM: Read sector 18517 (18667 - 150)...

0000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0018: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0048: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0060: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0078: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00A8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00C0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00D8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00F0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0108: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0120: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0138: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0150: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0168: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0180: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0198: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
01B0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
01C8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
01E0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
01F8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0210: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0228: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0240: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0258: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0270: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0288: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02A0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02B8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02D0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02E8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0300: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0318: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0330: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0348: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0360: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0378: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0390: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
03A8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
03C0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
03D8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
03F0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0408: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0420: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0438: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0450: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0468: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0480: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0498: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
04B0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
04C8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
04E0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
04F8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0510: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0528: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0540: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0558: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0570: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0588: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
05A0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
05B8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
05D0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
05E8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0600: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0618: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0630: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0648: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0660: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0678: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0690: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
06A8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
06C0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
06D8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
06F0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0708: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0720: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0738: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0750: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0768: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0780: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0798: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
07B0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
07C8: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00[54 41 49 52]54 41
07E0: 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41
07F8: 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41 49 52 54 41
0810: 49 52 54 41 49 52[54 41 49 52]54 41 52 41 20 49 50 41 52 50 56 4F 44 45
0828: 44 20 54 41 20 41 45 48 44 41 52 45 41 20 52 54 20 49[00 00 00 50]01 00
0840: 80 83 FC 23 07 00 07 00 F0 00 0C 21 FC 23 07 00 07 00 F1 00 0C A1 FC 33
0858: FF FF F0 00 4E 00 7C 2E 1F 00 FC FF 00 61 08 00 F9 4E 00 00 00 51 E7 48
0870: 00 FE 39 30 F1 00 02 40 40 02 10 00 00 67 1C 00 79 42 01 00 8C D3 3C 34
0888: 37 03 3C 30 81 05 3C 3C 0A 01 3C 38 F1 00 00 60 1A 00 FC 33 01 00 01 00
08A0: 8C D3 3C 34 4B 03 3C 30 65 05 3C 3C 42 01 3C 38 1F 01 C0 33 01 00 88 D3
08B8: C4 33 01 00 8A D3 00 32 41 E2 41 94 7C D4 04 00 7C 92 01 00 41 00 00 04
08D0: C1 33 01 00 82 D3 C1 33 F0 00 3C 00 C2 33 01 00 80 D3 C2 33 F0 00 38 00
08E8: C2 33 F0 00 3A 00 06 3A 44 9A C5 33 01 00 84 D3 44 DC C6 33 01 00 86 D3
0900: F9 33 01 00 84 D3 F0 00 46 00 FC 33 FF FF F0 00 48 00 FC 23 00 00 00 00
0918: F0 00 2A 00 FC 33 00 00 F0 00 58 00 DF 4C 7F 00 75 4E 00 00 00 00 00 00

Raw P-W subchannel data:

00: 80 80 C0 80 80 80 80 C0 80 80 80 80 80 80 C0 80
10: 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80
20: 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 C0
30: 80 80 80 80 80 80 80 80 80 80 80 80 80 C0 80 80
40: 80 80 80 80 C0 80 80 80 80 C0 C0 80 80 C0 C0 80
50: C0 80 80 C0 C0 C0 80 80 C0 80 80 80 C0 80 80 80

P subchannel data: FF FF FF FF FF FF FF FF FF FF FF FF
Q subchannel data: 21 02 00 00 00 01 00 04 08 66 9C 88

Run address: $5000, Length: $18380
*/

