"""The in-game menu key (Controls page, input.ini key.overlay)."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_i18n

SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


def ini_lines(settings):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'input.ini'
        with mock.patch.object(LAUNCHER, 'input_ini_path', return_value=path):
            LAUNCHER.write_input_ini(settings)
        return path.read_text(encoding='utf-8').splitlines()


def fake_launcher(bindings=None):
    """The part of Launcher that set_key and key_list use, without a window."""
    app = {'key_bindings': dict(bindings or {})}
    shown = []
    this = SimpleNamespace(app=app, capturing=('overlay', 0), shown=shown,
                           status=mock.Mock(), release_capture=lambda: None,
                           show_keys=lambda: shown.append(1))
    this.key_list = lambda action: LAUNCHER.Launcher.key_list(this, action)
    this.set_key = lambda action, slot, name: LAUNCHER.Launcher.set_key(this, action, slot, name)
    return this


class OverlayKeyIniTests(unittest.TestCase):
    def test_default_is_insert(self):
        self.assertEqual(LAUNCHER.DEFAULT_KEYS['overlay'], 'Insert')
        self.assertIn('key.overlay=Insert', ini_lines({}))

    def test_saved_key_is_written(self):
        self.assertIn('key.overlay=F10', ini_lines({'key_bindings': {'overlay': 'F10'}}))

    def test_two_bindings_and_mouse_button_are_written(self):
        lines = ini_lines({'key_bindings': {'overlay': 'F10|Mouse Middle'}})
        self.assertIn('key.overlay=F10|Mouse Middle', lines)

    def test_cleared_key_is_written_empty(self):
        lines = ini_lines({'key_bindings': {'overlay': ''}})
        self.assertIn('key.overlay=', lines)
        self.assertNotIn('key.overlay=Insert', lines)

    def test_other_bindings_are_unchanged(self):
        lines = ini_lines({'key_bindings': {'overlay': 'F10'}})
        self.assertIn('key.cross=Space', lines)
        self.assertIn('key.options=Escape|Return', lines)
        self.assertNotIn('pad.overlay=none', lines)

    def test_game_environment_exports_the_file_with_the_key(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            settings = {**LAUNCHER.APP_DEFAULTS, 'game_dir': 'game',
                        'key_bindings': {'overlay': 'F9'}}
            with mock.patch.dict(os.environ, {'BB_INPUT_CONFIG': str(path)}):
                env = LAUNCHER.game_environment(settings)
            self.assertEqual(env['BB_INPUT_CONFIG'], str(path))
            self.assertIn('key.overlay=F9\n', path.read_text(encoding='utf-8'))


class OverlayKeyCaptureTests(unittest.TestCase):
    def test_f_keys_and_insert_are_names_the_widget_accepts(self):
        self.assertEqual(LAUNCHER.TK_KEYS['Insert'], 'Insert')
        self.assertNotIn('Insert', LAUNCHER.RESERVED_KEYS)

    def test_setting_the_key_is_saved(self):
        this = fake_launcher()
        this.set_key('overlay', 0, 'F10')
        self.assertEqual(this.app['key_bindings']['overlay'], 'F10')
        self.assertIsNone(this.capturing)

    def test_a_key_of_a_game_action_moves_to_the_menu(self):
        this = fake_launcher()
        this.set_key('overlay', 0, 'Space')  # Cross's default key
        self.assertEqual(this.app['key_bindings']['overlay'], 'Space')
        self.assertEqual(this.app['key_bindings']['cross'], '')

    def test_the_menu_key_moves_to_a_game_action(self):
        this = fake_launcher()
        this.set_key('cross', 0, 'Insert')
        self.assertEqual(this.app['key_bindings']['cross'], 'Insert')
        self.assertEqual(this.app['key_bindings']['overlay'], '')
        lines = ini_lines(this.app)
        self.assertIn('key.overlay=', lines)
        self.assertIn('key.cross=Insert', lines)

    def test_left_and_right_buttons_and_wheel_are_refused(self):
        for name in ('Mouse Left', 'Mouse Right', 'Wheel Up', 'Wheel Down'):
            with self.subTest(name=name):
                this = fake_launcher()
                this.set_key('overlay', 0, name)
                self.assertNotIn('overlay', this.app['key_bindings'])
                this.status.configure.assert_called_once()

    def test_side_and_middle_buttons_are_accepted(self):
        for name in ('Mouse Middle', 'Mouse 4', 'Mouse 5'):
            with self.subTest(name=name):
                this = fake_launcher()
                this.set_key('overlay', 0, name)
                self.assertEqual(this.app['key_bindings']['overlay'], name)


class OverlayKeyTextTests(unittest.TestCase):
    def test_label_is_translated_into_every_language(self):
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            for text in ('Open in-game menu',
                         'The in-game menu cannot use the left or right mouse button or the wheel.'):
                with self.subTest(language=language, text=text):
                    self.assertNotEqual(uncharted_i18n.translate(language, text), text)


if __name__ == '__main__':
    unittest.main()
