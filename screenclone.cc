#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/record.h>
#include <X11/extensions/Xrandr.h>

//#define ENABLE_NVCTRL

#ifdef ENABLE_NVCTRL
#	include <NVCtrl/NVCtrlLib.h>
#endif	// ENABLE_NVCTRL

#define STR2(x) #x
#define STR(x) STR2( x )
#define ERR throw std::runtime_error( std::string() + __FILE__ + ":" + STR( __LINE__ ) + " " + __FUNCTION__ )
#define ERR2(msg) throw std::runtime_error( std::string() + __FILE__ + ":" + STR( __LINE__ ) + " " + __FUNCTION__  + ": " + msg)

#if 1 // debugging off
# define DBG(x)
#else // debugging on
# define DBG(x) x
#endif

uint64_t microtime() {
	struct timeval tv;
	gettimeofday( &tv, NULL );
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

#if 1 // time tracing off
# define TC(x) x
#else // time tracing on
# define TC(x) \
	({ uint64_t t1, t2; t1 = microtime(); auto ret = ( x ); t2 = microtime(); \
	printf( "%15llu - %5d: %s\n", t2 - t1, __LINE__, #x ); ret; })
#endif

struct window;
struct xinerama_screen;

struct display {
	Display *dpy;
	int damage_event, damage_error;
	int xfixes_event, xfixes_error;

	display( const std::string &name );
	display clone() const;
	window root() const;
	XEvent next_event();
	int pending();

	template < typename Fun > void record_pointer_events( Fun *callback );
	void select_cursor_input( const window &win );

	typedef std::vector< xinerama_screen > screens_vector;
	screens_vector xinerama_screens();
};

struct window {
	const display *d;
	Window win;
	Damage dmg;

	window( const display &_d, Window _win ) : d( &_d ), win( _win ), dmg( 0 ) {}
	void create_damage();
	void clear_damage();
	void warp_pointer( int x, int y );
	void define_cursor( Cursor c );
};

struct xinerama_screen {
	const display *d;
	XineramaScreenInfo info;

	xinerama_screen( const display &_d, const XineramaScreenInfo &_info )
		: d( &_d ), info( _info ) {}

	bool in_screen( int x, int y ) const;
	bool intersect_rectangle( const XRectangle &rec ) const;
};

display::display( const std::string &name ) {
	dpy = XOpenDisplay( name.c_str() );
	if ( !dpy ) ERR;

	if ( !XDamageQueryExtension( dpy, &damage_event, &damage_error ) )
		ERR;
	if ( !XFixesQueryExtension( dpy, &xfixes_event, &xfixes_error ) )
		ERR;
}

display display::clone() const {
	return display( DisplayString( dpy ) );
}

window display::root() const {
	return window( *this, DefaultRootWindow( dpy ) );
}

XEvent display::next_event() {
	XEvent e;
	if ( XNextEvent( dpy, &e ) ) ERR;
	return e;
}

int display::pending() {
	return XPending( dpy );
}

template < typename Fun >
void record_callback( XPointer priv, XRecordInterceptData *data ) {
	Fun *f = (Fun *) priv;
	(*f)( data );
}

template < typename Fun >
void record_thread( display data, Fun *callback ) {
	int fd = ConnectionNumber( data.dpy );
	fd_set fds;
	FD_ZERO( &fds );

	for ( ;; ) {
		FD_SET( fd, &fds );
		select( fd + 1, &fds, NULL, NULL, NULL );
		XRecordProcessReplies( data.dpy );
	}
}

template < typename Fun >
void display::record_pointer_events( Fun *callback ) {
	display data = clone();

	XRecordRange *rr = XRecordAllocRange();
	if ( !rr ) ERR;
	rr->device_events.first = rr->device_events.last = MotionNotify;

	XRecordClientSpec rcs = XRecordAllClients;

	XRecordContext rc = XRecordCreateContext( dpy, 0, &rcs, 1, &rr, 1 );
	if ( !rc ) ERR;

	// sync, otherwise XRecordEnableContextAsync fails
	XSync( dpy, false );
	XSync( data.dpy, false );

	if ( !XRecordEnableContextAsync( data.dpy, rc, &record_callback< Fun >, (XPointer) callback ) )
		ERR;

	std::thread( &record_thread< Fun >, data, callback ).detach();
}

void display::select_cursor_input( const window &win ) {
	XFixesSelectCursorInput( dpy, win.win, XFixesDisplayCursorNotifyMask );
}

display::screens_vector display::xinerama_screens() {
	int number;
	XineramaScreenInfo *screens = XineramaQueryScreens( dpy, &number );
	if ( !screens ) ERR;

	screens_vector vec;
	for ( int i = 0; i < number; ++i )
		vec.push_back( xinerama_screen( *this, screens[ i ] ) );

	XFree( screens );
	return vec;
}

void window::create_damage() {
	if ( !( dmg = XDamageCreate( d->dpy, win, XDamageReportRawRectangles ) ) )
		ERR;
}

void window::clear_damage() {
	if ( !dmg ) ERR;

	XDamageSubtract( d->dpy, dmg, None, None );
}

void window::warp_pointer( int x, int y ) {
	TC( XWarpPointer( d->dpy, None, win, 0, 0, 0, 0, x, y ) );
}

void window::define_cursor( Cursor c ) {
	TC( XDefineCursor( d->dpy, win, c ) );
}

bool xinerama_screen::in_screen( int x, int y ) const {
	return x >= info.x_org && x < info.x_org + info.width
		&& y >= info.y_org && y < info.y_org + info.height;
}

bool segment_intersect( int a1, int a2, int b1, int b2 ) {
	return a1 < b1 ? a2 > b1 : b2 > a1;
}

bool xinerama_screen::intersect_rectangle( const XRectangle &rec ) const {
	return segment_intersect( rec.x, rec.x + rec.width,  info.x_org, info.x_org + info.width  )
		&& segment_intersect( rec.y, rec.y + rec.height, info.y_org, info.y_org + info.height );
}

struct image_replayer {
	const display *src, *dst;
	const xinerama_screen *src_screen, *dst_screen;
	window src_window, dst_window;
	XShmSegmentInfo src_info, dst_info;
	XImage *src_image, *dst_image;
	GC dst_gc;
	bool damaged;

	image_replayer( const display &_src, const display &_dst, const xinerama_screen &_src_screen, const xinerama_screen &_dst_screen )
		: src( &_src ), dst( &_dst)
		, src_screen( &_src_screen ), dst_screen( &_dst_screen )
		, src_window( src->root() ), dst_window( dst->root() )
		, damaged( true )
	{	
		size_t sz = src_screen->info.width * src_screen->info.height * 4;
		src_info.shmid = dst_info.shmid = shmget( IPC_PRIVATE, sz, IPC_CREAT | 0666 );
		src_info.shmaddr = dst_info.shmaddr = (char *) shmat( src_info.shmid, 0, 0);
		src_info.readOnly = dst_info.readOnly = false;
		shmctl( src_info.shmid, IPC_RMID, NULL );

		src_image = XShmCreateImage( src->dpy, DefaultVisual( src->dpy, DefaultScreen( src->dpy ) ),
			DefaultDepth( src->dpy, DefaultScreen( src->dpy ) ), ZPixmap, src_info.shmaddr,
			&src_info, src_screen->info.width, src_screen->info.height );
		dst_image = XShmCreateImage( dst->dpy, DefaultVisual( dst->dpy, DefaultScreen( dst->dpy ) ),
			DefaultDepth( dst->dpy, DefaultScreen( dst->dpy ) ), ZPixmap, dst_info.shmaddr,
			&dst_info, src_screen->info.width, src_screen->info.height );

		XShmAttach( src->dpy, &src_info );
		XShmAttach( dst->dpy, &dst_info );

		dst_gc = DefaultGC( dst->dpy, DefaultScreen( dst->dpy ) );
	}

	void copy_if_damaged() {
		if ( !damaged )
			return;

		TC( XShmGetImage( src->dpy, src_window.win, src_image,
				src_screen->info.x_org, src_screen->info.y_org, AllPlanes) );
		TC( XShmPutImage( dst->dpy, dst_window.win, dst_gc, dst_image, 0, 0,
				dst_screen->info.x_org, dst_screen->info.y_org,
				dst_image->width, dst_image->height, False ) );
		TC( XFlush( dst->dpy ) );

		DBG( std::cout << "damaged" << std::endl );

		damaged = false;
	}

	void damage( const XRectangle &rec ) {
		damaged = damaged || src_screen->intersect_rectangle( rec );
	}
};

struct mouse_replayer {
	const display src, dst;
	const xinerama_screen src_screen, dst_screen;
	window dst_window;
	Cursor invisibleCursor;
	volatile bool on;
	bool wiggle;
	std::recursive_mutex cursor_mutex;

	mouse_replayer( const display &_src, const display &_dst, const xinerama_screen &_src_screen, const xinerama_screen &_dst_screen, bool _wiggle )
		: src( _src ), dst( _dst), src_screen( _src_screen ), dst_screen( _dst_screen ), dst_window( dst.root() )
		, on( false ), wiggle( _wiggle )
	{
		// create invisible cursor
		Pixmap bitmapNoData;
		XColor black;
		static char noData[] = { 0,0,0,0,0,0,0,0 };
		black.red = black.green = black.blue = 0;

		bitmapNoData = XCreateBitmapFromData( dst.dpy, dst_window.win, noData, 8, 8 );
		invisibleCursor = XCreatePixmapCursor( dst.dpy, bitmapNoData, bitmapNoData,
				&black, &black, 0, 0);

		dst_window.define_cursor( invisibleCursor );
	}

	void operator() ( XRecordInterceptData *data ) {
		if ( data->category == XRecordFromServer ) {
			const xEvent &e = * (const xEvent *) data->data;

			if ( e.u.u.type == MotionNotify ) {
				mouse_moved( e.u.keyButtonPointer.rootX, e.u.keyButtonPointer.rootY );
			}
		}

		XRecordFreeData( data );
	}

	void mouse_moved( int x, int y ) {
		std::lock_guard< std::recursive_mutex > guard( cursor_mutex );

		bool old_on = on;
		on = src_screen.in_screen( x, y );

		if ( on )
			dst_window.warp_pointer( x - src_screen.info.x_org + dst_screen.info.x_org,
				y - src_screen.info.y_org + dst_screen.info.y_org );
		else if ( wiggle )
			// wiggle the cursor a bit to keep screensaver away
			dst_window.warp_pointer( x % 50, y % 50 );

		if ( old_on != on ) {
			if ( on )
				cursor_changed();
			else
				dst_window.define_cursor( invisibleCursor );
		}

		TC( XFlush( dst.dpy ) );

		DBG( std::cout << "mouse moved" << std::endl );
	}

	void cursor_changed() {
		std::lock_guard< std::recursive_mutex > guard( cursor_mutex );

		if ( !on )
			return;

		XFixesCursorImage *cur;
		XcursorImage image;
		Cursor cursor;

		cur = TC( XFixesGetCursorImage( src.dpy ) );
		memset( &image, 0, sizeof( image ) );
		image.width  = cur->width;
		image.height = cur->height;
		image.size	 = std::max( image.width, image.height );
		image.xhot	 = cur->xhot;
		image.yhot	 = cur->yhot;

		if ( 0 && sizeof( * image.pixels ) == sizeof( * cur->pixels ) ) {
			// 32-bit machines where int is long
			image.pixels = (unsigned int *) cur->pixels;
		} else {
			image.pixels = (unsigned int *) alloca(
				image.width * image.height * sizeof( unsigned int ) );
			for ( unsigned i = 0; i < image.width * image.height; ++i )
				image.pixels[ i ] = cur->pixels[ i ];
		}

		cursor = TC( XcursorImageLoadCursor( dst.dpy, &image ) );
		XFree( cur );

		TC( XDefineCursor( dst.dpy, dst_window.win, cursor ) );
		XFreeCursor( dst.dpy, cursor );

		TC( XFlush( dst.dpy ) );

		DBG( std::cout << "cursor changed" << std::endl );
	}
};

void usage( const char *name )
{
	std::cerr
		<< "Usage: " << name << " <options>" << std::endl
		<< "Options:" << std::endl
		<< " -s <source display name> (default :0)" << std::endl
		<< " -d <target display name> (default :1)" << std::endl
		<< " -x <xinerama screen number on source> (default 0)" << std::endl
		<< " -D <xinerama screen number on target> (default 0)" << std::endl
		<< " -w do not wiggle the mouse (screensaver might come on, but necessary for multi-clones)" << std::endl;
	exit( 0 );
}

#ifdef ENABLE_NVCTRL

void split_at_comma(char* str, std::vector<std::string>& parts) {
	char* comma;
	do {
		// skip spaces
		while (*str == ' ' || *str == '\n')
			++str;

		// find next comma
		comma = strchr(str, ',');

		// last characters is either before the comma or last in string
		char* last = (comma ? comma-1 : str+strlen(str)-1);

		// skip whitespace at end of current part
		while (last >= str && (*last == ' ' || *last == '\n'))
			--last;

		// add it, unless it's empty
		if (last >= str) {
			// terminate it
			last++;
			*last = 0;

			// and then add it
			parts.push_back(std::string(str));
		}

		// next time, we start after the comma
		str = comma+1;
	} while (comma);	// do that until we cannot find any comma
}

int parse_display(const char* name) {
	// see http://disper.sourcearchive.com/documentation/0.3.0-1/nvctrl_8py_source.html
	int base;
	const char* num;
	if ( memcmp(name, "DFP-", 4) == 0 ) {
		base = 16;
		num = name + 4;
	} else if ( memcmp(name, "TV-", 3) == 0 ) {
		base = 8;
		num = name + 3;
	} else if ( memcmp(name, "CRT-", 4) == 0 ) {
		base = 0;
		num = name + 4;
	} else {
		return -1;
	}

	return base + atoi(num);
}

void get_enabled_displays_in_xinerama_order(
		std::vector<std::string>& enabled_displays_in_xinerama_order,
		std::vector<int>& enabled_display_nums_in_xinerama_order,
		int enabled_displays, char* xinerama_order) {
	// split order string at commas
	std::vector<std::string> parts;
	split_at_comma(xinerama_order, parts);

	// add default order
	parts.push_back("CRT");
	parts.push_back("DFP");
	parts.push_back("TV");

	int done_displays = 0;

	for (auto part = parts.begin(); part != parts.end(); ++part) {
		int display_num = parse_display(part->c_str());
		if (display_num >= 0) {
			if ( (done_displays & (1<<display_num)) == 0 ) {
				done_displays |= (1<<display_num);
				if ( (enabled_displays & (1<<display_num)) != 0 ) {
					enabled_displays_in_xinerama_order.push_back(*part);
					enabled_display_nums_in_xinerama_order.push_back(display_num);
				}
			}
		} else {
			int base;
			if (*part == "DFP")
				base = 16;
			else if (*part == "TV")
				base = 8;
			else if (*part == "CRT")
				base = 0;
			else
				base = -1;

			if (base < 0) {
				std::cerr << "WARN: ignoring item in TwinViewXineramaInfoOrder: " << *part << std::endl;
			} else {
				for (int i=0;i<8;i++) {
					display_num = base + i;
					if ( (done_displays & (1<<display_num)) == 0 ) {
						done_displays |= (1<<display_num);
						if ( (enabled_displays & (1<<display_num)) != 0 ) {
							char buffer[20];
							sprintf(buffer, "%s-%d", part->c_str(), i);
							enabled_displays_in_xinerama_order.push_back(std::string(buffer));
							enabled_display_nums_in_xinerama_order.push_back(display_num);
						}
					}
				}
			}
		}
	}
}

xinerama_screen& get_xinerama_screen_nvidia(display& disp, int screen, display::screens_vector& screens, char* name)
{
	int display_num = parse_display(name);

	if (display_num < 0) {
		int connected_displays;
		if (! XNVCTRLQueryAttribute(disp.dpy, screen, 0, NV_CTRL_CONNECTED_DISPLAYS, &connected_displays)) {
			ERR2("couldn't determine connected displays");
		}

		for (int display=0;display < 32;display++) {
			char* display_name;

			if ((connected_displays & (1<<display)) == 0)
				continue;

			if (XNVCTRLQueryStringAttribute(disp.dpy, screen, (1<<display), NV_CTRL_STRING_DISPLAY_DEVICE_NAME, &display_name)) {
				bool right_name = strcmp(display_name, name) == 0;

				XFree(display_name);

				if (right_name) {
					display_num = display;
					break;
				}
			}
		}

		if (display_num < 0) {
			std::cerr << "invalid screen name" << std::endl;
			std::cerr << "valid names (for NVidia): DFP-n, TV-n and CRT-n (with 0 <= n < 8) or monitor name:" << std::endl;
			for (int display=0;display < 32;display++) {
				char* display_name;

				if ((connected_displays & (1<<display)) == 0)
					continue;

				if (XNVCTRLQueryStringAttribute(disp.dpy, screen, (1<<display), NV_CTRL_STRING_DISPLAY_DEVICE_NAME, &display_name)) {
					std::cerr << " - " << display_name << std::endl;

					XFree(display_name);
				}
			}

			ERR2("invalid screen name (NVidia only has DFP-n, TV-n and CRT-n or a monitor name)");
		}
	}

	// is it enabled?
	// (Only enabled displays get a Xinerama screen.)
	int enabled_displays;
	if (! XNVCTRLQueryAttribute(disp.dpy, screen, 0, NV_CTRL_ENABLED_DISPLAYS, &enabled_displays)) {
		ERR2("couldn't determine enabled displays");
	}

	if ( (enabled_displays & (1<<display_num)) == 0 )
		ERR2("display not enabled");

	// We assume that TwinView is enabled.
	int twinview_enabled;
	if ( !XNVCTRLQueryAttribute(disp.dpy, screen, 0, NV_CTRL_TWINVIEW, &twinview_enabled) )
		ERR2("cannot get twinview status");
	if ( ! twinview_enabled )
		ERR2("TwinView must be enabled");

	char* xinerama_order;
	if ( ! XNVCTRLQueryStringAttribute(disp.dpy, screen, 0,
			NV_CTRL_STRING_TWINVIEW_XINERAMA_INFO_ORDER, &xinerama_order))
		ERR2("couldn't read attribute TWINVIEW_XINERAMA_INFO_ORDER");

	// xinerama_order is the value of the TwinViewXineramaInfoOrder X setting
	// ftp://download.nvidia.com/XFree86/Linux-x86_64/1.0-9626/README/appendix-d.html
	std::vector<std::string> enabled_displays_in_xinerama_order;
	std::vector<int> enabled_display_nums_in_xinerama_order;
	get_enabled_displays_in_xinerama_order(enabled_displays_in_xinerama_order,
			enabled_display_nums_in_xinerama_order, enabled_displays, xinerama_order);

	XFree(xinerama_order);

	int xinerama_num = 0;
	for (auto display_num2 = enabled_display_nums_in_xinerama_order.begin(); display_num2 != enabled_display_nums_in_xinerama_order.end(); ++display_num2) {
		if (*display_num2 == display_num)
			return screens[xinerama_num];

		xinerama_num++;
	}

	printf("display_num: %d\n", display_num);

	std::cout << "enabled devices in xinerama order:" << std::endl;
	auto num = enabled_display_nums_in_xinerama_order.begin();
	for (auto display_name = enabled_displays_in_xinerama_order.begin();
			display_name != enabled_displays_in_xinerama_order.end();
			++display_name, ++num) {
		std::cout << "- " << *display_name << " (" << *num << ")" << std::endl;
	}

	ERR2("display not found in Xinerama order -> probably not enabled");
}

#endif

xinerama_screen& get_xinerama_screen(display& disp, display::screens_vector& screens, char* name)
{
	if ( !name )
		return screens[0];

	else if ( name[0] >= '0' && name[0] <= '9' ) {
		unsigned number = atoi( name );

		printf("screens: %d\n", screens.size());

		if ( number < 0 || number >= screens.size() )
			ERR2("invalid screen number");

		return screens[number];
	}

#	ifdef ENABLE_NVCTRL

	bool use_nvidia = true;

	int event_basep, error_basep;
	if (! XNVCTRLQueryExtension (disp.dpy, &event_basep, &error_basep) ) {
		use_nvidia = false;
	}

	int nv_screen = DefaultScreen (disp.dpy);

	if ( use_nvidia && ! XNVCTRLIsNvScreen(disp.dpy, nv_screen)) {
		use_nvidia = false;
	}

	if (use_nvidia) {
		return get_xinerama_screen_nvidia(disp, nv_screen, screens, name);
	}

#endif	// ENABLE_NVCTRL

	// It seems to be a name
	// -> look at Xrandr info and find the name

    int screen = DefaultScreen (disp.dpy);
	Window root = RootWindow (disp.dpy, screen);
	XRRScreenResources* res =  XRRGetScreenResources(disp.dpy, root);
	xinerama_screen* result = NULL;
    for (int o = 0; o < res->noutput; o++)
    {
		XRROutputInfo	*output_info = XRRGetOutputInfo (disp.dpy, res, res->outputs[o]);

		/*const char* connection;
		switch (output_info->connection) {
			case RR_Connected:
				connection = "connected";
				break;
			case RR_Disconnected:
				connection = "disconnected";
				break;
			case RR_UnknownConnection:
			default:
				connection = "unknown";
				break;
		}
		printf("name: %s - %s\n", output_info->name, connection);*/

		if ( strcmp(output_info->name, name) != 0 )
			continue;

		//NOTE There is also a list of crtcs...
		/*for (int j=0;j<res->ncrtc;j++) {
			XRRGetCrtcInfo(disp.dpy, res, res->crtcs[j]);
		}*/

		if (output_info->crtc != 0) {
			XRRCrtcInfo* crtc_info = XRRGetCrtcInfo(disp.dpy, res, output_info->crtc);

			//printf("  x = %d, y = %d, width = %d, height = %d\n",
			//	crtc_info->x, crtc_info->y, crtc_info->width, crtc_info->height);

			// find a matching Xinerama screen
			for (unsigned i = 0;i<screens.size();i++) {
				auto& screen = screens[i];
				if (             screen.info.x_org  == crtc_info->x
					&&           screen.info.y_org  == crtc_info->y
					&& (unsigned)screen.info.width  == crtc_info->width
					&& (unsigned)screen.info.height == crtc_info->height ) {
						result = &screen;
						break;
				}
			}

			if (!result)
				ERR2("couldn't find matching Xinerama screen");

			XRRFreeCrtcInfo(crtc_info);
		} else {
			ERR2("no CRTC for that screen");
		}

		XRRFreeOutputInfo(output_info);

		break;
	}

	XRRFreeScreenResources(res);

	if (!result)
		ERR2("no screen with that name");

	return *result;
}

int main( int argc, char *argv[] )
{
	XInitThreads();

	std::string src_name( ":0" ), dst_name( ":1" );
	char *src_screen_name = NULL,
	     *dst_screen_name = NULL;
	bool wiggle = true;

	int opt;
	while ( ( opt = getopt( argc, argv, "s:d:x:D:hw" ) ) != -1 )
		switch ( opt ) {
		case 's':
			src_name = optarg;
			break;
		case 'd':
			dst_name = optarg;
			break;
		case 'x':
			src_screen_name = optarg;
			break;
		case 'D':
			dst_screen_name = optarg;
			break;
		case 'w':
			wiggle = false;
			break;
		default:
			usage( argv[ 0 ] );
		}

	if ( src_name == dst_name )
		ERR;
	display src( src_name ), dst( dst_name );

	auto src_screens = src.xinerama_screens();
	auto dst_screens = dst.xinerama_screens();

	auto &src_screen = get_xinerama_screen(src, src_screens, src_screen_name);
	auto &dst_screen = get_xinerama_screen(dst, dst_screens, dst_screen_name);

	// Clone src not to fight with the blocking loop.
	mouse_replayer mouse( src.clone(), dst, src_screen, dst_screen, wiggle );
	image_replayer image( src, dst, src_screen, dst_screen );

	window root = src.root();
	root.create_damage();

	src.record_pointer_events( &mouse );
	src.select_cursor_input( root );

	for ( ;; ) {
		do {
			const XEvent e = src.next_event();
			if ( e.type == src.damage_event + XDamageNotify ) {
				const XDamageNotifyEvent &de = * (const XDamageNotifyEvent *) &e;
				image.damage( de.area );
			} else if ( e.type == src.xfixes_event + XFixesCursorNotify ) {
				mouse.cursor_changed();
			}
		} while ( src.pending() );

		root.clear_damage();
		image.copy_if_damaged();
	}
}
