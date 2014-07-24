
#include "joystick-config.h"

#include <linux/joystick.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Assemble configuration file name from config dir and js_name.
// Return 1 on success 0 on failure.
static int AssembleFilename(const char *config_dir, const char *js_name,
                            char *out_filename, int len) {
    struct stat buf;
    if (stat(config_dir, &buf) < 0) {
        perror("Trouble stat()ing directory.");
        return 0;
    }
    if (!S_ISDIR(buf.st_mode)) {
        fprintf(stderr, "Not a directory: %s\n", config_dir);
        return 0;
    }
    snprintf(out_filename, len, "%s/%s.config", config_dir, js_name);
    return 1;
}

void WriteConfig(const char *config_dir, const char *js_name,
                 const struct Configuration *config) {
    char filename[1024];
    if (!AssembleFilename(config_dir, js_name, filename, sizeof(filename)))
        return;
    FILE *out = fopen(filename, "w");
    if (out == NULL) return;
    for (int i = 0; i < NUM_AXIS; ++i) {
        fprintf(out, "A:%d %d %d\n",
                config->axis_config[i].channel,
                config->axis_config[i].zero,
                config->axis_config[i].max_value);
    }
    fprintf(out, "B:%d\n", config->home_button);
    fclose(out);
}

// Return 1 on success.
int ReadConfig(const char *config_dir, const char *js_name,
               struct Configuration *config) {
    char filename[1024];
    if (!AssembleFilename(config_dir, js_name, filename, sizeof(filename)))
        return 0;
    FILE *in = fopen(filename, "r");
    if (in == NULL)
        return 0;
    for (int i = 0; i < NUM_AXIS; ++i) {
        if (3 != fscanf(in, "A:%d %d %d\n",
                        &config->axis_config[i].channel,
                        &config->axis_config[i].zero,
                        &config->axis_config[i].max_value))
            return 0;
    }
    if (1 != fscanf(in, "B:%d\n", &config->home_button))
        return 0;
    fclose(in);
    return 1;
}

static int64_t GetMillis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static void FindLargestAxis(int js_fd, struct AxisConfig *axis_config) {
    struct js_event e;
    for (;;) {
        if (JoystickWaitForEvent(js_fd, &e, 1000) <= 0)
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
        if (JoystickWaitForEvent(js_fd, &e, 1000) <= 0)
            continue;
        if (e.type == JS_EVENT_AXIS && e.number == channel
            && abs(e.value) < 5000) {
            zero_value = e.value;
            break;
        }
    }
    // Now, we read the values while they come in, assuming the last one
    // is the 'zero' position.
    const int64_t end_time = GetMillis() + 100;
    while (GetMillis() < end_time) {
        if (JoystickWaitForEvent(js_fd, &e, 100) <= 0)
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
        if (JoystickWaitForEvent(js_fd, &e, 1000) <= 0)
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
        if (JoystickWaitForEvent(js_fd, &e, 1000) <= 0)
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
int CreateConfig(int js_fd, struct Configuration *config) {
    GetAxisConfig(js_fd, "Move X all the way to the right ->  ",
                  &config->axis_config[AXIS_X]);
    GetAxisConfig(js_fd, "Move Y all the way up            ^  ",
                  &config->axis_config[AXIS_Y]);
    GetAxisConfig(js_fd, "Move Z all the way up            ^  ",
                  &config->axis_config[AXIS_Z]);
    GetButtonConfig(js_fd, "Press HOME button.", &config->home_button);
    return 1;
}
