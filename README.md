Jogging a 3d printer with a gamepad.
====================================

First, create a configuration for your specific joystick. This will ask you
to move the joystick to get the right axis and button mapping:

    ./machine-jog -C joystick.config

For best flexibility, machine-jog communicates via stdin/stdout with the
machine, which means you need to 'wire up' this communication with the means
you can communicate with the machine.
For instance, if your machine is connected via a terminal line (very common),
you can use `socat` to wire it up:

    socat EXEC:"./machine-jog -j joystick.config" /dev/ttyACM0,raw,echo=0,b230400

This tells `socat` to execute the `machine-jog` binary with the joystick
configuration and connect its stdin/stdout to the `/dev/ttyACM0` terminal
with a bitrate of 230400.
