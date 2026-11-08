"""Robot library: checks on the Field Device framebuffer capture (800 x 480). Colours are the design tokens of
docs/lcd-ui-design/style-guide.md (theme.h)."""
from PIL import Image, ImageChops

BG = (0x0D, 0x11, 0x17)
SURF = (0x16, 0x1B, 0x22)
ACCENT = (0x7B, 0xAF, 0xD4)


def _near(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


class Screens:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def pixel_should_be_near(self, path, x, y, rgb, tolerance=6):
        """The pixel (x, y) is within `tolerance` of the colour given as 'R,G,B'."""
        want = tuple(int(v) for v in str(rgb).split(","))
        got = Image.open(path).convert("RGB").getpixel((int(x), int(y)))
        if not _near(got, want, int(tolerance)):
            raise AssertionError("pixel (%s,%s) is %s, expected about %s" % (x, y, got, want))

    def region_should_contain_colour(self, path, x1, y1, x2, y2, rgb, min_pixels=20, tolerance=10):
        """At least `min_pixels` pixels in the box are within `tolerance` of the colour 'R,G,B'."""
        want = tuple(int(v) for v in str(rgb).split(","))
        img = Image.open(path).convert("RGB").crop((int(x1), int(y1), int(x2), int(y2)))
        n = sum(1 for p in img.getdata() if _near(p, want, int(tolerance)))
        if n < int(min_pixels):
            raise AssertionError("only %d pixels near %s in the box (need %s)" % (n, want, min_pixels))

    def count_colour_pixels(self, path, x1, y1, x2, y2, rgb, tolerance=10):
        """Number of pixels in the box within `tolerance` of the colour 'R,G,B'."""
        want = tuple(int(v) for v in str(rgb).split(","))
        img = Image.open(path).convert("RGB").crop((int(x1), int(y1), int(x2), int(y2)))
        return sum(1 for p in img.getdata() if _near(p, want, int(tolerance)))

    def region_should_have_text(self, path, x1, y1, x2, y2, min_bright=30):
        """The box holds bright text pixels (luminance above 150)."""
        img = Image.open(path).convert("L").crop((int(x1), int(y1), int(x2), int(y2)))
        n = sum(1 for p in img.getdata() if p > 150)
        if n < int(min_bright):
            raise AssertionError("no text in the box: %d bright pixels" % n)

    def regions_should_match_golden(self, path, golden, x1, y1, x2, y2, tolerance=12, max_bad_percent=1.0):
        """The box of the capture equals the same box of the simulator's golden image."""
        a = Image.open(path).convert("RGB").crop((int(x1), int(y1), int(x2), int(y2)))
        b = Image.open(golden).convert("RGB").crop((int(x1), int(y1), int(x2), int(y2)))
        diff = ImageChops.difference(a, b).convert("L")
        bad = sum(1 for p in diff.getdata() if p > int(tolerance))
        pct = 100.0 * bad / (a.width * a.height)
        if pct > float(max_bad_percent):
            raise AssertionError("%.2f%% of the box differs from the golden image (limit %s%%)" % (pct, max_bad_percent))

    def screen_should_not_be_blank(self, path):
        img = Image.open(path).convert("RGB")
        if len(set(img.resize((80, 48)).getdata())) < 6:
            raise AssertionError("the framebuffer is blank")
