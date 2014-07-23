/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#ifndef MACHINE_JOG_H
#define MACHINE_JOG_H

// Axes we are interested in.
enum Axis {
    AXIS_X,
    AXIS_Y,
    AXIS_Z,
    NUM_AXIS
};

// Buttons we are interested in.
enum Button {
    BUTTON_HOME,
    BUTTON_STORE_A,
    BUTTON_STORE_B,
    BUTTON_STORE_C,
    BUTTON_STORE_D,
    BUTTON_STORE_E,
    BUTTON_STORE_F,
    NUM_BUTTONS
};

struct js_event;
int JoystickWaitForEvent(int fd, struct js_event *event, int timeout_ms);

#endif // MACHINE_JOG_H
