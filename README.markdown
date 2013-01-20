This is a fork of [liskin's screenclone][liskin-screenclone]. Most of the work has been done by him.

# hybrid-screenclone

This is a reimplementation of [hybrid-windump][hybrid-windump] with the
opposite use-case: doing all rendering using the integrated intel card and
using the additional card just to get more outputs (e.g. a triple-head with
ThinkPad T420). As such, it uses the DAMAGE extension to avoid unnecessary
redraws and the RECORD extension to capture mouse movements, which are
translated to mouse movements on the destination X server.

For this to work correctly, an additional virtual Xinerama screen must be
available. To get one, see my [virtual CRTC for intel][patch] patch.

# features added by my fork

* It can be used for cloning more than one display:
 * Parameter for the target display
 * Mouse 'wiggling' can be disabled
* Displays can be selected by their Xrandr name (also by monitor name (EDID) for NVidia)

If you want to clone more than one screen, you must disable 'mouse wiggling'
(parameter -w). This means that the screensaver may be activated for the
'NVidia' displays, if you don't move the mouse on them in a while.

If you clone the same area of the screen to more than one place, we would
have to clone the mouse to more than one place. Of course, this cannot work
because the second X-Server has only one mouse pointer. The pointer will
jump rapidly between the positions, so try to avoid that situation.

[hybrid-windump]: https://github.com/harp1n/hybrid-windump
[patch]: https://github.com/liskin/patches/blob/master/hacks/xserver-xorg-video-intel-2.18.0_virtual_crtc.patch
[liskin-screenclone]: https://github.com/liskin/hybrid-screenclone
