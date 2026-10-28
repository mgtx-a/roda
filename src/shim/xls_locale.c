/* libxls's locale helpers (its locale.c), done with Windows' own UTF-16 to UTF-8 conversion instead of
   the C runtime's ".65001" locale, which older C runtimes don't have. Roda only ever asks for UTF-8. */
#include <windows.h>
#include <stdlib.h>
#include "../../third_party/libxls-1.6.3/include/libxls/locale.h"

xls_locale_t xls_createlocale(void) { return (xls_locale_t)1; }   /* never dereferenced */
void xls_freelocale(xls_locale_t locale) { (void)locale; }

size_t xls_wcstombs_l(char *restrict s, const wchar_t *restrict pwcs, size_t n, xls_locale_t loc)
{
    (void)loc;
    int len = (int)wcslen(pwcs);
    if (!s) return (size_t)WideCharToMultiByte(CP_UTF8, 0, pwcs, len, NULL, 0, NULL, NULL);
    int put = WideCharToMultiByte(CP_UTF8, 0, pwcs, len, s, (int)(n > 0x7fffffff ? 0x7fffffff : n), NULL, NULL);
    return put > 0 ? (size_t)put : (size_t)-1;
}
