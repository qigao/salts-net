// re2c --lang c
#include "uri_parser.h"
#include <stdio.h>
#include <stdlib.h>

int uri_parse_internal(const char *url_str, uri_t *uri)
{
    const char *marker;
    const char *src = url_str;
    int pos = 0;
    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:define:YYCURSOR = url_str;
      re2c:define:YYMARKER = marker;
      re2c:yyfill:enable = 0;

      EOF = "\x00";
      ALPHA = [a-zA-Z];
      DIGIT = [0-9];
      HEXDIG = [0-9a-fA-F];

      SUB_DELIMS = [!$&'()*+,;=] ;
      GEN_DELIMS = [:/?#\[\]@]   ;

      RESERVED = GEN_DELIMS | SUB_DELIMS ;
      UNRESERVED = ALPHA | DIGIT | [-._~] ;

      PCT_ENCODED = "%" HEXDIG HEXDIG ;

      PCHAR = UNRESERVED | PCT_ENCODED | SUB_DELIMS | ":" | "@" ;

      SEGMENT_NZ_NC = ( UNRESERVED | PCT_ENCODED | SUB_DELIMS | "@")+;
      SEGMENT_NZ = PCHAR+;
      SEGMENT = PCHAR*;

      USERINFO = UNRESERVED | PCT_ENCODED | SUB_DELIMS | ":";

      DEC_OCT = DIGIT | [1-9] DIGIT | "1" DIGIT{2} | "2" [0-4] DIGIT | "25"
      [0-5] ;

      IPV4ADDR = DEC_OCT "." DEC_OCT "." DEC_OCT "." DEC_OCT;

      H16 = HEXDIG{1,4};

      REGNAME = UNRESERVED | PCT_ENCODED | SUB_DELIMS ;

      // scheme
      * { return 0; }

      ALPHA (ALPHA | DIGIT | [+.-])* ":" {
        int len =  (int)(url_str - src - 1);
        uri_copy_substring(src, pos, len, uri->scheme, sizeof(uri->scheme));
        pos += len + 1; // skip ":"
        goto hier_part;
      }
    */

hier_part:
    /*!re2c
      * { goto hier_part_path; }
      EOF { return 0;}
      "//"  { pos +=2; goto hier_part_authority; }
    */

hier_part_authority:
    /*!re2c
      [^] { url_str--; goto hier_part_host; }
      EOF { return 0;}
      USERINFO* "@" {
        int len =  (int)( url_str - src - pos - 1); // unshift 1 char for "@"
        uri_copy_substring(src, pos, len, uri->userinfo, sizeof(uri->userinfo));
        pos += len + 1; // shift for "@" char
        goto hier_part_host;
      }
    */

hier_part_host:
    /*!re2c
      EOF { return 0;}
      * { return 0; }
      "" / "/" {
        uri->host_type = URI_HOST_UNKNOWN;
        goto hier_part_path;
      }
      IPV4ADDR {
        int len = (int)( url_str - src - pos);
        uri->host_type = URI_HOST_IPV4ADDR;
        uri_copy_substring(src, pos, len, uri->host, sizeof(uri->host));
        pos += len;
        goto hier_part_port;
      }
      "[v" [^\]\x00]+ "]" {
        pos++; // shift "["
        int len =  (int)(url_str - src - pos - 1);
        uri->host_type = URI_HOST_IPVFUTURE;
        uri_copy_substring(src, pos, len, uri->host, sizeof(uri->host));
        pos += len + 1;
        goto hier_part_port;
      }
      "[" [^\]\x00]+ "]" {
        pos++; // shift "["
        int len =  (int)(url_str - src - pos - 1); // skip "]"
        uri->host_type = URI_HOST_IPV6ADDR;
        uri_copy_substring(src, pos, len, uri->host, sizeof(uri->host));
        pos += len + 1;
        goto hier_part_port;
      }
      REGNAME+ {
        int len = (int)( url_str - src - pos);
        uri->host_type = URI_HOST_REGNAME;
        uri_copy_substring(src, pos, len, uri->host, sizeof(uri->host));
        pos += len;
        goto hier_part_port;
      }
    */

hier_part_port:
    /*!re2c
      EOF {
        uri->valid = 1;
        return 1;
      }
      "" { goto hier_part_path; }
      ":" DIGIT* {
        pos++; // shift ":"
        int len =  (int)(url_str - src - pos);
        char port_str[16];
        uri_copy_substring(src, pos, len, port_str, sizeof(port_str));

        // Use strtol to parse port, preserve original value
        char* endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long < 0) {
          uri->port = -1;  // Invalid format
          uri->valid = 0;
        } else {
          uri->port = (int)port_long;  // Preserve original value (even if > 65535)
          // Don't set valid=0 here - let upper layer validate port range
        }

        pos += len;
        goto hier_part_path;
      }
    */

hier_part_path:
    /*!re2c
      EOF {
        uri->valid = 1;
        return 1;
      }
      * {goto query_frag;}
      ("/" SEGMENT)+ {
        int len = (int)(url_str - src - pos);
        uri_copy_substring(src, pos, len, uri->path, sizeof(uri->path));
        pos += len;
        goto query_frag;
      }
    */

query_frag:
    /*!re2c
      * { return 0; }
      EOF {
        uri->valid = 1;
        return 1;
      }
      "?" (PCHAR | [/?])* {
        pos++; // shift "?"
        int len = (int)( url_str - src - pos);
        uri_copy_substring(src, pos, len, uri->query, sizeof(uri->query));
        pos += len;
        goto query_frag;
      }
      "#" (PCHAR | [/?])* {
        pos++; // shift "#"
        int len =  (int)(url_str - src - pos);
        uri_copy_substring(src, pos, len, uri->fragment, sizeof(uri->fragment));
        uri->valid = 1;
        return 1;
      }
    */
}
