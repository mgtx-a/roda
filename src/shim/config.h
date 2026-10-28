/* libxls expects autoconf's config.h. Roda builds it without iconv: its UTF-16 text is turned into
   UTF-8 by xls_locale.c (Windows' own converter). */
#define PACKAGE_VERSION "1.6.3"
