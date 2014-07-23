Jogging a 3d printer with a gamepad.
====================================

Options
-------
     Usage: ./machine-jog <options>
       -C <config-dir>  : Create a configuration file for Joystick
       -j <config-dir>  : Jog machine using config
       -h               : Home on startup
       -p <persist-file>: persist saved points in given file
       -L <x,y,z>       : Machine limits in mm
       -x <speed>       : feedrate for xy in mm/s
       -z <speed>       : feedrate for z in mm/s
       -s               : machine not connected; simulate.
       -q               : Quiet. No chatter on stderr.

Joystick Configuration
----------------------

First, create a configuration for your specific joystick. This will ask you
to move the joystick to get the right axis and button mapping:

    mkdir js-conf
    ./machine-jog -C js-conf/

The directory will contain a configuration with the name of the joystick. That
way, it is possible to have different configurations depending on the joystick
name.

The configuration asks you to move the X,Y,Z to their extreme values to learn
which is your preferred axis. Also it asks you for a 'home' button, which
is used to home the machine and six additional buttons that you can use to
store and retrieve 'waypoints'.

Wire Up
-------
For best flexibility, machine-jog communicates via stdin/stdout with the
machine, which means you need to 'wire up' this communication with the machine.
For instance, if your machine is connected via a terminal line (very common),
you can use `socat` to wire it up:

    socat EXEC:"./machine-jog -j js-conf/ -x 120 -z 50 -h -p savedpoints.data" /dev/ttyACM0,raw,echo=0,b230400

This tells `socat` to execute the `machine-jog` binary with the joystick
configuration and connect its stdin/stdout to the `/dev/ttyACM0` terminal
with a bitrate of 230400.

In another program that has has a connection open to the printer, this can
start the machine-jog program in a sub-process and send everything from the
stdout stream to the printer and back from the printer into stdin of the process.
(This would be awesome in OctoPrint; if it sees `/dev/input/js0` to exist, it can
start machine-jog in a sub-process and connect the streams).

Use
---
To move around, use the joysticks to manipulate x/y/z. The speed of movement is
proportional to the deflection of the joystick.

To 'store' a current point in one of the six memory buttons, just do a
long-press on the button. A short-press on that button will go back to that
position.
