Jogging a 3d printer with a gamepad.
====================================

First, create a configuration for your specific joystick. This will ask you
to move the joystick to get the right axis and button mapping:

    ./machine-jog -C joystick.config

The machine-jog config communicates via stdin/stdout with the machine; for
a machine that is connected via a terminal line, you can use socat to wire
it up:

    socat EXEC:"./machine-jog -j joystick.config" /dev/ttyACM0,raw,echo=0,b230400

