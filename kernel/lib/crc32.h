#ifndef NEOOS_CRC32_H
#define NEOOS_CRC32_H

#include <stdint.h>

// Standard CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), as
// GPT uses. Pass 0 to start; pass a previous result to continue.
// crc32(0, "123456789", 9) == 0xCBF43926.
uint32_t crc32(uint32_t crc, const void *buf, uint64_t len);

#endif
