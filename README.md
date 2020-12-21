# vid-tiberius

Minimalistic, low latency video streaming pipeline for Tiberius Robot based on ffmpeg.

### Sender

* Looks for specific video4linux2 webcam
* Reestablishes webcam if lost
* Streams frames over RTP with high x264 compression at 10 fps
* Provides a control socket for remotely setting compression level and requesting 720p JPEG snapshot frames

### Receiver

* Targets Siemens Comfort HMI panel; therefore, it is a Windows CE application (no really)
* Looks for all .sdp files next to .exe and launches an instance for each
* .sdp files may specify initial window (x,y) position and title
* Slider control to request compression quality
* Snapshot button for requesting 720p JPEG frame to open in a screen-filling window

### Requirements

* GCC 10+
* CMake 3.10+
* Native ffmpeg development packages (for sender)
* CEGCC x86 mingw toolchain built from source
  * Hasn't been updated in nearly a decade, but still works!
  * Use a [code snapshot of r1449](https://sourceforge.net/p/cegcc/code/HEAD/tree/)
  * Apply `cegcc-code-r1449-trunk-gcc10.diff` from this repository to massage the code to build with GCC 10
  * Run `cegcc/src/scripts/build-x86.sh` to automatically build and install the toolchain in `/opt/x86mingw32ce`
* pthreads-w32 installed into CEGCC prefix
  * Use [source of v2.9.1](ftp://sourceware.org/pub/pthreads-win32/pthreads-w32-2-9-1-release.tar.gz)
  * Apply `pthreads-w32-2-9-1-release-fixes.diff` from this repository to build for WinCE
  * Build with `make GC-static`
  * Copy `libpthreadGC2.a` to `/opt/x86mingw32ce/i386-mingw32ce/lib`
  * Copy `pthread.h`, `sched.h`, and `need_errno.h` to `/opt/x86mingw32ce/i386-mingw32ce/include`
* [Custom fork of ffmpeg](https://github.com/jackoalan/FFmpeg/tree/wince) (for receiver)
  installed into CEGCC prefix
  * Use this configure command:

```bash
CFLAGS="-mwin32 -DWINCE -D_WIN32_IE=0x0600 -D_WIN32_WCE=0x0600 -D__CE_VERSION__=0x0600 -DPTW32_STATIC_LIB" \
./configure --prefix=/opt/x86mingw32ce/i386-mingw32ce --enable-cross-compile --cross-prefix=i386-mingw32ce- \
--arch=i386 --cpu=pentium3 --target-os=mingw32ce --disable-everything --enable-decoder=h264 \
--enable-parser=h264 --enable-decoder=mjpeg --enable-parser=mjpeg --enable-demuxer=sdp --enable-protocol=rtp \
--enable-protocol=file --disable-avfilter --disable-swresample --disable-avdevice --disable-autodetect \
--disable-programs --enable-pthreads --disable-w32threads
```

Then `make`, `make install` ffmpeg as usual

### Building Sender

```shell script
cd <path-to-repository>
mkdir build-sender
cd build-sender
cmake ..
cmake --build .
```

*Install target and systemd service coming soon*

### Building Receiver

```shell script
cd <path-to-repository>
mkdir build-receiver
cd build-receiver
cmake -DCMAKE_C_COMPILER=/opt/x86mingw32ce/bin/i386-mingw32ce-gcc -DCMAKE_CXX_COMPILER=/opt/x86mingw32ce/bin/i386-mingw32ce-g++ ..
cmake --build .
```

Deploy `vid-receiver.exe` along with any number of .sdp files on the CE environment.

Here is a sample SDP where the receiver IPv4 address is 10.10.20.10.
H.264 video is streamed to port 1936 and JPEG snapshots are streamed to port 1938.

```
v=0
s=Front Camera
i=x:100 y:100
c=IN IP4 10.10.20.10
m=video 1936 RTP/AVP 96
a=rtpmap:96 H264/90000
m=video 1938 RTP/AVP 26
```
