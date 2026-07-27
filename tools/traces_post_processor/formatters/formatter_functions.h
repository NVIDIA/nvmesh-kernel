#ifndef FORAMATTER_FUNCTION
#define FORAMATTER_FUNCTION

/**
 * Generic prototype of any formatter function.
 * Any formatter function exported by foramtter library must implement this
 * prototype.
 * A formatter function shall process input data and write its output into
 * dst_buf as a NULL terminated string.
 * @param buf Destination buffer function shall ouput to
 * @param len Destination buffer length
 * @param arg Input data, in whatever format expected by the implementation - pointer, bmp etc.
 * @return Upon success positive number of characters written, excluding null terminator
 *         Upon failure negative value
 */
typedef int (*formatter_function)(char *buf, int len, long arg, int datalen);

#endif /*FORAMATTER_FUNCTION*/