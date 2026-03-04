
enum gf_return_val nvmeib_raid_encode(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy);
enum gf_return_val nvmeib_raid_update(int len, int k, int vec_i, unsigned char **data, unsigned char **coding, u32 *crc, unsigned char *data_copy);
enum gf_return_val nvmeib_raid_decode(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc);
#ifdef __x86_64__
u32 nvmeib_raid_ec_crc(u32 init_crc, const u8 *buf, unsigned int len);
#elif KS_CRC32C_USES_SIZE_T
u32 nvmeib_raid_ec_crc(u32 init_crc, const void *buf, size_t len);
#else
u32 nvmeib_raid_ec_crc(u32 init_crc, const void *buf, unsigned int len);
#endif
bool nvmeib_raid_kernel_builtin_supported(void);