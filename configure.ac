#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([ptyterm], [0.9.0], [mako10k@mk10.org])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h sys/ioctl.h termios.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_CHECK_FUNCS([atexit dup2 memmove select])

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT

