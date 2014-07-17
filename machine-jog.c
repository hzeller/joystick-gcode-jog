/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#include <fcntl.h>
#include <getopt.h>
#include <linux/joystick.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const int max_feedrate_mm_p_sec_xy = 120;
static const int max_feedrate_mm_p_sec_z = 1;  // Z is typically pretty slow

static const long interval_msec = 20;

static const long machine_interval_msec = 20;

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
    NUM_BUTTONS
};

struct Vector {
    float axis[NUM_AXIS];
};
struct ButtonState {
    char button[NUM_BUTTONS];
};

struct AxisConfig {
    int channel;
    int zero;
    int max_value;
};

struct Configuration {
    struct AxisConfig axis_config[NUM_AXIS];
    int button_channel[NUM_BUTTONS];
};

struct SavedPoints {
    struct Vector a;
    struct Vector b;
    struct Vector c;
    struct Vector d;
};

void DumpConfig(const char *filename, const struct Configuration *config) {
    FILE *out = fopen(filename, "w");
    if (out == NULL) return;
    for (int i = 0; i < NUM_AXIS; ++i) {
        fprintf(out, "A:%d %d %d\n",
                config->axis_config[i].channel,
                config->axis_config[i].zero,
                config->axis_config[i].max_value);
    }
    for (int i = 0; i < NUM_BUTTONS; ++i)
        fprintf(out, "B:%d\n", config->button_channel[i]);
    fclose(out);
}

// Return 1 on success.
int ReadConfig(const char *filename, struct Configuration *config) {
    FILE *in = fopen(filename, "r");
    for (int i = 0; i < NUM_AXIS; ++i) {
        if (3 != fscanf(in, "A:%d %d %d\n",
                        &config->axis_config[i].channel,
                        &config->axis_config[i].zero,
                        &config->axis_config[i].max_value))
            return 0;
    }
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        if (1 != fscanf(in, "B:%d\n", &config->button_channel[i]))
            return 0;
    }
    fclose(in);
    return 1;
}

// Wait for input to become ready for read or timeout reached.
// If the file-descriptor becomes readable, returns number of millis
// left. Returns 0 on timeout (i.e. nothing to be read).
// Returns -1
static int ReadWait(int fd, int timeout_millis) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = timeout_millis * 1000;

    FD_SET(fd, &read_fds);
    int s = select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (s <= 0)
        return s;
    return tv.tv_usec / 1000;
}

// Returns 0 on timeout, -1 on error and a positive number on event.
static int WaitForEvent(int fd, struct js_event *event, int timeout_ms) {
    int timeout_left = ReadWait(fd, timeout_ms);
    if (timeout_left > 0) {
        read(fd, event, sizeof(*event));
    }
    return timeout_left;
}

// Wait for a joystick button up to "timeout_ms" long. Returns negative
// value on timeout or the button-id when a button event happened.
// In case the axis position is changing, it updates "axis", but does not
// return before the timeout, as this does not require immediate attention.
static int WaitForJoystickButton(int fd, int timeout_ms,
                                 const struct Configuration *config,
                                 struct Vector *axis,
                                 struct ButtonState *button) {
    int timeout_left = timeout_ms;
    for (;;) {
        struct js_event e;
        timeout_left = WaitForEvent(fd, &e, timeout_left);
        if (timeout_left < 0) {
            perror("Trouble reading joystick. Nastily exiting.");
            exit(1);
        }
        if (timeout_left == 0) {
            return -1;
        }

        if (e.type == JS_EVENT_AXIS) {
            for (int a = 0; a < NUM_AXIS; ++a) {
                if (config->axis_config[a].channel == e.number) {
                    int normalized = e.value - config->axis_config[a].zero;
                    axis->axis[a] = (normalized *
                                     1.0 / config->axis_config[a].max_value);
                }
            }
        } else if (e.type == JS_EVENT_BUTTON) {
            for (int b = 0; b < NUM_BUTTONS; ++b) {
                if (config->button_channel[b] == e.number) {
                    button->button[b] = e.value; 
                    return b;
                }
            }
        }
    }
}

static int64_t GetMillis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static void DiscardAllInput(int fd, int timeout, char do_echo) {
    char c;
    while (ReadWait(fd, timeout) > 0) {
        read(fd, &c, 1);
        if (do_echo) write(STDERR_FILENO, &c, 1);  // echo back.
    }
}

// Ok comes on a single line.
static void WaitForOk(FILE *in) {
    char done = 0;
    while (!done) {
        char *buffer = NULL;
        size_t len;
        if (getline(&buffer, &len, in) < 0)
            break;
        if (strcmp(buffer, "ok\n") == 0) {
            done = 1;
        } else {
            fprintf(stderr, "SKIP %s", buffer);
        }
        free(buffer);
    }
}

static void FindLargestAxis(int js_fd, struct AxisConfig *axis_config) {
    struct js_event e;
    for (;;) {
        if (WaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_AXIS
            && abs(e.value) > 32000) {
            axis_config->channel = e.number;
            axis_config->max_value = (e.value < 0 ? -1 : 1) * 32767;
            return;
        }
    }
}

static void WaitForReleaseAxis(int js_fd, int channel, int *zero) {
    int zero_value = 1 << 17;
    struct js_event e;
    for (;;) {
        if (WaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_AXIS && e.number == channel
            && abs(e.value) < 5000) {
            zero_value = e.value;
            break;
        }
    }
    // Now, we read the values while they come in, assuming the last one
    // is the 'zero' position.
    const int64_t end_time = GetMillis() + 500;
    while (GetMillis() < end_time) {
        if (WaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_AXIS && e.number == channel) {
            zero_value = e.value;
        }
    }
    *zero = zero_value;
}

static void WaitAnyButtonPress(int js_fd, int *button_channel) {
    struct js_event e;
    for (;;) {
        if (WaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_BUTTON && e.value > 0) {
            *button_channel = e.number;
            return;
        }
    }
}

static void WaitForButtonRelease(int js_fd, int channel) {
    struct js_event e;
    for (;;) {
        if (WaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_BUTTON && e.number == channel && e.value == 0)
            return;
    }
}

static void GetAxisConfig(int js_fd,
                          const char *msg, struct AxisConfig *axis_config) {
    fprintf(stderr, "%s", msg); fflush(stderr);
    FindLargestAxis(js_fd, axis_config);
    fprintf(stderr, "Thanks. Nove move to center.\n");
    WaitForReleaseAxis(js_fd, axis_config->channel, &axis_config->zero);
}

static void GetButtonConfig(int js_fd, const char *msg, int *channel) {
    fprintf(stderr, "%s", msg); fflush(stderr);
    WaitAnyButtonPress(js_fd, channel);
    fprintf(stderr, "Thanks. Now release."); fflush(stderr);
    WaitForButtonRelease(js_fd, *channel);
    fprintf(stderr, "\n");
}

// Create configuration
static int CreateConfig(int js_fd, struct Configuration *config) {
    GetAxisConfig(js_fd, "Move X all the way to the right ->  ",
                  &config->axis_config[AXIS_X]);
    GetAxisConfig(js_fd, "Move Y all the way up            ^  ",
                  &config->axis_config[AXIS_Y]);
    GetAxisConfig(js_fd, "Move Z all the way up            ^  ",
                  &config->axis_config[AXIS_Z]);
    GetButtonConfig(js_fd,
                    "Press HOME button.", &config->button_channel[BUTTON_HOME]);
    GetButtonConfig(js_fd, "Press Memory A button.",
                    &config->button_channel[BUTTON_STORE_A]);
    GetButtonConfig(js_fd, "Press Memory B button.",
                    &config->button_channel[BUTTON_STORE_B]);
    GetButtonConfig(js_fd, "Press Memory C button.",
                    &config->button_channel[BUTTON_STORE_C]);
    GetButtonConfig(js_fd, "Press Memory D button.",
                    &config->button_channel[BUTTON_STORE_D]);
    return 1;
}

// Read coordinates from printer.
static int GetCoordinates(struct Vector *pos) {
    DiscardAllInput(STDIN_FILENO, 10, 0);

    printf("M114\n"); fflush(stdout);  // read coordinates.
    fprintf(stderr, "Reading initial absolute position\n");
    if (fscanf(stdin, "X:%fY:%fZ:%f", &pos->axis[AXIS_X], &pos->axis[AXIS_Y],
               &pos->axis[AXIS_Z]) == 3) {
        WaitForOk(stdin);
        fprintf(stderr, "Got (x/y/z) = (%.3f/%.3f/%.3f)\n",
                pos->axis[AXIS_X], pos->axis[AXIS_Y],
                pos->axis[AXIS_Z]);
        return 1;
    } else {
        fprintf(stderr, "Didn't get readable coordinates.\n");
        return 0;
    }
}

void GCodeGoto(struct Vector *pos, float feedrate_mm_sec) {
    printf("G1 X%.3f Y%.3f Z%.3f F%.3f\n", pos->axis[AXIS_X], pos->axis[AXIS_Y],
           pos->axis[AXIS_Z], feedrate_mm_sec * 60);
    fflush(stdout);
}

// Returns 1 if any gcode has been output or 0 if there was no need.
int OutputJogGCode(struct Vector *pos, const struct Vector *speed) {
    // We get the timeout in regular intervals.
    const float euklid = sqrtf(speed->axis[AXIS_X] * speed->axis[AXIS_X]
                                + speed->axis[AXIS_Y] * speed->axis[AXIS_Y]
                                + speed->axis[AXIS_Z] * speed->axis[AXIS_Z]);

    const float feedrate = euklid * ((fabs(speed->axis[AXIS_Z]) > 0.01)
                                    ? max_feedrate_mm_p_sec_z
                                    : max_feedrate_mm_p_sec_xy);
    if (fabs(feedrate) < 0.1)
        return 0;

    float interval = machine_interval_msec / 1000.0;
    for (int a = AXIS_X; a < NUM_AXIS; ++a) {
        pos->axis[a] = pos->axis[a] + speed->axis[a] * feedrate * interval;
        if (pos->axis[a] < 0) pos->axis[a] = 0;
    }
    GCodeGoto(pos, feedrate);
    fprintf(stderr, "Goto (x/y/z) = (%.3f/%.3f/%.3f) "
            "(js:%.1f/%.1f/%.1f) F=%.3f mm/s\n",
            pos->axis[AXIS_X], pos->axis[AXIS_Y], pos->axis[AXIS_Z],
            speed->axis[AXIS_X], speed->axis[AXIS_Y],
            speed->axis[AXIS_Z], feedrate);
    return 1;
}

void HandlePlaceMemory(enum Button button, char is_pressed,
                       struct Vector *storage,
                       int *accumulated_timeout,
                       struct Vector *machine_pos) {
    if (is_pressed) {
        *accumulated_timeout = 0;
    } else {  // we act on release
        if (*accumulated_timeout > 500) {
            memcpy(storage, machine_pos, sizeof(*storage)); // save
        } else {
            memcpy(machine_pos, storage, sizeof(*storage)); // restore
            GCodeGoto(machine_pos, max_feedrate_mm_p_sec_xy);
        }
    }
}

int JogMachine(int js_fd, char do_homing, const struct Configuration *config) {
    struct Vector speed_vector;
    struct Vector machine_pos;
    struct ButtonState buttons;
    struct SavedPoints saved;
    memset(&speed_vector, 0, sizeof(speed_vector));
    memset(&machine_pos, 0, sizeof(machine_pos));
    memset(&buttons, 0, sizeof(buttons));
    memset(&saved, 0, sizeof(saved));

    // Skip initial stuff coming from the machine. We need to have
    // a defined starting way to read the absolute coordinates.
    // Wait until board is initialized. Some Marlin versions dump some
    // stuff out there which we want to ignore.
    fprintf(stderr, "Init "); fflush(stderr);
    DiscardAllInput(STDIN_FILENO, 5000, 1);
    fprintf(stderr, "done.\n");

    printf("G21\n"); fflush(stdout); WaitForOk(stdin);  // Switch to metric.

    char is_homed = 0;

    if (do_homing) {
        // Unfortunately, connecting to some Marlin instances resets it.
        // So home that we are in a defined state.
        printf("G28\n"); fflush(stdout); WaitForOk(stdin);   // Home.
        is_homed = 1;
    }

    // Relative mode (G91) seems to be pretty badly implemented and does not
    // deal with very small increments (which are rounded away).
    // So let's be absolute and keep track of the current position ourself.
    printf("G90\n"); fflush(stdout); WaitForOk(stdin);  // Absolute coordinates.

    if (!GetCoordinates(&machine_pos))
        return 1;

    int accumulated_timeout;

    for (;;) {
        switch (WaitForJoystickButton(js_fd, interval_msec,
                                      config, &speed_vector, &buttons)) {
        case -1:  // timeout, i.e. our regular update interval.
            accumulated_timeout += interval_msec;
            if (OutputJogGCode(&machine_pos, &speed_vector)) {
                // We did emit some gcode. Now we're not homed anymore
                is_homed = 0;
            } else {
                // Some quiet phase: discard all the 'ok's
                DiscardAllInput(STDIN_FILENO, 10, 0);
            }
            break;

        case BUTTON_STORE_A:
            HandlePlaceMemory(BUTTON_STORE_A, buttons.button[BUTTON_STORE_A],
                              &saved.a, &accumulated_timeout, &machine_pos);
            break;

        case BUTTON_STORE_B:
            HandlePlaceMemory(BUTTON_STORE_B, buttons.button[BUTTON_STORE_B],
                              &saved.b, &accumulated_timeout, &machine_pos);
            break;

        case BUTTON_STORE_C:
            HandlePlaceMemory(BUTTON_STORE_C, buttons.button[BUTTON_STORE_C],
                              &saved.c, &accumulated_timeout, &machine_pos);
            break;

        case BUTTON_STORE_D:
            HandlePlaceMemory(BUTTON_STORE_D, buttons.button[BUTTON_STORE_D],
                              &saved.d, &accumulated_timeout, &machine_pos);
            break;

        case BUTTON_HOME:  // only home if not already.
            if (buttons.button[BUTTON_HOME] && !is_homed) {
                is_homed = 1;
                DiscardAllInput(STDIN_FILENO, 10, 0);
                printf("G28\n"); fflush(stdout); WaitForOk(stdin);
                if (!GetCoordinates(&machine_pos))
                    return 1;
            }
            break;
        }
    }
}

static int usage(const char *progname) {
    fprintf(stderr, "Usage: %s <options>\n"
            "  -C <config>  : Create a configuration file for Joystick\n"
            "  -j <config>  : Jog machine using config\n",
            progname);
    return 1;
}

int main(int argc, char **argv) {
    char do_homing = 0;
    struct Configuration config;
    memset(&config, 0, sizeof(config));

    const int js_fd = open("/dev/input/js0", O_RDONLY);
    if (js_fd < 0) {
        perror("Opening joystick");
        return 1;
    }

    enum Operation {
        DO_NOTHING,
        DO_CREATE_CONFIG,
        DO_JOG
    } op = DO_NOTHING;
    const char *filename = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "C:j:")) != -1) {
        switch (opt) {
        case 'C':
            op = DO_CREATE_CONFIG;
            filename = strdup(optarg);
            break;

        case 'h':
            do_homing = 1;
            break;

        case 'j':
            op = DO_JOG;
            filename = strdup(optarg);
            break;

        default: /* '?' */
            return usage(argv[0]);
        }
    }

    switch (op) {
    case DO_CREATE_CONFIG:
        CreateConfig(js_fd, &config);
        DumpConfig(filename, &config);
        break;
        
    case DO_JOG:
        if (ReadConfig(filename, &config) == 0) {
            fprintf(stderr, "Problem reading config file\n");
            return 1;
        }
        JogMachine(js_fd, do_homing, &config);
        break;

    case DO_NOTHING:
        return usage(argv[0]);
    }
    return 0;
}
