/* The ParavirtualizedGraphics display path went away with the Windows direction:
   f81479e deleted pvg_lite.h, pvg.h, pvg_colorprobe.h and metallib.h along with
   the vmapple board it drew for. The Windows runner paints through
   devices/winvirt.c's ramfb/virtio-gpu and devices/fb_metal.m, so this object is
   intentionally empty -- the iOS library still lists it, nothing links to it. */
