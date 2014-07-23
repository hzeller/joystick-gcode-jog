/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#include <ctype.h>
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

#include "machine-jog.h"
#include "joystick-config.h"

static const int kMaxFeedrate_xy = 120;
static const int kMaxFeedrate_z = 10;  // Z is typically pretty slow

// Some global state.
static int max_feedrate_mm_p_sec_xy;
static int max_feedrate_mm_p_sec_z;

static const long interval_msec = 20;   // update interval between reads.
static const long machine_interval_msec = 20;

// Flags.
static int simulate_machine = 0;
static int quiet = 0;   // quiet - don't print random stuff to screen

static const char *persistent_store = NULL;  // filename to store memory points.

// Streams to connect machine to.
static FILE *gcode_out = NULL;          // we write with fprintf() etc.
static int gcode_in_fd = STDIN_FILENO;  // .. and read directly from file desc.

struct Vector {
    float axis[NUM_AXIS];
};
struct ButtonState {
    char button[NUM_BUTTONS];
};

struct SavedPoints {
    struct Vector storage[NUM_BUTTONS];
};

static int quantize(int value, int q) {
    return value / q * q;
}

void WriteSavedPoints(const char *filename, struct SavedPoints *values) {
    if (filename == NULL) return;
    FILE *out = fopen(filename, "w");  // Overwriting for now. TODO: tmp file.
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        if (i == BUTTON_HOME) continue;
        fprintf(out, "%.2f %.2f %.2f\n",
                values->storage[i].axis[AXIS_X],
                values->storage[i].axis[AXIS_Y],
                values->storage[i].axis[AXIS_Z]);
    }
    fclose(out);
}

void ReadSavedPoints(const char *filename, struct SavedPoints *values) {
    if (filename == NULL) return;
    FILE *in = fopen(filename, "r");
    if (in == NULL) return;
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        if (i == BUTTON_HOME) continue;
        if (3 != fscanf(in, "%f %f %f\n",
                        &values->storage[i].axis[AXIS_X],
                        &values->storage[i].axis[AXIS_Y],
                        &values->storage[i].axis[AXIS_Z]))
            break;
    }
    fclose(in);
}

// Wait for input to become ready for read or timeout reached.
// If the file-descriptor becomes readable, returns number of milli-seconds
// left.
// Returns 0 on timeout (i.e. no millis left and nothing to be read).
// Returns -1 on error.
static int AwaitReadReady(int fd, int timeout_millis) {
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

static int ReadLine(int fd, char *result, int len, char do_echo) {
    int bytes_read = 0;
    char c = 0;
    while (c != '\n' && c != '\r' && bytes_read < len) {
        if (read(fd, &c, 1) < 0)
            return -1;
        ++bytes_read;
        *result++ = c;
        if (do_echo && !quiet) write(STDERR_FILENO, &c, 1);  // echo back.
    }
    *result = '\0';
    return bytes_read;
}

// Returns 0 on timeout, -1 on error and a positive number on event.
int JoystickWaitForEvent(int fd, struct js_event *event, int timeout_ms) {
    const int timeout_left = AwaitReadReady(fd, timeout_ms);
    if (timeout_left > 0) {
        read(fd, event, sizeof(*event));
    }
    return timeout_left;
}

static void JoystickInitialState(int js_fd, struct Configuration *config) {
    struct js_event e;
    // The initial state is sent on connect.
    while (JoystickWaitForEvent(js_fd, &e, 50) > 0) {
        if ((e.type & JS_EVENT_INIT) == 0)
            break;  // done init events.
        if ((e.type & JS_EVENT_AXIS) != 0) {
            // read zero position.
            for (int a = 0; a < NUM_AXIS; ++a) {
                if (config->axis_config[a].channel == e.number) {
                    config->axis_config[a].zero = e.value;
                    if (!quiet) fprintf(stderr, "Zero axis %d : %d\n",
                                        a, e.value);
                }
            }
        }
    }
}

// Wait for a joystick button up to "timeout_ms" long. Returns negative
// value on timeout or the button-id when a button event happened.
// In case the axis position is changing, it updates "axis", but does not
// return before the timeout, as this does not require immediate attention.
static int JoystickWaitForButton(int fd, int timeout_ms,
                                 const struct Configuration *config,
                                 struct Vector *axis,
                                 struct ButtonState *button) {
    int timeout_left = timeout_ms;
    for (;;) {
        struct js_event e;
        timeout_left = JoystickWaitForEvent(fd, &e, timeout_left);
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
                    int quant = abs(config->axis_config[a].max_value / 16);
                    axis->axis[a] = (quantize(normalized, quant) *
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

static void DiscardAllInput(int timeout) {
    if (simulate_machine) return;
    char c;
    while (AwaitReadReady(gcode_in_fd, timeout) > 0) {
        read(gcode_in_fd, &c, 1);
        if (!quiet) write(STDERR_FILENO, &c, 1);  // echo back.
    }
}

// Ok comes on a single line.
static void WaitForOk() {
    if (simulate_machine) return;
    const int fd = gcode_in_fd;
    char buffer[512];
    char done = 0;
    while (!done) {
        if (ReadLine(fd, buffer, sizeof(buffer), 0) < 0)
            break;
        if (strncmp(buffer, "ok", 2) == 0) {
            done = 1;
        }
    }
}


// Read coordinates from printer.
static int GetCoordinates(struct Vector *pos) {
    if (simulate_machine) return 1;
    DiscardAllInput(100);

    fprintf(gcode_out, "M114\n"); // read coordinates.
    if (!quiet) fprintf(stderr, "Reading initial absolute position\n");
    char buffer[512];
    ReadLine(gcode_in_fd, buffer, sizeof(buffer), 1);
    if (sscanf(buffer, "X:%f Y:%f Z:%f", &pos->axis[AXIS_X], &pos->axis[AXIS_Y],
               &pos->axis[AXIS_Z]) == 3) {
        WaitForOk();
        if (!quiet) fprintf(stderr, "Got (x/y/z) = (%.3f/%.3f/%.3f)\n",
                            pos->axis[AXIS_X], pos->axis[AXIS_Y],
                            pos->axis[AXIS_Z]);
        return 1;
    } else {
        fprintf(stderr, "Didn't get readable coordinates.\n");
        return 0;
    }
}

void GCodeGoto(struct Vector *pos, float feedrate_mm_sec) {
    if (simulate_machine) return;
    fprintf(gcode_out, "G1 X%.3f Y%.3f Z%.3f F%.3f\n",
            pos->axis[AXIS_X], pos->axis[AXIS_Y], pos->axis[AXIS_Z],
            feedrate_mm_sec * 60);
    WaitForOk();
}

// Returns 1 if any gcode has been output or 0 if there was no need.
int OutputJogGCode(struct Vector *pos, const struct Vector *speed,
                   const struct Vector *limit) {
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
        if (pos->axis[a] > limit->axis[a]) pos->axis[a] = limit->axis[a];
    }
    GCodeGoto(pos, feedrate);
    if (!quiet) fprintf(stderr, "Goto (x/y/z) = (%.2f/%.2f/%.2f)      \r",
                        pos->axis[AXIS_X], pos->axis[AXIS_Y], pos->axis[AXIS_Z]);
    return 1;
}

void HandlePlaceMemory(enum Button button, char is_pressed,
                       struct SavedPoints *saved,
                       int *accumulated_timeout,
                       struct Vector *machine_pos) {
    const char button_letter = 'A' + (button - BUTTON_STORE_A);
    struct Vector *storage = &saved->storage[button];
    if (is_pressed) {
        *accumulated_timeout = 0;
    } else {  // we act on release
        if (*accumulated_timeout >= 500) {
            memcpy(storage, machine_pos, sizeof(*storage)); // save
            WriteSavedPoints(persistent_store, saved);
            if (!quiet) fprintf(stderr, "\nStored in %c (%.2f, %.2f, %.2f)\n",
                                button_letter,
                                machine_pos->axis[AXIS_X],
                                machine_pos->axis[AXIS_Y],
                                machine_pos->axis[AXIS_Z]);
        } else {
            if (storage->axis[AXIS_X] >= 0) {
                memcpy(machine_pos, storage, sizeof(*storage)); // restore
                if (!quiet) fprintf(stderr,
                                    "\nGoto position %c -> (%.2f, %.2f, %.2f)\n",
                                    button_letter,
                                    machine_pos->axis[AXIS_X],
                                    machine_pos->axis[AXIS_Y],
                                    machine_pos->axis[AXIS_Z]);
                GCodeGoto(machine_pos, max_feedrate_mm_p_sec_xy);
            } else {
                if (!quiet) fprintf(stderr,
                                    "\nButton %c undefined\n", button_letter);
            }
        }
        *accumulated_timeout = -1;
    }
}

int JogMachine(int js_fd, char do_homing, const struct Vector *machine_limit,
               const struct Configuration *config) {
    struct Vector speed_vector;
    struct Vector machine_pos;
    struct ButtonState buttons;
    struct SavedPoints saved;
    memset(&speed_vector, 0, sizeof(speed_vector));
    memset(&machine_pos, 0, sizeof(machine_pos));
    memset(&buttons, 0, sizeof(buttons));
    memset(&saved, 0, sizeof(saved));
    for (int i = 0; i < NUM_BUTTONS; ++i)
        saved.storage[i].axis[AXIS_X] = -1;
    ReadSavedPoints(persistent_store, &saved);

    // Skip initial stuff coming from the machine. We need to have
    // a defined starting way to read the absolute coordinates.
    // Wait until board is initialized. Some Marlin versions dump some
    // stuff out there which we want to ignore.
    if (!quiet) fprintf(stderr, "Init "); fflush(stderr);
    DiscardAllInput(5000);
    if (!quiet) fprintf(stderr, "done.\n");

    fprintf(gcode_out, "G21\n"); WaitForOk();  // Switch to metric.

    char is_homed = 0;

    if (do_homing) {
        // Unfortunately, connecting to some Marlin instances resets it.
        // So home that we are in a defined state.
        fprintf(gcode_out, "G28\n"); WaitForOk();   // Home.
        is_homed = 1;
    }

    // Relative mode (G91) seems to be pretty badly implemented and does not
    // deal with very small increments (which are rounded away).
    // So let's be absolute and keep track of the current position ourself.
    fprintf(gcode_out, "G90\n"); WaitForOk();  // Absolute coordinates.

    if (!GetCoordinates(&machine_pos))
        return 1;

    int accumulated_timeout = -1;

    for (;;) {
        int button_ev = JoystickWaitForButton(js_fd, interval_msec,
                                              config, &speed_vector, &buttons);
        switch (button_ev) {
        case -1:  // timeout, i.e. our regular update interval.
            if (accumulated_timeout >= 0) {
                if (accumulated_timeout < 500
                    && accumulated_timeout + interval_msec >= 500) {
                    // If the user releases the button now, things will be
                    // stored. Let them know.
                    if (!quiet) fprintf(stderr, "\nStore...");
                }
                accumulated_timeout += interval_msec;
            }
            if (OutputJogGCode(&machine_pos, &speed_vector, machine_limit)) {
                // We did emit some gcode. Now we're not homed anymore
                is_homed = 0;
            }
            break;

        case BUTTON_HOME:  // only home if not already.
            if (buttons.button[BUTTON_HOME] && !is_homed) {
                is_homed = 1;
                fprintf(gcode_out, "G28\n"); WaitForOk();
                if (!GetCoordinates(&machine_pos))
                    return 1;
            }
            break;

        default:
            HandlePlaceMemory(button_ev, buttons.button[button_ev],
                              &saved, &accumulated_timeout, &machine_pos);
            break;
        }
    }
}

static int usage(const char *progname) {
    fprintf(stderr, "Usage: %s <options>\n"
            "  -C <config-dir>  : Create a configuration file for Joystick\n"
            "  -j <config-dir>  : Jog machine using config\n"
            "  -h           : Home on startup\n"
            "  -p <persist-file> : persist saved points in given file\n"
            "  -L <x,y,z>   : Machine limits in mm\n"
            "  -x <speed>   : feedrate for xy in mm/s\n"
            "  -z <speed>   : feedrate for z in mm/s\n"
            "  -s           : machine not connected; simulate.\n"
            "  -q           : Quiet. No chatter on stderr.\n",
            progname);
    return 1;
}

int main(int argc, char **argv) {
    max_feedrate_mm_p_sec_xy = kMaxFeedrate_xy;
    max_feedrate_mm_p_sec_z  = kMaxFeedrate_z;
    char do_homing = 0;
    struct Configuration config;
    memset(&config, 0, sizeof(config));
    struct Vector machine_limits;
    machine_limits.axis[AXIS_X]
        = machine_limits.axis[AXIS_Y]
        = machine_limits.axis[AXIS_Z] = 305;

    // The Joystick. The first time we open it, the zero values are not
    // yet properly established. So let's close the first instance right awawy
    // and use the next open :)
    close(open("/dev/input/js0", O_RDONLY));
    const int js_fd = open("/dev/input/js0", O_RDONLY);
    if (js_fd < 0) {
        perror("Opening joystick");
        return 1;
    }

    char joystick_name[512];
    if (ioctl(js_fd, JSIOCGNAME(sizeof(joystick_name)), joystick_name) < 0)
        strncpy(joystick_name, "unknown-joystick", sizeof(joystick_name));
    // Make a filename-friendly name out of it.
    for (char *x = joystick_name; *x; ++x) {
        if (isspace(*x)) *x = '-';
    }
    printf("Joystick-Name: %s\n", joystick_name);

    enum Operation {
        DO_NOTHING,
        DO_CREATE_CONFIG,
        DO_JOG
    } op = DO_NOTHING;
    const char *config_dir = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "C:j:x:z:L:hsp:q:")) != -1) {
        switch (opt) {
        case 'C':
            op = DO_CREATE_CONFIG;
            config_dir = strdup(optarg);
            break;

        case 'h':
            do_homing = 1;
            break;

        case 's':
            simulate_machine = 1;
            break;

        case 'q':
            quiet = 1;
            break;

        case 'p':
            persistent_store = strdup(optarg);
            break;

        case 'x':
            max_feedrate_mm_p_sec_xy = atoi(optarg);
            if (max_feedrate_mm_p_sec_xy <= 1) {
                fprintf(stderr, "Peculiar value -x %d",
                        max_feedrate_mm_p_sec_xy);
                return usage(argv[0]);
            }
            break;

        case 'L':
            if (3 != sscanf(optarg, "%f,%f,%f",
                            &machine_limits.axis[AXIS_X],
                            &machine_limits.axis[AXIS_Z],
                            &machine_limits.axis[AXIS_Z]))
                return usage(argv[0]);
            break;

        case 'z':
            max_feedrate_mm_p_sec_z = atoi(optarg);
            if (max_feedrate_mm_p_sec_z <= 1) {
                fprintf(stderr, "Peculiar value -z %d",
                        max_feedrate_mm_p_sec_z);
                return usage(argv[0]);
            }
            break;

        case 'j':
            op = DO_JOG;
            config_dir = strdup(optarg);
            break;

        default: /* '?' */
            return usage(argv[0]);
        }
    }

    // Connection to the machine reading gcode. TODO: maybe provide
    // listening on a socket ?
    gcode_out = stdout;
    gcode_in_fd = STDIN_FILENO;
    setvbuf(gcode_out, NULL, _IONBF, 0);  // Don't buffer.

    switch (op) {
    case DO_CREATE_CONFIG:
        CreateConfig(js_fd, &config);
        WriteConfig(config_dir, joystick_name, &config);
        break;
        
    case DO_JOG:
        if (ReadConfig(config_dir, joystick_name, &config) == 0) {
            fprintf(stderr, "Problem reading joystick config file.\n"
                    "Create a fresh one with\n\t%s -C %s\n", argv[0], config_dir);
            return 1;
        }
        JoystickInitialState(js_fd, &config);
        JogMachine(js_fd, do_homing, &machine_limits, &config);
        break;

    case DO_NOTHING:
        return usage(argv[0]);
    }
    return 0;
}
