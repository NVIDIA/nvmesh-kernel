/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#ifndef __PLATFORM_SERVICES_H__
#define __PLATFORM_SERVICES_H__

struct nvmeib_pet_base_controller;

//data path algorithms and implementation depends on services provided by the platform.
//We have 4 different platforms: linux kernel, user mode and simulators. 
//
//dp_platform_services is a container for those services interfaces

struct dp_platform_services{
    struct nvmeib_pet_base_controller* io_pet_controller;
};

static inline struct dp_platform_services 
dp_platform_services_make(struct nvmeib_pet_base_controller* io_pet_controller)
{
    return (struct dp_platform_services){
        .io_pet_controller = io_pet_controller
    };
} 

#endif//__PLATFORM_SERVICES_H__