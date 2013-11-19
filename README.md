bmd-tools
=========

Tools to connect with and manage Blackmagic Design video
equipment with built-in H.264 encoder.

Currently tested device:

 * ATEM TV studio

Use *bmd-extractfw* to extract the firmware out from
BMDStreamingServer.exe (part of the Windows drivers).

*bmd-streamer* can be used to upload the extracted firmwares,
and to stream out (currently to stdout) the MPEG TS stream
from the device.

