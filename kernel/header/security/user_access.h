#ifndef USER_ACCESS_H
#define USER_ACCESS_H

#include <stdint.h>
#include <stddef.h>

int security_valid_user_range(const void* ptr, size_t size);
int security_valid_user_write_range(const void* ptr, size_t size);
int security_valid_user_string(const char* ptr, size_t max_len);
int security_valid_user_argv(const char* const* argv, size_t max_args,
							 size_t max_string_len);
int security_copy_from_user(void* dest, const void* src, size_t size);
int security_copy_to_user(void* dest, const void* src, size_t size);
int security_copy_user_string(char* dest, size_t dest_size,
							  const char* src, size_t max_len);

#endif