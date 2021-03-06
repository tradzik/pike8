//! The drawing area is a window you can draw in.
//! Please note that you @b{must@} handle refresh and resize events
//! on your own. Use W(pDrawingArea) for a drawingarea with automatic
//! refresh/resize handling.
//!@expr{ GTK1.DrawingArea()->set_usize(100,100)@}
//!@xml{<image>../images/gtk1_drawingarea.png</image>@}
//!
//!
//!

inherit GTK1.Widget;

GTK1.DrawingArea clear( int|void x, int|void y, int|void width, int|void height );
//! Either clears the rectangle defined by the arguments, of if no
//! arguments are specified, the whole drawable.
//!
//!

GTK1.DrawingArea copy_area( GDK1.GC gc, int xdest, int ydest, GTK1.Widget source, int xsource, int ysource, int width, int height );
//! Copies the rectangle defined by xsource,ysource and width,height
//! from the source drawable, and places the results at xdest,ydest in
//! the drawable in which this function is called.
//!
//!

protected GTK1.DrawingArea create( );
//!

GTK1.DrawingArea draw_arc( GDK1.GC gc, int filledp, int x1, int y1, int x2, int y2, int angle1, int angle2 );
//! Draws a single circular or elliptical arc.  Each arc is specified
//! by a rectangle and two angles. The center of the circle or ellipse
//! is the center of the rectangle, and the major and minor axes are
//! specified by the width and height.  Positive angles indicate
//! counterclockwise motion, and negative angles indicate clockwise
//! motion. If the magnitude of angle2 is greater than 360 degrees,
//! it is truncated to 360 degrees.
//!
//!

GTK1.DrawingArea draw_bitmap( GDK1.GC gc, GDK1.Bitmap bitmap, int xsrc, int ysrc, int xdest, int ydest, int width, int height );
//! Draw a GDK(Bitmap) in this drawable.
//! @b{NOTE:@} This drawable must be a bitmap as well. This will be
//! fixed in GTK 1.3
//!
//!

GTK1.DrawingArea draw_image( GDK1.GC gc, GDK1.Image image, int xsrc, int ysrc, int xdest, int ydest, int width, int height );
//! Draw the rectangle specified by xsrc,ysrc+width,height from the
//! GDK(Image) at xdest,ydest in the destination drawable
//!
//!

GTK1.DrawingArea draw_line( GDK1.GC gc, int x1, int y1, int x2, int y2 );
//! img_begin
//! w = GTK1.DrawingArea()->set_usize(100,100);
//! delay: g = GDK1.GC(w)->set_foreground( GDK1.Color(255,0,0) );
//! delay:  for(int x = 0; x<10; x++) w->draw_line(g,x*10,0,100-x*10,99);
//! img_end
//!
//!

GTK1.DrawingArea draw_pixmap( GDK1.GC gc, GDK1.Pixmap pixmap, int xsrc, int ysrc, int xdest, int ydest, int width, int height );
//! Draw the rectangle specified by xsrc,ysrc+width,height from the
//! GDK(Pixmap) at xdest,ydest in the destination drawable
//!
//!

GTK1.DrawingArea draw_point( GDK1.GC gc, int x, int y );
//! img_begin
//! w = GTK1.DrawingArea()->set_usize(10,10);
//! delay: g = GDK1.GC(w)->set_foreground( GDK1.Color(255,0,0) );
//! delay:  for(int x = 0; x<10; x++) w->draw_point(g, x, x);
//! img_end
//!
//!

GTK1.DrawingArea draw_rectangle( GDK1.GC gc, int filledp, int x1, int y1, int x2, int y2 );
//! img_begin
//!  w = GTK1.DrawingArea()->set_usize(100,100);
//! delay:  g = GDK1.GC(w)->set_foreground( GDK1.Color(255,0,0) );
//! delay: for(int x = 0; x<10; x++) w->draw_rectangle(g,0,x*10,0,100-x*10,99);
//! img_end
//! img_begin
//! w = GTK1.DrawingArea()->set_usize(100,100);
//! delay:   g = GDK1.GC(w);
//! delay:  for(int x = 0; x<30; x++) {
//! delay:   g->set_foreground(GDK1.Color(random(255),random(255),random(255)) );
//! delay:   w->draw_rectangle(g,1,x*10,0,100-x*10,99);
//! delay: }
//! img_end
//!
//!

GTK1.DrawingArea draw_text( GDK1.GC gc, GDK1.Font font, int x, int y, string text, int forcewide );
//! y is used as the baseline for the text.
//! If forcewide is true, the string will be expanded to a wide string
//! even if it is not already one. This is useful when writing text
//! using either unicode or some other 16 bit font.
//!
//!

GTK1.DrawingArea size( int width, int height );
//! This function is OBSOLETE
//!
//!
