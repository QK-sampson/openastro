/*****************************************************************************
 *
 * ptrController.c -- PTR device control functions
 *
 * Copyright 2015,2016,2017 James Fidell (james@openastroproject.org)
 *
 * License:
 *
 * This file is part of the Open Astro Project.
 *
 * The Open Astro Project is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The Open Astro Project is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the Open Astro Project.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

#include <oa_common.h>

#include <pthread.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>
#include <ctype.h>
#include <sys/select.h>

#include <openastro/util.h>
#include <openastro/timer.h>
#include <openastro/ptr/controls.h>

#include "oaptrprivate.h"
#include "unimplemented.h"
#include "ptr.h"


static int	_processSetControl ( PRIVATE_INFO*, OA_COMMAND* );
static int	_processGetControl ( PRIVATE_INFO*, OA_COMMAND* );
static int	_processReset ( PRIVATE_INFO* );
static int	_processPTRStart ( PRIVATE_INFO*, OA_COMMAND* );
static int	_processPTRStop ( PRIVATE_INFO* );
static int	_processTimestampFetch ( PRIVATE_INFO*, OA_COMMAND* );
static int	_processGPSFetch ( PRIVATE_INFO*, OA_COMMAND* );
static int	_doSync ( PRIVATE_INFO* );
static int	_readTimestamp ( uint32_t, int, char* );


void*
oaPTRcontroller ( void* param )
{
  oaPTR*		device = param;
  PRIVATE_INFO*		deviceInfo = device->_private;
  OA_COMMAND*		command;
  int			exitThread = 0;
  int			resultCode, running = 0;
  fd_set		readable;
  struct timeval	timeout;
  // use the longest version here
  char			readBuffer[ PTR_TIMESTAMP_BUFFER_LEN_V1_1 + 1 ];
  char			numberBuffer[ 8 ];
  int			frameNumber, numRead, available, i;
  int			timestampLength;

  timestampLength = ( deviceInfo->version < 0x0101 ) ?
      PTR_TIMESTAMP_BUFFER_LEN_V1_0 : PTR_TIMESTAMP_BUFFER_LEN_V1_1;
  timeout.tv_sec = 0;
  timeout.tv_usec = 10000;
  do {
    pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
    exitThread = deviceInfo->stopControllerThread;
    pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );
    if ( exitThread ) {
      break;
    } else {
      pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
      // stop us busy-waiting
      if ( oaDLListIsEmpty ( deviceInfo->commandQueue )) {
        pthread_cond_wait ( &deviceInfo->commandQueued,
            &deviceInfo->commandQueueMutex );
      }
      pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );
    }

    pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
    running = deviceInfo->isRunning;
    pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );

    if ( running ) {
      FD_ZERO ( &readable );
      FD_SET ( deviceInfo->fd, &readable );
      if ( select ( deviceInfo->fd + 1, &readable, 0, 0, &timeout ) == 1 ) {
        numRead = _readTimestamp ( deviceInfo->version, deviceInfo->fd,
            readBuffer );
        if ( numRead != timestampLength ) {
          fprintf ( stderr, "%s: read incorrect timestamp length %d (",
              __FUNCTION__, numRead );
          if ( numRead > 0 ) {
            for ( i = 0; i < numRead; i++ ) {
              if ( readBuffer[i] < 32 ) {
                fprintf ( stderr, "%02x ", readBuffer[i] );
              } else {
                fprintf ( stderr, "%c ", readBuffer[i] );
              }
            }
          }
          fprintf ( stderr, ")\n" );
        } else {
          readBuffer[numRead] = 0;
          numRead--;
          while ( readBuffer[numRead] == '\r' ||
              readBuffer[numRead] == '\n' ) {
            readBuffer[numRead--] = 0;
          }
          if (( readBuffer[0] != 'T' && readBuffer[0] != 'S' ) ||
              readBuffer[1] != ':' || readBuffer[8] != ':' ) {
            if ( strncmp ( readBuffer, "Acquisition sequence complete", 29 )) {
              fprintf ( stderr, "%s: read invalid timestamp format '%s'\n",
                  __FUNCTION__, readBuffer );
            }
          } else {
            strncpy ( numberBuffer, readBuffer + 2, 6 );
            frameNumber = atoi ( numberBuffer );
            if ( frameNumber != deviceInfo->timestampExpected ) {
              fprintf ( stderr, "%s: read timestamp %d, expected %d\n",
                  __FUNCTION__, frameNumber, deviceInfo->timestampExpected );
            } else {
              pthread_mutex_lock ( &deviceInfo->callbackQueueMutex );
              available = deviceInfo->timestampsAvailable;
              pthread_mutex_unlock ( &deviceInfo->callbackQueueMutex );
              if ( available < OA_TIMESTAMP_BUFFERS ) {
                ( void ) strcpy ( deviceInfo->timestampBuffer[
                    ( deviceInfo->timestampExpected - 1 ) %
                    OA_TIMESTAMP_BUFFERS ].timestamp, readBuffer + 9 );
                deviceInfo->timestampBuffer[( deviceInfo->timestampExpected -
                    1 ) % OA_TIMESTAMP_BUFFERS ].index = frameNumber;
                pthread_mutex_lock ( &deviceInfo->callbackQueueMutex );
                if ( deviceInfo->firstTimestamp < 0 ) {
                  deviceInfo->firstTimestamp = 0;
                }
                deviceInfo->timestampsAvailable++;
                pthread_mutex_unlock ( &deviceInfo->callbackQueueMutex );
                pthread_cond_broadcast ( &deviceInfo->callbackQueued );
                if ( !--deviceInfo->timestampCountdown ) {
                  deviceInfo->isRunning = 0;
                }
              } else {
                fprintf ( stderr, "%s: timestamp buffer overflow\n",
                    __FUNCTION__ );
              }
              deviceInfo->timestampExpected++;
            }
          }
        }
      }
    }
    // do {
      command = oaDLListRemoveFromHead ( deviceInfo->commandQueue );
      if ( command ) {
        switch ( command->commandType ) {
          case OA_CMD_CONTROL_SET:
            resultCode = _processSetControl ( deviceInfo, command );
            break;
          case OA_CMD_CONTROL_GET:
            resultCode = _processGetControl ( deviceInfo, command );
            break;
          case OA_CMD_RESET:
            resultCode = _processReset ( deviceInfo );
            break;
          case OA_CMD_START:
            resultCode = _processPTRStart ( deviceInfo, command );
            break;
          case OA_CMD_STOP:
            resultCode = _processPTRStop ( deviceInfo );
            break;
          case OA_CMD_DATA_GET:
            resultCode = _processTimestampFetch ( deviceInfo, command );
            break;
          case OA_CMD_GPS_GET:
            resultCode = _processGPSFetch ( deviceInfo, command );
            break;
          default:
            fprintf ( stderr, "Invalid command type %d in controller\n",
                command->commandType );
            resultCode = -OA_ERR_INVALID_CONTROL;
            break;
        }
        if ( command->callback ) {
//fprintf ( stderr, "CONT: command has callback\n" );
        } else {
          pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
          command->completed = 1;
          command->resultCode = resultCode;
          pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );
          pthread_cond_broadcast ( &deviceInfo->commandComplete );
        }
      }
    // } while ( command );

  } while ( !exitThread );

  return 0;
}


static int
_processSetControl ( PRIVATE_INFO* deviceInfo, OA_COMMAND* command )
{
  int			control = command->controlId;
  oaControlValue*	val = command->commandData;

  switch ( control ) {

    case OA_TIMER_CTRL_SYNC:
      if ( deviceInfo->isRunning ) {
        return -OA_ERR_TIMER_RUNNING;
      }
      return _doSync ( deviceInfo );
      break;

    case OA_TIMER_CTRL_NMEA:
      // FIX ME -- implement this
      return -OA_ERR_INVALID_CONTROL;
      break;

    case OA_TIMER_CTRL_STATUS:
      // FIX ME
      return -OA_ERR_INVALID_CONTROL;
      break;

    case OA_TIMER_CTRL_COUNT:
      if ( val->valueType != OA_CTRL_TYPE_INT32 ) {
        fprintf ( stderr, "%s: invalid control type %d where int32 expected\n",
            __FUNCTION__, val->valueType );
        return -OA_ERR_INVALID_CONTROL_TYPE;
      }
      deviceInfo->requestedCount = val->int32;
      break;

    case OA_TIMER_CTRL_INTERVAL:
      if ( val->valueType != OA_CTRL_TYPE_INT32 ) {
        fprintf ( stderr, "%s: invalid control type %d where int32 expected\n",
            __FUNCTION__, val->valueType );
        return -OA_ERR_INVALID_CONTROL_TYPE;
      }
      deviceInfo->requestedInterval = val->int32;
      break;

    case OA_TIMER_CTRL_MODE:
      if ( val->valueType != OA_CTRL_TYPE_MENU ) {
        fprintf ( stderr, "%s: invalid control type %d where menu expected\n",
            __FUNCTION__, val->valueType );
        return -OA_ERR_INVALID_CONTROL_TYPE;
      }
      deviceInfo->requestedMode = val->menu;
      break;
  }
  return OA_ERR_NONE;
}


static int
_processGetControl ( PRIVATE_INFO* deviceInfo, OA_COMMAND* command )
{
  int			control = command->controlId;
  oaControlValue*	val = command->commandData;

  switch ( control ) {

    case OA_TIMER_CTRL_COUNT:
      val->valueType = OA_CTRL_TYPE_INT32;
      val->int32 = deviceInfo->requestedCount;
      break;

    case OA_TIMER_CTRL_INTERVAL:
      val->valueType = OA_CTRL_TYPE_INT32;
      val->int32 = deviceInfo->requestedInterval;
      break;

    case OA_TIMER_CTRL_MODE:
      val->valueType = OA_CTRL_TYPE_MENU;
      val->menu = deviceInfo->requestedMode;
      break;

    default:
      return -OA_ERR_INVALID_CONTROL;
  }

  return OA_ERR_NONE;
}


static int
_processReset ( PRIVATE_INFO* deviceInfo )
{
  int		ptrDesc = deviceInfo->fd;
  int		numRead;
  char		buffer[512];
  char*		namePtr;
  char*		endPtr;

  if ( deviceInfo->isRunning ) {
    pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
    deviceInfo->isRunning = 0;
    pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );
  }

  tcflush ( ptrDesc, TCIFLUSH );
  usleep ( 100000 );

  // send ctrl-C
  if ( _ptrWrite ( ptrDesc, "\003", 1 )) {
    fprintf ( stderr, "%s: failed to write ctrl-C to %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }
  usleep ( 100000 );

#if 0
  numRead = read ( ptrDesc, buffer, sizeof ( buffer ) - 1 );
  if ( numRead <= 0 ) {
    fprintf ( stderr, "%s: failed to read name from %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }
#endif

  if ( _ptrWrite ( ptrDesc, "sysreset\r", 9 )) {
    fprintf ( stderr, "%s: failed to write sysreset to %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }
  usleep ( 2500000 );

  numRead = read ( ptrDesc, buffer, sizeof ( buffer ) - 1 );
  if ( numRead > 0 ) {
    buffer[numRead] = 0;
    numRead--; 
    while ( buffer[numRead] == '\r' || buffer[numRead] == '\n' ) {
      buffer[numRead--] = 0;
    }
  } else {
    fprintf ( stderr, "%s: failed to read name from %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }

  if (( namePtr = strstr ( buffer, "PTR-" )) && isdigit ( namePtr[4] )
      && namePtr[5] == '.' ) {
    endPtr = namePtr + 5;
    deviceInfo->majorVersion = namePtr[4] - '0';
    deviceInfo->minorVersion = namePtr[6] - '0';
    deviceInfo->version = ( deviceInfo->majorVersion << 8 ) &
        deviceInfo->minorVersion;
    while ( *endPtr++ != ' ' );
    sprintf ( endPtr, "(%s)", deviceInfo->devicePath );
  } else {
    fprintf ( stderr, "%s: Can't find PTR name from %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }

  tcflush ( ptrDesc, TCIFLUSH );
  usleep ( 100000 );

  return OA_ERR_NONE;
}


static int
_processPTRStart ( PRIVATE_INFO* deviceInfo, OA_COMMAND* command )
{
  char		commandStr[128], buffer[128];
  int		commandLen, readBytes;
  CALLBACK*	cb = command->commandData;

  if ( deviceInfo->isRunning ) {
    return -OA_ERR_TIMER_RUNNING;
  }

  if ( cb ) {
    deviceInfo->timestampCallback.callback = cb->callback;
    deviceInfo->timestampCallback.callbackArg = cb->callbackArg;
  } else {
    deviceInfo->timestampCallback.callback = 0;
    deviceInfo->timestampCallback.callbackArg = 0;
  }

  tcflush ( deviceInfo->fd, TCIFLUSH );
  usleep ( 100000 );

  deviceInfo->timestampsAvailable = 0;
  deviceInfo->timestampExpected = 1;
  deviceInfo->timestampCountdown = deviceInfo->requestedCount;
  deviceInfo->firstTimestamp = -1;
  switch ( deviceInfo->requestedMode ) {
    case OA_TIMER_MODE_TRIGGER:
      sprintf ( commandStr, "trigger %d %3.3f\r", deviceInfo->requestedCount,
          ( float ) deviceInfo->requestedInterval / 1000 );
      break;

    case OA_TIMER_MODE_STROBE:
      sprintf ( commandStr, "strobe %d\r", deviceInfo->requestedCount );
      break;

    default:
      return -OA_ERR_INVALID_TIMER_MODE;
      break;
  }

  commandLen = strlen ( commandStr );
  if ( _ptrWrite ( deviceInfo->fd, commandStr, commandLen )) {
    fprintf ( stderr, "%s: failed to write command:\n%s\n  to %s\n",
        __FUNCTION__, commandStr, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }

  if (( readBytes = _ptrRead ( deviceInfo->fd, buffer, 127 )) !=
      commandLen + 1 ) {
    fprintf ( stderr, "%s: failed to read back command:\n%s\n"
        "  from %s, commandLen = %d, read len = %d\n",
        __FUNCTION__, commandStr, deviceInfo->devicePath, commandLen,
        readBytes );
    if ( readBytes > 0 ) {
      buffer[ readBytes ] = 0;
      fprintf ( stderr, "  string read = '%s'\n", buffer );
    }
    return -OA_ERR_SYSTEM_ERROR;
  }

  pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
  deviceInfo->isRunning = 1;
  pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );

  return -OA_ERR_NONE;
}


static int
_processPTRStop ( PRIVATE_INFO* deviceInfo )
{
  int           ptrDesc = deviceInfo->fd;
  int		queueEmpty;

  if ( !deviceInfo->isRunning ) {
    return -OA_ERR_INVALID_COMMAND;
  }

  pthread_mutex_lock ( &deviceInfo->commandQueueMutex );
  deviceInfo->isRunning = 0;
  pthread_mutex_unlock ( &deviceInfo->commandQueueMutex );

  // only do this if running?
  {
    tcflush ( ptrDesc, TCIFLUSH );
    usleep ( 100000 );
    // send ctrl-C
    if ( _ptrWrite ( ptrDesc, "\003", 1 )) {
      fprintf ( stderr, "%s: failed to write ctrl-C to %s\n",
          __FUNCTION__, deviceInfo->devicePath );
      return -OA_ERR_SYSTEM_ERROR;
    }
  }

  // If we're going to provide timestamps to the user with a callback then
  // depending on the implementation we may need to wait here until the
  // callback queue has drained, otherwise a future close of the device
  // could rip the data out from underneath the callback

  if ( deviceInfo->timestampCallback.callback ) {
    queueEmpty = 0;
    do {
      pthread_mutex_lock ( &deviceInfo->callbackQueueMutex );
      queueEmpty = deviceInfo->timestampsAvailable;
      pthread_mutex_unlock ( &deviceInfo->callbackQueueMutex );
      if ( !queueEmpty ) {
        usleep ( 10000 );
      }
    } while ( !queueEmpty );
  } else {
    deviceInfo->timestampsAvailable = deviceInfo->timestampCountdown = 0;
  }

  tcflush ( ptrDesc, TCIFLUSH );
  usleep ( 100000 );
  return OA_ERR_NONE;
}


static int
_doSync ( PRIVATE_INFO* deviceInfo )
{
  char		buffer[ 128 ];
  int		numRead;

  tcflush ( deviceInfo->fd, TCIFLUSH );
  usleep ( 100000 );

  if ( _ptrWrite ( deviceInfo->fd, "sync\r", 5 )) {
    fprintf ( stderr, "%s: failed to write sync command to %s\n",
        __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }

  // FIX ME -- perhaps there should be a select() with a timeout here?
  numRead = read ( deviceInfo->fd, buffer, sizeof ( buffer ) - 1 );
  if ( numRead > 0 ) {
    buffer[numRead] = 0;
    numRead--;
    while ( buffer[numRead] == '\r' || buffer[numRead] == '\n' ) {
      buffer[numRead--] = 0;
    }
  } else {
    fprintf ( stderr, "%s: failed to read sync response from %s\n",
      __FUNCTION__, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }

  if ( strncmp ( buffer, "Internal clock synchronized: ", 29 )) {
    fprintf ( stderr, "%s: unexpected sync response from %s\n%s\n",
        __FUNCTION__, deviceInfo->devicePath, buffer );
    return -OA_ERR_SYSTEM_ERROR;
  }

  tcflush ( deviceInfo->fd, TCIFLUSH );
  usleep ( 100000 );

  return OA_ERR_NONE;
}


static int
_processTimestampFetch ( PRIVATE_INFO* deviceInfo, OA_COMMAND* command )
{
  int			first, available;
  char*			p;
  char*			q;
  oaTimerStamp*		tsp = ( oaTimerStamp* ) command->resultData;

  pthread_mutex_lock ( &deviceInfo->callbackQueueMutex );
  first = deviceInfo->firstTimestamp;
  available = deviceInfo->timestampsAvailable;
  pthread_mutex_unlock ( &deviceInfo->callbackQueueMutex );

  if ( !available ) {
    *tsp->timestamp = 0;
    tsp->index = 0;
    fprintf ( stderr, "%s: no timestamp buffered yet\n", __FUNCTION__ );
    return OA_ERR_NONE;
  }

  // PTR < v1.1 returns a timestamp as YYMMDDThhmmss.sss
  // convert it to CCYY-MM-DDThh:mm:ss.sss
  // PTR >= v1.1 returns YYYY-MM-DDThh:mm:ss.sss

  p = tsp->timestamp;
  q = deviceInfo->timestampBuffer [ first ].timestamp;
  if ( deviceInfo->version < 0x0101 ) {
    *p++ = *q++; // C
    *p++ = *q++; // C
    *p++ = *q++; // Y
    *p++ = *q++; // Y
    *p++ = '-';
    *p++ = *q++; // M
    *p++ = *q++; // M
    *p++ = '-';
    *p++ = *q++; // D
    *p++ = *q++; // D
    *p++ = *q++; // T
    *p++ = *q++; // h
    *p++ = *q++; // h
    *p++ = ':';
    *p++ = *q++; // m
    *p++ = *q++; // m
    *p++ = ':';
  }
  ( void ) strcpy ( p, q );
  tsp->index = deviceInfo->timestampBuffer [ first ].index;

  pthread_mutex_lock ( &deviceInfo->callbackQueueMutex );
  deviceInfo->firstTimestamp = ( deviceInfo->firstTimestamp + 1 ) %
      OA_TIMESTAMP_BUFFERS;
  deviceInfo->timestampsAvailable--;
  pthread_mutex_unlock ( &deviceInfo->callbackQueueMutex );

  return OA_ERR_NONE;
}


static int
_readTimestamp ( uint32_t version, int fd, char* buffer )
{
  int		readSoFar = 0, i, l, maxChars;
  int		__attribute__((unused)) dummy;
  char*		p = buffer;
  char*		complete = "Acquisition sequence complete";
  char		crlf[2];

  // FIX ME -- this should be done far more neatly
  // read what's available each time the select finishes in the caller,
  // and once we have enough data to form a timestamp, add that to the
  // queue

  memset ( buffer, 0, ( version < 0x0101 ) ?
      PTR_TIMESTAMP_BUFFER_LEN_V1_0 : PTR_TIMESTAMP_BUFFER_LEN_V1_1 );
  *p = 0;
  if ( read ( fd, p, 1 ) != 1 ) {
    return readSoFar;
  }
  readSoFar++;

  // We're expecting something that matches either of:
  // "[ST]:\d{6}:\{8}T\d{6}.d{3}" (or the 1.1+ equivalent)
  // "Acquisition sequence complete"

  if ( *p != 'S' && *p != 'T' && *p != 'A' ) {
    return readSoFar;
  }

  if ( *p == 'A' ) {
    p++;
    l = strlen ( complete );
    for ( i = 1; i < l; i++ ) {
      if ( read ( fd, p, 1 ) != 1 ) {
        return readSoFar;
      }
      readSoFar++;
      if ( *p != complete[i] ) {
        return readSoFar;
      }
      p++;
    }
  } else {
    p++;
    // read ":"
    if ( read ( fd, p, 1 ) != 1 ) {
      return readSoFar;
    }
    readSoFar++;
    if ( *p != ':' ) {
      return readSoFar;
    }
    p++;
    // read "\d{6}" (index number)
    for ( i = 0; i < 6; i++ ) {
      if ( read ( fd, p, 1 ) != 1 ) {
        return readSoFar;
      }
      readSoFar++;
      if ( !isdigit ( *p )) {
        return readSoFar;
      }
      p++;
    }
    // read ":"
    if ( read ( fd, p, 1 ) != 1 ) {
      return readSoFar;
    }
    readSoFar++;
    if ( *p != ':' ) {
      return readSoFar;
    }
    p++;
    // read "\d{8}" (YYYYMMDD) or "\d{4}-\d\d-\d\d" for PTR >= 1.1
    maxChars = ( version >= 0x0101 ) ? 10 : 8;
    for ( i = 0; i < maxChars; i++ ) {
      if ( read ( fd, p, 1 ) != 1 ) {
        return readSoFar;
      }
      readSoFar++;
      if ( !isdigit ( *p )) {
        if (!( version >= 0x0101 && ( i == 4 || i == 7 ) &&
            *p == '-' )) {
          return readSoFar;
        }
      }
      p++;
    }
    // read "T"
    if ( read ( fd, p, 1 ) != 1 ) {
      return readSoFar;
    }
    readSoFar++;
    if ( *p != 'T' ) {
      return readSoFar;
    }
    p++;
    // read "\d{6}" (HHmmss) or "\d\d:\d\d:\d\d"
    maxChars = ( version >= 0x0101 ) ? 8 : 6;
    for ( i = 0; i < maxChars; i++ ) {
      if ( read ( fd, p, 1 ) != 1 ) {
        return readSoFar;
      }
      readSoFar++;
      if ( !isdigit ( *p )) {
        if (!( version >= 0x0101 && ( i == 2 || i == 5 ) &&
            *p == ':' )) {
          return readSoFar;
        }
      }
      p++;
    }
    // read "."
    if ( read ( fd, p, 1 ) != 1 ) {
      return readSoFar;
    }
    readSoFar++;
    if ( *p != '.' ) {
      return readSoFar;
    }
    p++;
    // read "\d{3}" (milliseconds)
    for ( i = 0; i < 3; i++ ) {
      if ( read ( fd, p, 1 ) != 1 ) {
        return readSoFar;
      }
      readSoFar++;
      if ( !isdigit ( *p )) {
        return readSoFar;
      }
      p++;
    }
  }

  // At this point the next two characters will be 0d 0a, so we read those
  // and throw them

  dummy = read ( fd, crlf, 2 );

  return readSoFar;
}


static int
_processGPSFetch ( PRIVATE_INFO* deviceInfo, OA_COMMAND* command )
{
  char		commandStr[128], buffer[128];
  int		commandLen, readBytes;
  double	latDeg, latMin, longDeg, longMin, alt;
  double*	r = command->resultData;

  if ( deviceInfo->isRunning ) {
    return -OA_ERR_TIMER_RUNNING;
  }

  tcflush ( deviceInfo->fd, TCIFLUSH );
  // usleep ( 100000 );

  ( void ) strcpy ( commandStr, "geo\r" );
  commandLen = strlen ( commandStr );
  if ( _ptrWrite ( deviceInfo->fd, commandStr, commandLen )) {
    fprintf ( stderr, "%s: failed to write command:\n%s\n  to %s\n",
        __FUNCTION__, commandStr, deviceInfo->devicePath );
    return -OA_ERR_SYSTEM_ERROR;
  }
  usleep ( 200000 );

  // Only < 1 here because the preceding tcflush may actually have eaten some
  // of the data
  if (( readBytes = _ptrRead ( deviceInfo->fd, buffer, 127 )) < 1 ) {
    fprintf ( stderr, "%s: failed to read back command:\n%s\n"
        "  from %s, commandLen = %d, read len = %d\n",
        __FUNCTION__, commandStr, deviceInfo->devicePath, commandLen,
        readBytes );
    if ( readBytes > 0 ) {
      buffer[ readBytes ] = 0;
      fprintf ( stderr, "  string read = '%s'\n", buffer );
    }
    return -OA_ERR_SYSTEM_ERROR;
  }

  // We expect to get a string back of the form:
  // G:+ddmm.mmmm,-dddmm.mmmm,aaa.a
  // where aaa does not have a fixed length.

  memset ( buffer, 0, 128 );
  if (( readBytes = _ptrRead ( deviceInfo->fd, buffer, 40 )) < 28 ) {
    fprintf ( stderr, "%s, failed to read response to 'geo' command\n",
        __FUNCTION__ );
    if ( readBytes > 0 ) {
      buffer[ readBytes ] = 0;
      fprintf ( stderr, "  string read (%d bytes) = '%s'\n", readBytes,
          buffer );
    }
    return -OA_ERR_SYSTEM_ERROR;
  }

  if ( *buffer != 'G' || *( buffer + 1 ) != ':' ) {
    fprintf ( stderr, "%s, geo string '%s' has invalid format\n",
        __FUNCTION__, buffer );
    return -OA_ERR_SYSTEM_ERROR;
  }

  if ( sscanf ( buffer + 2, "%3lf%lf,%4lf%lf,%lf", &latDeg, &latMin, &longDeg,
      &longMin, &alt ) < 5 ) {
    fprintf ( stderr, "%s, geo string '%s' fails to match expected format",
        __FUNCTION__, buffer + 2 );
  }

  if ( latDeg < 0 ) { latMin = -latMin; }
  if ( longDeg < 0 ) { longMin = -longMin; }

  deviceInfo->latitude = latDeg + latMin / 60;
  deviceInfo->longitude = longDeg + longMin / 60;
  deviceInfo->altitude = alt;

  r[0] = deviceInfo->latitude;
  r[1] = deviceInfo->longitude;
  r[2] = deviceInfo->altitude;

  tcflush ( deviceInfo->fd, TCIFLUSH );
  usleep ( 100000 );

  return -OA_ERR_NONE;
}
