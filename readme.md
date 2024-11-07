# Screencast

This program for Linux allows to stream the contents of the screen or a window to a DLNA client. I wrote this program, because no existing solutions worked for me (I am using Linux Mint with Cinnamon). More precisely, Miracast solutions did not work. Miracast is a very complex protocol, so I opted to use DLNA (UPnP), which is much simpler. It has a high latency, but Miracast (I've tried it with Android), has a high latency, too, at least with my TV set.

To build the program, just run `make`. You will need the development packages of X11, alsa/asound and ffmpeg.

By default, the program will only send video, use `-a default` to send audio, too. This will record the default audio device. Since you will probably want to send the audio played by your computer, you will have to select the Monitor source in the Pulse audio volume control. This of course supposes that you have pulseaudio, but if you want to send the output of your computer, it's probably a desktop computer, so you probably have it.

This is the syntax:

```
screencast [options]
        -H, --help                         Print this help
        -f <fps>, --fps <fps>              Frames per second, default 30
        -b <bitrate>, --bitrate <bitrate>  Bitrate, default 2000000
        -w <width>, --width <width>        Output width, default 1920
        -h <height>, --height <height>     Output height, default 1080
        -p <port>, --port <port>           Local TCP port for the HTTP server, default 8080
        -a <device>, --audiodev <device>   Name of the audio device for sending audio, default none
```

Then open a DLNA client, you should see `Screencast DLNA server` in the list of DLNA servers. If you select it, it should show you a list of windows, the first one being `Desktop`.
