#include "lexer.h"

int parse(char const* url_str, url_t* url)
{
    char const* marker;
    char const* ctxmarker;
    char const* src = url_str;
    int pos = 0;
    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:define:YYCURSOR = url_str;
      re2c:define:YYMARKER = marker;
      re2c:define:YYCTXMARKER = ctxmarker;
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
        int len = url_str - src - 1;
        copy_substring(src, pos, len, url->scheme, sizeof(url->scheme));
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
        int len = url_str - src - pos - 1; // unshift 1 char for "@"
        copy_substring(src, pos, len, url->userinfo, sizeof(url->userinfo));
        pos += len + 1; // shift for "@" char
        goto hier_part_host;
      }
    */

hier_part_host:
    /*!re2c
      EOF { return 0;}
      * { return 0; }
      "" / "/" {
        url->host_type = HOST_UNKNOWN;
        goto hier_part_path;
      }
      IPV4ADDR {
        int len = url_str - src - pos;
        url->host_type = HOST_IPV4ADDR;
        copy_substring(src, pos, len, url->host, sizeof(url->host));
        pos += len;
        goto hier_part_port;
      }
      "[v" [^\]]+ "]" {
        pos++; // shift "["
        int len = url_str - src - pos - 1;
        url->host_type = HOST_IPVFUTURE;
        copy_substring(src, pos, len, url->host, sizeof(url->host));
        pos += len + 1;
        goto hier_part_port;
      }
      "[" [^\]]+ "]" {
        pos++; // shift "["
        int len = url_str - src - pos - 1; // skip "]"
        url->host_type = HOST_IPV6ADDR;
        copy_substring(src, pos, len, url->host, sizeof(url->host));
        pos += len + 1;
        goto hier_part_port;
      }
      REGNAME+ {
        int len = url_str - src - pos;
        url->host_type = HOST_REGNAME;
        copy_substring(src, pos, len, url->host, sizeof(url->host));
        pos += len;
        goto hier_part_port;
      }
    */

hier_part_port:
    /*!re2c
      EOF {
        url->valid = 1;
        return 1;
      }
      "" { goto hier_part_path; }
      ":" DIGIT* {
        pos++; // shift ":"
        int len = url_str - src - pos;
        char port_str[16];
        copy_substring(src, pos, len, port_str, sizeof(port_str));
        url->port = atoi(port_str);
        pos += len;
        goto hier_part_path;
      }
    */

hier_part_path:
    /*!re2c
      EOF {
        url->valid = 1;
        return 1;
      }
      * {goto query_frag;}
      ("/" SEGMENT)+ {
        int len = url_str - src - pos;
        copy_substring(src, pos, len, url->path, sizeof(url->path));
        pos += len;
        goto query_frag;
      }
    */

query_frag:
    /*!re2c
      * { return 0; }
      EOF {
        url->valid = 1;
        return 1;
      }
      "?" (PCHAR | [/?])* {
        pos++; // shift "?"
        int len = url_str - src - pos;
        copy_substring(src, pos, len, url->query, sizeof(url->query));
        pos += len;
        goto query_frag;
      }
      "#" (PCHAR | [/?])* {
        pos++; // shift "#"
        int len = url_str - src - pos;
        copy_substring(src, pos, len, url->fragment, sizeof(url->fragment));
        return 1;
      }
    */
    return 0;
}
