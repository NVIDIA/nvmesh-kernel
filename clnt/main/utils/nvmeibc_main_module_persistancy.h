#ifndef NVMEIBC_MAIN_MODULE_PERSISTANCY_H
#define NVMEIBC_MAIN_MODULE_PERSISTANCY_H

/********************** API for persistency storage ***************************/
/* As long as client is alive (no rmmod command), the driver provides
   persistance storage, for volumes (volume can save data on detach and load it
   on the next attach. All methods below identify user of storage by its unique
   name/uuid (simmilar to file path in FS). Each user can store 1 buffer of
   unlimited length (simmilar to file). Volume can have multiple users (files)*/

/* buf - kmalloc() of length 'len' is stored in the persistency. Do not kfree()
   it, because persistency does not copy your info. 0=success, negative=error */
int nvmeibc_volume_persistency_store(const char* user, const void*buf,
														const int len);
/* Delete the file if it exists. Buffer gets kfree(). */
int nvmeibc_volume_persistency_del(  const char* user);

/* Read the buffer. Sets your 'bug' to point to the content, returns length or
   negative value on eror*/
int nvmeibc_volume_persistency_fetch(const char* user, const void**buf);

#endif

