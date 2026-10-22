import ctypes
from contextlib import ExitStack
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_launcher


class FakeRoot:
    def __init__(self):
        self.calls = []
        self.map_callback = None
        self.tk = Mock()

    def update(self):
        pass

    def title(self, value):
        self.calls.append(('title', value))

    def configure(self, **kwargs):
        self.calls.append(('configure', kwargs))

    def winfo_fpixels(self, _value):
        return 96.0

    def winfo_id(self):
        return 0x123456

    def geometry(self, value):
        self.calls.append(('geometry', value))

    def minsize(self, *value):
        self.calls.append(('minsize', value))

    def protocol(self, *value):
        self.calls.append(('protocol', value))

    def after(self, *value):
        self.calls.append(('after', value))

    def bind(self, sequence, callback, add=None):
        if sequence == '<Map>':
            self.map_callback = callback
        self.calls.append(('bind', sequence, add))
        return 'map-binding'

    def unbind(self, sequence, binding):
        self.calls.append(('unbind', sequence, binding))


class LauncherWindowThemeTests(unittest.TestCase):
    def test_launcher_sets_dark_native_caption_and_light_caption_text(self):
        attributes = []

        def set_window_attribute(hwnd, attribute, value_pointer, size):
            value = ctypes.cast(value_pointer, ctypes.POINTER(ctypes.c_uint32)).contents.value
            attributes.append((hwnd.value, attribute.value, value, getattr(size, 'value', size)))
            return 0

        def get_root_window(hwnd, _flag):
            self.assertEqual(hwnd.value, root.winfo_id())
            return 0x654321

        fake_dwm = SimpleNamespace(DwmSetWindowAttribute=set_window_attribute)
        root = FakeRoot()
        fake_user32 = SimpleNamespace(GetAncestor=get_root_window)
        fake_windll = SimpleNamespace(dwmapi=fake_dwm, user32=fake_user32)

        with ExitStack() as stack:
            stack.enter_context(patch.object(sys, 'platform', 'win32'))
            stack.enter_context(patch.object(uncharted_launcher.ctypes, 'windll', fake_windll))
            stack.enter_context(patch.object(
                uncharted_launcher, 'load_json', return_value={'check_updates': False}))
            stack.enter_context(patch.object(uncharted_launcher, 'work_metrics', return_value=(None, None)))
            stack.enter_context(patch.object(uncharted_launcher.Launcher, 'content', Mock(), create=True))
            stack.enter_context(patch.object(uncharted_launcher.Launcher, 'pages', {}, create=True))
            for method in ('configure_art', 'set_icon', 'load_fonts', 'style', 'build', 'show'):
                stack.enter_context(patch.object(uncharted_launcher.Launcher, method))

            uncharted_launcher.Launcher(root, None, None, None, None)
            self.assertIsNotNone(root.map_callback)
            root.map_callback(SimpleNamespace(widget=SimpleNamespace()))
            self.assertEqual(attributes, [])
            root.map_callback(SimpleNamespace(widget=root))

        settings = {attribute: value for hwnd, attribute, value, _size in attributes
                    if hwnd == 0x654321}
        self.assertEqual(settings.get(20), 1)
        self.assertEqual(settings.get(35), 0x000A0908)  # #08090a caption, COLORREF order
        self.assertEqual(settings.get(36), 0x00E6EFF3)  # #f3efe6 caption text

    def test_non_windows_platform_skips_dwm(self):
        with patch.object(sys, 'platform', 'linux'):
            self.assertFalse(uncharted_launcher.set_dark_title_bar(FakeRoot()))

    def test_legacy_windows_dark_mode_attribute_is_used_as_fallback(self):
        attributes = []

        def set_window_attribute(_hwnd, attribute, _value, _size):
            attributes.append(attribute.value)
            return -1 if attribute.value == 20 else 0

        fake_windll = SimpleNamespace(
            user32=SimpleNamespace(GetAncestor=lambda hwnd, _flag: hwnd.value),
            dwmapi=SimpleNamespace(DwmSetWindowAttribute=set_window_attribute),
        )
        with patch.object(sys, 'platform', 'win32'), \
                patch.object(uncharted_launcher.ctypes, 'windll', fake_windll):
            self.assertTrue(uncharted_launcher.set_dark_title_bar(FakeRoot()))

        self.assertEqual(attributes[:2], [20, 19])


if __name__ == '__main__':
    unittest.main()
