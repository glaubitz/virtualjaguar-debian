//
// OS agnostic CDROM interface functions
//
// by James Hammons
// (C) 2010 Underground Software
//
// JLH = James Hammons <jlhamm@acm.org>
//
// Who  When        What
// ---  ----------  ------------------------------------------------------------
// JLH  01/16/2010  Created this log ;-)
//

//
// This now uses the supposedly cross-platform libcdio to do the necessary
// low-level CD twiddling we need that libSDL can't do currently. Jury is
// still out on whether or not to make this a conditional compilation or not.
//

// Comment this out if you don't have libcdio installed
// (Actually, this is defined in the Makefile to prevent having to edit
//  things too damn much. Jury is still out whether or not to make this
//  change permanent.)
//#define HAVE_LIB_CDIO

#include "cdintf.h"				// Every OS has to implement these

#ifdef HAVE_LIB_CDIO
#include <cdio/cdio.h>			// Now using OS agnostic CD access routines!
#include <cdio/util.h>
#endif
#include "log.h"

// Private function prototypes



#ifdef HAVE_LIB_CDIO
static CdIo_t * cdHandle = NULL;
#endif

// Exported vars
TOCEntry track[99];
uint16_t startTrack;
uint16_t endTrack;
//uint16_t numTracks;
uint16_t numSessions;


bool CDIntfInit(void)
{
#ifdef HAVE_LIB_CDIO
	// The native CD-ROM routines all hinge on this open call: if the open call
	// fails, the emulated CD-ROM will not use any of the other CDIntf*
	// functions. Those functions all operate under the assumption that the open
	// call was successful.
	cdHandle = cdio_open(NULL, DRIVER_DEVICE);

	if (cdHandle == NULL)
	{
		WriteLog("CDINTF: No suitable CD-ROM driver found.\n");
		return false;
	}

	WriteLog("CDINTF: Successfully opened CD-ROM interface.\n");

//simple TOC reading test goes here...
#if 0
	track_t first_track_num = cdio_get_first_track_num(cdHandle);
	track_t i_tracks        = cdio_get_num_tracks(cdHandle);
	int j, i=first_track_num;

	printf("CD-ROM Track List (%i - %i)\n", first_track_num, i_tracks);
	printf("  #:  LSN\n");

	for(j=0; j<i_tracks; i++, j++)
	{
		lsn_t lsn = cdio_get_track_lsn(cdHandle, i);

		if (CDIO_INVALID_LSN != lsn)
			printf("%3d: %06d\n", (int) i, lsn);
	}

	printf("%3X: %06d  leadout\n", CDIO_CDROM_LEADOUT_TRACK,
		cdio_get_track_lsn(cdHandle, CDIO_CDROM_LEADOUT_TRACK));

	lsn_t lastSession;
	driver_return_code_t code = cdio_get_last_session(cdHandle, &lastSession);

	printf("cdio_get_last_session LSN = %d\n", lastSession);
#endif
/*
Superfly DX:
-------------------------------------------------------------------------------
CD-ROM Track List (1 - 6)
  #:  LSN
  1: 000000 [00:02:00] <-- msf = LSN + 150...
  2: 003346 [00:46:46]

  3: 015640 [03:30:40]
  4: 016258 [03:38:58]
  5: 016927 [03:47:52]
  6: 017596 [03:56:46]
 AA: 018044  leadout [04:02:44]
Last session start LSN: 015640


Baldies:
-------------------------------------------------------------------------------
CD-ROM Track List (1 - 11)
  #:  LSN
  1: 000000 [00:02:00] {00:02:00}

  2: 016165 [03:37:40] {03:37:40}
  3: 016759 [03:45:34] {03:45:34}
  4: 021483 [04:48:33] {04:48:33}
  5: 022971 [05:08:21] {05:08:21}
  6: 034938 [07:47:63] {07:47:63}
  7: 035535 [07:55:60] {07:55:60}
  8: 036132 [08:03:57] {08:03:57}
  9: 036729 [08:11:54] {08:11:54}
 10: 037326 [08:19:51] {08:19:51}
 11: 038460 [08:34:60] {08:34:60}
 AA: 038907  leadout [08:40:57]
Last session start LSN: 016165
*/

	return true;
#else
	WriteLog("CDINTF: CDIO not compiled into Jaguar core; CD-ROM will be unavailable.\n");
	return false;
#endif
}


void CDIntfDone(void)
{
	WriteLog("CDINTF: Shutting down CD-ROM subsystem.\n");

#ifdef HAVE_LIB_CDIO
	if (cdHandle)
		cdio_destroy(cdHandle);
#endif
}


//
// Convert LSN to MSF. We have to write this because libcdio doesn't have one.
// :-P
//
msf_t LSNToMSF(int lsn)
{
	// MSF is ahead of LSN by 150 frames for some reason...
	lsn += 150;

	// 75 frames to a second...
	int hi = lsn / 75, lo = lsn % 75;

	msf_t time;
	time.f = lo;
	time.s = hi % 60;
	time.m = hi / 60;

	return time;
}


//
// We use the "TOCEntry" struct here (exported as track[]), using track[0] as
// our "short" TOC for the desired session.
// According to the BIOS source, this is reading the min/max track for this
// session, and the session lead out. Returns 0 on success, or DSA error code.
//
uint16_t CDIntfReadShortTOC(uint32_t session)
{
#ifdef HAVE_LIB_CDIO
	// Sanity check
	if (!cdHandle)
		return 0xFFFF;

	track_t firstTrack = cdio_get_first_track_num(cdHandle);
	track_t numTracks = cdio_get_num_tracks(cdHandle);

	// This gets the first track of the last session of the disc :-/
	lsn_t lastSessionLSN;
	driver_return_code_t code = cdio_get_last_session(cdHandle, &lastSessionLSN);
	lsn_t firstTrackLSN = cdio_get_track_lsn(cdHandle, 1);
	lsn_t leadOutLSN = cdio_get_track_lsn(cdHandle, CDIO_CDROM_LEADOUT_TRACK);

	// Figure out if we have a multisession disc or not
	if (lastSessionLSN == firstTrackLSN)
	{
		// Return DSA error "Illegal value" if session # is out of whack (i.e.,
		// it's a single session disc and the user is requesting a higher
		// session #).
		if (session > 0)
			return 0x0429;

		// Not a multisession disc...
		msf_t time = LSNToMSF(leadOutLSN);

		track[0].firstTrack = firstTrack;
		track[0].lastTrack = numTracks;
		track[0].mins = time.m;
		track[0].secs = time.s;
		track[0].frms = time.f;

		return 0x0000;
	}

	// Multisession disc!
	// The way libcdio does multisession (that is, hardly at all) means that we
	// are forced to make an assumption: multisession discs have only two
	// sessions. Thus the following ickyness.
	if (session == 0)
	{
		track[0].firstTrack = firstTrack;

		for(int i=1; i<=numTracks; i++)
		{
			lsn_t currentTrack = cdio_get_track_lsn(cdHandle, i);

			if (lastSessionLSN == currentTrack)
			{
				msf_t time = LSNToMSF(currentTrack);
				track[0].lastTrack = i - 1;
				track[0].mins = time.m;
				track[0].secs = time.s;
				track[0].frms = time.f;
				break;
			}
		}
	}
	else if (session == 1)
	{
		track[0].lastTrack = numTracks;

		for(int i=1; i<=numTracks; i++)
		{
			lsn_t currentTrack = cdio_get_track_lsn(cdHandle, i);

			if (lastSessionLSN == currentTrack)
			{
				msf_t time = LSNToMSF(leadOutLSN);
				track[0].firstTrack = i;
				track[0].mins = time.m;
				track[0].secs = time.s;
				track[0].frms = time.f;
				break;
			}
		}
	}
	else
	{
		// > 2 sessions == failure; return DSA error "Invalid value"
		return 0x0429;
	}

	return 0x0000;
#else
	return 0xFFFF;
#endif
}


//
// Return values are sent in the track[] array; *the* return value from this
// function is either 0 for success or the error code we want to return from
// BUTCH.
//
uint16_t CDIntfReadLongTOC(uint32_t session)
{
#ifdef HAVE_LIB_CDIO
	// Sanity check
	if (!cdHandle)
		return false;

	track_t firstTrack = cdio_get_first_track_num(cdHandle);
	track_t numTracks = cdio_get_num_tracks(cdHandle);

	// This gets the first track of the last session of the disc :-/
	lsn_t lastSessionLSN;
	driver_return_code_t code = cdio_get_last_session(cdHandle, &lastSessionLSN);
	lsn_t firstTrackLSN = cdio_get_track_lsn(cdHandle, 1);
	msf_t time;

	startTrack = 1;
	endTrack = numTracks;

	// Figure out if we have a multisession disc or not
	if (lastSessionLSN == firstTrackLSN)
	{
		// Return DSA error "Illegal value" if session # is out of whack (i.e.,
		// it's a single session disc and the user is requesting a higher
		// session #).
		if (session > 0)
			return 0x0429;

		// Not a multisession disc...
		for(int i=1; i<=numTracks; i++)
		{
			bool success = cdio_get_track_msf(cdHandle, i, &time);

			track[i].firstTrack = i;
			track[i].lastTrack = 0x01;
			track[i].mins = cdio_from_bcd8(time.m);
			track[i].secs = cdio_from_bcd8(time.s);
			track[i].frms = cdio_from_bcd8(time.f);
		}

		return 0x0000;
	}

	// We have a multisession disc! Return the tracks of the session requested.
	if (session == 0)
	{
WriteLog("CDINTF: In session 0, getting tracks (lastSessionLSN=%i)...\n", lastSessionLSN);
		for(int i=1; i<=numTracks; i++)
		{
			bool success = cdio_get_track_msf(cdHandle, i, &time);
			lsn_t trackLSN = cdio_get_track_lsn(cdHandle, i);
WriteLog("CDINTF: Getting data for track %i (%02X:%02X:%02X, LSN=%i)\n", i, time.m, time.s, time.f, trackLSN);

			if (trackLSN == lastSessionLSN)
			{
				endTrack = i - 1;
				break;
			}

			track[i].firstTrack = i;
			track[i].lastTrack = 0x01;
			track[i].mins = cdio_from_bcd8(time.m);
			track[i].secs = cdio_from_bcd8(time.s);
			track[i].frms = cdio_from_bcd8(time.f);
		}
WriteLog("CDINTF: Start/end track is %i/%i\n", startTrack, endTrack);
	}
	else if (session == 1)
	{
WriteLog("CDINTF: In session 1, getting tracks (lastSessionLSN=%i)...\n", lastSessionLSN);
		bool seenSession2 = false;
//		int j = 1;

		for(int i=1; i<=numTracks; i++)
		{
			bool success = cdio_get_track_msf(cdHandle, i, &time);
			lsn_t trackLSN = cdio_get_track_lsn(cdHandle, i);

			if (trackLSN == lastSessionLSN)
			{
				startTrack = i;
				seenSession2 = true;
			}

			if (seenSession2)
			{
WriteLog("CDINTF: Getting data for track %i (%02X:%02X:%02X, LSN=%i)\n", i, time.m, time.s, time.f, trackLSN);
				track[i].firstTrack = i;//j++;
				track[i].lastTrack = 0x01;
				track[i].mins = cdio_from_bcd8(time.m);
				track[i].secs = cdio_from_bcd8(time.s);
				track[i].frms = cdio_from_bcd8(time.f);
			}
		}
WriteLog("CDINTF: Start/end track is %i/%i\n", startTrack, endTrack);
	}
	else
		return 0x0429;

	return 0x0000;
#else
	return 0xFFFF;
#endif
}


bool CDIntfReadBlock(uint32_t sector, uint8_t * buffer)
{
#ifdef HAVE_LIB_CDIO
	driver_return_code_t code = cdio_read_sector(cdHandle, buffer, sector, CDIO_READ_MODE_AUDIO);

	return (code == DRIVER_OP_SUCCESS ? true: false);
#else
	return false;
#endif
}


uint32_t CDIntfGetNumSessions(void)
{
#ifdef HAVE_LIB_CDIO
#warning "!!! FIX !!! CDIntfGetNumSessions not implemented!"
	// Still need relevant code here... !!! FIX !!!
	return 2;
#else
	return 0;
#endif
}


void CDIntfSelectDrive(uint32_t driveNum)
{
#ifdef HAVE_LIB_CDIO
#warning "!!! FIX !!! CDIntfSelectDrive not implemented!"
	// !!! FIX !!!
	WriteLog("CDINTF: SelectDrive unimplemented!\n");
#endif
}


uint32_t CDIntfGetCurrentDrive(void)
{
#ifdef HAVE_LIB_CDIO
#warning "!!! FIX !!! CDIntfGetCurrentDrive not implemented!"
	WriteLog("CDINTF: GetCurrentDrive unimplemented!\n");
	return 0;
#else
	return 0;
#endif
}


const uint8_t * CDIntfGetDriveName(uint32_t driveNum)
{
#warning "!!! FIX !!! CDIntfGetDriveName driveNum is currently ignored!"
	// driveNum is currently ignored... !!! FIX !!!

#ifdef HAVE_LIB_CDIO
	uint8_t * driveName = (uint8_t *)cdio_get_default_device(cdHandle);
	WriteLog("CDINTF: The drive name for the current driver is %s.\n", driveName);

	return driveName;
#else
	return (uint8_t *)"NONE";
#endif
}


uint8_t CDIntfGetSessionInfo(uint32_t session, uint32_t offset)
{
#ifdef HAVE_LIB_CDIO
#warning "!!! FIX !!! CDIntfGetSessionInfo not implemented!"
	WriteLog("CDINTF: GetSessionInfo unimplemented!\n");
	return 0xFF;
#else
	return 0xFF;
#endif
}


//
// The track parameter is easy to figure out, but what is the offset???
//
uint8_t CDIntfGetTrackInfo(uint32_t track, uint32_t offset)
{
#ifdef HAVE_LIB_CDIO
#warning "!!! FIX !!! CDIntfTrackInfo not implemented!"
	WriteLog("CDINTF: GetTrackInfo unimplemented!\n");
	return 0xFF;
#else
	return 0xFF;
#endif
}

