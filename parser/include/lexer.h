#pragma once

#include "uri.h"
#include <stdlib.h>
#include <string.h>

// re2c generated parser function - now uses stack-allocated structure
int parse(char const* url_str, url_t* url);
