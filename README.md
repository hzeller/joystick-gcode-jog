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

First, create a configuration for your specific joystick. Since each joystick
has its own mapping of axis and buttons, we need to first tell machine-jog
where these are.
This will ask you to move the joystick to get the right axis and button mapping:

    mkdir js-conf
    ./machine-jog -C js-conf/

The directory will contain a configuration with the name of the joystick. That
way, it is possible to have different configurations depending on the joystick
name.

The configuration asks you to move the X,Y,Z to their extreme values to learn
which is your preferred axis. Also it asks you for the button you want to use
as the 'home' button (typically there is some center button).
(All other buttons will be used to store and retrieve positions).

Typically all USB gamepads either for PS3 or Xbox should work. On my beaglebone
I found that the xpad kernel module was missing, so only a PS3 gamepad worked right
out of the box. On my regular Linux notebook, both types worked right away.

Wire Up
-------
For best flexibility, machine-jog communicates via stdin/stdout with the
machine, which means you need to 'wire up' this communication with the machine.
For instance, if your machine is connected via a terminal line (very common),
you can use `socat` to wire it up:

    socat EXEC:"./machine-jog -j js-conf/ -x 120 -z 50 -h -p savedpoints.data" /dev/ttyACM0,raw,echo=0,b230400

This tells `socat` to execute the `machine-jog` binary with the joystick
configuration and connect its stdin/stdout to the `/dev/ttyACM0` terminal
with a bitrate of 230400. If you use BeagleG, then you'd connect it to the
TCP socket:

    socat EXEC:"./machine-jog -j config/ -x 100 -z 20 -p savedpoints.data" TCP4:beagleg-machine.local:4000

This could be automatically started in a udev-rule for instance.

The typical use-case, however, is to use `machine-jog` from within
another program that already has the serial line open and 'owns' it.
In this case, that program would start `machine-jog` in a sub-process
and send everything from its `stdout` stream to the printer and back from
the printer into `stdin` of the process. Make sure to not do any buffering.
(This would be awesome in [OctoPrint](http://octoprint.org); if it
sees `/dev/input/js0` to exist, it could `subprocess.call(...)` `machine-jog`
and connect the streams when the user is in the control panel, so that
both, the manual jogging buttons and the joystick works.
Should be fairly easy to add for someone who knows Python...)

Use
---
To move around, use the joysticks to manipulate x/y/z. The speed of movement is
proportional to the deflection of the joystick. Hitting the limits of the machine
is fed back with a short rumble (if supported by gamepad).

To 'store' a current point in one of the six memory buttons, just do a
long-press on the button (acknowledged by a short rumble). A short-press on
that button will go back to that position.
