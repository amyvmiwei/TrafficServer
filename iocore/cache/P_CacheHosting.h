/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#ifndef __P_CACHE_HOSTING_H__
#define __P_CACHE_HOSTING_H__
#include "P_Cache.h"

#define CACHE_MEM_FREE_TIMEOUT     HRTIME_SECONDS(1)

struct Vol;
struct CacheVol;

struct CacheHostResult;
struct Cache;

struct CacheHostRecord
{
  int Init(int typ);
  int Init(matcher_line *line_info, int typ);
  void UpdateMatch(CacheHostResult *r, char *rd);
  void Print();
  ~CacheHostRecord()
  {
    ats_free(vols);
    ats_free(vol_hash_table);
    ats_free(cp);
  }

  int type;
  Vol **vols;
  volatile int good_num_vols;
  volatile int num_vols;
  int num_initialized;
  unsigned short *vol_hash_table;
  CacheVol **cp;
  int num_cachevols;

  CacheHostRecord():
    type(0), vols(NULL), good_num_vols(0), num_vols(0),
    num_initialized(0), vol_hash_table(0), cp(NULL), num_cachevols(0)
  { }

};

void build_vol_hash_table(CacheHostRecord *cp);

struct CacheHostResult
{
  CacheHostRecord *record;

  CacheHostResult()
    : record(NULL)
  { }
};


class CacheHostMatcher
{
public:
  CacheHostMatcher(const char *name, const char *filename, int typ);
  ~CacheHostMatcher();

  void Match(char *rdata, int rlen, CacheHostResult *result);
  void AllocateSpace(int num_entries);
  void NewEntry(matcher_line *line_info);
  void Print();

  int getNumElements() const { return num_el; }
  CacheHostRecord *getDataArray() const { return data_array; }
  HostLookup *getHLookup() const { return host_lookup; }

private:
  static void PrintFunc(void *opaque_data);
  HostLookup *host_lookup;      // Data structure to do the lookups
  CacheHostRecord *data_array;  // array of all data items
  int array_len;                // the length of the arrays
  int num_el;                   // the numbe of itmems in the tree
  const char *matcher_name;     // Used for Debug/Warning/Error messages
  const char *file_name;        // Used for Debug/Warning/Error messages
  int type;
};

class CacheHostTable
{
public:
  // Parameter name must not be deallocated before this
  //  object is
  CacheHostTable(Cache *c, int typ);
   ~CacheHostTable();
  int BuildTable();
  int BuildTableFromString(char *str);
  void Match(char *rdata, int rlen, CacheHostResult *result);
  void Print();

  int getEntryCount() const { return m_numEntries; }
  CacheHostMatcher *getHostMatcher() const { return hostMatch; }

  static int config_callback(const char *, RecDataT, RecData, void *);

  void register_config_callback(CacheHostTable ** p)
  {
    IOCORE_RegisterConfigUpdateFunc("proxy.config.cache.hosting_filename", CacheHostTable::config_callback, (void *) p);
  }

  int type;
  Cache *cache;
  int m_numEntries;
  CacheHostRecord gen_host_rec;

private:
  CacheHostMatcher *hostMatch;
  const matcher_tags *config_tags;
  char config_file_path[PATH_NAME_MAX];
  const char *matcher_name;     // Used for Debug/Warning/Error messages
};

struct CacheHostTableConfig;
typedef int (CacheHostTableConfig::*CacheHostTabHandler) (int, void *);
struct CacheHostTableConfig: public Continuation
{
  CacheHostTable **ppt;
  CacheHostTableConfig(CacheHostTable ** appt)
    : Continuation(NULL), ppt(appt)
  {
    SET_HANDLER((CacheHostTabHandler) & CacheHostTableConfig::mainEvent);
  }

  int mainEvent(int event, Event *e)
  {
    (void) e;
    (void) event;
    CacheHostTable *t = NEW(new CacheHostTable((*ppt)->cache, (*ppt)->type));
    CacheHostTable *old = (CacheHostTable *) ink_atomic_swap_ptr(&t, ppt);
    new_Deleter(old, CACHE_MEM_FREE_TIMEOUT);
    return EVENT_DONE;
  }
};


/* list of volumes in the volume.config file */
struct ConfigVol
{
  int number;
  int scheme;
  int size;
  bool in_percent;
  int percent;
  CacheVol *cachep;
  LINK(ConfigVol, link);
};

struct ConfigVolumes
{
  int num_volumes;
  int num_http_volumes;
  int num_stream_volumes;
  Queue<ConfigVol> cp_queue;
  void read_config_file();
  void BuildListFromString(char *config_file_path, char *file_buf);

  void clear_all(void)
  {
    // remove all the volumes from the queue
    for (int i = 0; i < num_volumes; i++) {
      cp_queue.pop();
    }
    // reset count variables
    num_volumes = 0;
    num_http_volumes = 0;
    num_stream_volumes = 0;
  }
};

#endif
