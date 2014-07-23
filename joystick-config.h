/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 */

#include "machine-jog.h"

struct AxisConfig {
    int channel;
    int zero;
    int max_value;
};

struct Configuration {
    struct AxisConfig axis_config[NUM_AXIS];
    int button_channel[NUM_BUTTONS];
};

// Interactively create a configuration
int CreateConfig(int js_fd, struct Configuration *config);

// Write configuration to file.
void WriteConfig(const char *filename, const struct Configuration *config);

// Read config.
int ReadConfig(const char *filename, struct Configuration *config);
