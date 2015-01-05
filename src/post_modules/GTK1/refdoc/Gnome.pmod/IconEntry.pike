//! This widget provides the facilities to select an icon. An icon is
//! displayed inside a button, when the button is pressed, an Icon
//! selector (a dialog with a W(GnomeIconSelection) widget) pops up to let
//! the user choose an icon. It also allows one to Drag and Drop the
//! images to and from the preview button.
//!
//!

inherit GTK1.Vbox;

protected Gnome.IconEntry create( string history_id, string title );
//! Creates a new icon entry widget
//!
//!

string get_filename( );
//! Gets the file name of the image if it was possible to load it into
//! the preview. That is, it will only return a filename if the image
//! exists and it was possible to load it as an image.
//!
//!

Gnome.Entry gnome_entry( );
//! Get the W(GnomeEntry) widget that's part of the entry
//!
//!

Gnome.FileEntry gnome_file_entry( );
//! Get the W(GnomeFileEntry) widget that's part of the entry
//!
//!

GTK1.Entry gtk_entry( );
//! Get the W(Entry) widget that's part of the entry
//!
//!

Gnome.IconEntry set_pixmap_subdir( string subdir );
//! Sets the subdirectory below gnome's default pixmap directory to use
//! as the default path for the file entry.
//!
//!
