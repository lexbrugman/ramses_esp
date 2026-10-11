/********************************************************************
 * ramses_esp
 * ramses_ota.h
 *
 * (C) 2023 Peter Price
 *
 * Over The Air upgrade
 *
 */
#ifndef _RAMSES_OTA_H_
#define _RAMSES_OTA_H_

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

// Snapshot of the OTA state, for reporting it to Home Assistant
struct ota_status {
  uint32_t seq;      // Changes whenever any other field changes
  char latest[33];   // Version published on the OTA channel, "" until known
  bool in_progress;
  int8_t percent;    // Download progress, -1 when unknown
};

extern void ota_get_status( struct ota_status *status );
extern char const *ota_channel( void );
extern char const *ota_release_url( void );

extern void ota_set_url( const char * url );
extern void ota_check( void );
extern void ota_start( uint8_t force);

extern void ramses_ota_init( BaseType_t coreID );

#endif // _RAMSES_OTA_H_
