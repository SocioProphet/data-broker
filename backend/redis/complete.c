/*
 * Copyright © 2018,2019 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "common/completion.h"
#include "complete.h"
#include "namespace.h"

#include <stddef.h>
#include <errno.h>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif


/*
 * convert a redis request in result stage into a completion
 * returns NULL on error and pointer to completion on success
 */
dbBE_Completion_t* dbBE_Redis_complete_command( dbBE_Redis_request_t *request,
                                                dbBE_Redis_result_t *result,
                                                int64_t rc )
{
  if(( request == NULL ) || ( result == NULL ))
  {
    errno = EINVAL;
    return NULL;
  }

  DBR_Errorcode_t status = DBR_SUCCESS; // optimistic approach ;-)
  int64_t localrc = rc > 0 ? rc : 0;
  dbBE_Completion_t *completion = request->_completion;

  // if this is a request that had the completion created in an earlier stage
  // need to grab the latest status from there.
  if( completion != NULL )
    return completion;

  dbBE_Redis_command_stage_spec_t *spec = request->_step;
  if( spec == NULL )
  {
    errno = EPROTO;
    return NULL;
  }

  // opcode-independent mappings of rc/errno to dbr error:
  switch( rc )
  {
    case 0: break;
    case -ENOENT: status = DBR_ERR_UNAVAIL; break;
    case -EEXIST: status = DBR_ERR_EXISTS; break;
    case -EPROTO: status = DBR_ERR_BE_GENERAL; break;
    case -ENOMEM: status = DBR_ERR_NOMEMORY; break;
    case -EBADMSG:
    case -EINVAL: status = DBR_ERR_INVALID; break;
    case -EAGAIN: status = DBR_ERR_INPROGRESS; break;
    default:
      if( rc < 0 )
      {
        status = DBR_ERR_BE_GENERAL;
        localrc = -rc;
      }
      break;
  }

  if( result->_type != dbBE_REDIS_TYPE_INT )
  {
    LOG( DBG_ERR, stderr, "Invalid result type while creating completion: %d; request opcode %d, rc=%ld\n", result->_type, request->_user->_opcode, rc );
    exit(1);
  }

  switch( request->_user->_opcode )
  {
    case DBBE_OPCODE_PUT:
      switch( rc )
      {
        case 0:
          localrc = 1;
          break;
        default:
          break;
      }
      break;
    case DBBE_OPCODE_GET:
    case DBBE_OPCODE_READ:
      switch( rc )
      {
        case 0:
          if( result->_data._integer < 0 )
            status = DBR_ERR_INPROGRESS;
          else
          {
            status = DBR_SUCCESS;
            localrc = result->_data._integer;
          }
          break;
        case -ENOSPC:
          status = DBR_ERR_UBUFFER;
          localrc = result->_data._integer;
          break;
        default:
          break;
      }
      break;
    case DBBE_OPCODE_MOVE:
      switch( rc )
      {
        case -ENODATA: // if a transfer couldn't receive/send the full dump of an entry
          status = DBR_ERR_BE_GENERAL;
          localrc = 0;
          break;
        case -ESTALE: // happens when delete phase hits an unexpected error
          status = DBR_ERR_NOFILE;
          localrc = 0;
          break;
        default:
          break;
      }
      break;
    case DBBE_OPCODE_DIRECTORY:
      if( spec->_result != 0 )
      {
        switch( rc )
        {
          case 0:
            if( ! result->_data._integer ) // ToDo: check for < 0 instead of !0
              status = DBR_ERR_INPROGRESS;
            else
            {
              status = DBR_SUCCESS;
              localrc = result->_data._integer;
            }
            break;
          default:
            break;
        }
      }
      else if( status == DBR_ERR_INPROGRESS )
      {
        LOG( DBG_ERR, stderr, "completion directory REACHED SUSPECTED DEAD CODE\n" );
        if( result->_data._integer ) // ToDo: check for < 0 instead of 0 <
        {
          status = DBR_SUCCESS;
          localrc = result->_data._integer;
        }
      }
      break;
    case DBBE_OPCODE_NSCREATE:
    case DBBE_OPCODE_NSATTACH:
      if( rc == 0 )
      {
        localrc = result->_data._integer;
      }
      else
      {
        localrc = 0;
        status = (request->_user->_opcode == DBBE_OPCODE_NSATTACH) ? DBR_ERR_NSINVAL : DBR_ERR_EXISTS;
      }
      break;
    case DBBE_OPCODE_NSDETACH:
    case DBBE_OPCODE_NSDELETE:
      if( spec->_result != 0 ) // important check because DELETE creates completion in non-final stage
      {
        switch( rc )
        {
          case 0:
            break;
          case EBUSY:
            status = DBR_ERR_NSBUSY;
            localrc = result->_data._integer;
            break;
          default:
            break;
        }
      }
      break;
    case DBBE_OPCODE_NSQUERY:
      if( request->_step->_result != 0 )
      {
        if(( rc == 0 ) && ( result->_type == dbBE_REDIS_TYPE_INT ))
          localrc = result->_data._integer;
      }
      break;
    case DBBE_OPCODE_ITERATOR:
      if(( rc == 0 ) && ( result->_type == dbBE_REDIS_TYPE_INT ))
        localrc = result->_data._integer;  // int64 value contains the iterator pointer
      break;
    case DBBE_OPCODE_REMOVE:
    default:
      break;
  }

  return dbBE_Completion_create( request->_user, status, localrc );
}

dbBE_Completion_t* dbBE_Redis_complete_error( dbBE_Redis_request_t *request,
                                              DBR_Errorcode_t error,
                                              int64_t retval )
{
  return dbBE_Completion_create( request->_user, error, retval );
}

dbBE_Completion_t* dbBE_Redis_complete_cancel( dbBE_Redis_request_t *request )
{
  return dbBE_Completion_create( request->_user, DBR_ERR_CANCELLED, 0 );
}
