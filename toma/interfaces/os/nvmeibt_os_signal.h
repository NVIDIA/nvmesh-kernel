#ifndef NVMEIBT_OS_SIGNAL
#define NVMEIBT_OS_SIGNAL

int init_signal_handling(const char *exe_name);	// Returns signalfd descriptor

// Handle a signal received through the signalfd mechanism
void handle_sig_fd(int signals_fd, void (*sig_handler)(int32_t n, uint64_t addr));

int nvmeibt_nonblock_fd(int fd);	// Do not block when openning the file

#endif // #ifndef NVMEIBT_OS_SIGNAL

