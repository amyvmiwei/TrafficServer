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

#include "UrlRewrite.h"
#include "Main.h"
#include "P_EventSystem.h"
#include "StatSystem.h"
#include "P_Cache.h"
#include "ProxyConfig.h"
#include "ReverseProxy.h"
#include "MatcherUtils.h"
#include "Tokenizer.h"
#include "api/ts/remap.h"
#include "api/ts/ts.h"
#include "MappingTypes.h"
#include "MappingEntry.h"
#include "RemapParser.h"
#include "DirectiveParams.h"
#include "ACLDefineManager.h"
#include "ACLCheckList.h"
#include "MappingManager.h"

#include "ink_string.h"

extern TSReturnCode
TSHttpConfigParamSet(OverridableHttpConfigParams *overridableHttpConfig,
        const char* name, int nameLen, const char *value, int valueLen);

/**
  Determines where we are in a situation where a virtual path is
  being mapped to a server home page. If it is, we set a special flag
  instructing us to be on the lookout for the need to send a redirect
  to if the request URL is a object, opposed to a directory. We need
  the redirect for an object so that the browser is aware that it is
  real accessing a directory (albeit a virtual one).

*/
void
SetHomePageRedirectFlag(url_mapping *new_mapping, URL &new_to_url)
{
  int fromLen, toLen;
  const char *from_path = new_mapping->fromURL.path_get(&fromLen);
  const char *to_path = new_to_url.path_get(&toLen);

  new_mapping->homePageRedirect = (from_path && !to_path) ? true : false;
}

static int
get_real_path(const char *lpath, char *buf, int bufsize)
{
  int           rsize;
  const char    *ptr;
  char          tmpbuf[PATH_NAME_MAX];

  Debug("remap_plugin", "get_real_path for %s", lpath);

  memset(tmpbuf, 0, PATH_NAME_MAX);

  rsize = readlink(lpath, tmpbuf, PATH_NAME_MAX);
  if (rsize < 0) {
    Debug("remap_plugin", "Can't readlink \"%s\"", lpath);
    return rsize;
  }

  if (tmpbuf[0] == '/' || lpath[0] != '/') {
    rsize = snprintf(buf, bufsize, "%s", tmpbuf);
    return rsize;
  }

  ptr = strrchr(lpath, '/');
  rsize = snprintf(buf, bufsize, "%.*s%s", (int)(ptr-lpath+1), lpath, tmpbuf);

  return rsize;
}

//
// CTOR / DTOR for the UrlRewrite class.
//
UrlRewrite::UrlRewrite(const char *file_var_in)
 : nohost_rules(0), reverse_proxy(0), backdoor_enabled(0),
   mgmt_autoconf_port(0), default_to_pac(0), default_to_pac_port(0),
   file_var(NULL), ts_name(NULL), http_default_redirect_url(NULL),
   num_rules_forward(0), num_rules_reverse(0),
   num_rules_redirect_permanent(0), num_rules_redirect_temporary(0),
   num_rules_forward_with_recv_port(0), remap_pi_list(NULL), _valid(false),
   _oldDefineCheckers(NULL)
{
  char *config_file = NULL;

  ink_assert(file_var_in != NULL);
  this->file_var = ats_strdup(file_var_in);
  config_file_path[0] = '\0';

  REVERSE_ReadConfigStringAlloc(config_file, file_var_in);

  if (config_file == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to find proxy.config.url_remap.filename");
    Warning("%s Unable to locate remap.config.  No remappings in effect", modulePrefix);
    return;
  }

  this->ts_name = NULL;
  REVERSE_ReadConfigStringAlloc(this->ts_name, "proxy.config.proxy_name");
  if (this->ts_name == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to read proxy.config.proxy_name");
    Warning("%s Unable to determine proxy name.  Incorrect redirects could be generated", modulePrefix);
    this->ts_name = ats_strdup("");
  }

  this->http_default_redirect_url = NULL;
  REVERSE_ReadConfigStringAlloc(this->http_default_redirect_url, "proxy.config.http.referer_default_redirect");
  if (this->http_default_redirect_url == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to read proxy.config.http.referer_default_redirect");
    Warning("%s Unable to determine default redirect url for \"referer\" filter.", modulePrefix);
    this->http_default_redirect_url = ats_strdup("http://www.apache.org");
  }

  REVERSE_ReadConfigInteger(reverse_proxy, "proxy.config.reverse_proxy.enabled");
  REVERSE_ReadConfigInteger(mgmt_autoconf_port, "proxy.config.admin.autoconf_port");
  REVERSE_ReadConfigInteger(default_to_pac, "proxy.config.url_remap.default_to_server_pac");
  REVERSE_ReadConfigInteger(default_to_pac_port, "proxy.config.url_remap.default_to_server_pac_port");
  REVERSE_ReadConfigInteger(url_remap_mode, "proxy.config.url_remap.url_remap_mode");
  REVERSE_ReadConfigInteger(backdoor_enabled, "proxy.config.url_remap.handle_backdoor_urls");

  ink_strlcpy(config_file_path, system_config_directory, sizeof(config_file_path));
  ink_strlcat(config_file_path, "/", sizeof(config_file_path));
  ink_strlcat(config_file_path, config_file, sizeof(config_file_path));
  ats_free(config_file);

  if (0 == this->BuildTable()) {
    _valid = true;
    pcre_malloc = &ats_malloc;
    pcre_free = &ats_free;

    _oldDefineCheckers = ACLDefineManager::getInstance()->commit();

    if (is_debug_tag_set("url_rewrite"))
      Print();
  } else {
    ACLDefineManager::getInstance()->rollback();
    Warning("something failed during BuildTable() -- check your remap plugins!");
  }
}

UrlRewrite::~UrlRewrite()
{
  ats_free(this->file_var);
  ats_free(this->ts_name);
  ats_free(this->http_default_redirect_url);

  DestroyStore(forward_mappings);
  DestroyStore(reverse_mappings);
  DestroyStore(permanent_redirects);
  DestroyStore(temporary_redirects);
  DestroyStore(forward_mappings_with_recv_port);

  ACLDefineManager::freeDefineCheckers(_oldDefineCheckers);

  if (this->remap_pi_list)
      this->remap_pi_list->delete_my_list();

  _valid = false;
}

/** Sets the reverse proxy flag. */
void
UrlRewrite::SetReverseFlag(int flag)
{
  reverse_proxy = flag;
  if (is_debug_tag_set("url_rewrite"))
    Print();
}

/**
  Allocaites via new, and setups the default mapping to the PAC generator
  port which is used to serve the PAC (proxy autoconfig) file.

*/
url_mapping *
UrlRewrite::SetupPacMapping()
{
  const char *from_url = "http:///";
  const char *local_url = "http://127.0.0.1/";

  url_mapping *mapping;
  int pac_generator_port;

  mapping = new url_mapping;

  mapping->fromURL.create(NULL);
  mapping->fromURL.parse(from_url, strlen(from_url));

  mapping->toUrl.create(NULL);
  mapping->toUrl.parse(local_url, strlen(local_url));

  pac_generator_port = (default_to_pac_port < 0) ? mgmt_autoconf_port : default_to_pac_port;

  mapping->toUrl.port_set(pac_generator_port);

  return mapping;
}

/**
  Allocaites via new, and adds a mapping like this map /ink/rh
  http://{backdoor}/ink/rh

  These {backdoor} things are then rewritten in a request-hdr hook.  (In the
  future it might make sense to move the rewriting into HttpSM directly.)

*/
url_mapping *
UrlRewrite::SetupBackdoorMapping()
{
  const char from_url[] = "/ink/rh";
  const char to_url[] = "http://{backdoor}/ink/rh";

  url_mapping *mapping = new url_mapping;

  mapping->fromURL.create(NULL);
  mapping->fromURL.parse(from_url, sizeof(from_url) - 1);
  mapping->fromURL.scheme_set(URL_SCHEME_HTTP, URL_LEN_HTTP);

  mapping->toUrl.create(NULL);
  mapping->toUrl.parse(to_url, sizeof(to_url) - 1);

  return mapping;
}

/** Deallocated a hash table and all the url_mappings in it. */
void
UrlRewrite::_destroyTable(InkHashTable *h_table)
{
  InkHashTableEntry *ht_entry;
  InkHashTableIteratorState ht_iter;
  UrlMappingPathIndex *item;

  if (h_table != NULL) {        // Iterate over the hash tabel freeing up the all the url_mappings
    //   contained with in
    for (ht_entry = ink_hash_table_iterator_first(h_table, &ht_iter); ht_entry != NULL;) {
      item = (UrlMappingPathIndex *)ink_hash_table_entry_value(h_table, ht_entry);
      delete item;
      ht_entry = ink_hash_table_iterator_next(h_table, &ht_iter);
    }
    ink_hash_table_destroy(h_table);
  }
}

/** Debugging Method. */
void
UrlRewrite::Print()
{
  printf("URL Rewrite table with %d entries\n", num_rules_forward + num_rules_reverse +
         num_rules_redirect_temporary + num_rules_redirect_permanent + num_rules_forward_with_recv_port);
  printf("  Reverse Proxy is %s\n", (reverse_proxy == 0) ? "Off" : "On");

  printf("  Forward Mapping Table with %d entries\n", num_rules_forward);
  PrintStore(forward_mappings);

  printf("  Reverse Mapping Table with %d entries\n", num_rules_reverse);
  PrintStore(reverse_mappings);

  printf("  Permanent Redirect Mapping Table with %d entries\n", num_rules_redirect_permanent);
  PrintStore(permanent_redirects);

  printf("  Temporary Redirect Mapping Table with %d entries\n", num_rules_redirect_temporary);
  PrintStore(temporary_redirects);

  printf("  Forward Mapping With Recv Port Table with %d entries\n", num_rules_forward_with_recv_port);
  PrintStore(forward_mappings_with_recv_port);

  if (http_default_redirect_url != NULL) {
    printf("  Referer filter default redirect URL: \"%s\"\n", http_default_redirect_url);
  }
}

/** Debugging method. */
void
UrlRewrite::PrintStore(MappingsStore &store)
{
  if (store.hash_lookup != NULL) {
    InkHashTableEntry *ht_entry;
    InkHashTableIteratorState ht_iter;
    UrlMappingPathIndex *value;

    for (ht_entry = ink_hash_table_iterator_first(store.hash_lookup, &ht_iter); ht_entry != NULL;) {
      value = (UrlMappingPathIndex *) ink_hash_table_entry_value(store.hash_lookup, ht_entry);
      value->Print();
      ht_entry = ink_hash_table_iterator_next(store.hash_lookup, &ht_iter);
    }
  }

  if (store.suffix_trie != NULL) {
    printf("    suffix_trie_min_rank: %d, regex convert to suffix match:\n",
        store.suffix_trie_min_rank);

    int count;
    SuffixMappings **suffixMappings = store.suffix_trie->getNodes(&count);
    for (int i=0; i<count; i++) {
      suffixMappings[i]->mapping_paths.Print();
    }
  }

  if (!store.regex_list.empty()) {
    printf("    regex_list_min_rank: %d, Regex mappings:\n",
        store.regex_list_min_rank);
    forl_LL(RegexMapping, list_iter, store.regex_list) {
      list_iter->url_map->Print();
    }
  }
}

/**
  If a remapping is found, returns a pointer to it otherwise NULL is
  returned.

*/
url_mapping *
UrlRewrite::_tableLookup(InkHashTable *h_table, URL *request_url,
    char *request_host_key)
{
  UrlMappingPathIndex *ht_entry;
  url_mapping *um = NULL;
  int ht_result;

  ht_result = ink_hash_table_lookup(h_table, request_host_key, (void **) &ht_entry);

  if (likely(ht_result && ht_entry)) {
    // for empty host don't do a normal search, get a mapping arbitrarily
    um = ht_entry->Search(request_url);
  }
  return um;
}


// This is only used for redirects and reverse rules, and the homepageredirect flag
// can never be set. The end result is that request_url is modified per remap container.
void
UrlRewrite::_doRemap(UrlMappingContainer &mapping_container, URL *request_url)
{
  const char *requestPath;
  int requestPathLen;

  url_mapping *mapPtr = mapping_container.getMapping();
  URL *map_from = &mapPtr->fromURL;
  int fromPathLen;

  URL *map_to = mapping_container.getToURL();
  const char *toHost;
  const char *toPath;
  const char *toScheme;
  int toPathLen;
  int toHostLen;
  int toSchemeLen;

  requestPath = request_url->path_get(&requestPathLen);
  map_from->path_get(&fromPathLen);

  toHost = map_to->host_get(&toHostLen);
  toPath = map_to->path_get(&toPathLen);
  toScheme = map_to->scheme_get(&toSchemeLen);

  Debug("url_rewrite", "_doRemap(): Remapping rule id: %d matched", mapPtr->getRank());

  request_url->host_set(toHost, toHostLen);
  request_url->port_set(map_to->port_get_raw());
  request_url->scheme_set(toScheme, toSchemeLen);

  // Should be +3, little extra padding won't hurt. Use the stack allocation
  // for better performance (bummer that arrays of variable length is not supported
  // on Solaris CC.
  char *newPath = static_cast<char*>(alloca(sizeof(char*)*((requestPathLen - fromPathLen) + toPathLen + 8)));
  int newPathLen = 0;

  *newPath = 0;
  if (toPath) {
    memcpy(newPath, toPath, toPathLen);
    newPathLen += toPathLen;
  }

  // We might need to insert a trailing slash in the new portion of the path
  // if more will be added and none is present and one will be needed.
  if (!fromPathLen && requestPathLen && toPathLen && *(newPath + newPathLen - 1) != '/') {
    *(newPath + newPathLen) = '/';
    newPathLen++;
  }

  if (requestPath) {
    //avoid adding another trailing slash if the requestPath already had one and so does the toPath
    if (requestPathLen < fromPathLen) {
      if (toPathLen && requestPath[requestPathLen - 1] == '/' && toPath[toPathLen - 1] == '/') {
        fromPathLen++;
      }
    } else {
      if (toPathLen && requestPath[fromPathLen] == '/' && toPath[toPathLen - 1] == '/') {
        fromPathLen++;
      }
    }

    // copy the end of the path past what has been mapped
    if ((requestPathLen - fromPathLen) > 0) {
      memcpy(newPath + newPathLen, requestPath + fromPathLen, requestPathLen - fromPathLen);
      newPathLen += (requestPathLen - fromPathLen);
    }
  }

  // Skip any leading / in the path when setting the new URL path
  if (*newPath == '/') {
    request_url->path_set(newPath + 1, newPathLen - 1);
  } else {
    request_url->path_set(newPath, newPathLen);
  }
}


/** Used to do the backwards lookups. */
bool
UrlRewrite::ReverseMap(HTTPHdr *response_header)
{
  const char *location_hdr;
  URL location_url;
  int loc_length;
  bool remap_found = false;
  const char *host;
  int host_len;
  char *new_loc_hdr;
  int new_loc_length;

  if (unlikely(num_rules_reverse == 0)) {
    ink_assert(reverse_mappings.empty());
    return false;
  }

  location_hdr = response_header->value_get(MIME_FIELD_LOCATION, MIME_LEN_LOCATION, &loc_length);

  if (location_hdr == NULL) {
    Debug("url_rewrite", "Reverse Remap called with empty location header");
    return false;
  }

  location_url.create(NULL);
  location_url.parse(location_hdr, loc_length);

  host = location_url.host_get(&host_len);

  UrlMappingContainer reverse_mapping(response_header->m_heap);

  if (reverseMappingLookup(&location_url, location_url.port_get(), host, host_len, reverse_mapping)) {
    remap_found = true;
    _doRemap(reverse_mapping, &location_url);
    new_loc_hdr = location_url.string_get_ref(&new_loc_length);
    response_header->value_set(MIME_FIELD_LOCATION, MIME_LEN_LOCATION, new_loc_hdr, new_loc_length);
  }

  location_url.destroy();
  return remap_found;
}


/** Perform fast ACL filtering. */
int
UrlRewrite::PerformACLFiltering(HttpTransact::State *s, url_mapping *map)
{
  if (unlikely(!s || s->acl_filtering_performed || !s->client_connection_enabled)) {
    return ACL_ACTION_NONE_INT;
  }

  s->acl_filtering_performed = true;    // small protection against reverse mapping
  if (!map->needCheckMethodIp()) {
    return ACL_ACTION_ALLOW_INT;
  }

  ACLContext aclContext;
  aclContext.method.str = s->hdr_info.client_request.method_get(
      &aclContext.method.length);
  aclContext.clientIp = ntohl(s->client_info.addr.sin.sin_addr.s_addr);

  int action = map->checkMethodIp(aclContext);
  if (action == ACL_ACTION_DENY_INT) {
    Debug("url_rewrite", "matched ACL filter rule, denying request");
    s->client_connection_enabled = false;
  }

  return action;
}

/**
   Determines if a redirect is to occur and if so, figures out what the
   redirect is. This was plaguiarized from UrlRewrite::Remap. redirect_url
   ought to point to the new, mapped URL when the function exits.
*/
mapping_type
UrlRewrite::Remap_redirect(HTTPHdr *request_header, URL *redirect_url)
{
  URL *request_url;
  mapping_type mappingType;
  const char *host = NULL;
  int host_len = 0, request_port = 0;
  bool prt, trt;                        // existence of permanent and temporary redirect tables, respectively

  prt = (num_rules_redirect_permanent != 0);
  trt = (num_rules_redirect_temporary != 0);

  if (prt + trt == 0)
    return NONE;

  // Since are called before request validity checking
  //  occurs, make sure that we have both a valid request
  //  header and a valid URL
  //
  if (request_header == NULL) {
    Debug("url_rewrite", "request_header was invalid.  UrlRewrite::Remap_redirect bailing out.");
    return NONE;
  }
  request_url = request_header->url_get();
  if (!request_url->valid()) {
    Debug("url_rewrite", "request_url was invalid.  UrlRewrite::Remap_redirect bailing out.");
    return NONE;
  }

  host = request_url->host_get(&host_len);
  request_port = request_url->port_get();

  if (host_len == 0 && reverse_proxy != 0) {    // Server request.  Use the host header to figure out where
                                                // it goes.  Host header parsing is same as in ::Remap
    int host_hdr_len;
    const char *host_hdr = request_header->value_get(MIME_FIELD_HOST, MIME_LEN_HOST, &host_hdr_len);

    if (!host_hdr) {
      host_hdr = "";
      host_hdr_len = 0;
    }

    const char *tmp = (const char *) memchr(host_hdr, ':', host_hdr_len);

    if (tmp == NULL) {
      host_len = host_hdr_len;
    } else {
      host_len = tmp - host_hdr;
      request_port = ink_atoi(tmp + 1, host_hdr_len - host_len);

      // If atoi fails, try the default for the
      //   protocol
      if (request_port == 0) {
        request_port = request_url->port_get();
      }
    }

    host = host_hdr;
  }
  // Temporary Redirects have precedence over Permanent Redirects
  // the rationale behind this is that network administrators might
  // want quick redirects and not want to worry about all the existing
  // permanent rules
  mappingType = NONE;

  UrlMappingContainer redirect_mapping(request_header->m_heap);

  if (trt) {
    if (temporaryRedirectLookup(request_url, request_port, host, host_len, redirect_mapping)) {
      mappingType = TEMPORARY_REDIRECT;
    }
  }
  if ((mappingType == NONE) && prt) {
    if (permanentRedirectLookup(request_url, request_port, host, host_len, redirect_mapping)) {
      mappingType = PERMANENT_REDIRECT;
    }
  }

  if (mappingType != NONE) {
    ink_assert((mappingType == PERMANENT_REDIRECT) || (mappingType == TEMPORARY_REDIRECT));

    // Make a copy of the request url so that we can munge it
    //   for the redirect
    redirect_url->create(NULL);
    redirect_url->copy(request_url);

    // Perform the actual URL rewrite
    _doRemap(redirect_mapping, redirect_url);

    return mappingType;
  }
  ink_assert(mappingType == NONE);

  return NONE;
}

/**
  Returns the length of the URL.

  Will replace the terminator with a '/' if this is a full URL and
  there are no '/' in it after the the host.  This ensures that class
  URL parses the URL correctly.

*/
int
UrlRewrite::UrlWhack(char *toWhack, int *origLength)
{
  int length = strlen(toWhack);
  char *tmp;
  *origLength = length;

  // Check to see if this a full URL
  tmp = strstr(toWhack, "://");
  if (tmp != NULL) {
    if (strchr(tmp + 3, '/') == NULL) {
      toWhack[length] = '/';
      length++;
    }
  }
  return length;
}

UrlRewrite::SuffixMappings * UrlRewrite::_getSuffixMappings(
    url_mapping *new_mapping, const char *src_host, const int src_host_len,
    const int src_host_remain_len)
{
  bool have_group;
  const char *to_host_str;
  int to_host_len;
  int regex_len;
  int startOffset = *src_host == '^' ? 1 : 0;  //regex start char ^

  have_group = (*(src_host + startOffset) == '(');
  regex_len = startOffset + (have_group ? 4 : 2);

  to_host_str = new_mapping->toUrl.host_get(&to_host_len);
  bool tourl_need_replace = memchr(to_host_str, '$', to_host_len) != NULL;

  if (tourl_need_replace) {
    const char *end = to_host_str + to_host_len;
    const char *p = to_host_str;
    while (p < end) {
      if (*p != '$') {
          p++;
      }
      else if (*(p + 1) == '0') {
          p += 2;
      }
      else if (*(p + 1) == '1') {
        if (!have_group) {
          Warning("invalid group number: %c\n", *(p + 1));
          return NULL;
        }
        p += 2;
      }
      else {
        Warning("invalid group number: %c\n", *(p + 1));
        return NULL;
      }
    }
  }

  SuffixMappings *suffixMappings = new SuffixMappings;
  suffixMappings->to_url_host_template_len = to_host_len;
  suffixMappings->to_url_host_template = static_cast<char *>(ats_malloc(to_host_len));
  memcpy(suffixMappings->to_url_host_template, to_host_str, to_host_len);

  suffixMappings->from_hostname_tail_len = src_host_remain_len >= 0 ?
    src_host_remain_len : src_host_len - regex_len;
  suffixMappings->tourl_need_replace = tourl_need_replace;

  return suffixMappings;
}

bool UrlRewrite::_convertToSuffix(MappingsStore &store,
    url_mapping *new_mapping, char *src_host, int *err_no)
{
  int src_host_len;
  int startOffset = *src_host == '^' ? 1 : 0;  //regex start char ^

  *err_no = 0;
  src_host_len = strlen(src_host);

  char host_suffix[1024];
  char *remain_str;
  int remain_len;
  int regex_len = (*(src_host + startOffset) == '(') ? 4 : 2;
  remain_str = src_host + startOffset + regex_len;
  remain_len = (src_host + src_host_len) - remain_str;

  if (remain_len > 0 && (*(remain_str + remain_len - 1) == '$' ||
      memchr(remain_str, '\\', remain_len) != NULL))
  {
    if (remain_len >= (int)sizeof(host_suffix)) {
      fprintf(stderr, "src hostname %s is too long, exceeds %d",
          src_host, (int)sizeof(host_suffix));
      *err_no = ENAMETOOLONG;
      return false;
    }

    char *host_end;
    char *pSrc;
    char *pDest;

    pDest = host_suffix;
    host_end = remain_str + remain_len;
    if (*(host_end - 1) == '$') {
      --host_end; //skip regex end char $
    }
    for (pSrc=remain_str; pSrc<host_end; pSrc++) {
      if (*pSrc == '\\' && *(pSrc + 1) == '.') {
        *pDest++ = *(++pSrc);
      }
      else {
        *pDest++ = *pSrc;
      }
    }
    *pDest = '\0';

    remain_str = host_suffix;
    remain_len = pDest - host_suffix;
  }

  if (MappingManager::isRegex(remain_str, remain_len))
  {
    return false;
  }

  SuffixMappings *oldSuffixMappings = NULL;
  SuffixMappings *newSuffixMappings = _getSuffixMappings(new_mapping,
      src_host, src_host_len, remain_len);
  if (newSuffixMappings == NULL) {
    *err_no = errno != 0 ? errno : ENOMEM;
    return false;
  }

  char request_host_key[TS_MAX_HOST_NAME_LEN + 32];
  int host_key_len = _getHostnameKey(&new_mapping->fromURL, remain_str,
      request_host_key, sizeof(request_host_key));

  if (store.suffix_trie == NULL) {
    store.suffix_trie = new HostnameTrie<SuffixMappings>(false);
  }
  else {
    oldSuffixMappings = store.suffix_trie->lookupLast(
        request_host_key, host_key_len);
    if (oldSuffixMappings != NULL) {
      if (oldSuffixMappings->from_hostname_tail_len !=
          newSuffixMappings->from_hostname_tail_len)
      {
        Debug("url_rewrite", "set oldSuffixMappings to NULL for host [%s]", src_host);
        oldSuffixMappings = NULL;
      }
    }
  }

  SuffixMappings *suffixMappings;
  if (oldSuffixMappings == NULL) {
    if (!store.suffix_trie->insert(request_host_key, host_key_len,
          newSuffixMappings))
    {
      delete newSuffixMappings;
      Warning("duplcate from host: %s", src_host);
      *err_no = EEXIST;
      return false;
    }

    suffixMappings = newSuffixMappings;
  }
  else {
    if (!oldSuffixMappings->tourl_need_replace) {
      oldSuffixMappings->tourl_need_replace = newSuffixMappings->tourl_need_replace;
    }

    delete newSuffixMappings;
    suffixMappings = oldSuffixMappings;
  }

  if (!suffixMappings->mapping_paths.Insert(new_mapping)) {
    Warning("Could not insert new mapping");
    *err_no = EEXIST;
    return false;
  }

  return true;
}

inline bool
UrlRewrite::_addToStore(MappingsStore &store, url_mapping *new_mapping,
                        char *src_host, bool is_cur_mapping_regex, int &count)
{
  bool retval = false;
  if (is_cur_mapping_regex) {
    bool suffix = false;
    int offset = *src_host == '^' ? 1 : 0;  //regex start char ^
    if (strncmp(src_host + offset, ".*", 2) == 0 ||
        strncmp(src_host + offset, "(.*)", 4) == 0)
    {
      int result;
      if (_convertToSuffix(store, new_mapping, src_host, &result)) {
        retval = true;
        suffix = true;
        if (store.suffix_trie_min_rank < 0) {
          store.suffix_trie_min_rank = new_mapping->getRank();
        }
      }
      else {
        if (result != 0) {
          Warning("_convertToSuffix fail, error info: %s", strerror(result));
          retval = false;
          suffix = true;
        }
      }
    }

    if (!suffix) {
      RegexMapping* reg_map;
      reg_map = NEW(new RegexMapping);
      if (!_processRegexMappingConfig(src_host, new_mapping, reg_map)) {
        Warning("Could not process regex mapping config line");
        return false;
      }
      Debug("url_rewrite_regex", "Configured regex rule for host [%s]", src_host);

      store.regex_list.enqueue(reg_map);

      if (store.regex_list_min_rank < 0) {
        store.regex_list_min_rank = new_mapping->getRank();
      }
      retval = true;
    }
  } else {
    retval = TableInsert(store.hash_lookup, new_mapping, src_host);
  }

  if (retval) {
    ++count;
  }
  return retval;
}

/**
  Reads the configuration file and creates a new hash table.

  @return zero on success and non-zero on failure.

*/
int
UrlRewrite::BuildTable()
{
  char errBuf[1024];
  char errStrBuf[1024];
  bool alarm_already = false;
  const char *errStr;

  // Vars to build the mapping
  const char *fromScheme, *toScheme;
  int fromSchemeLen, toSchemeLen;
  const char *fromHost, *toHost;
  int fromHostLen, toHostLen;
  const char *map_from, *map_from_start;
  const char *map_to, *map_to_start;
  char *fromHost_lower = NULL;
  char *fromHost_lower_ptr = NULL;
  char fromHost_lower_buf[1024];
  url_mapping *new_mapping = NULL;
  mapping_type maptype;
  int origLength;
  int length;

  bool is_cur_mapping_regex;
  bool add_result;

  ink_assert(forward_mappings.empty());
  ink_assert(reverse_mappings.empty());
  ink_assert(permanent_redirects.empty());
  ink_assert(temporary_redirects.empty());
  ink_assert(forward_mappings_with_recv_port.empty());
  ink_assert(num_rules_forward == 0);
  ink_assert(num_rules_reverse == 0);
  ink_assert(num_rules_redirect_permanent == 0);
  ink_assert(num_rules_redirect_temporary == 0);
  ink_assert(num_rules_forward_with_recv_port == 0);

  forward_mappings.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  reverse_mappings.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  permanent_redirects.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  temporary_redirects.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  forward_mappings_with_recv_port.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);

  int result;
  RemapParser parser;
  DirectiveParams rootParams(0, NULL, 0, NULL, NULL, NULL, 0, true);

  Debug("url_rewrite", "[BuildTable] UrlRewrite::BuildTable()");
  if ((result=parser.loadFromFile(config_file_path, &rootParams)) != 0) {
    Warning("Can't load remapping configuration file - %s", config_file_path);
    return result;
  }

  ACLDefineManager *defineManager = ACLDefineManager::getInstance();
  if ((result=defineManager->init(&rootParams)) != 0) {
    return result;
  }

  //defineManager->print();

  MappingManager mappingManager;
  if ((result=mappingManager.load(&rootParams)) != 0) {
    Warning("MappingManager::load fail, error info: %s", strerror(result));
    return result;
  }

  const DynamicArray<MappingEntry *> &mappings = mappingManager.getMappings();
  MappingEntry *mappingEntry;
  const char *redirectUrl;
  const DynamicArray<ACLMethodIpCheckList *> *aclMethodIpCheckList;
  const DynamicArray<ACLRefererCheckList *> *aclRefererCheckList;
  int mappingFlags;

  if (is_debug_tag_set("url_rewrite")) {
    printf("mapping count: %d\n", mappings.count);
    mappingManager.print();
  }
  if ((result=mappingManager.expand()) != 0) {
    Warning("MappingManager::expand fail, error info: %s", strerror(result));
    return result;
  }
  if (is_debug_tag_set("url_rewrite")) {
    printf("mapping count after regex range expand: %d\n", mappings.count);
  }

  HttpConfigParams *httpConfig = HttpConfig::acquire();
  if (httpConfig == NULL) {
    Warning("HttpConfig::acquire() fail");
    return ENOENT;
  }

  for (int i=0; i<mappings.count; i++) {
    mappingEntry = mappings.items[i];
    mappingFlags = mappingEntry->getFlags();
    is_cur_mapping_regex = (mappingFlags & MAPPING_FLAG_REGEX) != 0;

    if (mappingEntry->getType() == MAPPING_TYPE_MAP) {
      if ((mappingFlags & MAP_FLAG_REVERSE) != 0) {
        Debug("url_rewrite", "[BuildTable] - REVERSE_MAP");
        maptype = REVERSE_MAP;
      }
      else if ((mappingFlags & MAP_FLAG_WITH_RECV_PORT) != 0) {
        Debug("url_rewrite", "[BuildTable] - FORWARD_MAP_WITH_RECV_PORT");
        maptype = FORWARD_MAP_WITH_RECV_PORT;
      }
      else {
        //Debug("url_rewrite", "[BuildTable] - FORWARD_MAP");
        maptype = FORWARD_MAP;
      }
    }
    else {
      if ((mappingFlags & REDIRECT_FALG_TEMPORARY) != 0) {
        Debug("url_rewrite", "[BuildTable] - TEMPORARY_REDIRECT");
        maptype = TEMPORARY_REDIRECT;
      }
      else {
        Debug("url_rewrite", "[BuildTable] - PERMANENT_REDIRECT");
        maptype = PERMANENT_REDIRECT;
      }
    }

    new_mapping = NEW(new url_mapping(mappingEntry->getLineNo()));  // use line # for rank for now

    map_from = mappingEntry->getFromUrl()->str;
    length = UrlWhack((char *)map_from, &origLength);

    // FIX --- what does this comment mean?
    //
    // URL::create modified map_from so keep a point to
    //   the beginning of the string
    if (length > 2 && map_from[length - 1] == '/' && map_from[length - 2] == '/') {
      new_mapping->unique = true;
      length -= 2;
    }

    map_from_start = map_from;
    new_mapping->fromURL.create(NULL);
    new_mapping->fromURL.parse_no_path_component_breakdown(map_from, length);
    *((char *)map_from_start + origLength) = '\0';  // Unwhack

    map_to = mappingEntry->getToUrl()->str;
    length = UrlWhack((char *)map_to, &origLength);
    map_to_start = map_to;

    new_mapping->toUrl.create(NULL);
    new_mapping->toUrl.parse_no_path_component_breakdown(map_to, length);
    *((char *)map_to_start + origLength) = '\0';    // Unwhack

    fromScheme = new_mapping->fromURL.scheme_get(&fromSchemeLen);
    // If the rule is "/" or just some other relative path
    //   we need to default the scheme to http
    if (fromScheme == NULL || fromSchemeLen == 0) {
      new_mapping->fromURL.scheme_set(URL_SCHEME_HTTP, URL_LEN_HTTP);
      fromScheme = new_mapping->fromURL.scheme_get(&fromSchemeLen);
      new_mapping->wildcard_from_scheme = true;
    }
    toScheme = new_mapping->toUrl.scheme_get(&toSchemeLen);

    // Include support for HTTPS scheme
    if ((fromScheme != URL_SCHEME_HTTP && fromScheme != URL_SCHEME_HTTPS &&
         fromScheme != URL_SCHEME_TUNNEL) ||
        (toScheme != URL_SCHEME_HTTP && toScheme != URL_SCHEME_HTTPS &&
         toScheme != URL_SCHEME_TUNNEL)) {
      errStr = "Only http, https, and tunnel remappings are supported";
      goto MAP_ERROR;
    }

    redirectUrl = mappingEntry->getRedirectUrl();
    if (redirectUrl != NULL && *redirectUrl != '\0') {
        new_mapping->filter_redirect_url = ats_strdup(redirectUrl);
        if (!strcasecmp(redirectUrl, "<default>") ||
            !strcasecmp(redirectUrl, "default") ||
            !strcasecmp(redirectUrl, "<default_redirect_url>") ||
            !strcasecmp(redirectUrl, "default_redirect_url"))
        {
          new_mapping->default_redirect_url = true;
        }
        else {
          char *newRedirectUrl;
          newRedirectUrl = strdup(redirectUrl);
          new_mapping->redir_chunk_list = redirect_tag_str::
            parse_format_redirect_url(newRedirectUrl);
          free(newRedirectUrl);
        }
    }
    else {
      new_mapping->default_redirect_url = true;
    }

    aclMethodIpCheckList = mappingEntry->getACLMethodIpCheckLists();
    aclRefererCheckList = mappingEntry->getACLRefererCheckLists();
    result = new_mapping->setMethodIpCheckLists(aclMethodIpCheckList->items, aclMethodIpCheckList->count);
    if (result != 0) {
      sprintf(errStrBuf, "setMethodIpCheckLists fail, error info: %s", strerror(result));
      errStr = errStrBuf;
      goto MAP_ERROR;
    }
    result = new_mapping->setRefererCheckLists(aclRefererCheckList->items, aclRefererCheckList->count);
    if (result != 0) {
      sprintf(errStrBuf, "setRefererCheckLists fail, error info: %s", strerror(result));
      errStr = errStrBuf;
      goto MAP_ERROR;
    }

    // Check to see the fromHost remapping is a relative one
    fromHost = new_mapping->fromURL.host_get(&fromHostLen);
    if (fromHost == NULL || fromHostLen <= 0) {
      if (maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER || maptype == FORWARD_MAP_WITH_RECV_PORT) {
        if (*map_from_start != '/') {
          errStr = "Relative remappings must begin with a /";
          goto MAP_ERROR;
        } else {
          fromHost = "";
          fromHostLen = 0;
        }
      } else {
        errStr = "Remap source in reverse mappings requires a hostname";
        goto MAP_ERROR;
      }
    }

    toHost = new_mapping->toUrl.host_get(&toHostLen);
    if (toHost == NULL || toHostLen <= 0) {
      errStr = "The remap destinations require a hostname";
      goto MAP_ERROR;
    }
    // Get rid of trailing slashes since they interfere
    //  with our ability to send redirects

    // You might be tempted to remove these lines but the new
    // optimized header system will introduce problems.  You
    // might get two slashes occasionally instead of one because
    // the rest of the system assumes that trailing slashes have
    // been removed.


    if (unlikely(fromHostLen >= (int) sizeof(fromHost_lower_buf))) {
      fromHost_lower = (fromHost_lower_ptr = (char *)ats_malloc(fromHostLen + 1));
    } else {
      fromHost_lower = fromHost_lower_buf;
    }
    // Canonicalize the hostname by making it lower case
    memcpy(fromHost_lower, fromHost, fromHostLen);
    fromHost_lower[fromHostLen] = 0;
    LowerCaseStr(fromHost_lower);

    // set the normalized string so nobody else has to normalize this
    new_mapping->fromURL.host_set(fromHost_lower, fromHostLen);

    // If a TS receives a request on a port which is set to tunnel mode
    // (ie, blind forwarding) and a client connects directly to the TS,
    // then the TS will use its IPv4 address and remap rules given
    // to send the request to its proper destination.
    // See HttpTransact::HandleBlindTunnel().
    // Therefore, for a remap rule like "map tunnel://hostname..."
    // in remap.config, we also needs to convert hostname to its IPv4 addr
    // and gives a new remap rule with the IPv4 addr.
    if ((maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER || maptype == FORWARD_MAP_WITH_RECV_PORT) &&
        fromScheme == URL_SCHEME_TUNNEL && (fromHost_lower[0]<'0' || fromHost_lower[0]> '9')) {
      addrinfo* ai_records; // returned records.
      ip_text_buffer ipb; // buffer for address string conversion.
      if (0 == getaddrinfo(fromHost_lower, 0, 0, &ai_records)) {
        for ( addrinfo* ai_spot = ai_records ; ai_spot ; ai_spot = ai_spot->ai_next) {
          if (ats_is_ip(ai_spot->ai_addr) &&
              !ats_is_ip_any(ai_spot->ai_addr)) {
            url_mapping *u_mapping;

            ats_ip_ntop(ai_spot->ai_addr, ipb, sizeof ipb);
            u_mapping = NEW(new url_mapping);
            u_mapping->fromURL.create(NULL);
            u_mapping->fromURL.copy(&new_mapping->fromURL);
            u_mapping->fromURL.host_set(ipb, strlen(ipb));
            u_mapping->toUrl.create(NULL);
            u_mapping->toUrl.copy(&new_mapping->toUrl);
            bool insert_result = (maptype != FORWARD_MAP_WITH_RECV_PORT) ?
              TableInsert(forward_mappings.hash_lookup, u_mapping, ipb) :
              TableInsert(forward_mappings_with_recv_port.hash_lookup, u_mapping, ipb);
            if (!insert_result) {
              errStr = "Unable to add mapping rule to lookup table";
              goto MAP_ERROR;
            }
            (maptype != FORWARD_MAP_WITH_RECV_PORT) ? ++num_rules_forward : ++num_rules_forward_with_recv_port;
            SetHomePageRedirectFlag(u_mapping, u_mapping->toUrl);
          }
        }
        freeaddrinfo(ai_records);
      }
    }

    if ((maptype == FORWARD_MAP || maptype == FORWARD_MAP_WITH_RECV_PORT)) {
      const DynamicArray<PluginInfo> *plugins = mappingEntry->getPlugins();
      for (int k=0; k<plugins->count; k++) {
        if (load_remap_plugin(plugins->items + k, mappingEntry,
              new_mapping, errStrBuf, sizeof(errStrBuf)) != 0)
        {
          Debug("remap_plugin", "Remap plugin load error - %s", errStrBuf[0] ? errStrBuf : "Unknown error");
          errStr = errStrBuf;
          goto MAP_ERROR;
        }
      }
    }

    // Now add the mapping to appropriate container
    add_result = false;
    switch (maptype) {
      case FORWARD_MAP:
      case FORWARD_MAP_REFERER:
        if ((add_result = _addToStore(forward_mappings, new_mapping, fromHost_lower,
                is_cur_mapping_regex, num_rules_forward)) == true) {
          // @todo: is this applicable to regex mapping too?
          SetHomePageRedirectFlag(new_mapping, new_mapping->toUrl);
        }
        break;
      case REVERSE_MAP:
        add_result = _addToStore(reverse_mappings, new_mapping, fromHost_lower,
            is_cur_mapping_regex, num_rules_reverse);
        new_mapping->homePageRedirect = false;
        break;
      case PERMANENT_REDIRECT:
        add_result = _addToStore(permanent_redirects, new_mapping, fromHost_lower,
            is_cur_mapping_regex, num_rules_redirect_permanent);
        break;
      case TEMPORARY_REDIRECT:
        add_result = _addToStore(temporary_redirects, new_mapping, fromHost_lower,
            is_cur_mapping_regex, num_rules_redirect_temporary);
        break;
      case FORWARD_MAP_WITH_RECV_PORT:
        add_result = _addToStore(forward_mappings_with_recv_port, new_mapping, fromHost_lower,
            is_cur_mapping_regex, num_rules_forward_with_recv_port);
        break;
      default:
        // 'default' required to avoid compiler warning; unsupported map
        // type would have been dealt with much before this
        break;
    }
    if (!add_result) {
      errStr = "Unable to add mapping rule to lookup table";
      goto MAP_ERROR;
    }

    fromHost_lower_ptr = (char *)ats_free_null(fromHost_lower_ptr);

    if (!_getRecordsConfig(new_mapping, mappingEntry->getConfigs() +
          CONFIG_TYPE_RECORDS_INDEX, httpConfig))
    {
      errStr = "Load records config fail";
      goto MAP_ERROR;
    }

    continue;

    // Deal with error / warning scenarios
  MAP_ERROR:
    HttpConfig::release(httpConfig);
    Warning("Could not add rule at line #%d; Aborting!",
        mappingEntry->getLineNo());
    snprintf(errBuf, sizeof(errBuf), "%s %s at line %d",
        modulePrefix, errStr, mappingEntry->getLineNo());
    SignalError(errBuf, alarm_already);
    return 2;
  }

  HttpConfig::release(httpConfig);

  // Add the mapping for backdoor urls if enabled.
  // This needs to be before the default PAC mapping for ""
  // since this is more specific
  if (unlikely(backdoor_enabled)) {
    new_mapping = SetupBackdoorMapping();
    if (TableInsert(forward_mappings.hash_lookup, new_mapping, "")) {
      num_rules_forward++;
    } else {
      Warning("Could not insert backdoor mapping into store");
      delete new_mapping;
      return 3;
    }
  }
  // Add the default mapping to the manager PAC file
  //  if we need it
  if (default_to_pac) {
    new_mapping = SetupPacMapping();
    if (TableInsert(forward_mappings.hash_lookup, new_mapping, "")) {
      num_rules_forward++;
    } else {
      Warning("Could not insert pac mapping into store");
      delete new_mapping;
      return 3;
    }
  }

  // Destroy unused tables
  if (num_rules_forward == 0) {
    forward_mappings.hash_lookup = ink_hash_table_destroy(forward_mappings.hash_lookup);
  } else {
    if (ink_hash_table_isbound(forward_mappings.hash_lookup, "")) {
      nohost_rules = 1;
    }
  }

  if (num_rules_reverse == 0) {
    reverse_mappings.hash_lookup = ink_hash_table_destroy(reverse_mappings.hash_lookup);
  }

  if (num_rules_redirect_permanent == 0) {
    permanent_redirects.hash_lookup = ink_hash_table_destroy(permanent_redirects.hash_lookup);
  }

  if (num_rules_redirect_temporary == 0) {
    temporary_redirects.hash_lookup = ink_hash_table_destroy(temporary_redirects.hash_lookup);
  }

  if (num_rules_forward_with_recv_port == 0) {
    forward_mappings_with_recv_port.hash_lookup = ink_hash_table_destroy(
      forward_mappings_with_recv_port.hash_lookup);
  }

  return 0;
}

/**
  Inserts arg mapping in h_table with key src_host chaining the mapping
  of existing entries bound to src_host if necessary.

*/
bool
UrlRewrite::TableInsert(InkHashTable *h_table, url_mapping *mapping, const char *src_host)
{
  char src_host_tmp_buf[1];
  char hostname_key[TS_MAX_HOST_NAME_LEN + 32];
  UrlMappingPathIndex *ht_contents;

  if (!src_host) {
    src_host = src_host_tmp_buf;
    *src_host_tmp_buf = '\0';
  }

  _getHostnameKey(&mapping->fromURL, src_host, hostname_key, sizeof(hostname_key));

  // Insert the new_mapping into hash table
  if (ink_hash_table_lookup(h_table, hostname_key, (void**) &ht_contents)) {
    // There is already a path index for this host
    if (ht_contents == NULL) {
      // why should this happen?
      Warning("Found entry cannot be null!");
      return false;
    }
  } else {
    ht_contents = new UrlMappingPathIndex();
    ink_hash_table_insert(h_table, hostname_key, ht_contents);
  }
  if (!ht_contents->Insert(mapping)) {
    Warning("Could not insert new mapping");
    return false;
  }
  return true;
}

int UrlRewrite::load_remap_plugin(const PluginInfo *plugin,
    const MappingEntry *mappingEntry, url_mapping *mp,
    char *errbuf, int errbufsize)
{
  TSRemapInterface ri;
  struct stat stat_buf;
  remap_plugin_info *pi;
  const char *filepath;
  char *err, tmpbuf[2048], default_path[PATH_NAME_MAX], rpath[PATH_NAME_MAX];
  int idx = 0, retcode = 0, rsize = 0;

  *tmpbuf = 0;

  filepath = plugin->filename.str;
  if (lstat(filepath, &stat_buf) != 0) {
    const char *plugin_default_path = TSPluginDirGet();

    // Try with the plugin path instead
    if (plugin->filename.length + strlen(plugin_default_path) > (PATH_NAME_MAX - 1)) {
      Debug("remap_plugin", "way too large a path specified for remap plugin");
      return -3;
    }

    snprintf(default_path, PATH_NAME_MAX, "%s/%s", plugin_default_path, filepath);
    Debug("remap_plugin", "attempting to stat default plugin path: %s", default_path);

    if (lstat(default_path, &stat_buf) == 0) {
      Debug("remap_plugin", "stat successful on %s using that", default_path);
      filepath = default_path;
    } else {
      snprintf(errbuf, errbufsize, "Can't find remap plugin file \"%s\"", filepath);
      return -3;
    }
  }

  if (S_ISLNK(stat_buf.st_mode)) {
    rsize = get_real_path(filepath, rpath, PATH_NAME_MAX);
    if (rsize < 0) {
      snprintf(errbuf, errbufsize, "Can't get_real_path \"%s\"", filepath);
      return -3;
    }

    filepath = rpath;
  }

  Debug("remap_plugin", "using path %s for plugin", filepath);

  if (!remap_pi_list || (pi = remap_pi_list->find_by_path(filepath)) == NULL) {
    pi = NEW(new remap_plugin_info(filepath));
    if (!remap_pi_list) {
      remap_pi_list = pi;
    } else {
      remap_pi_list->add_to_list(pi);
    }
    Debug("remap_plugin", "New remap plugin info created for \"%s\"", filepath);

    if ((pi->dlh = dlopen(filepath, RTLD_NOW)) == NULL) {
#if defined(freebsd) || defined(openbsd)
      err = (char *)dlerror();
#else
      err = dlerror();
#endif
      snprintf(errbuf, errbufsize, "Can't load plugin \"%s\" - %s", filepath, err ? err : "Unknown dlopen() error");
      return -4;
    }

    pi->fp_tsremap_init = (remap_plugin_info::_tsremap_init *) dlsym(pi->dlh, TSREMAP_FUNCNAME_INIT);
    pi->fp_tsremap_done = (remap_plugin_info::_tsremap_done *) dlsym(pi->dlh, TSREMAP_FUNCNAME_DONE);
    pi->fp_tsremap_new_instance = (remap_plugin_info::_tsremap_new_instance *) dlsym(pi->dlh, TSREMAP_FUNCNAME_NEW_INSTANCE);
    pi->fp_tsremap_delete_instance = (remap_plugin_info::_tsremap_delete_instance *) dlsym(pi->dlh, TSREMAP_FUNCNAME_DELETE_INSTANCE);
    pi->fp_tsremap_do_remap = (remap_plugin_info::_tsremap_do_remap *) dlsym(pi->dlh, TSREMAP_FUNCNAME_DO_REMAP);
    pi->fp_tsremap_os_response = (remap_plugin_info::_tsremap_os_response *) dlsym(pi->dlh, TSREMAP_FUNCNAME_OS_RESPONSE);

    if (!pi->fp_tsremap_init) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"", TSREMAP_FUNCNAME_INIT, filepath);
      retcode = -10;
    } else if (!pi->fp_tsremap_new_instance) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"",
                   TSREMAP_FUNCNAME_NEW_INSTANCE, filepath);
      retcode = -11;
    } else if (!pi->fp_tsremap_do_remap) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"", TSREMAP_FUNCNAME_DO_REMAP, filepath);
      retcode = -12;
    }
    if (retcode) {
      if (errbuf && errbufsize > 0)
        Debug("remap_plugin", "%s", errbuf);
      dlclose(pi->dlh);
      pi->dlh = NULL;
      return retcode;
    }

    memset(&ri, 0, sizeof(ri));
    ri.size = sizeof(ri);
    ri.tsremap_version = TSREMAP_VERSION;

    if (pi->fp_tsremap_init(&ri, tmpbuf, sizeof(tmpbuf) - 1) != TS_SUCCESS) {
      Warning("Failed to initialize plugin %s (non-zero retval) ... bailing out", pi->path);
      return -5;
    }
    Debug("remap_plugin", "Remap plugin \"%s\" - initialization completed", filepath);
  }

  if (!pi->dlh) {
    snprintf(errbuf, errbufsize, "Can't load plugin \"%s\"", filepath);
    return -6;
  }

  int parc = 0;
  char *parv[MAX_PARAM_NUM + 2];

  memset(parv, 0, sizeof(parv));
  parv[parc++] = (char *)mappingEntry->getFromUrl()->str;
  parv[parc++] = (char *)mappingEntry->getToUrl()->str;

  for (idx = 0; idx < plugin->paramCount; idx++) {
      parv[parc++] = (char *)plugin->params[idx].str;
  }

  Debug("url_rewrite", "Viewing parsed plugin parameters for %s", pi->path);
  for (int k = 0; k < parc; k++) {
    Debug("url_rewrite", "Argument %d: %s", k, parv[k]);
  }

  void* ih;

  Debug("remap_plugin", "creating new plugin instance");
  TSReturnCode res = pi->fp_tsremap_new_instance(parc, parv, &ih, tmpbuf, sizeof(tmpbuf) - 1);

  Debug("remap_plugin", "done creating new plugin instance");

  if (res != TS_SUCCESS) {
    snprintf(errbuf, errbufsize, "Can't create new remap instance for plugin \"%s\" - %s", filepath,
                 tmpbuf[0] ? tmpbuf : "Unknown plugin error");
    Warning("Failed to create new instance for plugin %s (not a TS_SUCCESS return)", pi->path);
    return -8;
  }

  mp->add_plugin(pi, ih);

  return 0;
}

/**  First looks up the hash table for "simple" mappings and then the
     regex mappings.  Only higher-ranked regex mappings are examined if
     a hash mapping is found; or else all regex mappings are examined

     Returns highest-ranked mapping on success, NULL on failure
*/
bool
UrlRewrite::_mappingLookup(MappingsStore &mappings, URL *request_url,
                           int request_port, const char *request_host, int request_host_len,
                           UrlMappingContainer &mapping_container)
{
  char request_host_key[TS_MAX_HOST_NAME_LEN + 32];
  char request_host_lower[TS_MAX_HOST_NAME_LEN];
  int host_key_len;

  if (!request_host || !request_url ||
      (request_host_len < 0) || (request_host_len >= TS_MAX_HOST_NAME_LEN)) {
    Debug("url_rewrite", "Invalid arguments!");
    return false;
  }
  // lowercase
  for (int i = 0; i < request_host_len; ++i) {
    request_host_lower[i] = tolower(request_host[i]);
  }
  request_host_lower[request_host_len] = 0;

  host_key_len = _getHostnameKey(request_url, request_host_lower,
      request_host_key, sizeof(request_host_key), request_port);

  bool retval = false;
  int rank_ceiling = -1;
  url_mapping *mapping = _tableLookup(mappings.hash_lookup, request_url,
      request_host_key);
  if (mapping != NULL) {
    rank_ceiling = mapping->getRank();
    Debug("url_rewrite", "Found 'simple' mapping with rank %d", rank_ceiling);
    mapping_container.set(mapping);
    retval = true;
  }

  if (mappings.suffix_trie != NULL && (rank_ceiling < 0 ||
        rank_ceiling > mappings.suffix_trie_min_rank) &&
      _suffixMappingLookup(mappings.suffix_trie, request_url,
        request_host_lower, request_host_len, request_host_key,
        host_key_len, rank_ceiling, mapping_container))
  {
    Debug("url_rewrite", "Found suffix trie mapping with rank %d",
        (mapping_container.getMapping())->getRank());
    rank_ceiling = mapping_container.getMapping()->getRank();
    retval = true;
  }

  if (!mappings.regex_list.empty() && (rank_ceiling < 0 ||
        rank_ceiling > mappings.regex_list_min_rank) &&
      _regexMappingLookup(mappings.regex_list, request_url, request_port,
        request_host_lower, request_host_len, rank_ceiling,
        mapping_container))
  {
    Debug("url_rewrite", "Using regex mapping with rank %d", (mapping_container.getMapping())->getRank());
    rank_ceiling = mapping_container.getMapping()->getRank();
    retval = true;
  }

  return retval;
}

// does not null terminate return string
int
UrlRewrite::_expandSubstitutions(int *matches_info, const RegexMapping *reg_map,
                                 const char *matched_string,
                                 char *dest_buf, int dest_buf_size)
{
  int cur_buf_size = 0;
  int token_start = 0;
  int n_bytes_needed;
  int match_index;
  for (int i = 0; i < reg_map->n_substitutions; ++i) {
    // first copy preceding bytes
    n_bytes_needed = reg_map->substitution_markers[i] - token_start;
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, reg_map->to_url_host_template + token_start, n_bytes_needed);
    cur_buf_size += n_bytes_needed;

    // then copy the sub pattern match
    match_index = reg_map->substitution_ids[i] * 2;
    n_bytes_needed = matches_info[match_index + 1] - matches_info[match_index];
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, matched_string + matches_info[match_index], n_bytes_needed);
    cur_buf_size += n_bytes_needed;

    token_start = reg_map->substitution_markers[i] + 2; // skip the place holder
  }

  // copy last few bytes (if any)
  if (token_start < reg_map->to_url_host_template_len) {
    n_bytes_needed = reg_map->to_url_host_template_len - token_start;
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, reg_map->to_url_host_template + token_start, n_bytes_needed);
    cur_buf_size += n_bytes_needed;
  }
  Debug("url_rewrite_regex", "Expanded substitutions and returning string [%.*s] with length %d",
        cur_buf_size, dest_buf, cur_buf_size);
  return cur_buf_size;

 lOverFlow:
  Warning("Overflow while expanding substitutions");
  return 0;
}

bool UrlRewrite::_setToUrlHostname(const SuffixMappings *suffixMappings,
    const char *request_host, const int request_host_len,
    UrlMappingContainer &mapping_container)
{
  if (!suffixMappings->tourl_need_replace) {
    return false;
  }

  char buf[1024];
  char *pSrc;
  char *pDest;
  char *template_end;
  int len;

  pDest = buf;
  template_end = suffixMappings->to_url_host_template +
    suffixMappings->to_url_host_template_len;
  for (pSrc=suffixMappings->to_url_host_template; pSrc<template_end; pSrc++) {
    if (*pSrc != '$') {
      *pDest++ = *pSrc;
      continue;
    }

    if ((int)(pDest - buf) + request_host_len + suffixMappings->
        to_url_host_template_len > (int)sizeof(buf))
    {
      Warning("file: "__FILE__", line: %d, buffer overflow!", __LINE__);
      return false;
    }

    if (*(pSrc + 1) == '1') { //$1
      len = request_host_len - suffixMappings->from_hostname_tail_len;
      if (len > 0) {
        memcpy(pDest, request_host, len);
        pDest += len;
      }
      pSrc++;
    }
    else if (*(pSrc + 1) == '0') { //$0

      memcpy(pDest, request_host, request_host_len);
      pDest += request_host_len;
      pSrc++;
    }
    else {
      Warning("file: "__FILE__", line: %d, unreachable statement!", __LINE__);
    }
  }

  URL *expanded_url = mapping_container.createNewToURL();
  expanded_url->copy(&(mapping_container.getMapping()->toUrl));
  expanded_url->host_set(buf, (int)(pDest - buf));
  Debug("url_rewrite", "Expanded toURL to [%.*s]",
      expanded_url->length_get(), expanded_url->string_get_ref());
  return true;
}

bool
UrlRewrite::_suffixMappingLookup(HostnameTrie<SuffixMappings> *suffix_trie,
    URL *request_url, const char *request_host, const int request_host_len,
    const char *request_host_key, int host_key_len, int rank_ceiling,
    UrlMappingContainer &mapping_container)
{
  url_mapping *um = NULL;
  url_mapping *found;

  if (rank_ceiling == -1) { // we will now look at all regex mappings
    rank_ceiling = INT_MAX;
  }

  HostnameTrie<SuffixMappings>::LookupState state;
  SuffixMappings *suffixMappings = NULL;
  SuffixMappings *suffixFound;

  suffixFound = suffix_trie->lookupFirst(request_host_key, host_key_len, &state);
  while (suffixFound != NULL) {
    found = suffixFound->mapping_paths.Search(request_url);
    if (found != NULL && found->getRank() < rank_ceiling) {
      suffixMappings = suffixFound;
      um = found;
      rank_ceiling = found->getRank();
    }

    suffixFound = suffix_trie->lookupNext(request_host_key, host_key_len, &state);
  }

  if (um != NULL) {
    mapping_container.set(um);

    _setToUrlHostname(suffixMappings, request_host, request_host_len,
        mapping_container);
    return true;
  }
  else {
    return false;
  }
}

bool
UrlRewrite::_regexMappingLookup(RegexMappingList &regex_mappings, URL *request_url, int request_port,
                                const char *request_host, int request_host_len, int rank_ceiling,
                                UrlMappingContainer &mapping_container)
{
  bool retval = false;

  if (rank_ceiling == -1) { // we will now look at all regex mappings
    rank_ceiling = INT_MAX;
    Debug("url_rewrite_regex", "Going to match all regexes");
  }
  else {
    Debug("url_rewrite_regex", "Going to match regexes with rank <= %d", rank_ceiling);
  }

  int request_scheme_len, reg_map_scheme_len;
  const char *request_scheme = request_url->scheme_get(&request_scheme_len), *reg_map_scheme;

  int request_path_len, reg_map_path_len;
  const char *request_path = request_url->path_get(&request_path_len), *reg_map_path;

  // Loop over the entire linked list, or until we're satisfied
  forl_LL(RegexMapping, list_iter, regex_mappings) {
    int reg_map_rank = list_iter->url_map->getRank();

    if (reg_map_rank > rank_ceiling) {
      break;
    }

    reg_map_scheme = list_iter->url_map->fromURL.scheme_get(&reg_map_scheme_len);
    if ((request_scheme_len != reg_map_scheme_len) ||
        strncmp(request_scheme, reg_map_scheme, request_scheme_len)) {
      Debug("url_rewrite_regex", "Skipping regex with rank %d as scheme does not match request scheme",
            reg_map_rank);
      continue;
    }

    if (list_iter->url_map->fromURL.port_get() != request_port) {
      Debug("url_rewrite_regex", "Skipping regex with rank %d as regex map port does not match request port. "
            "regex map port: %d, request port %d",
            reg_map_rank, list_iter->url_map->fromURL.port_get(), request_port);
      continue;
    }

    reg_map_path = list_iter->url_map->fromURL.path_get(&reg_map_path_len);
    if ((request_path_len < reg_map_path_len) ||
        strncmp(reg_map_path, request_path, reg_map_path_len)) { // use the shorter path length here
      Debug("url_rewrite_regex", "Skipping regex with rank %d as path does not cover request path",
            reg_map_rank);
      continue;
    }

    int matches_info[MAX_REGEX_SUBS * 3];
    int match_result = pcre_exec(list_iter->re, list_iter->re_extra, request_host, request_host_len,
                                 0, 0, matches_info, (sizeof(matches_info) / sizeof(int)));
    if (match_result > 0) {
      Debug("url_rewrite_regex", "Request URL host [%.*s] matched regex in mapping of rank %d "
            "with %d possible substitutions", request_host_len, request_host, reg_map_rank, match_result);

      mapping_container.set(list_iter->url_map);

      char buf[4096];
      int buf_len;

      // Expand substitutions in the host field from the stored template
      buf_len = _expandSubstitutions(matches_info, list_iter, request_host, buf, sizeof(buf));
      URL *expanded_url = mapping_container.createNewToURL();
      expanded_url->copy(&((list_iter->url_map)->toUrl));
      expanded_url->host_set(buf, buf_len);

      Debug("url_rewrite_regex", "Expanded toURL to [%.*s]",
            expanded_url->length_get(), expanded_url->string_get_ref());
      retval = true;
      break;
    } else if (match_result == PCRE_ERROR_NOMATCH) {
      Debug("url_rewrite_regex", "Request URL host [%.*s] did NOT match regex in mapping of rank %d",
            request_host_len, request_host, reg_map_rank);
    } else {
      Warning("pcre_exec() failed with error code %d", match_result);
      break;
    }
  }

  return retval;
}

void
UrlRewrite::_destroyList(RegexMappingList &mappings)
{
  forl_LL(RegexMapping, list_iter, mappings) {
    delete list_iter->url_map;
    if (list_iter->re) {
      pcre_free(list_iter->re);
    }
    if (list_iter->re_extra) {
      pcre_free(list_iter->re_extra);
    }
    if (list_iter->to_url_host_template) {
      ats_free(list_iter->to_url_host_template);
    }
    delete list_iter;
  }
  mappings.clear();
}

/** will process the regex mapping configuration and create objects in
    output argument reg_map. It assumes existing data in reg_map is
    inconsequential and will be perfunctorily null-ed;
*/
bool
UrlRewrite::_processRegexMappingConfig(const char *from_host_lower, url_mapping *new_mapping,
                                       RegexMapping *reg_map)
{
  const char *str;
  int str_index;
  const char *to_host;
  int to_host_len;
  int substitution_id;
  int substitution_count = 0;

  reg_map->re = NULL;
  reg_map->re_extra = NULL;
  reg_map->to_url_host_template = NULL;
  reg_map->to_url_host_template_len = 0;
  reg_map->n_substitutions = 0;

  reg_map->url_map = new_mapping;

  // using from_host_lower (and not new_mapping->fromURL.host_get())
  // as this one will be NULL-terminated (required by pcre_compile)
  reg_map->re = pcre_compile(from_host_lower, 0, &str, &str_index, NULL);
  if (reg_map->re == NULL) {
    Warning("pcre_compile failed! Regex has error starting at %s", from_host_lower + str_index);
    goto lFail;
  }

  reg_map->re_extra = pcre_study(reg_map->re, 0, &str);
  if ((reg_map->re_extra == NULL) && (str != NULL)) {
    Warning("pcre_study failed with message [%s]", str);
    goto lFail;
  }

  int n_captures;
  if (pcre_fullinfo(reg_map->re, reg_map->re_extra, PCRE_INFO_CAPTURECOUNT, &n_captures) != 0) {
    Warning("pcre_fullinfo failed!");
    goto lFail;
  }
  if (n_captures >= MAX_REGEX_SUBS) { // off by one for $0 (implicit capture)
    Warning("Regex has %d capturing subpatterns (including entire regex); Max allowed: %d",
            n_captures + 1, MAX_REGEX_SUBS);
    goto lFail;
  }

  to_host = new_mapping->toUrl.host_get(&to_host_len);
  for (int i = 0; i < (to_host_len - 1); ++i) {
    if (to_host[i] == '$') {
      if (substitution_count > MAX_REGEX_SUBS) {
        Warning("Cannot have more than %d substitutions in mapping with host [%s]",
                MAX_REGEX_SUBS, from_host_lower);
        goto lFail;
      }
      substitution_id = to_host[i + 1] - '0';
      if ((substitution_id < 0) || (substitution_id > n_captures)) {
        Warning("Substitution id [%c] has no corresponding capture pattern in regex [%s]",
              to_host[i + 1], from_host_lower);
        goto lFail;
      }
      reg_map->substitution_markers[reg_map->n_substitutions] = i;
      reg_map->substitution_ids[reg_map->n_substitutions] = substitution_id;
      ++reg_map->n_substitutions;
    }
  }

  // so the regex itself is stored in fromURL.host; string to match
  // will be in the request; string to use for substitutions will be
  // in this buffer
  str = new_mapping->toUrl.host_get(&str_index); // reusing str and str_index
  reg_map->to_url_host_template_len = str_index;
  reg_map->to_url_host_template = static_cast<char *>(ats_malloc(str_index));
  memcpy(reg_map->to_url_host_template, str, str_index);

  return true;

 lFail:
  if (reg_map->re) {
    pcre_free(reg_map->re);
    reg_map->re = NULL;
  }
  if (reg_map->re_extra) {
    pcre_free(reg_map->re_extra);
    reg_map->re_extra = NULL;
  }
  if (reg_map->to_url_host_template) {
    ats_free(reg_map->to_url_host_template);
    reg_map->to_url_host_template = NULL;
    reg_map->to_url_host_template_len = 0;
  }
  return false;
}

bool UrlRewrite::_getRecordsConfig(url_mapping *new_mapping,
    const DynamicArray<ConfigKeyValue> *configs,
    HttpConfigParams *httpConfig)
{
  if (configs->count == 0) {
    return true;
  }

  new_mapping->overridableHttpConfig = new OverridableHttpConfigParams(true);
  memcpy(new_mapping->overridableHttpConfig, &httpConfig->oride,
      sizeof(OverridableHttpConfigParams));

  ConfigKeyValue *kv;
  ConfigKeyValue *end;
  end = configs->items + configs->count;
  for (kv=configs->items; kv<end; kv++) {
    if (TSHttpConfigParamSet(new_mapping->overridableHttpConfig,
        kv->key.str, kv->key.length, kv->value.str, kv->value.length) != TS_SUCCESS)
    {
        Warning("set %s to %s fail, invalid parameter name!",
            kv->key.str, kv->value.str);
        return false;
    }

    //Debug("url_rewrite", "set %s to %s", kv->key.str, kv->value.str);
  }

  return true;
}

inline int UrlRewrite::_getHostnameKey(URL *url, const char *src_host,
    char *buff, const int buffSize, int request_port)
{
  if (request_port == 0) {
    request_port = url->port_get();
  }

  //must split by dot (.)
  return snprintf(buff, buffSize, "%s.%d.%d", src_host,
       request_port, url->scheme_get_wksidx());
}

