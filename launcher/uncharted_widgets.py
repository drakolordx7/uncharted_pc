"""Launcher widgets on Tk, styled after Drake's journal: square-cornered cards with a ruled heading,
toggle switches, a slider, parchment buttons and bookmark tabs for the sidebar. Edges are
anti-aliased: the shapes are drawn with PIL at four times the size and scaled down (Tk's canvas
draws them jagged). The parchment is a real texture (textures/paper.jpg, ambientCG Paper003, CC0)."""
import math
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageEnhance, ImageTk

SCALE = 4
_cache = {}


def _rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def _photo(key, build):
    if key not in _cache:
        _cache[key] = ImageTk.PhotoImage(build())
    return _cache[key]


def rounded_image(width, height, radius, fill, background, outline=None):
    """A rounded rectangle (anti-aliased) on `background`."""
    def build():
        w, h, r = width * SCALE, height * SCALE, radius * SCALE
        image = Image.new('RGB', (w, h), _rgb(background))
        draw = ImageDraw.Draw(image)
        if outline:
            draw.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=_rgb(outline))
            draw.rounded_rectangle((SCALE, SCALE, w - 1 - SCALE, h - 1 - SCALE), max(0, r - SCALE), fill=_rgb(fill))
        else:
            draw.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=_rgb(fill))
        return image.resize((width, height), Image.LANCZOS)
    return _photo(('round', width, height, radius, fill, background, outline), build)


def texture_path(name):
    """A texture under textures/ (beside this file, or bundled as textures/ in the frozen launcher)."""
    meipass = getattr(sys, '_MEIPASS', None)
    for root in ([Path(meipass)] if meipass else []) + [Path(__file__).resolve().parent]:
        path = root / 'textures' / name
        if path.is_file():
            return path
    return None


_paper = {}


def _paper_source():
    if 'image' not in _paper:
        path = texture_path('paper.jpg')
        try:
            with Image.open(path) as image:
                _paper['image'] = image.convert('RGB')
        except (OSError, TypeError, AttributeError):
            _paper['image'] = None
    return _paper['image']


def paper_image(width, height, radius, background, brightness=1.0, offset=(0, 0)):
    """A rounded piece of parchment (the paper texture, tiled when it is smaller); a flat parchment
    colour when the texture is missing."""
    def build():
        source = _paper_source()
        w, h = max(1, width), max(1, height)
        if source is None:
            piece = Image.new('RGB', (w, h), (224, 204, 162))
        else:
            piece = Image.new('RGB', (w, h))
            ox, oy = offset[0] % source.width, offset[1] % source.height
            for y in range(-oy, h, source.height):
                for x in range(-ox, w, source.width):
                    piece.paste(source, (x, y))
        if brightness != 1.0:
            piece = ImageEnhance.Brightness(piece).enhance(brightness)
        mask = Image.new('L', (w * SCALE, h * SCALE), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * SCALE - 1, h * SCALE - 1), radius * SCALE, fill=255)
        mask = mask.resize((w, h), Image.LANCZOS)
        out = Image.new('RGB', (w, h), _rgb(background))
        out.paste(piece, (0, 0), mask)
        return out
    return _photo(('paper', width, height, radius, background, brightness, offset), build)


def circle_image(diameter, fill, background, outline=None):
    def build():
        d = diameter * SCALE
        image = Image.new('RGB', (d, d), _rgb(background))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, d - 1, d - 1), fill=_rgb(outline or fill))
        if outline:
            draw.ellipse((SCALE, SCALE, d - 1 - SCALE, d - 1 - SCALE), fill=_rgb(fill))
        return image.resize((diameter, diameter), Image.LANCZOS)
    return _photo(('circle', diameter, fill, background, outline), build)


def _corner(radius, fill, background, outline, which):
    """One anti-aliased corner of a card (radius x radius), with its border."""
    def build():
        r = radius * SCALE
        image = Image.new('RGB', (2 * r, 2 * r), _rgb(background))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, 2 * r - 1, 2 * r - 1), fill=_rgb(outline))
        draw.ellipse((SCALE, SCALE, 2 * r - 1 - SCALE, 2 * r - 1 - SCALE), fill=_rgb(fill))
        box = {'nw': (0, 0, r, r), 'ne': (r, 0, 2 * r, r), 'sw': (0, r, r, 2 * r), 'se': (r, r, 2 * r, 2 * r)}[which]
        return image.crop(box).resize((radius, radius), Image.LANCZOS)
    return _photo(('corner', radius, fill, background, outline, which), build)


class Card:
    """A panel; widgets go into `inner` (a frame, grid columns 0 = label, 1 = control). The heading,
    when there is one, is set in the brand face with a ruled line after it, as in a field journal.
    wrap(label, offset) keeps a label's text wrapped to the card's width."""

    def __init__(self, tk, parent, colors, px, title=None, font=None):
        self.colors, self.px = colors, px
        self.radius = px(6)
        self.pad = px(20)
        self.wrapped = []
        bg, card = colors['surface'], colors['card']
        self.canvas = tk.Canvas(parent, bg=bg, highlightthickness=0, bd=0, height=10)
        self.inner = tk.Frame(self.canvas, bg=card)
        self.inner.columnconfigure(1, weight=1)
        self.window = self.canvas.create_window(self.pad, self.pad - px(4), window=self.inner, anchor='nw')
        self.canvas.bind('<Configure>', self._draw)
        self.inner.bind('<Configure>', self._fit)
        self._fit_pending = False
        self.refit()
        if title:
            head = tk.Frame(self.inner, bg=card)
            head.grid(row=0, column=0, columnspan=4, sticky='ew', pady=(0, px(8)))
            tk.Label(head, text=title, bg=card, fg=colors['gold'], font=font, anchor='w').pack(side='left')
            tk.Frame(head, bg=colors['line'], height=1).pack(side='left', fill='x', expand=True,
                                                             padx=(px(12), 0), pady=(px(2), 0))

    def wrap(self, label, offset=0):
        self.wrapped.append((label, offset))
        return label

    def refit(self):
        """Measure the content again once Tk is idle (the inner frame only reports its size while it
        is shown, so new content cannot wait for that)."""
        if not self._fit_pending:
            self._fit_pending = True
            self.canvas.after_idle(self._fit)

    def _fit(self, _event=None):
        self._fit_pending = False
        height = self.inner.winfo_reqheight() + 2 * self.pad - self.px(6)
        if int(self.canvas['height']) != height:
            self.canvas.configure(height=height)

    def _draw(self, _event=None):
        c, colors = self.canvas, self.colors
        w, h, r = c.winfo_width(), c.winfo_height(), self.radius
        if w < 2 * r + 2 or h < 2 * r + 2:
            return
        c.itemconfigure(self.window, width=w - 2 * self.pad)
        for label, offset in self.wrapped:
            try:
                label.configure(wraplength=max(self.px(160), w - 2 * self.pad - offset))
            except Exception:  # a label of a list that was rebuilt
                pass
        self.refit()  # text wraps differently at a new width
        c.delete('bg')
        fill, line, bg = colors['card'], colors.get('card_line', colors['line']), colors['surface']
        c.create_rectangle(r, 0, w - r, h, fill=fill, outline='', tags='bg')
        c.create_rectangle(0, r, w, h - r, fill=fill, outline='', tags='bg')
        c.create_line(r, 0, w - r, 0, fill=line, tags='bg')
        c.create_line(r, h - 1, w - r, h - 1, fill=line, tags='bg')
        c.create_line(0, r, 0, h - r, fill=line, tags='bg')
        c.create_line(w - 1, r, w - 1, h - r, fill=line, tags='bg')
        for which, x, y, anchor in (('nw', 0, 0, 'nw'), ('ne', w, 0, 'ne'), ('sw', 0, h, 'sw'), ('se', w, h, 'se')):
            c.create_image(x, y, image=_corner(r, fill, bg, line, which), anchor=anchor, tags='bg')
        c.tag_lower('bg')


class Toggle:
    """An on/off switch bound to a BooleanVar."""

    def __init__(self, tk, parent, variable, colors, px, background=None):
        self.var, self.colors = variable, colors
        self.w, self.h = px(40), px(22)
        self.bg = background or colors['card']
        self.canvas = tk.Canvas(parent, width=self.w, height=self.h, bg=self.bg, highlightthickness=0, bd=0,
                                cursor='hand2')
        self.canvas.bind('<Button-1>', lambda _e: self.var.set(not self.var.get()))
        self.var.trace_add('write', lambda *_a: self.draw())
        self.draw()

    def draw(self):
        c, colors, on = self.canvas, self.colors, bool(self.var.get())
        c.delete('all')
        track = colors['gold'] if on else colors['track']
        c.create_image(0, 0, anchor='nw', image=rounded_image(self.w, self.h, self.h // 2, track, self.bg))
        d = self.h - 6
        knob = circle_image(d, colors.get('ink', '#14110a') if on else colors['knob'], track)
        c.create_image(self.w - 3 - d if on else 3, 3, anchor='nw', image=knob)

    def grid(self, **kw):
        self.canvas.grid(**kw)

    def pack(self, **kw):
        self.canvas.pack(**kw)


class Slider:
    """A horizontal slider bound to a StringVar holding a number; shows the value next to it.
    log=True spaces the range logarithmically (a wide range stays fine to set near its low end);
    entry=True makes the value a box the number can be typed into (clamped to the range)."""

    def __init__(self, tk, parent, variable, low, high, step, colors, px, font, fmt='{:.2f}', width=320,
                 log=False, entry=False, suffix='×'):
        self.var, self.low, self.high, self.step, self.fmt, self.log = variable, low, high, step, fmt, log
        self.suffix = suffix
        self.colors, self.px = colors, px
        self.frame = tk.Frame(parent, bg=colors['card'])
        self.w, self.h = px(width), px(26)
        self.canvas = tk.Canvas(self.frame, width=self.w, height=self.h, bg=colors['card'], highlightthickness=0,
                                bd=0, cursor='hand2')
        self.canvas.pack(side='left')
        if entry:
            box = tk.Entry(self.frame, textvariable=variable, width=6, font=font, bg=colors['card_hi'], fg=colors['text'],
                           insertbackground=colors['text'], relief='flat', highlightthickness=1,
                           highlightbackground=colors['card_hi'], highlightcolor=colors['gold'])
            box.pack(side='left', padx=(px(12), 0), ipady=2)
            for sequence in ('<Return>', '<FocusOut>'):
                box.bind(sequence, lambda _e: self.var.set(self.fmt.format(self.value())))
            self.label = tk.Label(self.frame, bg=colors['card'], fg=colors['muted'], font=font, text='×')
        else:
            self.label = tk.Label(self.frame, bg=colors['card'], fg=colors['text'], font=font, width=6, anchor='w')
        self.label.pack(side='left', padx=(px(12) if not entry else px(4), 0))
        self.entry = entry
        for sequence in ('<Button-1>', '<B1-Motion>'):
            self.canvas.bind(sequence, self._pick)
        self.var.trace_add('write', lambda *_a: self.draw())
        self.draw()

    def value(self):
        try:
            return min(self.high, max(self.low, float(self.var.get())))
        except ValueError:
            return self.low

    def _pick(self, event):
        r = self.h // 2
        f = min(1.0, max(0.0, (event.x - r) / max(1, self.w - 2 * r)))
        if self.log:
            v = self.low * (self.high / self.low) ** f
            v = round(v, 2) if v < 1 else round(v, 1) if v < 10 else round(v)
        else:
            v = self.low + f * (self.high - self.low)
            v = round(round(v / self.step) * self.step, 4)
        self.var.set(self.fmt.format(min(self.high, max(self.low, v))))

    def draw(self):
        c, colors = self.canvas, self.colors
        c.delete('all')
        r, y = self.h // 2, self.h // 2
        t = max(2, self.px(4))
        f = math.log(self.value() / self.low) / math.log(self.high / self.low) if self.log             else (self.value() - self.low) / (self.high - self.low)
        x = r + f * (self.w - 2 * r)
        c.create_line(r, y, self.w - r, y, fill=colors['track'], width=t, capstyle='round')
        c.create_line(r, y, x, y, fill=colors['gold'], width=t, capstyle='round')
        d = self.px(18)
        c.create_image(x, y, image=circle_image(d, colors['knob_on'], colors['card'], colors['gold']))
        if not self.entry:
            self.label.configure(text=self.fmt.format(self.value()) + self.suffix)

    def grid(self, **kw):
        self.frame.grid(**kw)

    def lift(self):
        self.frame.lift()


class PillButton:
    """A button; kind 'primary' (a parchment plate with ink text), 'secondary' (outlined) or 'ghost'
    (text only until hovered). configure(state=, text=)."""

    def __init__(self, tk, parent, text, command, colors, px, font, kind='secondary', background=None,
                 padding=(18, 9), icon=None, icon_font=None):
        self.tk, self.colors, self.px, self.font, self.kind = tk, colors, px, font, kind
        self.command, self.text, self.icon, self.icon_font = command, text, icon, icon_font
        self.bg = background or colors['card']
        self.padding = padding
        self.state, self.hover = 'normal', False
        self.canvas = tk.Canvas(parent, bg=self.bg, highlightthickness=0, bd=0, cursor='hand2')
        self.canvas.bind('<Enter>', lambda _e: self._hover(True))
        self.canvas.bind('<Leave>', lambda _e: self._hover(False))
        self.canvas.bind('<ButtonRelease-1>', self._click)
        self.draw()

    def _hover(self, inside):
        self.hover = inside
        self.draw()

    def _click(self, event):
        if self.state == 'normal' and 0 <= event.x < self.canvas.winfo_width() and \
                0 <= event.y < self.canvas.winfo_height() and self.command:
            self.command()

    def configure(self, **kw):
        if 'state' in kw:
            self.state = kw['state']
        if 'text' in kw:
            self.text = kw['text']
        self.draw()

    config = configure

    def draw(self):
        import tkinter.font as tkfont
        c, colors = self.canvas, self.colors
        font = tkfont.Font(font=self.font)
        label = self.text
        text_w = font.measure(label)
        icon_w = 0
        if self.icon:
            icon_w = tkfont.Font(font=self.icon_font).measure(self.icon) + self.px(10)
        w = text_w + icon_w + 2 * self.px(self.padding[0])
        h = font.metrics('linespace') + 2 * self.px(self.padding[1])
        c.configure(width=w, height=h, cursor='hand2' if self.state == 'normal' else 'arrow')
        c.delete('all')
        disabled = self.state != 'normal'
        radius = max(2, self.px(4))
        if self.kind == 'primary':
            fg = colors.get('ink', '#16120a')
            if disabled:
                fg = colors['muted']
                image = rounded_image(w, h, radius, colors['track'], self.bg)
            else:
                image = paper_image(w, h, radius, self.bg, 1.06 if self.hover else 1.0, (w * 7, h * 3))
        elif self.kind == 'ghost':
            fill = colors['card_hi'] if self.hover and not disabled else self.bg
            fg = colors['muted'] if disabled else (colors['text'] if self.hover else colors['muted'])
            image = rounded_image(w, h, radius, fill, self.bg)
        else:
            fill = colors['button_hi'] if self.hover and not disabled else self.bg
            edge = colors['line'] if disabled else colors.get('line_hi', colors['line'])
            fg = colors['muted'] if disabled else colors['text']
            image = rounded_image(w, h, radius, fill, self.bg, edge)
        c.create_image(0, 0, anchor='nw', image=image)
        x = w // 2 - (text_w + icon_w) // 2
        if self.icon:
            c.create_text(x, h // 2, text=self.icon, anchor='w', fill=fg, font=self.icon_font)
            x += icon_w
        c.create_text(x, h // 2, text=label, anchor='w', fill=fg, font=self.font)

    def pack(self, **kw):
        self.canvas.pack(**kw)

    def grid(self, **kw):
        self.canvas.grid(**kw)

    def lift(self):
        self.canvas.lift()


class NavTab:
    """One sidebar entry. The open page's entry is a parchment bookmark that runs into the page;
    compact=True shows the icon alone (narrow windows)."""

    def __init__(self, tk, parent, text, icon, command, colors, px, font, icon_font, background):
        self.text, self.icon, self.colors, self.px = text, icon, colors, px
        self.font, self.icon_font, self.bg = font, icon_font, background
        self.active = self.hover = self.compact = False
        self.canvas = tk.Canvas(parent, bg=background, height=px(40), highlightthickness=0, bd=0,
                                cursor='hand2')
        self.canvas.bind('<Configure>', lambda _e: self.draw())
        self.canvas.bind('<Enter>', lambda _e: (self.set(hover=True), self._tip(True)))
        self.canvas.bind('<Leave>', lambda _e: (self.set(hover=False), self._tip(False)))
        self.canvas.bind('<Button-1>', lambda _e: (self._tip(False), command()))
        self.tk, self.tip = tk, None

    def _tip(self, show):
        """The page name beside the icon while the sidebar is folded."""
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None
        if not (show and self.compact):
            return
        c = self.canvas
        self.tip = tip = self.tk.Toplevel(c)
        tip.overrideredirect(True)
        tip.configure(bg=self.colors['line'])
        self.tk.Label(tip, text=self.text, bg=self.colors['card'], fg=self.colors['text'], font=self.font,
                      padx=self.px(10), pady=self.px(4)).pack(padx=1, pady=1)
        tip.geometry(f'+{c.winfo_rootx() + c.winfo_width() + self.px(6)}+{c.winfo_rooty() + self.px(6)}')

    def set(self, active=None, hover=None, compact=None):
        changed = False
        for name, value in (('active', active), ('hover', hover), ('compact', compact)):
            if value is not None and getattr(self, name) != value:
                setattr(self, name, value)
                changed = True
        if changed:
            self.draw()

    def draw(self):
        c, colors, px = self.canvas, self.colors, self.px
        w, h = c.winfo_width(), c.winfo_height()
        if w < 8 or h < 8:
            return
        c.delete('all')
        left = px(8) if self.compact else px(12)
        if self.active:
            # Wider than the canvas: the right-hand corners are cut off, so the tab meets the page.
            c.create_image(left, px(3), anchor='nw', image=paper_image(
                w - left + px(8), h - px(6), max(2, px(4)), self.bg, 1.0, (left * 13, h)))
            fg = icon_fg = colors.get('ink', '#16120a')
        elif self.hover:
            c.create_image(left, px(3), anchor='nw', image=rounded_image(
                w - left + px(8), h - px(6), max(2, px(4)), colors.get('side_hi', colors['card']), self.bg))
            fg, icon_fg = colors['text'], colors['gold']
        else:
            fg, icon_fg = colors['muted'], colors['muted']
        y = h // 2
        if self.compact:
            if self.icon and self.icon_font:
                c.create_text((w + left) // 2, y, text=self.icon, fill=icon_fg, font=self.icon_font)
            else:
                c.create_text((w + left) // 2, y, text=self.text[:1], fill=fg, font=self.font)
            return
        x = left + px(14)
        if self.icon and self.icon_font:
            c.create_text(x, y, text=self.icon, anchor='w', fill=icon_fg, font=self.icon_font)
            x += px(32)
        c.create_text(x, y, text=self.text, anchor='w', fill=fg, font=self.font)

    def pack(self, **kw):
        self.canvas.pack(**kw)
