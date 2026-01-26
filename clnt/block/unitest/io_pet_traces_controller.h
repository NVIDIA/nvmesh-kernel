#pragma once

struct nvmeib_pet_base_controller;
struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void);

void sim_io_pet_controller_rotate(char const* test_id);

