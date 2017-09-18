//
// eeprom.h: Header file
//

#ifndef __EEPROM_H__
#define __EEPROM_H__

#include <stdint.h>

void EepromInit(void);
void EepromReset(void);
void EepromDone(void);

uint8_t EepromReadByte(uint32_t offset);
uint16_t EepromReadWord(uint32_t offset);
void EepromWriteByte(uint32_t offset, uint8_t data);
void EepromWriteWord(uint32_t offset, uint16_t data);
uint32_t ButchEEReadLong(void);
void ButchEEWriteLong(uint32_t data);

#endif	// __EEPROM_H__

