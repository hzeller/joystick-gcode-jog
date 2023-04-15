/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#include "rumble.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int rumble_device_fd = -1;
static int rumble_effect_id = -1;

// Find input event for given joystick ID. Returns 1 on success.
static int FindInputEvent(int for_js, char *event_path, size_t len) {
    // Finding the associated input event requires to go through some
    // hoops.
    char sys_path[512];
    snprintf(sys_path, sizeof(sys_path), "/sys/class/input/js%d/device/",
             for_js);

    int ev_id = -1;
    struct dirent *entry;
    DIR *const dir = opendir(sys_path);
    while (dir && (entry = readdir(dir)) != NULL) {
        if (sscanf(entry->d_name, "event%d", &ev_id) == 1) break;
    }
    if (dir) closedir(dir);
    if (ev_id < 0) return 0;
    snprintf(event_path, len, "/dev/input/event%d", ev_id);
    return 1;
}

void JoystickRumbleInit(int joystick_id) {
    char event_path[512];
    if (!FindInputEvent(joystick_id, event_path, sizeof(event_path))) {
        fprintf(stderr, "No rumble available.");
        return;
    }
    rumble_device_fd = open(event_path, O_RDWR);
    if (rumble_device_fd < 0) {
        perror("Opening rumble");
        return;
    }

    struct ff_effect rumble_effect;
    memset(&rumble_effect, 0, sizeof(rumble_effect));
    rumble_effect.type = FF_RUMBLE;
    rumble_effect.id = -1;
    rumble_effect.direction = 0;
    rumble_effect.trigger.button = 42;  // not triggered by button.
    rumble_effect.u.rumble.strong_magnitude = 0xffff;
    rumble_effect.u.rumble.weak_magnitude = 0xffff;
    if (ioctl(rumble_device_fd, EVIOCSFF, &rumble_effect) < 0 ||
        rumble_effect.id < 0) {
        perror("Can't register rumble effect");
        close(rumble_device_fd);
        rumble_device_fd = -1;
        return;
    }
    rumble_effect_id = rumble_effect.id;
}

void JoystickRumble(int ms) {
    if (rumble_device_fd < 0) return;
    struct input_event play;
    play.type = EV_FF;
    play.code = rumble_effect_id;
    play.value = 1; /* on */
    if (write(rumble_device_fd, &play, sizeof(play)) < 0) {
        perror("rumble on");
    }
    struct timespec tv;
    tv.tv_sec = 0;
    tv.tv_nsec = ms * 1000000L;
    nanosleep(&tv, NULL);
    play.value = 0; /* off */
    if (write(rumble_device_fd, &play, sizeof(play)) < 0) {
        perror("rumble off");
    }
}
