/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#include "machine-jog.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/joystick.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "joystick-config.h"
#include "rumble.h"

// Prusa uses 'W' to indicate that we don't want bed-levelling on G28.
// If this results in a problem with other machines, remove the W0
// Also, on GRBL machines, this would be "$H\n"
#define HOMING_COMMAND "G28 W0\n"

static const int kMaxFeedrate_xy = 120;
static const int kMaxFeedrate_z = 10;  // Z is typically pretty slow
static const int kRumbleTimeMs = 80;
static const int kMotorTimeoutSeconds = 5;

// Some global state.
static int max_feedrate_mm_p_sec_xy;
static int max_feedrate_mm_p_sec_z;

static const long interval_msec = 20;  // update interval between reads.

// Flags.
static bool simulate_machine = false;
static bool quiet = false;  // quiet - don't print random stuff to screen

static const char *persistent_store = NULL;  // filename to store memory points.

// Streams to connect machine to.
static FILE *gcode_out = NULL;          // we write with fprintf() etc.
static int gcode_in_fd = STDIN_FILENO;  // .. and read directly from file desc.

struct Vector {
    float axis[NUM_AXIS];
};

// State for a particular button.
struct ButtonState {
    char is_pressed;
    struct Vector stored;
};
struct Buttons {
    int count;
    struct ButtonState state[0];  // trick for easy memory allocation.
};

static struct Buttons *new_Buttons(int n) {
    struct Buttons *result = (struct Buttons *)malloc(
      sizeof(struct Buttons) + n * sizeof(struct ButtonState));
    result->count = n;
    for (int i = 0; i < n; ++i) {
        result->state[i].is_pressed = 0;
        result->state[i].stored.axis[AXIS_X] = -1;
    }
    return result;
}
static void delete_Buttons(struct Buttons **b) {
    free(*b);
    *b = NULL;
}

static int quantize(int value, int q) { return value / q * q; }

void WriteSavedPoints(const char *filename, struct Buttons *buttons) {
    if (filename == NULL) return;
    FILE *out = fopen(filename, "w");  // Overwriting for now. TODO: tmp file.
    for (int i = 0; i < buttons->count; ++i) {
        if (buttons->state[i].stored.axis[AXIS_X] < 0) continue;
        fprintf(out, "%2d: %7.2f %7.2f %7.2f\n", i,
                buttons->state[i].stored.axis[AXIS_X],
                buttons->state[i].stored.axis[AXIS_Y],
                buttons->state[i].stored.axis[AXIS_Z]);
    }
    fclose(out);
}

void ReadSavedPoints(const char *filename, struct Buttons *buttons) {
    if (filename == NULL) return;
    FILE *in = fopen(filename, "r");
    if (in == NULL) return;
    int b;
    struct Vector vec;
    while (4 == fscanf(in, "%d: %f %f %f\n", &b, &vec.axis[AXIS_X],
                       &vec.axis[AXIS_Y], &vec.axis[AXIS_Z])) {
        if (b < 0 || b >= buttons->count) continue;
        buttons->state[b].stored = vec;
    }
    fclose(in);
}

static int64_t get_time_millis() {
    struct timeval tval;
    gettimeofday(&tval, NULL);
    return (int64_t)tval.tv_sec * 1000 + tval.tv_usec / 1000;
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
    tv.tv_sec = timeout_millis / 1000;
    tv.tv_usec = (timeout_millis % 1000) * 1000;

    FD_SET(fd, &read_fds);
    int s = select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (s < 0) return -1;
    return tv.tv_usec / 1000;
}

static int ReadLine(int fd, char *result, int len, bool do_echo) {
    int bytes_read = 0;
    char c = 0;
    while (c != '\n' && c != '\r' && bytes_read < len) {
        if (read(fd, &c, 1) < 0) return -1;
        ++bytes_read;
        *result++ = c;
        if (do_echo && !quiet && write(STDERR_FILENO, &c, 1) < 0) {  // echo
            perror("echo failed");
        }
    }
    *result = '\0';
    return bytes_read;
}

// Returns 0 on timeout, -1 on error and a positive number on event.
int JoystickWaitForEvent(int fd, struct js_event *event, int timeout_ms) {
    const int timeout_left = AwaitReadReady(fd, timeout_ms);
    if (timeout_left > 0) {
        if (read(fd, event, sizeof(*event)) < 0) {
            perror("Reading from joystick");
            return -1;
        }
    }
    return timeout_left;
}

static void JoystickInitialState(int js_fd, struct Configuration *config) {
    struct js_event e;
    config->highest_button = -1;
    // The initial state is sent on connect.
    while (JoystickWaitForEvent(js_fd, &e, 50) > 0) {
        if ((e.type & JS_EVENT_INIT) == 0) break;  // done init events.
        if ((e.type & JS_EVENT_AXIS) != 0) {
            // read zero position.
            for (int a = 0; a < NUM_AXIS; ++a) {
                if (config->axis_config[a].channel == e.number) {
                    config->axis_config[a].zero = e.value;
                    if (!quiet) {
                        fprintf(stderr, "Zero axis %d : %d\n", a, e.value);
                    }
                }
            }
        }
        if (e.type & JS_EVENT_BUTTON) {
            if (e.number > config->highest_button)
                config->highest_button = e.number;
        }
    }
}

enum EventOutput {
    JS_READ_ERROR = -3,
    JS_REACHED_TIMEOUT = -2,
    JS_HOME_BUTTON = -1,
    // values >= 0 are button values.
};
// Wait for a joystick button up to "timeout_ms" long. Returns one of
// EventOutput or a positive number (>= 0) denoting the button that
// has been pressed.
// In case the axis position is changing, it updates "axis", but does not
// return before the timeout, as this does not require immediate attention.
static int JoystickWaitForButton(int fd, int timeout_ms,
                                 const struct Configuration *config,
                                 struct Vector *axis, struct Buttons *buttons) {
    int timeout_left = timeout_ms;
    for (;;) {
        struct js_event e;
        timeout_left = JoystickWaitForEvent(fd, &e, timeout_left);
        if (timeout_left < 0) {
            return JS_READ_ERROR;
        }
        if (timeout_left == 0) {
            return JS_REACHED_TIMEOUT;
        }

        if (e.type == JS_EVENT_AXIS) {
            for (int a = 0; a < NUM_AXIS; ++a) {
                if (config->axis_config[a].channel == e.number) {
                    int normalized = e.value - config->axis_config[a].zero;
                    int quant = abs(config->axis_config[a].max_value / 16);
                    axis->axis[a] = (quantize(normalized, quant) * 1.0 /
                                     config->axis_config[a].max_value);
                }
            }
        } else if (e.type == JS_EVENT_BUTTON) {
            if (e.number <= config->highest_button) {
                buttons->state[e.number].is_pressed = e.value;
                if (e.number == config->home_button)
                    return JS_HOME_BUTTON;  // special button.
                else
                    return e.number;  // generic store button.
            }
        }
    }
}

// Discard all input until nothing is coming anymore within timeout. In
// particular on first connect, this helps us to get into a clean state.
static int DiscardAllInput(int timeout_ms) {
    if (simulate_machine) return 0;
    int total_bytes = 0;
    char buf[128];
    while (AwaitReadReady(gcode_in_fd, timeout_ms) > 0) {
        int r = read(gcode_in_fd, buf, sizeof(buf));
        if (r < 0) {
            perror("reading trouble");
            return -1;
        }
        total_bytes += r;
        if (!quiet && r > 0 && write(STDERR_FILENO, buf, r) < 0) {  // echo
            perror("echo failed");
        }
    }
    return total_bytes;
}

// 'ok' comes on a single line, maybe followed by something.
static void WaitForOk() {
    if (simulate_machine) return;
    char buffer[512];
    for (;;) {
        if (ReadLine(gcode_in_fd, buffer, sizeof(buffer), false) < 0) break;
        if (strncasecmp(buffer, "ok", 2) == 0) break;
    }
}

// Read coordinates from printer.
static bool GetCoordinates(struct Vector *pos) {
    if (simulate_machine) return 1;
    DiscardAllInput(100);

    fprintf(gcode_out, "M114\n");  // read coordinates.
    if (!quiet) fprintf(stderr, "Reading initial absolute position\n");
    char buffer[512];
    ReadLine(gcode_in_fd, buffer, sizeof(buffer), true);
    if (sscanf(buffer, "X:%f Y:%f Z:%f", &pos->axis[AXIS_X], &pos->axis[AXIS_Y],
               &pos->axis[AXIS_Z]) == 3) {
        WaitForOk();
        if (!quiet) {
            fprintf(stderr, "Got machine pos (x/y/z) = (%.3f/%.3f/%.3f)\n",
                    pos->axis[AXIS_X], pos->axis[AXIS_Y], pos->axis[AXIS_Z]);
        }
        return true;
    }
    fprintf(stderr, "Didn't get readable coordinates: '%s'\n", buffer);
    return false;
}

static time_t last_motor_on_time = 0;  // Quasi local state for motor move ops.
static void GCodeHome() {
    if (simulate_machine) return;
    fprintf(gcode_out, HOMING_COMMAND);
    WaitForOk();
    last_motor_on_time = time(NULL);
}

static void GCodeGoto(struct Vector *pos, float feedrate_mm_sec) {
    if (simulate_machine) return;
    fprintf(gcode_out, "G1 X%.3f Y%.3f Z%.3f F%.3f\n", pos->axis[AXIS_X],
            pos->axis[AXIS_Y], pos->axis[AXIS_Z], feedrate_mm_sec * 60);
    WaitForOk();
    last_motor_on_time = time(NULL);
}

static void GCodeEnsureMotorOff() {
    if (last_motor_on_time) {
        fprintf(gcode_out, "M84\n");
        WaitForOk();
        last_motor_on_time = 0;
    }
}

// Switch motor off if it has been idle for kMotorTimeoutSeconds
static void CheckMotorTimeout() {
    if (last_motor_on_time > 0 &&
        time(NULL) - last_motor_on_time > kMotorTimeoutSeconds) {
        GCodeEnsureMotorOff();
    }
}

// Returns 1 if any gcode has been output or 0 if there was no need.
int OutputJogGCode(int64_t interval_ms, struct Vector *pos,
                   const struct Vector *speed, const struct Vector *limit) {
    // We get the timeout in regular intervals.
    const float euklid = sqrtf(speed->axis[AXIS_X] * speed->axis[AXIS_X] +
                               speed->axis[AXIS_Y] * speed->axis[AXIS_Y] +
                               speed->axis[AXIS_Z] * speed->axis[AXIS_Z]);

    const float feedrate =
      euklid * ((fabs(speed->axis[AXIS_Z]) > 0.01) ? max_feedrate_mm_p_sec_z
                                                   : max_feedrate_mm_p_sec_xy);
    if (fabs(feedrate) < 0.1) return 0;

    // The interval_ms is empirically how long it took since the
    // last update.
    if (interval_ms > 100) interval_ms = 100;
    const float interval = interval_ms / 1000.0;
    bool do_rumble = false;
    for (int a = AXIS_X; a < NUM_AXIS; ++a) {
        const char at_limit_before =
          pos->axis[a] <= 0 || pos->axis[a] >= limit->axis[a];
        pos->axis[a] = pos->axis[a] + speed->axis[a] * feedrate * interval;
        if (pos->axis[a] < 0) {
            pos->axis[a] = 0;
            do_rumble |= !at_limit_before;
        }
        if (pos->axis[a] > limit->axis[a]) {
            pos->axis[a] = limit->axis[a];
            do_rumble |= !at_limit_before;
        }
    }
    GCodeGoto(pos, feedrate);
    if (!quiet) {
        fprintf(stderr, "Goto (x/y/z) = (%.2f/%.2f/%.2f)      \r",
                pos->axis[AXIS_X], pos->axis[AXIS_Y], pos->axis[AXIS_Z]);
    }
    if (do_rumble) JoystickRumble(kRumbleTimeMs);
    return 1;
}

void HandlePlaceMemory(int b, struct Buttons *buttons, int *accumulated_timeout,
                       struct Vector *machine_pos) {
    struct Vector *storage = &buttons->state[b].stored;
    if (buttons->state[b].is_pressed) {
        *accumulated_timeout = 0;
    } else {  // we act on release
        if (*accumulated_timeout >= 500) {
            *storage = *machine_pos;  // save
            WriteSavedPoints(persistent_store, buttons);
            JoystickRumble(kRumbleTimeMs);  // Feedback that it is stored now.
            if (!quiet) {
                fprintf(stderr, "\nStored in %d (%.2f, %.2f, %.2f)\n", b,
                        machine_pos->axis[AXIS_X], machine_pos->axis[AXIS_Y],
                        machine_pos->axis[AXIS_Z]);
            }
        } else {
            if (storage->axis[AXIS_X] >= 0) {
                *machine_pos = *storage;
                if (!quiet) {
                    fprintf(
                      stderr, "\nGoto position %d -> (%.2f, %.2f, %.2f)\n", b,
                      machine_pos->axis[AXIS_X], machine_pos->axis[AXIS_Y],
                      machine_pos->axis[AXIS_Z]);
                }
                GCodeGoto(machine_pos, max_feedrate_mm_p_sec_xy);
            } else {
                if (!quiet) fprintf(stderr, "\nButton %d undefined\n", b);
            }
        }
        *accumulated_timeout = -1;
    }
}

// Wait for the initial start-up of the machine and any initial
// chatter to subside (usually after connect, the printer/CNC machine resets
// and sends a bunch of configuration info before it is ready to start)
static void WaitForMachineStartup(int timeout_ms) {
    // Skip initial stuff coming from the machine. We need to have
    // a defined starting way to read the absolute coordinates.
    // Wait until board is initialized. Some Marlin versions dump some
    // stuff out there which we want to ignore.
    fprintf(gcode_out, "G21\n");  // Tickeling the serial line
    if (!quiet) fprintf(stderr, "Wait for initialization [");
    const int discarded = DiscardAllInput(timeout_ms);
    if (!quiet) fprintf(stderr, "] done (discarded %d bytes).\n", discarded);
    if (!quiet && discarded <= 0) {
        fprintf(stderr,
                "Mmmh, zero bytes is suspicious; we'd expect at least "
                "some bytes. Serial line ok ?\n");
    }
}

void JogMachine(int js_fd, bool do_homing, const struct Vector *machine_limit,
                const struct Configuration *config) {
    struct Vector speed_vector;
    struct Vector machine_pos;
    memset(&speed_vector, 0, sizeof(speed_vector));
    memset(&machine_pos, 0, sizeof(machine_pos));
    struct Buttons *buttons = new_Buttons(config->highest_button + 1);
    ReadSavedPoints(persistent_store, buttons);

    fprintf(gcode_out, "G21\n");
    WaitForOk();  // Switch to metric.

    char is_homed = 0;

    if (do_homing) {
        // Unfortunately, connecting to some Marlin instances resets it.
        // So home that we are in a defined state.
        GCodeHome();
        is_homed = 1;
    }

    // Relative mode (G91) seems to be pretty badly implemented and does not
    // deal with very small increments (which are rounded away).
    // So let's be absolute and keep track of the current position ourself.
    fprintf(gcode_out, "G90\n");
    WaitForOk();  // Absolute coordinates.

    if (!GetCoordinates(&machine_pos)) {
        delete_Buttons(&buttons);
        return;
    }

    fprintf(stderr, "Ready for Input\n");

    int64_t last_jog_time = 0;
    int accumulated_timeout = -1;
    int last_button_ev = 0;
    bool done = false;
    while (!done) {
        int button_ev = JoystickWaitForButton(js_fd, interval_msec, config,
                                              &speed_vector, buttons);
        switch (button_ev) {
        case JS_READ_ERROR:
            if (!quiet) fprintf(stderr, "Joystick unplugged\n");
            GCodeEnsureMotorOff();
            done = true;
            break;

        case JS_REACHED_TIMEOUT: {  // timeout, i.e. our regular update
                                    // interval.
            if (accumulated_timeout >= 0) {
                accumulated_timeout += interval_msec;
                if (accumulated_timeout > 500) {  // auto-release long press.
                    assert(buttons->state[last_button_ev].is_pressed);
                    buttons->state[last_button_ev].is_pressed = 0;
                    HandlePlaceMemory(last_button_ev, buttons,
                                      &accumulated_timeout, &machine_pos);
                }
            }
            const int64_t now = get_time_millis();
            if (OutputJogGCode(now - last_jog_time, &machine_pos, &speed_vector,
                               machine_limit)) {
                // We did emit some gcode. Now we're not homed anymore
                is_homed = 0;
                last_jog_time = now;
            } else {
                CheckMotorTimeout();
            }
        } break;

        case JS_HOME_BUTTON:  // only home if not already.
            if (buttons->state[config->home_button].is_pressed && !is_homed) {
                is_homed = 1;
                GCodeHome();
                if (!GetCoordinates(&machine_pos)) done = true;
            }
            break;

        default:
            HandlePlaceMemory(button_ev, buttons, &accumulated_timeout,
                              &machine_pos);
            last_button_ev = button_ev;
            break;
        }
    }
    delete_Buttons(&buttons);
}

static int usage(const char *progname, int initial_time) {
    fprintf(stderr,
            "Usage: %s <options>\n"
            "  -C <config-dir>  : Create a configuration file for Joystick, "
            "then exit.\n"
            "  -j <config-dir>  : Jog machine using config from directory.\n"
            "  -n <config-name> : Optional config name; otherwise derived from "
            "joystick name\n"
            "  -i <init-ms>     : Wait time for machine to initialize "
            "(default %d)\n"
            "  -h               : Home on startup\n"
            "  -p <persist-file>: persist saved points in given file\n"
            "  -L <x,y,z>       : Machine limits in mm\n"
            "  -x <speed>       : feedrate for xy in mm/s\n"
            "  -z <speed>       : feedrate for z in mm/s\n"
            "  -s               : machine not connected; simulate.\n"
            "  -q               : Quiet. No chatter on stderr.\n",
            progname, initial_time);
    return 1;
}

int main(int argc, char **argv) {
    max_feedrate_mm_p_sec_xy = kMaxFeedrate_xy;
    max_feedrate_mm_p_sec_z = kMaxFeedrate_z;
    bool do_homing = false;
    struct Configuration config;
    memset(&config, 0, sizeof(config));
    struct Vector machine_limits;
    machine_limits.axis[AXIS_X] = machine_limits.axis[AXIS_Y] =
      machine_limits.axis[AXIS_Z] = 305;

    enum Operation { DO_NOTHING, DO_CREATE_CONFIG, DO_JOG } op = DO_NOTHING;
    const char *config_dir = NULL;
    char joystick_name[512];
    memset(joystick_name, 0, sizeof(joystick_name));

    int startup_wait_ms = 20000;

    int opt;
    while ((opt = getopt(argc, argv, "C:j:x:z:L:hsp:q:n:i:")) != -1) {
        switch (opt) {
        case 'C':
            op = DO_CREATE_CONFIG;
            config_dir = strdup(optarg);
            break;

        case 'n':
            strncpy(joystick_name, optarg, sizeof(joystick_name) - 1);
            break;

        case 'h': do_homing = true; break;

        case 's': simulate_machine = true; break;

        case 'q': quiet = true; break;

        case 'p': persistent_store = strdup(optarg); break;

        case 'i': startup_wait_ms = atoi(optarg); break;

        case 'x':
            max_feedrate_mm_p_sec_xy = atoi(optarg);
            if (max_feedrate_mm_p_sec_xy <= 1) {
                fprintf(stderr, "Peculiar value -x %d",
                        max_feedrate_mm_p_sec_xy);
                return usage(argv[0], startup_wait_ms);
            }
            break;

        case 'L':
            // TODO: is there a gcode we can query ?
            if (3 != sscanf(optarg, "%f,%f,%f", &machine_limits.axis[AXIS_X],
                            &machine_limits.axis[AXIS_Z],
                            &machine_limits.axis[AXIS_Z])) {
                return usage(argv[0], startup_wait_ms);
            }
            break;

        case 'z':
            max_feedrate_mm_p_sec_z = atoi(optarg);
            if (max_feedrate_mm_p_sec_z <= 1) {
                fprintf(stderr, "Peculiar value -z %d",
                        max_feedrate_mm_p_sec_z);
                return usage(argv[0], startup_wait_ms);
            }
            break;

        case 'j':
            op = DO_JOG;
            config_dir = strdup(optarg);
            break;

        default: /* '?' */ return usage(argv[0], startup_wait_ms);
        }
    }

    if (op == DO_NOTHING) return usage(argv[0], startup_wait_ms);

    // Connection to the machine reading gcode. TODO: maybe provide
    // listening on a socket ?
    gcode_out = stdout;
    gcode_in_fd = STDIN_FILENO;
    setvbuf(gcode_out, NULL, _IONBF, 0);  // Don't buffer.

    // Stderr might be piped to another process. Make sure to flush that
    // immediately.
    setvbuf(stderr, NULL, _IONBF, 0);

    const int kJoystickId = 0;  // TODO: make configurable ?

    // The Joystick. The first time we open it, the zero values are not
    // yet properly established. So let's close the first instance right awawy
    // and use the next open :)
    // TODO: maybe not hardcode the joystick number. For now, expect it to be 0
    close(open("/dev/input/js0", O_RDONLY));             // kJoystickId
    const int js_fd = open("/dev/input/js0", O_RDONLY);  // kJoystickId
    if (js_fd < 0) {
        perror("Opening joystick");
        return 1;
    }

    if (joystick_name[0] == '\0') {
        if (ioctl(js_fd, JSIOCGNAME(sizeof(joystick_name)), joystick_name) < 0)
            strncpy(joystick_name, "unknown-joystick", sizeof(joystick_name));
        // Make a filename-friendly name out of it.
        for (char *x = joystick_name; *x; ++x) {
            if (isspace(*x)) *x = '-';
        }
    }
    if (!quiet) {
        fprintf(stderr, "joystick configuration name: %s\n", joystick_name);
    }
    if (op == DO_CREATE_CONFIG) {
        CreateConfig(js_fd, &config);
        WriteConfig(config_dir, joystick_name, &config);
    } else if (op == DO_JOG) {
        if (ReadConfig(config_dir, joystick_name, &config) == 0) {
            fprintf(stderr,
                    "Problem reading joystick config file.\n"
                    "Create a fresh one with\n\t%s -C %s\n",
                    argv[0], config_dir);
            return 1;
        }
        JoystickInitialState(js_fd, &config);
        JoystickRumbleInit(kJoystickId);
        WaitForMachineStartup(startup_wait_ms);
        JogMachine(js_fd, do_homing, &machine_limits, &config);
    }

    return 0;
}
