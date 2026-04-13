#include "html_medium.h"
#include <wchar.h>

int html_medium_char_width(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return (w < 0) ? 0 : w;
}
