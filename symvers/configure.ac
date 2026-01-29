AC_INIT(symvers, symvers-0.1)
AC_CONFIG_HEADERS([symvers_config.h])

AC_MSG_NOTICE([symvers kernel compability checks])

AC_ARG_WITH([kern-ver],
    [AS_HELP_STRING([--with-kern-ver=VERSION], [Specify the kernel version to use, or use the current running kernel version if not specified])],
    [kern_ver=$withval],
    [kern_ver=`uname -r`])

LINUX_SRC="/lib/modules/$kern_ver/build"

AC_CACHE_CHECK([for crc type in find_symbol()], [sv_cv_crctype], [
                AS_IF(
                      [ fgrep -A3 "const struct kernel_symbol *find_symbol" $LINUX_SRC/include/linux/module.h | \
                       fgrep -q "const s32 **crc"],
                      [ CONFIG_CRCTYPE="const s32" ],
                      [ CONFIG_CRCTYPE="const unsigned long" ])
                AC_MSG_RESULT($CONFIG_CRCTYPE)
                ])

AC_DEFINE_UNQUOTED(CRCTYPE, [$CONFIG_CRCTYPE], [argument type to find_symbol()])

AC_OUTPUT
