Jogging a 3d printer with a gamepad.
====================================

Usage

     Usage: ./machine-jog <options>
       -C <config-dir>  : Create a configuration file for Joystick
       -j <config-dir>  : Jog machine using config
       -h           : Home on startup
       -p <persist-file> : persist saved points in given file
       -L <x,y,z>   : Machine limits in mm
       -x <speed>   : feedrate for xy in mm/s
       -z <speed>   : feedrate for z in mm/s
       -s           : machine not connected; simulate.
       -q           : Quiet. No chatter on stderr.

First, create a configuration for your specific joystick. This will ask you
to move the joystick to get the right axis and button mapping:

    mkdir js-conf
    ./machine-jog -C js-conf/

The directory will contain a configuration with the name of the joystick. That
way, it is possible to have different configurations depending on the joystick
name.

For best flexibility, machine-jog communicates via stdin/stdout with the
machine, which means you need to 'wire up' this communication with the machine.
For instance, if your machine is connected via a terminal line (very common),
you can use `socat` to wire it up:

    socat EXEC:"./machine-jog -j js-conf/ -x 120 -z 50 -h" /dev/ttyACM0,raw,echo=0,b230400

This tells `socat` to execute the `machine-jog` binary with the joystick
configuration and connect its stdin/stdout to the `/dev/ttyACM0` terminal
with a bitrate of 230400.

In another program that has has a connection open to the printer, this can
start the machine-jog program in a sub-process and send everything from the
stdout stream to the printer and back from the printer into stdin of the process.
(This would be awesome in OctoPrint ...).
