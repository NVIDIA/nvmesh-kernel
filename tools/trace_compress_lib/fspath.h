/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#pragma once
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <libgen.h>
#include <string.h>
#include <sys/stat.h>

struct fspath{
    char path[PATH_MAX];
    int error;
};

static inline struct fspath __attribute__((format (printf, 1, 2)))
fspath_create(const char* fmt, ...){
    struct fspath result = {0};
    int rc = 0;

	va_list args;
	va_start(args, fmt);
	rc = vsnprintf(result.path, sizeof(result.path)-1, fmt, args);
	va_end(args);
	if (rc <0 ){
        return (struct fspath){.error = ENAMETOOLONG};
    }
    return result;
}

static inline struct fspath fspath_dirname(struct fspath const* fspath){
    struct fspath tmp = *fspath;
    return fspath_create("%s", dirname(tmp.path));
}

static inline struct fspath fspath_basename(struct fspath const* fspath){
    struct fspath tmp = *fspath;
    return fspath_create("%s", basename(tmp.path));
}

//returns filename without the final extension
static inline struct fspath fspath_stem(struct fspath const* fspath){
    struct fspath dpath = fspath_dirname(fspath);
    struct fspath fname = fspath_basename(fspath);
    char const * dot = strrchr(fname.path, '.');
    if (dot){
        if (dot == fname.path) {
            return *fspath;
        } else {
            fname.path[dot-fname.path] = '\0';
            return fspath_create("%s/%s", dpath.path, fname.path);
        }
    } else {
        return *fspath;
    }
}

static inline int fspath_mkdir(struct fspath const* dpath, mode_t mode){
	if(mkdir(dpath->path, mode) == 0 || EEXIST == errno){
		return 0;
	}
	return errno;
}
