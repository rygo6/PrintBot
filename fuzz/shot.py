import sys, gi
gi.require_version('Gdk','3.0'); gi.require_version('GdkX11','3.0')
from gi.repository import Gdk, GdkX11
disp = GdkX11.X11Display.get_default()
w = GdkX11.X11Window.foreign_new_for_display(disp, int(sys.argv[1],16))
g = w.get_geometry()
pb = Gdk.pixbuf_get_from_window(w, 0, 0, g.width, g.height)
pb.savev(sys.argv[2], 'png', [], [])
print(g.width, g.height)
