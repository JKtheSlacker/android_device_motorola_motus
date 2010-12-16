/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "qemu.h"
#include "power_qemu.h"
#include <fcntl.h>
#include <errno.h>
#include <hardware_legacy/power.h>

static void
set_a_light(const char*  name, unsigned  brightness)
{
    qemu_control_command( "power:light:brightness:%s:%d",
                          name, brightness );
}

int
qemu_set_light_brightness(unsigned int mask, unsigned int brightness)
{
    if (mask & KEYBOARD_LIGHT) {
        set_a_light("keyboard_backlight", brightness);
    }
    if (mask & SCREEN_LIGHT) {
        set_a_light("lcd_backlight", brightness);
    }
    if (mask & BUTTON_LIGHT) {
        set_a_light("button_backlight", brightness);
    }
    return 0;
}

int
qemu_set_screen_state(int on)
{
    qemu_control_command( "power:screen_state:%s", on ? "wake" : "standby" );
    return 0;
}

