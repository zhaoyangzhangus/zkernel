#!/usr/bin/env python3
"""把现有矢量图标编译为应用包中的 PNG；不依赖宿主图形会话。"""
import ctypes as C
import os
from pathlib import Path
import sys
import tempfile


def render(source, target):
    rsvg = C.CDLL('librsvg-2.so.2')
    cairo = C.CDLL('libcairo.so.2')
    objects = C.CDLL('libgobject-2.0.so.0')
    class Rect(C.Structure):
        _fields_ = [(key, C.c_double) for key in ('x', 'y', 'width', 'height')]
    rsvg.rsvg_handle_new_from_file.argtypes = [C.c_char_p, C.c_void_p]
    rsvg.rsvg_handle_new_from_file.restype = C.c_void_p
    rsvg.rsvg_handle_render_document.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(Rect), C.c_void_p]
    rsvg.rsvg_handle_render_document.restype = C.c_int
    cairo.cairo_image_surface_create.argtypes = [C.c_int, C.c_int, C.c_int]
    cairo.cairo_image_surface_create.restype = C.c_void_p
    cairo.cairo_create.argtypes = [C.c_void_p]
    cairo.cairo_create.restype = C.c_void_p
    cairo.cairo_surface_status.argtypes = [C.c_void_p]
    cairo.cairo_status.argtypes = [C.c_void_p]
    cairo.cairo_surface_write_to_png.argtypes = [C.c_void_p, C.c_char_p]
    cairo.cairo_destroy.argtypes = [C.c_void_p]
    cairo.cairo_surface_destroy.argtypes = [C.c_void_p]
    objects.g_object_unref.argtypes = [C.c_void_p]
    handle = rsvg.rsvg_handle_new_from_file(os.fsencode(source), None)
    if not handle:
        raise RuntimeError('无法读取 SVG: ' + str(source))
    surface = cairo.cairo_image_surface_create(0, 256, 256)
    context = cairo.cairo_create(surface)
    try:
        if (cairo.cairo_surface_status(surface) or cairo.cairo_status(context) or
                not rsvg.rsvg_handle_render_document(handle, context, C.byref(Rect(0, 0, 256, 256)), None) or
                cairo.cairo_status(context)):
            raise RuntimeError('图标绘制失败: ' + str(source))
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.icon-', dir=target.parent) as directory:
            candidate = Path(directory) / 'icon.png'
            if cairo.cairo_surface_write_to_png(surface, os.fsencode(candidate)):
                raise RuntimeError('图标保存失败: ' + str(target))
            os.replace(candidate, target)
    finally:
        cairo.cairo_destroy(context)
        cairo.cairo_surface_destroy(surface)
        objects.g_object_unref(handle)


if __name__ == '__main__':
    render(Path(sys.argv[1]), Path(sys.argv[2]))
