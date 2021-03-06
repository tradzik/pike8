//! Some gtk widgets don't have associated X windows, so they just draw
//! on their parents. Because of this, they cannot receive events and
//! if they are incorrectly sized, they don't clip so you can get messy
//! overwritting etc. If you require more from these widgets, the
//! EventBox is for you.
//!
//! At first glance, the EventBox widget might appear to be totally
//! useless. It draws nothing on the screen and responds to no
//! events. However, it does serve a function - it provides an X window
//! for its child widget. This is important as many GTK widgets do not
//! have an associated X window. Not having an X window saves memory
//! and improves performance, but also has some drawbacks. A widget
//! without an X window cannot receive events, and does not perform any
//! clipping on it's contents. Although the name EventBox emphasizes
//! the event-handling function, the widget can also be used for
//! clipping.
//! 
//! The primary use for this widget is when you want to receive events
//! for a widget without a window. Examples of such widgets are labels
//! and images.
//! 
//!@expr{ GTK1.EventBox()->set_usize(100,100)@}
//!@xml{<image>../images/gtk1_eventbox.png</image>@}
//!
//!
//!

inherit GTK1.Bin;

protected GTK1.EventBox create( );
//! Create a new event box widget
//!
//!
