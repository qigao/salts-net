#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int compute_reading_time(const char *content)
{
    int words = 0;
    const char *p = content;
    while (*p)
    {
        if ((p == content || isspace((unsigned char)*(p - 1))) &&
            !isspace((unsigned char)*p))
        {
            words++;
        }
        p++;
    }
    return (words + 199) / 200;
}
