//
// CDINTF.H: OS agnostic CDROM access funcions
//
// by James Hammons
//

#ifndef __CDINTF_H__
#define __CDINTF_H__

#include <stdint.h>

struct TOCEntry
{
	uint16_t firstTrack;
	uint16_t lastTrack;
	uint16_t mins;
	uint16_t secs;
	uint16_t frms;
};


bool CDIntfInit(void);
void CDIntfDone(void);
uint16_t CDIntfReadShortTOC(uint32_t session);
uint16_t CDIntfReadLongTOC(uint32_t session);
bool CDIntfReadBlock(uint32_t, uint8_t *);
uint32_t CDIntfGetNumSessions(void);
void CDIntfSelectDrive(uint32_t);
uint32_t CDIntfGetCurrentDrive(void);
const uint8_t * CDIntfGetDriveName(uint32_t);
uint8_t CDIntfGetSessionInfo(uint32_t, uint32_t);
uint8_t CDIntfGetTrackInfo(uint32_t, uint32_t);

// Exported vars
extern TOCEntry track[99];
extern uint16_t startTrack;
extern uint16_t endTrack;
//extern uint16_t numTracks;
extern uint16_t numSessions;

#endif	// __CDINTF_H__

